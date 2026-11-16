#include <windows.h>
BOOL WINAPI preview_enum(WNDENUMPROC visit, LPARAM data) { visit(reinterpret_cast<HWND>(2), data); return TRUE; }
BOOL WINAPI preview_visible(HWND) { return TRUE; }
HWND WINAPI preview_owner(HWND, UINT) { return nullptr; }
BOOL WINAPI preview_iconic(HWND) { return FALSE; }
BOOL WINAPI preview_foreground(HWND) { return FALSE; }
#define EnumWindows preview_enum
#define IsWindowVisible preview_visible
#define GetWindow preview_owner
#define IsIconic preview_iconic
#define SetForegroundWindow preview_foreground
#include "environment.hpp"

int main(int argc, char** argv) { try {
    CHECK(argc == 3);
    const std::string scenario = argv[1];
    fixture_path = argv[2];
    std::string sequence = "KeyDown(LeftCtrl);KeyDown(A);Wait(64);KeyUp(A);KeyUp(LeftCtrl)";
    int expected = 0;
    if (scenario == "app-denied") {
        // C:\Apps\b.exe, which the fake EnumWindows and image-query APIs find.
        sequence = "AppForeground(433a5c417070735c622e657865);Ctrl+W";
        expected = 3;
    } else if (scenario == "input-failed") {
        unsigned calls = 0;
        input_result = [calls](UINT count, LPINPUT) mutable { return ++calls == 2 ? 0u : count; };
        expected = 3;
    } else if (scenario == "focus-cancel") {
        after_sleep = [](DWORD duration) { if (duration == 16) foreground_app = 2; };
        expected = 4;
    } else if (scenario == "open-continues") {
        allow_open = fail_open = true;
        sequence = "A;Open(fake.exe);Wait(64);B";
    } else if (scenario != "recorded") throw std::runtime_error("unknown preview case");
    std::ofstream(fixture_path) << "[hotkeys]\nCtrl+Alt+F10=" << sequence << '\n';
    CHECK(play_actions(fixture_path.c_str(), true) == 0 && captured.empty() && tick == 100);
    CHECK(play_actions(fixture_path.c_str(), false) == expected);
    CHECK(connects == 0); // Preview must never open HID transport.
    if (scenario == "app-denied") CHECK(captured.empty());
    else if (scenario == "input-failed") {
        CHECK(captured.size() == 2 && captured[0].ki.wVk == VK_LCONTROL && captured[1].ki.wVk == VK_LCONTROL);
        CHECK(captured[1].ki.dwFlags & KEYEVENTF_KEYUP);
    } else if (scenario == "open-continues") {
        CHECK(captured.size() == 4 && captured[0].ki.wVk == 'A' && captured[2].ki.wVk == 'B');
    } else {
        CHECK(captured.size() == 4 && captured[0].ki.wVk == VK_LCONTROL && captured[1].ki.wVk == 'A');
        CHECK(captured[2].ki.wVk == 'A' && (captured[2].ki.dwFlags & KEYEVENTF_KEYUP));
        CHECK(captured[3].ki.wVk == VK_LCONTROL && (captured[3].ki.dwFlags & KEYEVENTF_KEYUP));
        CHECK(capture_times[0] == 3100 && capture_times[2] == (scenario == "recorded" ? 3164u : 3116u));
    }
    std::cout << "PASS actual play_actions " << scenario << " exit=" << expected << " with isolated input\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; } }
