#include "notifications_hud.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace notifications_hud {
namespace {

constexpr wchar_t hud_class[] = L"LogiNativeHudPill";
constexpr wchar_t spotlight_class[] = L"LogiNativeSpotlightOverlay";
constexpr UINT msg_update_hud = WM_APP + 11;
constexpr UINT msg_toggle_spotlight = WM_APP + 12;
constexpr UINT msg_toggle_stopwatch = WM_APP + 13;
constexpr UINT timer_hide_hud = 1;
constexpr UINT timer_spotlight = 2;
constexpr UINT timer_stopwatch = 3;

struct SharedState {
    std::mutex mutex;
    HWND hud_window{};
    HWND spotlight_window{};
    DWORD thread_id{};
    std::atomic<bool> thread_running{false};
    std::atomic<bool> spotlight_on{false};
    std::atomic<bool> stopwatch_on{false};
    ULONGLONG stopwatch_start_ms{};
    std::wstring title;
    std::wstring subtitle;
    Kind kind{Kind::info};
    unsigned duration_ms{1800};
    bool light{};
};

SharedState& state() {
    static SharedState s;
    return s;
}

void paint_hud(HWND window) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT rc{};
    GetClientRect(window, &rc);
    std::wstring title, subtitle;
    Kind kind{};
    bool light{};
    {
        std::lock_guard lock(state().mutex);
        title = state().title;
        subtitle = state().subtitle;
        kind = state().kind;
        light = state().light;
    }
    const COLORREF bg = light ? RGB(248, 249, 250) : RGB(24, 24, 26);
    const COLORREF border = light ? RGB(205, 210, 216) : RGB(56, 56, 62);
    const COLORREF fg = light ? RGB(22, 24, 28) : RGB(248, 249, 250);
    const COLORREF muted = light ? RGB(90, 96, 105) : RGB(180, 184, 192);
    const COLORREF accent = kind == Kind::battery ? RGB(255, 169, 48) : RGB(0, 234, 208);

    HBRUSH fill = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto old_brush = SelectObject(dc, fill);
    auto old_pen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 16, 16);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(fill);

    RECT bar{10, 14, 15, rc.bottom - 14};
    HBRUSH accent_brush = CreateSolidBrush(accent);
    FillRect(dc, &bar, accent_brush);
    DeleteObject(accent_brush);

    SetBkMode(dc, TRANSPARENT);
    HFONT title_font = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Microsoft YaHei UI");
    HFONT sub_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Microsoft YaHei UI");
    auto old_font = SelectObject(dc, title_font);
    SetTextColor(dc, fg);
    RECT title_rc{26, subtitle.empty() ? 18 : 11, rc.right - 16, subtitle.empty() ? rc.bottom - 10 : 38};
    DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &title_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!subtitle.empty()) {
        SelectObject(dc, sub_font);
        SetTextColor(dc, muted);
        RECT sub_rc{26, 38, rc.right - 16, rc.bottom - 10};
        DrawTextW(dc, subtitle.c_str(), static_cast<int>(subtitle.size()), &sub_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old_font);
    DeleteObject(title_font);
    DeleteObject(sub_font);
    EndPaint(window, &ps);
}

void update_spotlight_window(HWND window) {
    POINT pt{};
    GetCursorPos(&pt);
    constexpr int size = 148;
    SetWindowPos(window, HWND_TOPMOST, pt.x - size / 2, pt.y - size / 2, size, size,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN outer = CreateEllipticRgn(0, 0, size, size);
    HRGN inner = CreateEllipticRgn(10, 10, size - 10, size - 10);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    SetWindowRgn(window, outer, TRUE);
    DeleteObject(inner);
}

LRESULT CALLBACK hud_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT:
        paint_hud(window);
        return 0;
    case WM_TIMER:
        if (wparam == timer_hide_hud) {
            KillTimer(window, timer_hide_hud);
            if (!state().stopwatch_on.load()) ShowWindow(window, SW_HIDE);
        } else if (wparam == timer_stopwatch) {
            if (!state().stopwatch_on.load()) {
                KillTimer(window, timer_stopwatch);
                return 0;
            }
            const auto elapsed = GetTickCount64() - state().stopwatch_start_ms;
            const auto total_sec = elapsed / 1000;
            const auto tenths = (elapsed % 1000) / 100;
            wchar_t buf[64]{};
            std::swprintf(buf, std::size(buf), L"%02llu:%02llu.%llu",
                          static_cast<unsigned long long>(total_sec / 60),
                          static_cast<unsigned long long>(total_sec % 60),
                          static_cast<unsigned long long>(tenths));
            {
                std::lock_guard lock(state().mutex);
                state().title = L"秒表 · 计时中";
                state().subtitle = buf;
                state().kind = Kind::widget;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case msg_update_hud: {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        constexpr int width = 300, height = 68;
        const int x = work.right - width - 24;
        const int y = work.bottom - height - 24;
        SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(window, nullptr, TRUE);
        unsigned duration{};
        {
            std::lock_guard lock(state().mutex);
            duration = state().duration_ms;
        }
        KillTimer(window, timer_hide_hud);
        if (duration > 0) SetTimer(window, timer_hide_hud, duration, nullptr);
        return 0;
    }
    case msg_toggle_stopwatch: {
        const bool next = !state().stopwatch_on.load();
        state().stopwatch_on.store(next);
        if (next) {
            state().stopwatch_start_ms = GetTickCount64();
            {
                std::lock_guard lock(state().mutex);
                state().title = L"秒表 · 计时中";
                state().subtitle = L"00:00.0";
                state().kind = Kind::widget;
                state().duration_ms = 0;
            }
            SendMessageW(window, msg_update_hud, 0, 0);
            SetTimer(window, timer_stopwatch, 100, nullptr);
        } else {
            KillTimer(window, timer_stopwatch);
            const auto elapsed = GetTickCount64() - state().stopwatch_start_ms;
            wchar_t buf[64]{};
            std::swprintf(buf, std::size(buf), L"总计 %02llu:%02llu.%llu",
                          static_cast<unsigned long long>((elapsed / 1000) / 60),
                          static_cast<unsigned long long>((elapsed / 1000) % 60),
                          static_cast<unsigned long long>((elapsed % 1000) / 100));
            {
                std::lock_guard lock(state().mutex);
                state().title = L"秒表 · 已停止";
                state().subtitle = buf;
                state().kind = Kind::widget;
                state().duration_ms = 2400;
            }
            SendMessageW(window, msg_update_hud, 0, 0);
        }
        return 0;
    }
    case msg_toggle_spotlight: {
        auto* spot = state().spotlight_window;
        if (!spot) return 0;
        const bool next = !state().spotlight_on.load();
        state().spotlight_on.store(next);
        if (next) {
            update_spotlight_window(spot);
            SetTimer(spot, timer_spotlight, 16, nullptr);
        } else {
            KillTimer(spot, timer_spotlight);
            ShowWindow(spot, SW_HIDE);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK spotlight_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT rc{};
        GetClientRect(window, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(0, 234, 208));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wparam == timer_spotlight) {
            if (!state().spotlight_on.load()) {
                KillTimer(window, timer_spotlight);
                ShowWindow(window, SW_HIDE);
            } else {
                update_spotlight_window(window);
            }
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void ensure_thread() {
    if (state().thread_running.load()) return;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread([ready] {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = hud_proc;
        wc.hInstance = instance;
        wc.lpszClassName = hud_class;
        RegisterClassW(&wc);

        WNDCLASSW sc{};
        sc.lpfnWndProc = spotlight_proc;
        sc.hInstance = instance;
        sc.lpszClassName = spotlight_class;
        RegisterClassW(&sc);

        auto hud = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                   hud_class, L"", WS_POPUP, 0, 0, 300, 68, nullptr, nullptr, instance, nullptr);
        SetLayeredWindowAttributes(hud, 0, 238, LWA_ALPHA);
        auto spot = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                    spotlight_class, L"", WS_POPUP, 0, 0, 148, 148, nullptr, nullptr, instance, nullptr);
        SetLayeredWindowAttributes(spot, 0, 195, LWA_ALPHA);
        {
            std::lock_guard lock(state().mutex);
            state().hud_window = hud;
            state().spotlight_window = spot;
            state().thread_id = GetCurrentThreadId();
            state().thread_running.store(true);
        }
        SetEvent(ready);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        {
            std::lock_guard lock(state().mutex);
            state().hud_window = nullptr;
            state().spotlight_window = nullptr;
            state().thread_id = 0;
            state().thread_running.store(false);
        }
    }).detach();
    WaitForSingleObject(ready, 1500);
    CloseHandle(ready);
}

} // namespace

void show(std::wstring_view title, std::wstring_view subtitle, Kind kind, unsigned duration_ms, bool light) {
    ensure_thread();
    HWND hud{};
    {
        std::lock_guard lock(state().mutex);
        state().title = title;
        state().subtitle = subtitle;
        state().kind = kind;
        state().duration_ms = duration_ms;
        state().light = light;
        hud = state().hud_window;
    }
    if (hud) PostMessageW(hud, msg_update_hud, 0, 0);
}

void toggle_spotlight() {
    ensure_thread();
    HWND hud{};
    {
        std::lock_guard lock(state().mutex);
        hud = state().hud_window;
    }
    if (hud) PostMessageW(hud, msg_toggle_spotlight, 0, 0);
}

void close_spotlight() {
    if (!state().spotlight_on.load()) return;
    toggle_spotlight();
}

bool spotlight_active() {
    return state().spotlight_on.load();
}

void toggle_stopwatch(bool light) {
    ensure_thread();
    HWND hud{};
    {
        std::lock_guard lock(state().mutex);
        state().light = light;
        hud = state().hud_window;
    }
    if (hud) PostMessageW(hud, msg_toggle_stopwatch, 0, 0);
}

void init_hooks() {
    notify_hook() = &show;
    spotlight_hook() = &toggle_spotlight;
    stopwatch_hook() = &toggle_stopwatch;
}

void shutdown() {
    HWND hud{};
    {
        std::lock_guard lock(state().mutex);
        hud = state().hud_window;
    }
    if (hud) PostMessageW(hud, WM_CLOSE, 0, 0);
}

} // namespace notifications_hud
