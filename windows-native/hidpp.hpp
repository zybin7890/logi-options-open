#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace logi {

using Byte = std::uint8_t;

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

    std::span<const Byte> data() const { return {bytes.data() + 4, size - 4}; }
};

enum class Result { ok, timeout, io_error, device_error, stopped, hotkey, reload, notification_loss, command, control, busy };

struct Reply {
    Result result;
    Packet packet{};
    unsigned error{};

    explicit operator bool() const { return result == Result::ok; }
};

class Channel {
public:
    explicit Channel(const Interface& interface, HANDLE stop = nullptr);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && event_; }
    DWORD open_error() const { return open_error_; }
    void keep_notifications() { keep_notifications_ = true; }
    void wake_on_command(HANDLE event) { command_=event; }
    bool pending_notifications() const { return queued_!=0; }
    unsigned pending_notification_count() const { return queued_; }
    void discard_notifications() { discarded_notifications_ += queued_; queued_=next_=0; }
    // Loss is independent of the queue: clearing stale packets cannot hide it.
    bool pending_notification_loss() const { return pending_loss_ != 0; }
    std::uint64_t notification_losses() const { return lost_notifications_; }
    std::uint64_t notification_discards() const { return discarded_notifications_; }
    std::uint64_t take_notification_loss() {
        const auto lost = pending_loss_;
        if (lost) { discard_notifications(); pending_loss_ = 0; }
        return lost;
    }
    void finish_shutdown();
    Result begin_control(Byte slot, Byte feature, Byte function,
                         std::initializer_list<Byte> parameters = {}, DWORD timeout = 1200);
    std::optional<Reply> poll_control();
    bool control_pending() const { return control_active_; }
    // False means a cancelled write still owns its OVERLAPPED/buffer; keep pumping read.
    bool cancel_control();
    Reply request(Byte slot, Byte feature, Byte function,
                  std::initializer_list<Byte> parameters = {}, DWORD timeout = 1200);
    Result send(Byte slot, Byte feature, Byte function,
                std::initializer_list<Byte> parameters = {}, DWORD timeout = 1200);
    Reply read(DWORD timeout, bool hotkeys = false, HANDLE wake = nullptr);

private:
    Reply read_input(DWORD timeout, bool hotkeys = false, HANDLE wake = nullptr,HANDLE command=nullptr,HANDLE control_write=nullptr);
    Result transfer(bool write, std::span<Byte> buffer, DWORD timeout, DWORD& transferred,
                    bool hotkeys = false, HANDLE wake = nullptr,HANDLE command=nullptr,HANDLE control_write=nullptr);
    Byte next_software_id();
    void advance_control();
    void retire_control();
    bool route_control(const Reply& reply);
    void drain_control_write();

    HANDLE handle_;
    HANDLE event_;
    HANDLE stop_;
    DWORD open_error_;
    unsigned input_size_;
    unsigned output_size_;
    Byte software_id_{3};
    std::array<Byte, 20> input_{};
    std::array<Byte, 20> output_{};
    std::array<Packet, 16> notifications_{};
    unsigned next_{}, queued_{};
    std::uint64_t lost_notifications_{}, pending_loss_{}, discarded_notifications_{};
    bool keep_notifications_{};
    HANDLE command_{};
    HANDLE control_event_{};
    OVERLAPPED control_write_{};
    std::array<Byte,20> control_output_{};
    std::array<ULONGLONG,16> retired_ids_{};
    ULONGLONG control_deadline_{};
    DWORD control_timeout_{};
    bool control_active_{},control_writing_{},control_canceling_{},control_timed_out_{};
    std::optional<Reply> control_result_;
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
