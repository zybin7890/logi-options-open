#pragma once
#include "hidpp.hpp"

#include <algorithm>
#include <functional>

namespace logi::owner_profile {
struct Metadata {
    std::string name;
    std::vector<unsigned> choices;
    bool valid() const {
        return !name.empty() && name.size()<=255 && !choices.empty() && choices.size()<=2048 &&
            std::ranges::all_of(name,[](unsigned char ch){return ch>=32 && ch!=127;}) &&
            choices.front()>0 && choices.back()<=65535 &&
            std::adjacent_find(choices.begin(),choices.end(),std::greater_equal<>{})==choices.end();
    }
};
struct Features {
    Byte dpi{},shift{},wheel{},thumb{},buttons{},battery{};
    bool enhanced{},unified{};
    bool complete() const {
        // If an optional lookup timed out its zero must not be cached as absence.
        return dpi && shift && wheel && thumb && buttons && battery && enhanced && unified;
    }
};
struct Verified {
    OwnerIdentity identity;
    Features features;
    Metadata metadata;
};
inline bool populated(std::span<const Byte> bytes) {
    return !std::ranges::all_of(bytes,[](Byte b){return b==0;}) &&
           !std::ranges::all_of(bytes,[](Byte b){return b==0xff;});
}
inline std::optional<OwnerIdentity> verify_identity(const Device& device,const std::wstring& route,
        std::span<const Byte> info,std::span<const Byte> fw) {
    if(route.empty() || info.size()<13 || !info[0] || info[0]>8 || !(info[6]&15) ||
       fw.size()<8 || fw[0]!=0 || !populated(info.subspan(1,4)) || !populated(info.subspan(7,6)) ||
       !std::ranges::all_of(fw.subspan(1,3),[](Byte b){return b>=32 && b<127;}) ||
       !populated(fw.subspan(4,4)))return {};
    OwnerIdentity id{route,device.slot,device.major,device.minor};
    std::ranges::copy(info.subspan(1,4),id.unit.begin());
    std::ranges::copy(info.subspan(7,6),id.model.begin());
    std::ranges::copy(fw.subspan(1,7),id.firmware.begin());
    return id;
}

// Only display name and the sensor's advertised DPI choices go to HKCU.
// A persisted record can never supply an index used to address the device.
inline constexpr DWORD max_bytes=16384;
inline std::vector<Byte> prefix(const OwnerIdentity& id) {
    if(id.route.empty() || id.route.size()>2048)return {};
    std::vector<Byte> out{'L','N','M',1,id.slot,id.major,id.minor};
    out.insert(out.end(),id.unit.begin(),id.unit.end());
    out.insert(out.end(),id.model.begin(),id.model.end());
    out.insert(out.end(),id.firmware.begin(),id.firmware.end());
    out.push_back(static_cast<Byte>(id.route.size()>>8));out.push_back(static_cast<Byte>(id.route.size()));
    for(auto ch:id.route){out.push_back(static_cast<Byte>(ch>>8));out.push_back(static_cast<Byte>(ch));}
    return out;
}
inline std::uint32_t checksum(std::span<const Byte> data) {
    std::uint32_t hash=2166136261u;
    for(auto byte:data){hash^=byte;hash*=16777619u;}
    return hash;
}
inline std::vector<Byte> encode(const OwnerIdentity& id,const Metadata& data) {
    auto out=prefix(id);
    if(out.empty()||!data.valid())return {};
    out.push_back(static_cast<Byte>(data.name.size()));
    out.insert(out.end(),data.name.begin(),data.name.end());
    for(auto value:data.choices){out.push_back(static_cast<Byte>(value>>8));out.push_back(static_cast<Byte>(value));}
    auto hash=checksum(out);
    for(unsigned i=0;i<4;++i){out.push_back(static_cast<Byte>(hash));hash>>=8;}
    return out;
}
inline std::optional<Metadata> decode(const OwnerIdentity& id,std::span<const Byte> bytes) {
    const auto key=prefix(id);
    if(key.empty() || bytes.size()>max_bytes || bytes.size()<key.size()+8 || !std::ranges::starts_with(bytes,key))return {};
    const auto payload=bytes.first(bytes.size()-4);
    auto hash=checksum(payload);
    for(auto b:bytes.last(4)){if(b!=static_cast<Byte>(hash))return {}; hash>>=8;}
    auto data=payload.subspan(key.size());
    const auto length=data[0];
    if(!length || data.size()<1u+length+2)return {};
    Metadata result{std::string(reinterpret_cast<const char*>(data.data()+1),length),{}};
    data=data.subspan(1u+length);
    if(data.size()%2 || data.size()>4096)return {};
    for(std::size_t i=0;i<data.size();i+=2)result.choices.push_back((unsigned(data[i])<<8)|data[i+1]);
    if(!result.valid())return {};
    return result;
}
} // namespace logi::owner_profile
