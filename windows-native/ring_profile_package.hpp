#pragma once

#include "ring_metadata.hpp"
#include "zip_codec.hpp"
#include "ui_settings.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace ring_profile_package {
inline constexpr std::wstring_view marker=L"# LogiNative Ring profile 1\n";
inline std::vector<unsigned char> read_image(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("Cannot read a profile image");
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};
    if(bytes.empty()||bytes.size()>150*1024)throw std::runtime_error("A profile image exceeds 150 KB");
    return bytes;
}
inline std::wstring encode(std::wstring_view mappings,const ring_metadata::Settings& metadata) {
    std::wstring result(marker);std::map<std::wstring,bool> included;
    constexpr wchar_t hex[]=L"0123456789ABCDEF";
    for(const auto&[section,icons]:metadata.icons)for(const auto&icon:icons)if(!icon.image.empty()&&!included.contains(icon.image)) {
        const auto bytes=read_image(icon.image);included[icon.image]=true;
        result+=L"# ring-asset="+ring_metadata::hex(icon.image)+L" ";
        for(const auto byte:bytes){result+=hex[byte>>4];result+=hex[byte&15];}result+=L'\n';
    }
    return result+ring_metadata::write(mappings,metadata);
}
struct Package {std::wstring source;std::map<std::wstring,std::vector<unsigned char>> images;};
inline Package decode(std::wstring_view input) {
    if(!input.starts_with(marker))throw std::runtime_error("Unsupported Ring profile format");
    Package result;std::wistringstream stream(std::wstring{input.substr(marker.size())});std::wstring line;
    while(std::getline(stream,line)) {
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        if(!line.starts_with(L"# ring-asset=")){result.source+=line+L'\n';continue;}
        const auto space=line.find(L' ',13);if(space==line.npos)throw std::runtime_error("Malformed profile image");
        const auto name=ring_metadata::unhex(std::wstring_view(line).substr(13,space-13));const auto hex=std::wstring_view(line).substr(space+1);
        if(!name||name->empty()||hex.empty()||hex.size()%2||hex.size()>300*1024)throw std::runtime_error("Malformed profile image");
        std::vector<unsigned char> bytes;bytes.reserve(hex.size()/2);
        for(std::size_t i=0;i<hex.size();i+=2){unsigned byte{};for(auto ch:hex.substr(i,2)){const int value=ch>=L'0'&&ch<=L'9'?ch-L'0':ch>=L'A'&&ch<=L'F'?ch-L'A'+10:ch>=L'a'&&ch<=L'f'?ch-L'a'+10:-1;if(value<0)throw std::runtime_error("Malformed profile image");byte=(byte<<4)|value;}bytes.push_back(static_cast<unsigned char>(byte));}
        result.images[*name]=std::move(bytes);
    }
    const auto metadata=ring_metadata::read(result.source);
    for(const auto&[section,icons]:metadata.icons)for(const auto&icon:icons)if(!icon.image.empty()&&!result.images.contains(icon.image))throw std::runtime_error("Profile image is not embedded");
    return result;
}

inline std::vector<std::uint8_t> encode_lp5(std::wstring_view mappings,const ring_metadata::Settings& metadata,
                                            std::wstring_view profile_name=L"Actions Ring") {
    std::map<std::string,std::vector<std::uint8_t>> files;
    const auto ini_wide=encode(mappings,metadata);
    const auto ini_utf8=ui_settings::wide_to_utf8(ini_wide);
    files["ring_profile.ini"]=std::vector<std::uint8_t>(ini_utf8.begin(),ini_utf8.end());
    std::ostringstream manifest;
    manifest<<"{\"formatVersion\":5,\"origin\":\"LogiNative\",\"name\":\"";
    for(char c:ui_settings::wide_to_utf8(profile_name))manifest<<(c=='"'||c=='\\'?'_':c);
    manifest<<"\",\"slots\":[";
    std::wistringstream lines(std::wstring{mappings});
    std::wstring line;
    int slot_idx=0;bool first=true;
    while(std::getline(lines,line)&&slot_idx<8) {
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        if(line.empty()||line.starts_with(L'#')||line.starts_with(L'['))continue;
        const auto eq=line.find(L'=');if(eq==line.npos)continue;
        const auto label=line.substr(0,eq),action=line.substr(eq+1);
        if(!first)manifest<<',';
        first=false;
        manifest<<"{\"slot\":"<<(slot_idx+1)<<",\"displayName\":\""<<ui_settings::wide_to_utf8(label)
                <<"\",\"action\":\""<<ui_settings::wide_to_utf8(action)<<"\"}";
        ++slot_idx;
    }
    manifest<<"]}";
    const auto m_str=manifest.str();
    files["manifest.json"]=std::vector<std::uint8_t>(m_str.begin(),m_str.end());
    return zip_codec::pack_stored(files);
}

inline Package decode_bytes(std::span<const std::uint8_t> bytes) {
    if(zip_codec::is_zip(bytes)) {
        const auto entries=zip_codec::unpack(bytes);
        if(const auto it=entries.find("ring_profile.ini");it!=entries.end()) {
            const std::string_view sv(reinterpret_cast<const char*>(it->second.data()),it->second.size());
            return decode(ui_settings::utf8_to_wide(sv));
        }
        // Fallback: parse generic .lp4/.lp5 JSON manifest entries cleanly.
        std::string json_text;
        for(const auto&[name,data]:entries) {
            if(name.ends_with(".json")) {
                json_text.assign(reinterpret_cast<const char*>(data.data()),data.size());
                break;
            }
        }
        if(json_text.empty())throw std::runtime_error("Unsupported .lp5/.lp4 package");
        std::wstring source=L"[ring:ImportedLP5]\n";
        auto extract_next=[&](std::size_t& pos,std::string_view key)->std::string {
            const auto needle="\""+std::string(key)+"\"";
            const auto k=json_text.find(needle,pos);
            if(k==std::string::npos)return {};
            const auto colon=json_text.find(':',k+needle.size());
            if(colon==std::string::npos)return {};
            const auto q1=json_text.find('"',colon+1);
            if(q1==std::string::npos)return {};
            const auto q2=json_text.find('"',q1+1);
            if(q2==std::string::npos)return {};
            pos=q2+1;
            return json_text.substr(q1+1,q2-q1-1);
        };
        std::size_t pos=0;int count=0;
        while(count<8) {
            auto label=extract_next(pos,"displayName");
            if(label.empty())break;
            auto action=extract_next(pos,"action");
            if(action.empty())action="None";
            source+=ui_settings::utf8_to_wide(label)+L"="+ui_settings::utf8_to_wide(action)+L"\n";
            ++count;
        }
        while(count++<8)source+=L"空槽位=None\n";
        Package pkg;pkg.source=std::move(source);
        return pkg;
    }
    std::string_view sv(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if(sv.starts_with("\xEF\xBB\xBF"))sv.remove_prefix(3);
    return decode(ui_settings::utf8_to_wide(sv));
}

inline void materialize(const Package& package,ring_metadata::Settings& settings,const std::filesystem::path& directory) {
    if(package.images.empty())return;
    std::filesystem::create_directories(directory);
    std::map<std::wstring,std::wstring> paths;
    for(const auto&[name,bytes]:package.images){
        // Content names survive source removal and allow a repeated import to reuse the same image.
        unsigned long long hash=1469598103934665603ull;for(const auto byte:bytes){hash^=byte;hash*=1099511628211ull;}
        std::wostringstream filename;filename<<L"import-"<<std::hex<<hash<<L".img";const auto path=directory/filename.str();
        if(!std::filesystem::exists(path)){std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!output)throw std::runtime_error("Cannot save imported image");}
        paths.emplace(name,path.wstring());
    }
    for(auto&[section,icons]:settings.icons)for(auto&icon:icons)if(!icon.image.empty())icon.image=paths.at(icon.image);
}
} // namespace ring_profile_package
