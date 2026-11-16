#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace notifications_hud {

enum class Kind { info, lock, mic, battery, flow, widget };

// Displays a non-stealing translucent OSD pill near the bottom-right corner
// of the primary work area and dismisses it automatically.
void show(std::wstring_view title, std::wstring_view subtitle = {}, Kind kind = Kind::info,
          unsigned duration_ms = 1800, bool light = false);

// Toggles or resets the Spotlight cursor focus highlight overlay.
void toggle_spotlight();
void close_spotlight();
bool spotlight_active();

// Toggles a lightweight always-on-top compact Stopwatch overlay.
void toggle_stopwatch(bool light = false);
void init_hooks();
void shutdown();

using NotifyHook = void (*)(std::wstring_view title, std::wstring_view subtitle, Kind kind,
                            unsigned duration_ms, bool light);
using VoidHook = void (*)();
using StopwatchHook = void (*)(bool light);

inline NotifyHook& notify_hook() {
    static NotifyHook hook{};
    return hook;
}

inline VoidHook& spotlight_hook() {
    static VoidHook hook{};
    return hook;
}

inline StopwatchHook& stopwatch_hook() {
    static StopwatchHook hook{};
    return hook;
}

inline void dispatch_show(std::wstring_view title, std::wstring_view subtitle = {},
                          Kind kind = Kind::info, unsigned duration_ms = 1800, bool light = false) {
    if (auto* fn = notify_hook()) fn(title, subtitle, kind, duration_ms, light);
}

inline void dispatch_toggle_spotlight() {
    if (auto* fn = spotlight_hook()) fn();
}

inline void dispatch_toggle_stopwatch(bool light = false) {
    if (auto* fn = stopwatch_hook()) fn(light);
}

} // namespace notifications_hud
