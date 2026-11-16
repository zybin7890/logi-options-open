#pragma once

#include "hidpp.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pairing {

struct FirmwareInfo {
    std::wstring serial;
    std::wstring firmware_version;
    std::wstring receiver_firmware;
    std::wstring connection_kind;
    unsigned model_id{};
};

struct BluetoothDeviceInfo {
    std::wstring name;
    std::wstring address;
    bool connected{};
    bool remembered{};
};

struct ReceiverSlotInfo {
    unsigned slot{};
    unsigned pid{};
    std::wstring name;
    bool active{};
};

enum class WizardStage { idle, choose_transport, scanning_bolt, scanning_bt, paired, failed, cancelled };

// Pure HID++ 2.0 0x0003 parser helpers for unit testing & live hardware readback.
std::optional<std::wstring> parse_fw_version(std::span<const logi::Byte> data);
std::optional<std::wstring> parse_serial_unit_id(std::span<const logi::Byte> data);

// Reads device & receiver firmware/serial cleanly using HID++ feature 0x0003.
FirmwareInfo read_firmware_info(logi::Channel& channel, logi::Byte device_slot, bool via_receiver);

// Queries Logi Bolt / Unifying receiver paired devices (slot 1..6 via HID++ 1.0 registers).
std::vector<ReceiverSlotInfo> scan_bolt_receiver_slots(logi::Channel& channel);

// Opens/closes Bolt/Unifying receiver pairing lock (HID++ 1.0 SET_REG 0xB2, duration in seconds).
bool set_receiver_pairing_lock(logi::Channel& channel, bool open, unsigned timeout_seconds = 30);

// Enumerates paired / discovered Windows Bluetooth devices without hard-linking bthprops.lib.
std::vector<BluetoothDeviceInfo> enumerate_bluetooth_devices();
bool open_windows_bluetooth_settings();

} // namespace pairing
