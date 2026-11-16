#include <windows.h>
#include <array>
#include <atomic>
#include <cassert>
#include <deque>
#include <vector>

static std::vector<INPUT> captured;
static std::deque<UINT> accepts;
static std::array<std::atomic_bool, 256> logical{};
static std::atomic<ULONGLONG> clock_tick{100};
static std::atomic<HWND> foreground{reinterpret_cast<HWND>(1)};
static std::atomic<HOOKPROC> keyboard_hook{};
static std::atomic<DWORD> hook_thread{};
static bool reject_hook{};
struct PhysicalEvent { KBDLLHOOKSTRUCT input; std::atomic_bool processed{}; };

UINT WINAPI captured_input(UINT count, INPUT* input, int size) {
    assert(size == sizeof(INPUT));
    UINT accepted = count;
    if (!accepts.empty()) { accepted = std::min(count, accepts.front()); accepts.pop_front(); }
    captured.insert(captured.end(), input, input + accepted);
    for (UINT i = 0; i < accepted; ++i)
        if (input[i].type == INPUT_KEYBOARD && !(input[i].ki.dwFlags & KEYEVENTF_UNICODE))
            logical[input[i].ki.wVk].store(!(input[i].ki.dwFlags & KEYEVENTF_KEYUP));
    return accepted;
}
SHORT WINAPI captured_state(int key) { return logical[key] ? SHORT(0x8000) : 0; }
ULONGLONG WINAPI captured_time() { return clock_tick; }
HWND WINAPI captured_foreground() { return foreground; }
DWORD WINAPI captured_pid(HWND window, DWORD* pid) { *pid = 77; return 1; }
HHOOK WINAPI captured_hook(int kind, HOOKPROC callback, HINSTANCE, DWORD) {
    assert(kind == WH_KEYBOARD_LL);
    if (reject_hook) return nullptr;
    keyboard_hook = callback;
    hook_thread = GetCurrentThreadId();
    return reinterpret_cast<HHOOK>(1);
}
BOOL WINAPI captured_unhook(HHOOK hook) { assert(hook == reinterpret_cast<HHOOK>(1)); keyboard_hook = nullptr; return TRUE; }
LRESULT WINAPI captured_next(HHOOK, int, WPARAM, LPARAM) { return 0; }
BOOL WINAPI captured_message(LPMSG message, HWND window, UINT first, UINT last) {
    const auto result = GetMessageW(message, window, first, last);
    if (result > 0 && message->message == WM_APP + 91) {
        auto& event = *reinterpret_cast<PhysicalEvent*>(message->lParam);
        keyboard_hook.load()(HC_ACTION, event.input.flags & LLKHF_UP ? WM_KEYUP : WM_KEYDOWN,
                             reinterpret_cast<LPARAM>(&event.input));
        event.processed = true;
        message->message = WM_NULL;
    }
    return result;
}
#define SendInput captured_input
#define GetAsyncKeyState captured_state
#define GetTickCount64 captured_time
#define GetForegroundWindow captured_foreground
#define GetWindowThreadProcessId captured_pid
#define SetWindowsHookExW captured_hook
#define UnhookWindowsHookEx captured_unhook
#define CallNextHookEx captured_next
#define GetMessageW captured_message
#include "reject_effects.hpp"
#include "../remap.cpp"
#undef SendInput
#undef GetAsyncKeyState
#undef GetTickCount64
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef CallNextHookEx
#undef GetMessageW
int choose_ring_action(std::span<const std::wstring>, HANDLE, bool, std::span<const RingIcon>, float) { throw std::runtime_error("unexpected Ring window"); }

static void physical(WORD key, bool down, bool injected = false) {
    PhysicalEvent event{{key, 0, DWORD((down ? 0 : LLKHF_UP) | (injected ? LLKHF_INJECTED : 0))}};
    if (!injected) logical[key] = down;
    assert(PostThreadMessageW(hook_thread, WM_APP + 91, 0, reinterpret_cast<LPARAM>(&event)));
    for (unsigned i = 0; !event.processed && i < 1000; ++i) Sleep(1);
    assert(event.processed);
}
static Binding binding(std::wstring_view actions) {
    Binding result{{}, 0x52, Gesture::tap, {}};
    assert(parse_actions(actions, result.actions));
    return result;
}
static void reset() {
    captured.clear(); accepts.clear();
    for (auto& key : logical) key = false;
    foreground = reinterpret_cast<HWND>(1); clock_tick = 100;
}
static bool up(std::size_t index, WORD key) {
    return captured.at(index).ki.wVk == key && (captured[index].ki.dwFlags & KEYEVENTF_KEYUP);
}
int main() {
    for (auto text : {L"KeyDown(LeftCtrl)", L"KeyUp(RightAlt)", L"KeyDown(186)", L"KeyUp(1)"}) {
        Action action; assert(parse_action(text, action));
    }
    for (auto text : {L"KeyDown(MouseBack)", L"KeyUp(255)", L"KeyDown(03)", L"KeyUp(-1)", L"KeyDown(A+B)", L"KeyDown()"}) {
        Action action; assert(!parse_action(text, action));
    }
    Action digit; assert(parse_action(L"KeyDown(1)", digit) && digit.keys[0] == '1');
    Action right; assert(parse_action(L"KeyDown(RightCtrl)", right) && extended(right.keys[0]));
    for (const auto [name, code] : {std::pair{L"CapsLock", WORD(VK_CAPITAL)}, {L"NumLock", WORD(VK_NUMLOCK)}, {L"Insert", WORD(VK_INSERT)}}) {
        Action action;
        assert(parse_action(std::wstring(L"KeyDown(") + name + L")", action) && action.keys[0] == code);
        assert(key(name) == code);
    }
    std::wstring many = L"None";
    for (int i = 1; i < 1024; ++i) many += L";None";
    std::vector<Action> actions; assert(parse_actions(many, actions));
    actions.clear(); assert(!parse_actions(many + L";None", actions));
    logi::Mouse mouse{}; Mappings mappings;
    auto hold = binding(L"KeyDown(LeftCtrl);KeyDown(A);Wait(200);KeyUp(A);KeyUp(LeftCtrl)");
    reset(); {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse);
        assert(captured.size() == 2 && logical['A'] && logical[VK_LCONTROL]);
        assert(sequences.service(mouse) == 16 && captured.size() == 2);
        clock_tick = 300; assert(sequences.service(mouse) == INFINITE);
        assert(captured.size() == 4 && up(2, 'A') && up(3, VK_LCONTROL));
        assert(sequences.runs.empty());
    }
    // An unmatched down is released at normal completion, including a final wait.
    reset(); {
        Sequences sequences{{}, mappings}; auto tail = binding(L"KeyDown(A);Wait(200)");
        sequences.start(tail, mouse); assert(captured.size() == 1);
        clock_tick = 300; sequences.service(mouse); assert(captured.size() == 2 && up(1, 'A'));
    }
    reset(); {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse);
        foreground = reinterpret_cast<HWND>(2); sequences.service(mouse);
        assert(captured.size() == 4 && up(2, 'A') && up(3, VK_LCONTROL) && sequences.runs.empty());
    }
    reset(); {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse); sequences.cancel();
        assert(captured.size() == 4 && up(2, 'A') && up(3, VK_LCONTROL));
        sequences.cancel(); assert(captured.size() == 4);
    }
    // Concurrent sequences share a down; foreign and duplicate key-ups own nothing.
    reset(); {
        Sequences sequences{{}, mappings}; auto a = binding(L"KeyDown(A);Wait(200);KeyUp(A)");
        auto b = binding(L"KeyDown(A);Wait(400);KeyUp(A);KeyUp(A)");
        auto foreign = binding(L"KeyUp(A)");
        sequences.start(a, mouse); sequences.start(b, mouse); sequences.start(foreign, mouse);
        assert(captured.size() == 1); clock_tick = 300; sequences.service(mouse); assert(captured.size() == 1);
        clock_tick = 500; sequences.service(mouse); assert(captured.size() == 2 && up(1, 'A'));
    }
    // An initially physical down, and a physical takeover during the wait, are never lifted.
    reset(); logical[VK_LCONTROL] = true; {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse); sequences.cancel();
        assert(captured.size() == 2 && captured[0].ki.wVk == 'A' && up(1, 'A') && logical[VK_LCONTROL]);
    }
    reset(); {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse);
        physical('A', true); sequences.cancel();
        assert(captured.size() == 3 && up(2, VK_LCONTROL) && logical['A']);
    }
    reset(); {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse);
        physical('A', true, true); sequences.cancel();
        assert(captured.size() == 4 && up(2, 'A'));
    }
    // Failed second down releases only the first accepted down and aborts the macro.
    reset(); accepts = {1, 0}; {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse);
        assert(captured.size() == 2 && up(1, VK_LCONTROL) && sequences.runs.empty());
    }
    // A rejected explicit up is not retried on later user input; remaining ownership is cleaned.
    reset(); {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse); accepts = {0, 1};
        clock_tick = 300; sequences.service(mouse);
        assert(captured.size() == 3 && up(2, VK_LCONTROL) && sequences.runs.empty());
        physical('A', true); sequences.cancel(); assert(captured.size() == 3);
    }
    // A partial ordinary shortcut aborts and unwinds both its accepted prefix and recorded holds.
    reset(); {
        Sequences sequences{{}, mappings}; auto mixed = binding(L"KeyDown(LeftCtrl);Wait(10);B;C");
        sequences.start(mixed, mouse); accepts = {1}; clock_tick = 110; sequences.service(mouse);
        assert(captured.size() == 4 && captured[1].ki.wVk == 'B' && up(2, 'B') && up(3, VK_LCONTROL));
        assert(sequences.runs.empty());
    }
    reset(); reject_hook = true; {
        Sequences sequences{{}, mappings}; sequences.start(hold, mouse);
        assert(captured.empty() && sequences.runs.empty());
    }
    std::puts("Recorded keys: production parser/execute/Sequences timing, shared leases, physical takeover, focus, cancellation and injection faults passed.");
}
