#pragma once
#include <windows.h>
#include <stop_token>

// A queued operation belongs to the window that requested it, never whichever
// window happens to have focus after a slow driver/COM call finishes.
struct ActionContext {
    HWND window{};
    DWORD pid{};
    std::stop_token stop;
    bool active() const {
        if(stop.stop_requested())return false;
        if(!window)return true; // Standalone callers have no foreground lease.
        DWORD current{};
        return GetForegroundWindow()==window && GetWindowThreadProcessId(window,&current) && current==pid;
    }
};
