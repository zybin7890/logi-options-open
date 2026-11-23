#pragma once

#include <windows.h>
#include <string>

namespace tray_recovery {

// Serialize the interval before FindWindow can see the new GUI. Recovery ticks
// never wait for a competing launch. A manual launch can then show its window.
class StartupGate {
public:
    explicit StartupGate(bool recovery) {
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\LogiNative.TrayStartup");
        if (!mutex_) return;
        const DWORD result = WaitForSingleObject(mutex_, recovery ? 0 : 10000);
        locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    StartupGate(const StartupGate&) = delete;
    StartupGate& operator=(const StartupGate&) = delete;
    ~StartupGate() { release(); }
    bool acquired() const { return locked_; }
    void release() {
        if (locked_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
        locked_ = false;
        mutex_ = nullptr;
    }
private:
    HANDLE mutex_{};
    bool locked_{};
};

inline bool agent_has_owner() {
    const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LogiNative.Agent");
    if (!mutex) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(mutex);
    return true;
}

enum class ExitMarker { read, clear, set };

inline bool session_exit_marker(ExitMarker action) {
    // Authentication IDs distinguish successive logons. The volatile key also
    // disappears when the user hive unloads, including across a cold boot.
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_STATISTICS stats{};
    DWORD bytes{};
    const BOOL token_ok = GetTokenInformation(token, TokenStatistics, &stats, sizeof(stats), &bytes);
    CloseHandle(token);
    DWORD session{};
    if (!token_ok || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return false;
    wchar_t module[4096]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, 4096);
    if (!length || length >= 4096) return false;
    const std::wstring value = std::to_wstring(session) + L":" + module;
    constexpr auto key_path = L"Software\\LogiNative\\TrayRecovery";
    if (action == ExitMarker::read) {
        LUID stored{};
        DWORD size = sizeof(stored);
        return RegGetValueW(HKEY_CURRENT_USER, key_path, value.c_str(), RRF_RT_REG_BINARY,
                            nullptr, &stored, &size) == ERROR_SUCCESS && size == sizeof(stored)
            && stored.LowPart == stats.AuthenticationId.LowPart && stored.HighPart == stats.AuthenticationId.HighPart;
    }
    if (action == ExitMarker::clear) {
        return RegDeleteKeyValueW(HKEY_CURRENT_USER, key_path, value.c_str()) == ERROR_SUCCESS;
    }
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path, 0, nullptr, REG_OPTION_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto result = RegSetValueExW(key, value.c_str(), 0, REG_BINARY,
                                       reinterpret_cast<const BYTE*>(&stats.AuthenticationId), sizeof(LUID));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

} // namespace tray_recovery
