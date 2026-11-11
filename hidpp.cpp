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
    if (event_) CloseHandle(event_);
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

Result Channel::transfer(bool write, std::span<Byte> buffer, DWORD timeout, DWORD& transferred,
                         bool hotkeys, HANDLE wake) {
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

    HANDLE events[]{event_,nullptr,nullptr};
    DWORD count=1;
    if (stop_) events[count++]=stop_;
    const DWORD wake_index=count;
    if (wake) events[count++]=wake;
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
    if (hotkeys && wait == WAIT_OBJECT_0 + count) return completed ? Result::ok : Result::hotkey;
    if (wait == WAIT_TIMEOUT && completed) return Result::ok;
    return wait == WAIT_TIMEOUT ? Result::timeout : Result::io_error;
}

Reply Channel::read_input(DWORD timeout, bool hotkeys, HANDLE wake) {
    DWORD count = 0;
    const auto status = transfer(false, std::span{input_}.first(input_size_), timeout, count, hotkeys,wake);
    if (status != Result::ok) return {status};
    const unsigned size = input_[0] == 0x11 ? 20 : input_[0] == 0x10 ? 7 : 0;
    if (!size || count < size) return {Result::ok};
    return {Result::ok, {input_, size}};
}

Reply Channel::read(DWORD timeout, bool hotkeys, HANDLE wake) {
    if (stop_ && WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) return {Result::stopped};
    if (wake && WaitForSingleObject(wake,0)==WAIT_OBJECT_0) return {Result::reload};
    if (!queued_) return read_input(timeout, hotkeys,wake);
    const auto index = (next_ + notifications_.size() - queued_) % notifications_.size();
    --queued_;
    return {Result::ok, notifications_[index]};
}

Result Channel::send(Byte slot, Byte feature, Byte function,
                     std::initializer_list<Byte> parameters, DWORD timeout) {
    // Rotate the software ID to avoid accepting late replies after a timeout.
    software_id_ = software_id_ == 15 ? 3 : software_id_ + 1;
    const auto command = static_cast<Byte>((function << 4) | software_id_);
    output_ = {0x11, slot, feature, command};
    std::ranges::copy(parameters, output_.begin() + 4);
    DWORD count = 0;
    const auto status = transfer(true, std::span{output_}.first(output_size_), timeout, count);
    if (status == Result::ok && count != output_size_) return Result::io_error;
    return status;
}

Reply Channel::request(Byte slot, Byte feature, Byte function,
                       std::initializer_list<Byte> parameters, DWORD timeout) {
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
            notifications_[next_] = packet;
            next_ = (next_ + 1) % notifications_.size();
            if (queued_ < notifications_.size()) ++queued_;
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
