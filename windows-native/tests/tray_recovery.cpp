#include "../windows_autostart.hpp"
#include "../tray_recovery.hpp"

#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct RegistryFixture {
    HKEY user{}, root{};
    std::wstring path = L"Software\\LogiNative-TrayRecoveryTest-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64());
    bool created{}, redirected{};

    void open() {
        require(RegOpenCurrentUser(KEY_ALL_ACCESS, &user) == ERROR_SUCCESS, "cannot open fixture parent");
        DWORD disposition{};
        require(RegCreateKeyExW(user, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root,
                                &disposition) == ERROR_SUCCESS, "cannot create fixture");
        created = disposition == REG_CREATED_NEW_KEY;
        require(created, "fixture name already in use");
        redirected = RegOverridePredefKey(HKEY_CURRENT_USER, root) == ERROR_SUCCESS;
        require(redirected, "cannot redirect this process's HKCU");
    }
    ~RegistryFixture() {
        if (redirected) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (root) RegCloseKey(root);
        if (created) RegDeleteTreeW(user, path.c_str());
        if (user) RegCloseKey(user);
    }
};

void set_login_command(const std::filesystem::path& executable, const wchar_t* arguments) {
    const auto command = L"\"" + executable.wstring() + L"\" " + arguments;
    require(RegSetKeyValueW(HKEY_CURRENT_USER, ui_settings::run_key, L"LogiNative", REG_SZ,
                            command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
            "cannot write fixture Run command");
}

void verify_autostart(const std::filesystem::path& executable) {
    using ui_settings::current_install_autostarts;
    require(!current_install_autostarts(), "absent Run command enabled recovery");
    const auto other = executable.parent_path() / L"another installation" / executable.filename();
    set_login_command(other, L"--tray");
    require(ui_settings::install_autostarts(other) && !current_install_autostarts(),
            "a second installation was allowed to recover");
    set_login_command(executable, L"--recover-tray");
    require(!current_install_autostarts(), "recovery command was treated as login opt-in");
    set_login_command(executable, L"--tray");
    require(current_install_autostarts(), "matching login command did not enable recovery");
    std::array<BYTE, 12> approval{0x03};
    require(RegSetKeyValueW(HKEY_CURRENT_USER, ui_settings::approval_key, L"LogiNative", REG_BINARY,
                            approval.data(), static_cast<DWORD>(approval.size())) == ERROR_SUCCESS, "cannot disable approval");
    require(!current_install_autostarts(), "Task Manager disable was ignored");
    approval.front() = 0x02;
    require(RegSetKeyValueW(HKEY_CURRENT_USER, ui_settings::approval_key, L"LogiNative", REG_BINARY,
                            approval.data(), static_cast<DWORD>(approval.size())) == ERROR_SUCCESS, "cannot enable approval");
    require(current_install_autostarts(), "Task Manager re-enable was ignored");
}

void verify_exit_marker(const std::filesystem::path& executable) {
    using tray_recovery::ExitMarker;
    using tray_recovery::session_exit_marker;
    require(!session_exit_marker(ExitMarker::read), "clean logon unexpectedly suppressed");
    require(session_exit_marker(ExitMarker::set) && session_exit_marker(ExitMarker::read),
            "explicit exit did not suppress recovery");
    DWORD session{};
    require(ProcessIdToSessionId(GetCurrentProcessId(), &session), "cannot read session ID");
    const auto value = std::to_wstring(session) + L":" + executable.wstring();
    constexpr auto key = LR"(Software\LogiNative\TrayRecovery)";
    LUID previous{};
    DWORD size = sizeof(previous);
    require(RegGetValueW(HKEY_CURRENT_USER, key, value.c_str(), RRF_RT_REG_BINARY, nullptr, &previous, &size)
            == ERROR_SUCCESS && size == sizeof(previous), "logon identity missing from app-scoped marker");
    previous.LowPart ^= 1;
    require(RegSetKeyValueW(HKEY_CURRENT_USER, key, value.c_str(), REG_BINARY, &previous, sizeof(previous))
            == ERROR_SUCCESS, "cannot seed a different logon identity");
    require(!session_exit_marker(ExitMarker::read), "previous logon suppressed current logon");
    require(session_exit_marker(ExitMarker::set) && session_exit_marker(ExitMarker::clear)
            && !session_exit_marker(ExitMarker::read), "manual reopen failed to clear explicit exit");
}

} // namespace

int main() {
    try {
        RegistryFixture registry;
        registry.open();
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        require(length && length < path.size(), "cannot identify this test executable");
        verify_autostart(path.data());
        verify_exit_marker(path.data());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
