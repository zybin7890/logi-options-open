#include "pairing.hpp"

#include <windows.h>
#include <bluetoothapis.h>
#include <shellapi.h>

#include <cstdio>

namespace pairing {
namespace {

std::optional<std::wstring> read_slot_fw_string(logi::Channel& channel, logi::Byte slot) {
    const auto idx = logi::feature(channel, slot, 0x0003);
    if (!idx) return std::nullopt;
    const auto info = channel.request(slot, idx, 0);
    if (!info || info.packet.data().empty()) return std::nullopt;
    const unsigned count = std::min<unsigned>(info.packet.data()[0], 8);
    for (unsigned entity = 0; entity < count; ++entity) {
        const auto fw = channel.request(slot, idx, 1, {static_cast<logi::Byte>(entity)});
        if (!fw) continue;
        const auto d = fw.packet.data();
        if (!d.empty() && d[0] == 0) return parse_fw_version(d);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::wstring> parse_fw_version(std::span<const logi::Byte> data) {
    return logi::parse_fw_version_0003(data);
}

std::optional<std::wstring> parse_serial_unit_id(std::span<const logi::Byte> data) {
    return logi::parse_serial_unit_id_0003(data);
}

FirmwareInfo read_firmware_info(logi::Channel& channel, logi::Byte device_slot, bool via_receiver) {
    FirmwareInfo result;
    result.connection_kind = via_receiver ? L"Logi Bolt" : L"蓝牙 / USB";
    if (const auto idx = logi::feature(channel, device_slot, 0x0003)) {
        const auto info = channel.request(device_slot, idx, 0);
        if (info && info.packet.data().size() >= 5) {
            if (const auto ser = parse_serial_unit_id(info.packet.data())) result.serial = *ser;
            const unsigned entities = std::min<unsigned>(info.packet.data()[0], 8);
            for (unsigned e = 0; e < entities; ++e) {
                const auto fw = channel.request(device_slot, idx, 1, {static_cast<logi::Byte>(e)});
                if (fw && !fw.packet.data().empty() && fw.packet.data()[0] == 0) {
                    if (const auto parsed = parse_fw_version(fw.packet.data())) {
                        result.firmware_version = *parsed;
                        break;
                    }
                }
            }
        }
    }
    if (via_receiver) {
        if (const auto recv_fw = read_slot_fw_string(channel, 0xFF))
            result.receiver_firmware = *recv_fw;
    }
    return result;
}

std::vector<ReceiverSlotInfo> scan_bolt_receiver_slots(logi::Channel& channel) {
    std::vector<ReceiverSlotInfo> slots;
    for (unsigned slot = 1; slot <= 6; ++slot) {
        // Probe HID++ 2.0 IRoot/DeviceName cleanly per receiver slot (read-only ping).
        const auto dev = logi::probe(channel, static_cast<logi::Byte>(slot));
        if (!dev) continue;
        std::wstring name(dev->name.begin(), dev->name.end());
        slots.push_back({slot, 0, std::move(name), true});
    }
    return slots;
}

bool set_receiver_pairing_lock(logi::Channel& channel, bool open, unsigned timeout_seconds) {
    // HID++ 1.0 SET_REGISTER 0xB2 (Receiver pairing open_lock): cmd=0x80, sub=0xB2, [open(1/2), timeout]
    const logi::Byte flag = open ? 0x01 : 0x02;
    const logi::Byte secs = static_cast<logi::Byte>(open ? std::min(timeout_seconds, 60u) : 0u);
    const auto reply = channel.request(0xFF, 0x80, 0xB2, {flag, secs});
    return static_cast<bool>(reply);
}

std::vector<BluetoothDeviceInfo> enumerate_bluetooth_devices() {
    std::vector<BluetoothDeviceInfo> devices;
    HMODULE bt = LoadLibraryW(L"BluetoothAPIs.dll");
    if (!bt) return devices;

    using FindFirstFn = HBLUETOOTH_DEVICE_FIND (WINAPI*)(const BLUETOOTH_DEVICE_SEARCH_PARAMS*, BLUETOOTH_DEVICE_INFO*);
    using FindNextFn = BOOL (WINAPI*)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO*);
    using FindCloseFn = BOOL (WINAPI*)(HBLUETOOTH_DEVICE_FIND);

    auto find_first = reinterpret_cast<FindFirstFn>(reinterpret_cast<void*>(GetProcAddress(bt, "BluetoothFindFirstDevice")));
    auto find_next = reinterpret_cast<FindNextFn>(reinterpret_cast<void*>(GetProcAddress(bt, "BluetoothFindNextDevice")));
    auto find_close = reinterpret_cast<FindCloseFn>(reinterpret_cast<void*>(GetProcAddress(bt, "BluetoothFindDeviceClose")));
    if (!find_first || !find_next || !find_close) {
        FreeLibrary(bt);
        return devices;
    }

    BLUETOOTH_DEVICE_SEARCH_PARAMS params{};
    params.dwSize = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered = TRUE;
    params.fReturnUnknown = FALSE;
    params.fReturnConnected = TRUE;
    params.fIssueInquiry = FALSE;
    params.cTimeoutMultiplier = 1;

    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    HBLUETOOTH_DEVICE_FIND handle = find_first(&params, &info);
    if (handle) {
        do {
            wchar_t addr[32]{};
            const auto& b = info.Address.rgBytes;
            std::swprintf(addr, std::size(addr), L"%02X:%02X:%02X:%02X:%02X:%02X",
                          b[5], b[4], b[3], b[2], b[1], b[0]);
            devices.push_back({info.szName, addr, info.fConnected != FALSE, info.fRemembered != FALSE});
            info.dwSize = sizeof(info);
        } while (find_next(handle, &info));
        find_close(handle);
    }
    FreeLibrary(bt);
    return devices;
}

bool open_windows_bluetooth_settings() {
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

} // namespace pairing
