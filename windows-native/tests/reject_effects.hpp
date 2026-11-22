#pragma once
#include <windows.h>
#include <shellapi.h>
#include <stdexcept>
#include "../system_actions.hpp"
#include "../screen_capture.hpp"

#ifndef LOGI_CAPTURE_SYSTEM_ACTIONS
bool system_actions::perform(Action,const ActionContext&) { throw std::runtime_error("unexpected system action"); }
#endif
#ifndef LOGI_CAPTURE_VOLUME_STEPS
system_actions::VolumeResult system_actions::step_output_volume(bool,unsigned,const ActionContext&) {
    throw std::runtime_error("unexpected output volume action");
}
#endif
#ifndef LOGI_CAPTURE_SCREEN_ACTIONS
bool screen_capture::capture(Area,Destination,const ActionContext&) { throw std::runtime_error("unexpected screen capture"); }
#endif

// Standalone production-parser/executor tests must install their own fake for
// each exercised effect. An unexpected branch fails instead of touching Windows.
#ifndef SendInput
UINT WINAPI rejected_input(UINT, LPINPUT, int) { throw std::runtime_error("unexpected input injection"); }
#define SendInput rejected_input
#endif
#ifndef SetWindowsHookExW
HHOOK WINAPI rejected_hook(int, HOOKPROC, HINSTANCE, DWORD) { throw std::runtime_error("unexpected keyboard hook"); }
#define SetWindowsHookExW rejected_hook
#endif
#ifndef GetAsyncKeyState
SHORT WINAPI rejected_key_state(int) { throw std::runtime_error("unexpected physical key read"); }
#define GetAsyncKeyState rejected_key_state
#endif
#ifndef ShellExecuteW
HINSTANCE WINAPI rejected_shell(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { throw std::runtime_error("unexpected application launch"); }
#define ShellExecuteW rejected_shell
#endif
#ifndef ShowWindowAsync
BOOL WINAPI rejected_show(HWND, int) { throw std::runtime_error("unexpected window change"); }
#define ShowWindowAsync rejected_show
#endif
#ifndef SetForegroundWindow
BOOL WINAPI rejected_foreground(HWND) { throw std::runtime_error("unexpected foreground change"); }
#define SetForegroundWindow rejected_foreground
#endif
#ifndef PostMessageW
BOOL WINAPI rejected_post(HWND, UINT, WPARAM, LPARAM) { throw std::runtime_error("unexpected window message"); }
#define PostMessageW rejected_post
#endif
#ifndef LockWorkStation
BOOL WINAPI rejected_lock() { throw std::runtime_error("unexpected workstation lock"); }
#define LockWorkStation rejected_lock
#endif
