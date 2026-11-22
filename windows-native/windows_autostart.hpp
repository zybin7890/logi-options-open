#pragma once

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "recovery_task.hpp"

namespace ui_settings {

inline constexpr auto run_key = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
inline constexpr auto approval_key = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)";

inline bool login_start_approved() {
    std::array<BYTE, 32> approval{};
    DWORD size = static_cast<DWORD>(approval.size());
    const auto rc = RegGetValueW(HKEY_CURRENT_USER, approval_key, L"LogiNative",
                                 RRF_RT_REG_BINARY, nullptr, approval.data(), &size);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return true;
    return rc == ERROR_SUCCESS && size >= sizeof(DWORD) && (approval[0] & 1u) == 0;
}

inline bool install_autostarts(const std::filesystem::path& exe) {
    DWORD size{};
    if (RegGetValueW(HKEY_CURRENT_USER, run_key, L"LogiNative", RRF_RT_REG_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS || !size || size > 65536) return false;
    std::wstring command(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, run_key, L"LogiNative", RRF_RT_REG_SZ,
                     nullptr, command.data(), &size) != ERROR_SUCCESS) return false;
    int argc{};
    auto argv = CommandLineToArgvW(command.c_str(), &argc);
    if (!argv) return false;
    const bool matches = argc == 2 && std::wstring_view(argv[1]) == L"--tray" &&
        CompareStringOrdinal(argv[0], -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
    LocalFree(argv);
    return matches && login_start_approved();
}

inline bool current_install_autostarts() {
    std::array<wchar_t, 32768> path;
    const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return n && n < path.size() && install_autostarts(path.data());
}

inline bool is_windows_autostart_enabled() { return current_install_autostarts(); }

inline bool ensure_recovery_task_for_install(const std::filesystem::path& directory) {
    const auto exe = directory / L"logi-native.exe";
    return !install_autostarts(exe) || recovery_task::ensure(exe);
}

inline bool set_windows_autostart(bool enable, const std::filesystem::path& directory) {
    if (!enable) {
        const auto rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, L"LogiNative");
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND && rc != ERROR_PATH_NOT_FOUND) return false;
        return recovery_task::remove();
    }
    const auto exe = directory / L"logi-native.exe";
    const auto command = L"\"" + exe.wstring() + L"\" --tray";
    if (RegSetKeyValueW(HKEY_CURRENT_USER, run_key, L"LogiNative", REG_SZ, command.c_str(),
                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) return false;
    // A user explicitly enabling login startup here can undo a previous Task Manager disable.
    if (!login_start_approved() &&
        RegDeleteKeyValueW(HKEY_CURRENT_USER, approval_key, L"LogiNative") != ERROR_SUCCESS) return false;
    return recovery_task::ensure(exe);
}

} // namespace ui_settings
