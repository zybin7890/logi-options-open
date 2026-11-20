#pragma once

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>

namespace lazy_win32 {

template<class Fn>
inline Fn load_proc(const wchar_t* module_name, const char* proc_name, HMODULE* out_module = nullptr) {
    HMODULE mod = GetModuleHandleW(module_name);
    if (!mod) mod = LoadLibraryW(module_name);
    if (!mod) return nullptr;
    if (out_module) *out_module = mod;
    const FARPROC proc = GetProcAddress(mod, proc_name);
    if (!proc) return nullptr;
    Fn fn{};
    static_assert(sizeof(fn) == sizeof(proc));
    std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

inline bool init_common_controls(DWORD icc) {
    static const auto fn = load_proc<decltype(&InitCommonControlsEx)>(L"comctl32.dll", "InitCommonControlsEx");
    if (!fn) return false;
    INITCOMMONCONTROLSEX common{sizeof(common), icc};
    return fn(&common) != FALSE;
}

inline BOOL set_window_subclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data) {
    static const auto fn = load_proc<decltype(&SetWindowSubclass)>(L"comctl32.dll", "SetWindowSubclass");
    return fn ? fn(hwnd, proc, id, data) : FALSE;
}

inline BOOL remove_window_subclass(HWND hwnd, SUBCLASSPROC proc, UINT_PTR id) {
    static const auto fn = load_proc<decltype(&RemoveWindowSubclass)>(L"comctl32.dll", "RemoveWindowSubclass");
    return fn ? fn(hwnd, proc, id) : FALSE;
}

inline LRESULT def_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static const auto fn = load_proc<decltype(&DefSubclassProc)>(L"comctl32.dll", "DefSubclassProc");
    return fn ? fn(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

inline HRESULT set_window_theme(HWND hwnd, const wchar_t* sub_app, const wchar_t* sub_id) {
    using Fn = HRESULT (WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static const auto fn = load_proc<Fn>(L"uxtheme.dll", "SetWindowTheme");
    return fn ? fn(hwnd, sub_app, sub_id) : E_NOTIMPL;
}

template<class Fn, class Arg>
inline BOOL call_comdlg(const char* proc_name, Arg arg) {
    HMODULE mod = LoadLibraryW(L"comdlg32.dll");
    if (!mod) return FALSE;
    const FARPROC proc = GetProcAddress(mod, proc_name);
    BOOL result = FALSE;
    if (proc) {
        Fn fn{};
        static_assert(sizeof(fn) == sizeof(proc));
        std::memcpy(&fn, &proc, sizeof(fn));
        result = fn(arg);
    }
    FreeLibrary(mod);
    return result;
}

inline BOOL get_open_file_name(LPOPENFILENAMEW ofn) {
    return call_comdlg<decltype(&GetOpenFileNameW)>("GetOpenFileNameW", ofn);
}

inline BOOL get_save_file_name(LPOPENFILENAMEW ofn) {
    return call_comdlg<decltype(&GetSaveFileNameW)>("GetSaveFileNameW", ofn);
}

inline BOOL choose_color(LPCHOOSECOLORW cc) {
    return call_comdlg<decltype(&ChooseColorW)>("ChooseColorW", cc);
}

} // namespace lazy_win32
