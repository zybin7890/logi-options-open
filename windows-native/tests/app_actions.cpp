#include <windows.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
static HWND front = reinterpret_cast<HWND>(1);
static bool allow_foreground = true, minimize_target{}, missing_target{}, close_ok = true;
static HWND affected{};
static int show_command{};
static unsigned closes{};
static std::vector<INPUT> inputs;
HWND WINAPI app_front() { return front; }
DWORD WINAPI app_pid(HWND window, DWORD* pid) { *pid = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(window)) + 10; return 1; }
BOOL WINAPI app_enum(WNDENUMPROC visit, LPARAM data) {
    for (UINT_PTR i = 1; i < 5; ++i) if (!visit(reinterpret_cast<HWND>(i), data)) break;
    return TRUE;
}
BOOL WINAPI app_visible(HWND window) { return window != reinterpret_cast<HWND>(4); }
HWND WINAPI app_owner(HWND window, UINT relationship) { assert(relationship == GW_OWNER); return window == reinterpret_cast<HWND>(3) ? reinterpret_cast<HWND>(2) : nullptr; }
HANDLE WINAPI app_process(DWORD access, BOOL inherit, DWORD pid) { assert(access == PROCESS_QUERY_LIMITED_INFORMATION && !inherit); return reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(pid)); }
BOOL WINAPI app_query(HANDLE handle, DWORD flags, wchar_t* output, DWORD* count) {
    assert(!flags);
    std::wstring path = handle == reinterpret_cast<HANDLE>(12) && !missing_target ? L"C:\\Apps\\Example.exe" : L"D:\\Other\\Example.exe";
    assert(*count > path.size()); std::memcpy(output, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); *count = static_cast<DWORD>(path.size()); return TRUE;
}
BOOL WINAPI app_closehandle(HANDLE) { return TRUE; }
BOOL WINAPI app_iconic(HWND window) { return window == reinterpret_cast<HWND>(2) && minimize_target; }
BOOL WINAPI app_show(HWND window, int command) { affected = window; show_command = command; return TRUE; }
BOOL WINAPI app_foreground(HWND window) { if (allow_foreground) front = window; return allow_foreground; }
BOOL WINAPI app_post(HWND window, UINT message, WPARAM, LPARAM) { assert(message == WM_CLOSE); affected = window; ++closes; return close_ok; }
SHORT WINAPI app_key(int) { return 0; }
UINT WINAPI app_input(UINT count, INPUT* values, int) { inputs.insert(inputs.end(), values, values + count); return count; }
#define GetForegroundWindow app_front
#define GetWindowThreadProcessId app_pid
#define EnumWindows app_enum
#define IsWindowVisible app_visible
#define GetWindow app_owner
#define OpenProcess app_process
#define QueryFullProcessImageNameW app_query
#define CloseHandle app_closehandle
#define IsIconic app_iconic
#define ShowWindowAsync app_show
#define SetForegroundWindow app_foreground
#define PostMessageW app_post
#define GetAsyncKeyState app_key
#define SendInput app_input
#include "reject_effects.hpp"
#include "../remap.cpp"
int choose_ring_action(std::span<const std::wstring>, HANDLE, bool, std::span<const RingIcon>, float) { throw std::runtime_error("unexpected Ring window"); }

static std::wstring hex(std::string_view value) {
    const wchar_t* digits = L"0123456789abcdef"; std::wstring result;
    for (unsigned char ch : value) { result += digits[ch >> 4]; result += digits[ch & 15]; }
    return result;
}
int main() {
    const auto path = hex("C:/Apps/Example.exe");
    Action activate, close, maximize, minimize;
    assert(parse_action(L"AppForeground(" + path + L")", activate));
    assert(parse_action(L"AppClose(" + path + L")", close));
    assert(parse_action(L"AppMaximize(" + path + L")", maximize));
    assert(parse_action(L"AppMinimize(" + path + L")", minimize));
    for (const auto invalid : {L"AppForeground(00)", L"AppClose(zz)", L"AppMinimize(ff)", L"AppMaximize(616263)", L"AppClose()"}) {
        Action action; assert(!parse_action(invalid, action));
    }
    InputContext context;
    assert(execute(activate, nullptr, &context) && front == reinterpret_cast<HWND>(2) && context.pid == 12);
    assert(execute(close, nullptr) && affected == reinterpret_cast<HWND>(2) && closes == 1);
    front = reinterpret_cast<HWND>(1);
    assert(execute(maximize, nullptr) && affected == reinterpret_cast<HWND>(2) && show_command == SW_MAXIMIZE);
    assert(front == reinterpret_cast<HWND>(1));
    assert(execute(minimize, nullptr) && affected == reinterpret_cast<HWND>(2) && show_command == SW_MINIMIZE);
    minimize_target = true;
    assert(execute(activate, nullptr) && show_command == SW_RESTORE);
    Mappings mappings; logi::Mouse mouse{};
    Binding shortcut{{}, 0x52, Gesture::tap, {}};
    assert(parse_actions(L"AppForeground(" + path + L");Ctrl+W", shortcut.actions));
    {
        Sequences sequences{{}, mappings}; front = reinterpret_cast<HWND>(1); allow_foreground = false;
        sequences.start(shortcut, mouse); assert(inputs.empty() && sequences.runs.empty() && front == reinterpret_cast<HWND>(1));
        allow_foreground = true; missing_target = true;
        sequences.start(shortcut, mouse); assert(inputs.empty() && sequences.runs.empty());
        missing_target = false;
        sequences.start(shortcut, mouse); assert(inputs.size() == 4 && front == reinterpret_cast<HWND>(2));
        inputs.clear();
        Binding delayed{{}, 0x52, Gesture::tap, {}};
        assert(parse_actions(L"AppForeground(" + path + L");Wait(100);A", delayed.actions));
        sequences.start(delayed, mouse); front = reinterpret_cast<HWND>(1); sequences.service(mouse);
        assert(inputs.empty() && sequences.runs.empty());
    }
    missing_target = true; affected = nullptr;
    assert(!execute(close, nullptr) && !affected && closes == 1);
    missing_target = false; close_ok = false;
    assert(!execute(close, nullptr));
    std::puts("App actions: exact executable path/window selection, foreground verification, failed activation abort, restore/maximize/minimize/close target and delayed shortcut ownership passed.");
}
