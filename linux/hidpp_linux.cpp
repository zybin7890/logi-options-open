#include "hidpp.hpp"
#include <hidapi/hidapi.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>

namespace logi {
namespace { using Clock=std::chrono::steady_clock; }
std::vector<Interface> enumerate() {
    std::vector<Interface> result;
    auto* devices=hid_enumerate(0x046d,0);
    for (auto* item=devices;item;item=item->next) {
        if (!item->path || (item->usage_page && item->usage_page<0xff00)) continue;
        const auto pid=item->product_id;
        const bool receiver=pid==0xc548 || pid==0xc52b || pid==0xc532 || pid==0xc534 || pid==0xc539;
        const auto path=std::filesystem::path(item->path).wstring();
        if (std::ranges::any_of(result,[&](const auto& found){return found.path==path;})) continue;
        result.push_back({path,pid,item->usage_page,item->usage,20,20,receiver});
    }
    hid_free_enumeration(devices);
    return result;
}
Channel::Channel(const Interface& interface,StopHandle stop)
    :stop_(stop),open_error_(0),input_size_(20),output_size_(20) {
    handle_=hid_open_path(std::filesystem::path(interface.path).c_str());
    if (!handle_) open_error_=errno ? errno : EACCES;
}
Channel::~Channel() { if(handle_) hid_close(static_cast<hid_device*>(handle_)); }
Result Channel::transfer(bool writing,std::span<Byte> buffer,Milliseconds timeout,Milliseconds& transferred,
                         bool,StopHandle wake) {
    transferred=0;
    if (!valid()) return Result::io_error;
    if (stop_requested(stop_)) return Result::stopped;
    if (writing) {
        const int count=hid_write(static_cast<hid_device*>(handle_),buffer.data(),buffer.size());
        if(count<0) return Result::io_error;
        transferred=static_cast<Milliseconds>(count);return Result::ok;
    }
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
    do {
        if(stop_requested(stop_)) return Result::stopped;
        if(stop_requested(wake)) return Result::reload;
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        const int count=hid_read_timeout(static_cast<hid_device*>(handle_),buffer.data(),buffer.size(),
                                        static_cast<int>(std::clamp<long long>(remaining,0,100)));
        if(count<0) return Result::io_error;
        if(count>0) {transferred=static_cast<Milliseconds>(count);return Result::ok;}
    } while(Clock::now()<deadline);
    return Result::timeout;
}
Reply Channel::read_input(Milliseconds timeout,bool hotkeys,StopHandle wake) {
    Milliseconds count{};
    const auto result=transfer(false,input_,timeout,count,hotkeys,wake);
    if(result!=Result::ok) return {result};
    const unsigned size=input_[0]==0x11?20:input_[0]==0x10?7:0;
    if(!size || count<size) return {Result::ok};
    return {Result::ok,{input_,size}};
}
Reply Channel::read(Milliseconds timeout,bool hotkeys,StopHandle wake) {
    if(stop_requested(stop_)) return {Result::stopped};
    if(!queued_) return read_input(timeout,hotkeys,wake);
    const auto index=(next_+notifications_.size()-queued_)%notifications_.size();
    --queued_;return {Result::ok,notifications_[index]};
}
Result Channel::send(Byte slot,Byte feature,Byte function,std::initializer_list<Byte> parameters,Milliseconds timeout) {
    if(parameters.size()>16 || function>15) return Result::io_error;
    software_id_=software_id_==15?3:software_id_+1;
    output_={0x11,slot,feature,static_cast<Byte>((function<<4)|software_id_)};
    std::ranges::copy(parameters,output_.begin()+4);
    Milliseconds count{};const auto result=transfer(true,output_,timeout,count);
    return result==Result::ok && count!=output_.size()?Result::io_error:result;
}
Reply Channel::request(Byte slot,Byte feature,Byte function,std::initializer_list<Byte> parameters,Milliseconds timeout) {
    const auto sent=send(slot,feature,function,parameters,timeout);
    if(sent!=Result::ok) return {sent};
    const auto command=output_[3];const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
    while(Clock::now()<deadline) {
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        auto reply=read_input(static_cast<Milliseconds>(std::max<long long>(0,remaining)));
        if(!reply) return reply;
        const auto& bytes=reply.packet.bytes;
        if(!reply.packet.size || bytes[1]!=slot) continue;
        if((bytes[2]==0xff || bytes[2]==0x8f) && bytes[3]==feature && bytes[4]==command)
            return {Result::device_error,reply.packet,bytes[5]};
        if(reply.packet.size==20 && bytes[2]==feature && bytes[3]==command) return reply;
        if(keep_notifications_ && !(bytes[3]&0xf)) {
            // Lost transitions cannot be replayed safely: surface overflow as I/O failure.
            if(queued_==notifications_.size()) return {Result::io_error};
            notifications_[next_]=reply.packet;next_=(next_+1)%notifications_.size();++queued_;
        }
    }
    return {Result::timeout};
}
Byte feature(Channel& channel,Byte slot,unsigned id) {
    const auto reply=channel.request(slot,0,0,{high(id),low(id)});
    return reply && !(reply.packet.data()[1]&0x80)?reply.packet.data()[0]:0;
}
std::optional<Device> probe(Channel& channel,Byte slot) {
    constexpr Byte challenge=0x6d;
    const auto ping=channel.request(slot,0,1,{0,0,challenge},1250);
    if(!ping || ping.packet.data()[2]!=challenge || ping.packet.data()[0]<2) return {};
    Device device{slot,ping.packet.data()[0],ping.packet.data()[1],{}};
    const auto name=feature(channel,slot,0x0005);if(!name) return device;
    const auto length=channel.request(slot,name,0);if(!length) return device;
    while(device.name.size()<length.packet.data()[0]) {
        const auto chunk=channel.request(slot,name,1,{static_cast<Byte>(device.name.size())});
        if(!chunk) break;
        const auto bytes=chunk.packet.data();const auto count=std::min(bytes.size(),length.packet.data()[0]-device.name.size());
        device.name.append(reinterpret_cast<const char*>(bytes.data()),count);
    }
    return device;
}
unsigned word(std::span<const Byte> data) {return data.size()<2?0:(unsigned{data[0]}<<8)|data[1];}
Byte high(unsigned value) {return static_cast<Byte>(value>>8);}
Byte low(unsigned value) {return static_cast<Byte>(value);}
} // namespace logi
