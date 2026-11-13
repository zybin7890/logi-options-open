#include "hidpp.hpp"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <memory>

namespace logi {

std::vector<Interface> enumerate() {
    GUID guid{};
    HidD_GetHidGuid(&guid);
    const auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return {};

    std::vector<Interface> interfaces;
    SP_DEVICE_INTERFACE_DATA entry{};
    entry.cbSize = sizeof(entry);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry);
         ++index) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
        if (!size) continue;
        auto storage = std::make_unique<std::byte[]>(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.get());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr))
            continue;

        const auto handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA preparsed{};
        const bool logitech = HidD_GetAttributes(handle, &attributes) && attributes.VendorID == 0x046D;
        if (logitech && HidD_GetPreparsedData(handle, &preparsed)) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }
        CloseHandle(handle);
        if (caps.UsagePage < 0xFF00 || caps.InputReportByteLength != 20 ||
            caps.OutputReportByteLength != 20) continue;

        const auto pid = attributes.ProductID;
        const bool receiver = pid == 0xC548 || pid == 0xC52B || pid == 0xC532 ||
                              pid == 0xC534 || pid == 0xC539;
        interfaces.push_back({detail->DevicePath, pid, caps.UsagePage, caps.Usage,
                              caps.InputReportByteLength, caps.OutputReportByteLength, receiver});
    }
    SetupDiDestroyDeviceInfoList(devices);
    return interfaces;
}

Channel::Channel(const Interface& interface, HANDLE stop)
    : handle_(CreateFileW(interface.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr)),
      event_(nullptr), stop_(stop),
      open_error_(handle_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS),
      input_size_(interface.input_size), output_size_(interface.output_size) {
    if (handle_ == INVALID_HANDLE_VALUE) return;
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_) open_error_ = GetLastError();
}

Channel::~Channel() {
    drain_control_write();
    if (control_event_) CloseHandle(control_event_);
    if (event_) CloseHandle(event_);
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

Result Channel::transfer(bool write, std::span<Byte> buffer, DWORD timeout, DWORD& transferred,
                         bool hotkeys, HANDLE wake,HANDLE command,HANDLE control_write) {
    if (!valid()) return Result::io_error;
    if (stop_ && WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) return Result::stopped;
    ResetEvent(event_);
    OVERLAPPED operation{};
    operation.hEvent = event_;
    const auto size = static_cast<DWORD>(buffer.size());
    const auto ready = write
        ? WriteFile(handle_, buffer.data(), size, &transferred, &operation)
        : ReadFile(handle_, buffer.data(), size, &transferred, &operation);
    if (ready) return Result::ok;
    if (GetLastError() != ERROR_IO_PENDING) return Result::io_error;

    HANDLE events[]{event_,nullptr,nullptr,nullptr,nullptr};
    DWORD count=1;
    if (stop_) events[count++]=stop_;
    const DWORD wake_index=count;
    if (wake) events[count++]=wake;
    const DWORD command_index=count;
    if(command)events[count++]=command;
    const DWORD control_index=count;
    if(control_write)events[count++]=control_write;
    const auto wait = hotkeys
        ? MsgWaitForMultipleObjectsEx(count, events, timeout, QS_HOTKEY, MWMO_INPUTAVAILABLE)
        : WaitForMultipleObjects(count, events, FALSE, timeout);
    if (wait == WAIT_OBJECT_0) {
        return GetOverlappedResult(handle_, &operation, &transferred, FALSE)
            ? Result::ok : Result::io_error;
    }
    CancelIoEx(handle_, &operation);
    const auto completed = GetOverlappedResult(handle_, &operation, &transferred, TRUE);
    if (stop_ && wait == WAIT_OBJECT_0 + 1) return Result::stopped;
    if (wake && wait==WAIT_OBJECT_0+wake_index) return completed ? Result::ok : Result::reload;
    if(command && wait==WAIT_OBJECT_0+command_index)return completed?Result::ok:Result::command;
    if(control_write && wait==WAIT_OBJECT_0+control_index)return completed?Result::ok:Result::control;
    if (hotkeys && wait == WAIT_OBJECT_0 + count) return completed ? Result::ok : Result::hotkey;
    if (wait == WAIT_TIMEOUT && completed) return Result::ok;
    return wait == WAIT_TIMEOUT ? Result::timeout : Result::io_error;
}

Reply Channel::read_input(DWORD timeout, bool hotkeys, HANDLE wake,HANDLE command,HANDLE control_write) {
    DWORD count = 0;
    const auto status = transfer(false, std::span{input_}.first(input_size_), timeout, count, hotkeys,wake,command,control_write);
    if (status != Result::ok) return {status};
    const unsigned size = input_[0] == 0x11 ? 20 : input_[0] == 0x10 ? 7 : 0;
    if (!size || count < size) return {Result::ok};
    return {Result::ok, {input_, size}};
}

Reply Channel::read(DWORD timeout, bool hotkeys, HANDLE wake) {
    if (stop_ && WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) return {Result::stopped};
    if (pending_loss_) return {Result::notification_loss};
    const bool cancelling_write=control_canceling_ && control_writing_;
    if (!cancelling_write && wake && WaitForSingleObject(wake,0)==WAIT_OBJECT_0) return {Result::reload};
    if(!cancelling_write && !queued_ && command_ && WaitForSingleObject(command_,0)==WAIT_OBJECT_0)return {Result::command};
    if (!queued_) {
        advance_control();
        if(control_active_ && !control_writing_ && (control_result_ || control_canceling_))return {Result::control};
        if(control_active_ && !control_canceling_) {
            const auto now=GetTickCount64();
            timeout=std::min<DWORD>(timeout,static_cast<DWORD>(control_deadline_>now?control_deadline_-now:0));
        }
        auto reply=read_input(timeout,hotkeys,cancelling_write?nullptr:wake,cancelling_write?nullptr:command_,control_writing_?control_event_:nullptr);
        if(route_control(reply))return {Result::control};
        advance_control();
        // A packet completed concurrently with timeout/cancellation still belongs to the caller.
        if(reply.result==Result::timeout && control_active_ && control_result_)return {Result::control};
        return reply;
    }
    const auto index = (next_ + notifications_.size() - queued_) % notifications_.size();
    --queued_;
    return {Result::ok, notifications_[index]};
}

Result Channel::send(Byte slot, Byte feature, Byte function,
                     std::initializer_list<Byte> parameters, DWORD timeout) {
    if(control_active_)return Result::busy;
    const auto id=next_software_id();
    if(!id)return Result::busy;
    const auto command = static_cast<Byte>((function << 4) | id);
    output_ = {0x11, slot, feature, command};
    std::ranges::copy(parameters, output_.begin() + 4);
    DWORD count = 0;
    const auto status = transfer(true, std::span{output_}.first(output_size_), timeout, count);
    if (status == Result::ok && count != output_size_) return Result::io_error;
    return status;
}

Byte Channel::next_software_id() {
    const auto now=GetTickCount64();
    for(unsigned i=0;i<13;++i) {
        software_id_=software_id_==15?3:software_id_+1;
        if(retired_ids_[software_id_]<=now)return software_id_;
    }
    return 0;
}

void Channel::retire_control() {
    const auto id=control_output_[3]&15;
    // The wire protocol has no generation field. Quarantine cancelled/timed-out IDs
    // through the old deadline plus one full response window before reusing them.
    retired_ids_[id]=std::max(retired_ids_[id],control_deadline_+std::max<DWORD>(1200,control_timeout_));
}

Result Channel::begin_control(Byte slot,Byte feature,Byte function,
                              std::initializer_list<Byte> parameters,DWORD timeout) {
    if(control_active_)return Result::busy;
    if(pending_loss_)return Result::notification_loss;
    if(!valid() || parameters.size()>16)return Result::io_error;
    if(stop_ && WaitForSingleObject(stop_,0)==WAIT_OBJECT_0)return Result::stopped;
    if(!control_event_)control_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!control_event_)return Result::io_error;
    const auto id=next_software_id();if(!id)return Result::busy;
    control_output_={0x11,slot,feature,static_cast<Byte>((function<<4)|id)};
    std::ranges::copy(parameters,control_output_.begin()+4);
    ResetEvent(control_event_);control_write_={};control_write_.hEvent=control_event_;
    control_active_=true;control_writing_=control_canceling_=control_timed_out_=false;control_result_.reset();
    control_timeout_=timeout;control_deadline_=GetTickCount64()+timeout;
    DWORD transferred{};
    if(WriteFile(handle_,control_output_.data(),output_size_,&transferred,&control_write_)) {
        if(transferred!=output_size_)control_result_=Reply{Result::io_error};
    } else if(GetLastError()==ERROR_IO_PENDING)control_writing_=true;
    else control_result_=Reply{Result::io_error};
    return Result::ok;
}

void Channel::advance_control() {
    if(!control_active_)return;
    // Even an early reply cannot release a still-pending write's OVERLAPPED storage.
    // Bound that write by the same deadline, then wait for its cancellation completion.
    if(!control_canceling_ && (control_writing_ || !control_result_) && GetTickCount64()>=control_deadline_) {
        control_timed_out_=true;control_canceling_=true;retire_control();
        if(control_writing_)CancelIoEx(handle_,&control_write_);
    }
    if(control_writing_) {
        DWORD transferred{};
        const auto complete=GetOverlappedResult(handle_,&control_write_,&transferred,FALSE);
        const auto error=complete?ERROR_SUCCESS:GetLastError();
        if(!complete && error==ERROR_IO_INCOMPLETE)return;
        control_writing_=false;
        if(!control_canceling_ && (!complete || transferred!=output_size_))control_result_=Reply{Result::io_error};
    }
    if(control_timed_out_ && !control_writing_)control_result_=Reply{Result::timeout};
}

bool Channel::route_control(const Reply& reply) {
    if(!control_active_ || control_canceling_ || control_result_ || !reply || reply.packet.size<7)return false;
    const auto& b=reply.packet.bytes;
    if(b[1]!=control_output_[1])return false;
    if((b[2]==0xFF || b[2]==0x8F) && b[3]==control_output_[2] && b[4]==control_output_[3]) {
        control_result_=Reply{Result::device_error,reply.packet,b[5]};return true;
    }
    if(b[2]==control_output_[2] && b[3]==control_output_[3]){control_result_=reply;return true;}
    return false;
}

std::optional<Reply> Channel::poll_control() {
    advance_control();
    if(!control_active_ || control_writing_ || !control_result_)return {};
    const auto result=control_result_;control_result_.reset();control_active_=false;
    control_canceling_=control_timed_out_=false;return result;
}

bool Channel::cancel_control() {
    if(!control_active_)return true;
    if(!control_canceling_) {
        control_canceling_=true;control_result_.reset();retire_control();
        if(control_writing_)CancelIoEx(handle_,&control_write_);
    }
    advance_control();
    if(control_writing_)return false;
    control_active_=control_canceling_=control_timed_out_=false;control_result_.reset();return true;
}

void Channel::drain_control_write() {
    if(!cancel_control()) {
        DWORD transferred{};
        GetOverlappedResult(handle_,&control_write_,&transferred,TRUE);
        control_writing_=control_active_=control_canceling_=control_timed_out_=false;control_result_.reset();
    }
}

void Channel::finish_shutdown() {
    drain_control_write();stop_=nullptr;keep_notifications_=false;
    discard_notifications();pending_loss_=0;
}

Reply Channel::request(Byte slot, Byte feature, Byte function,
                       std::initializer_list<Byte> parameters, DWORD timeout) {
    if (pending_loss_) return {Result::notification_loss};
    const auto status = send(slot, feature, function, parameters, timeout);
    if (status != Result::ok) return {status};
    const auto command = output_[3];

    const auto deadline = GetTickCount64() + timeout;
    for (auto now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        auto response = read_input(static_cast<DWORD>(deadline - now));
        if (!response) return response;
        const auto& packet = response.packet;
        const auto& bytes = packet.bytes;
        if (!packet.size || bytes[1] != slot) continue;
        if ((bytes[2] == 0xFF || bytes[2] == 0x8F) &&
            bytes[3] == feature && bytes[4] == command) {
            return {Result::device_error, packet, bytes[5]};
        }
        if (bytes[2] == feature && bytes[3] == command) return response;
        if (keep_notifications_ && !(bytes[3] & 0x0F)) {
            if (queued_ == notifications_.size()) {
                // Do not wait for the control reply while input state is lost.
                // The event consumer cancels its current actions before resuming.
                ++lost_notifications_; ++pending_loss_;
                return {Result::notification_loss};
            }
            notifications_[next_] = packet;
            next_ = (next_ + 1) % notifications_.size();
            ++queued_;
        }
    }
    return {Result::timeout};
}

Byte feature(Channel& channel, Byte slot, unsigned id) {
    const auto reply = channel.request(slot, 0, 0, {high(id), low(id)});
    return reply && !(reply.packet.data()[1] & 0x80) ? reply.packet.data()[0] : 0;
}

std::optional<Device> probe(Channel& channel, Byte slot) {
    constexpr Byte challenge = 0x6D;
    const auto ping = channel.request(slot, 0, 1, {0, 0, challenge}, 1250);
    if (!ping || ping.packet.data()[2] != challenge || ping.packet.data()[0] < 2 ||
        ping.packet.data()[0] == 0x8F) return std::nullopt;

    Device device{slot, ping.packet.data()[0], ping.packet.data()[1], {}};
    const auto name = feature(channel, slot, 0x0005);
    if (!name) return device;
    const auto length = channel.request(slot, name, 0);
    if (!length) return device;
    while (device.name.size() < length.packet.data()[0]) {
        const auto chunk = channel.request(slot, name, 1, {static_cast<Byte>(device.name.size())});
        if (!chunk) break;
        const auto bytes = chunk.packet.data();
        const auto count = std::min(bytes.size(), length.packet.data()[0] - device.name.size());
        device.name.append(reinterpret_cast<const char*>(bytes.data()), count);
    }
    return device;
}

unsigned word(std::span<const Byte> data) { return (unsigned{data[0]} << 8) | data[1]; }
Byte high(unsigned value) { return static_cast<Byte>(value >> 8); }
Byte low(unsigned value) { return static_cast<Byte>(value); }

} // namespace logi
