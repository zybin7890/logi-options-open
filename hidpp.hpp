#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <atomic>
#endif

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace logi {

using Byte = std::uint8_t;
#ifdef _WIN32
using StopHandle = HANDLE;
using Milliseconds = DWORD;
inline bool stop_requested(StopHandle stop) { return stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0; }
#else
using StopHandle = std::atomic_bool*;
using Milliseconds = unsigned long;
inline bool stop_requested(StopHandle stop) { return stop && stop->load(std::memory_order_relaxed); }
#endif

struct Interface {
    std::wstring path;
    unsigned product;
    unsigned page;
    unsigned usage;
    unsigned input_size;
    unsigned output_size;
    bool receiver;
};

std::vector<Interface> enumerate();

struct Packet {
    std::array<Byte, 20> bytes{};
    unsigned size{};

    std::span<const Byte> data() const { return {bytes.data() + 4, size >= 4 && size <= bytes.size() ? size - 4 : 0}; }
};

enum class Result { ok, timeout, io_error, device_error, stopped, hotkey, reload };

struct Reply {
    Result result;
    Packet packet{};
    unsigned error{};

    explicit operator bool() const { return result == Result::ok; }
};

class Channel {
public:
    explicit Channel(const Interface& interface, StopHandle stop = nullptr);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    bool valid() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE && event_;
#else
        return handle_ != nullptr;
#endif
    }
    Milliseconds open_error() const { return open_error_; }
    void keep_notifications() { keep_notifications_ = true; }
    void discard_notifications() { queued_=next_=0; }
    void finish_shutdown() { stop_ = nullptr; keep_notifications_ = false; }
    Reply request(Byte slot, Byte feature, Byte function,
                  std::initializer_list<Byte> parameters = {}, Milliseconds timeout = 1200);
    Result send(Byte slot, Byte feature, Byte function,
                std::initializer_list<Byte> parameters = {}, Milliseconds timeout = 1200);
    Reply read(Milliseconds timeout, bool hotkeys = false, StopHandle wake = nullptr);

private:
    Reply read_input(Milliseconds timeout, bool hotkeys = false, StopHandle wake = nullptr);
    Result transfer(bool write, std::span<Byte> buffer, Milliseconds timeout, Milliseconds& transferred,
                    bool hotkeys = false, StopHandle wake = nullptr);

#ifdef _WIN32
    HANDLE handle_;
    HANDLE event_;
#else
    void* handle_{};
#endif
    StopHandle stop_;
    Milliseconds open_error_;
    unsigned input_size_;
    unsigned output_size_;
    Byte software_id_{3};
    std::array<Byte, 20> input_{};
    std::array<Byte, 20> output_{};
    std::array<Packet, 16> notifications_{};
    unsigned next_{}, queued_{};
    bool keep_notifications_{};
};

struct Device {
    Byte slot;
    Byte major;
    Byte minor;
    std::string name;
};

std::optional<Device> probe(Channel& channel, Byte slot);
Byte feature(Channel& channel, Byte slot, unsigned id);
unsigned word(std::span<const Byte> data);
Byte high(unsigned value);
Byte low(unsigned value);

} // namespace logi
