#include "mouse.hpp"
#include "remap.hpp"

#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <thread>

namespace {

struct Setting {
    const wchar_t* id;
    const wchar_t* title;
};

constexpr Setting settings[]{
    {L"dpi", L"Pointer DPI"}, {L"smartshift", L"SmartShift (0 = off)"},
    {L"ratchet", L"Ratcheted wheel"}, {L"hires", L"High-resolution scroll"},
    {L"invert", L"Vertical scroll reversed"}, {L"thumb-invert", L"Thumb wheel reversed"},
};

constexpr UINT completed = WM_APP + 1;
enum : WORD { refresh = 1, apply, edit, start, stop, preview, first_field = 20 };

struct App {
    HWND window{}, device{}, info{}, message{}, apply_button{};
    std::array<HWND, std::size(settings)> fields{};
    std::array<std::optional<unsigned>, std::size(settings)> baseline{};
    std::array<std::optional<unsigned>, std::size(settings)> values{};
    std::wstring name, details, result;
    std::filesystem::path directory;
    std::thread work;
};

std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

int px(int logical) {
    return MulDiv(logical, static_cast<int>(GetDpiForSystem()), 96);
}

HWND widget(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
            int x, int y, int w, int h, WORD id = 0, DWORD ext = 0) {
    const auto child = CreateWindowExW(ext, type, text, WS_CHILD | WS_VISIBLE | style,
                                       px(x), px(y), px(w), px(h),
                                       parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                       GetModuleHandleW(nullptr), nullptr);
    static const auto font = [] {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            if (const auto created = CreateFontIndirectW(&metrics.lfMessageFont)) return created;
        }
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }();
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return child;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    return file.bad() ? std::nullopt : std::optional{std::move(text)};
}

struct MappingEditor {
    std::filesystem::path path;
    std::string original;
    std::wstring initial_text;
    HWND edit{};
    HWND status{};
};

bool save_mappings(MappingEditor& editor) {
    const auto size = GetWindowTextLengthW(editor.edit);
    std::wstring content(static_cast<std::size_t>(size) + 1, L'\0');
    GetWindowTextW(editor.edit, content.data(), size + 1);
    content.resize(size);
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), size,
                                            nullptr, 0, nullptr, nullptr);
    if (!bytes) return false;
    std::string utf8(bytes, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), size, utf8.data(),
                            bytes, nullptr, nullptr)) return false;
    if (read_file(editor.path) != editor.original) {
        SetWindowTextW(editor.status, L"File changed on disk. Reopen the editor to reload it.");
        return false;
    }
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(editor.path.parent_path().c_str(), L"LNV", 0, temporary)) {
        SetWindowTextW(editor.status, L"Could not create a temporary file in this folder.");
        return false;
    }
    std::ofstream stream(std::filesystem::path{temporary}, std::ios::binary);
    stream.write(utf8.data(), utf8.size());
    stream.close();
    const bool valid = stream && check_mappings(temporary);
    const bool saved = valid && MoveFileExW(temporary, editor.path.c_str(),
                                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!saved) DeleteFileW(temporary);
    SetWindowTextW(editor.status, saved ? L"Saved. Restart remaps to apply this file."
                                       : valid ? L"Could not replace mappings.ini." : L"Invalid mappings. File left intact.");
    if (saved) {
        editor.original = std::move(utf8);
        SendMessageW(editor.edit, EM_SETMODIFY, FALSE, 0);
    }
    return saved;
}

LRESULT CALLBACK editor_proc(HWND window, UINT message, WPARAM parameter, LPARAM payload) {
    auto* editor = reinterpret_cast<MappingEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        editor = static_cast<MappingEditor*>(reinterpret_cast<CREATESTRUCTW*>(payload)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
        widget(window, L"STATIC", L"Profiles, button gestures, Smart Actions shortcuts and Ring menus",
               0, 14, 12, 620, 22);
        editor->edit = widget(window, L"EDIT", editor->initial_text.c_str(),
                              WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                              14, 42, 602, 377, 1, WS_EX_CLIENTEDGE);
        SendMessageW(editor->edit, EM_SETLIMITTEXT, 65535, 0);
        widget(window, L"BUTTON", L"Validate and save", WS_TABSTOP, 14, 435, 128, 30, 2);
        widget(window, L"BUTTON", L"Close", WS_TABSTOP, 512, 435, 104, 30, 3);
        editor->status = widget(window, L"STATIC", L"Settings are stored in mappings.ini.",
                                0, 14, 475, 602, 22);
        return 0;
    }
    if (editor && message == WM_COMMAND && HIWORD(parameter) == BN_CLICKED) {
        if (LOWORD(parameter) == 2) { save_mappings(*editor); return 0; }
        if (LOWORD(parameter) == 3) { SendMessageW(window, WM_CLOSE, 0, 0); return 0; }
    }
    if (editor && message == WM_CLOSE) {
        if (!SendMessageW(editor->edit, EM_GETMODIFY, 0, 0) ||
            MessageBoxW(window, L"Discard unsaved changes?", L"Edit mappings", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, parameter, payload);
}

void edit_mappings(HWND parent, const std::filesystem::path& path) {
    const auto original = read_file(path);
    if (!original) {
        MessageBoxW(parent, L"Could not read mappings.ini.", L"Edit mappings", MB_OK | MB_ICONERROR);
        return;
    }
    auto initial = widen(original->starts_with("\xEF\xBB\xBF") ? original->substr(3) : *original);
    std::wstring windows_lines;
    for (std::size_t i = 0; i < initial.size(); ++i) {
        if (initial[i] == L'\n' && (!i || initial[i - 1] != L'\r')) windows_lines.push_back(L'\r');
        windows_lines.push_back(initial[i]);
    }
    MappingEditor editor{path, *original, std::move(windows_lines)};
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = editor_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    type.lpszClassName = L"LogiNativeMappingEditor";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT area{0, 0, px(630), px(505)};
    AdjustWindowRect(&area, style, FALSE);
    const auto window = CreateWindowW(type.lpszClassName, L"LogiNative Mapping Editor", style,
                                      CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left,
                                      area.bottom - area.top, parent, nullptr, instance, &editor);
    if (!window) return;
    EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW);
    MSG msg{};
    while (IsWindow(window)) {
        const auto message = GetMessageW(&msg, nullptr, 0, 0);
        if (message <= 0) {
            if (message == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(window)) DestroyWindow(window);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

void run_io(App& app, bool write) {
    if (app.work.joinable()) return;
    auto desired = app.baseline;
    if (write) {
        if (options_running()) {
            SetWindowTextW(app.message, L"Exit the Options+ agent before applying changes.");
            return;
        }
        for (unsigned i = 0; i < app.fields.size(); ++i) {
            if (!desired[i]) continue;
            wchar_t text[16]{};
            GetWindowTextW(app.fields[i], text, std::size(text));
            desired[i] = logi::number(text);
            if (!desired[i] || (i == 1 && *desired[i] > 50) || (i > 1 && *desired[i] > 1)) {
                SetWindowTextW(app.message, L"Check the numeric fields. Wheel switches use 0 or 1.");
                return;
            }
        }
        if (desired[5] != app.baseline[5] && agent_running()) {
            SetWindowTextW(app.message, L"Stop remaps before changing thumb wheel direction.");
            return;
        }
    }
    EnableWindow(app.apply_button, FALSE);
    SetWindowTextW(app.message, write ? L"Applying changed values…" : L"Reading HID++ device…");
    app.work = std::thread([&app, desired, write] {
        app.name = L"No responsive HID++ mouse";
        app.details.clear();
        app.result = L"Wake the mouse and click Refresh.";
        app.values = {};
        if (auto mouse = logi::connect()) {
            app.name = widen(mouse->device.name);
            app.details = L"HID++ " + std::to_wstring(mouse->device.major) + L"." +
                          std::to_wstring(mouse->device.minor) + L"  ·  Slot " +
                          std::to_wstring(mouse->device.slot);
            if (const auto battery = mouse->request(mouse->battery, mouse->unified_battery ? 1 : 0);
                battery && battery.packet.data()[0] > 0 && battery.packet.data()[0] <= 100) {
                app.details += L"  ·  Battery " + std::to_wstring(battery.packet.data()[0]) + L"%";
            }
            if (const auto host = logi::get(*mouse, L"host"))
                app.details += L"  ·  Easy-Switch " + std::to_wstring(*host);
            app.result = write ? L"Changes verified by device readback." : L"Device settings loaded.";
            for (unsigned i = 0; i < app.fields.size(); ++i) {
                const auto id = settings[i].id;
                if (write && desired[i] && desired[i] != app.baseline[i]) {
                    if (!logi::set(*mouse, id, *desired[i]) || logi::get(*mouse, id) != desired[i]) {
                        app.result = std::wstring(settings[i].title) + L" could not be applied.";
                        break;
                    }
                }
            }
            for (unsigned i = 0; i < app.fields.size(); ++i)
                app.values[i] = logi::get(*mouse, settings[i].id);
        }
        PostMessageW(app.window, completed, 0, 0);
    });
}

void start_remaps(App& app) {
    if (agent_running()) {
        SetWindowTextW(app.message, L"A LogiNative agent is already active.");
        return;
    }
    const auto exe = app.directory / L"logi-native.exe";
    const auto config = app.directory / L"mappings.ini";
    const auto log = app.directory / L"agent.log";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const auto output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = startup.hStdError = output;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + exe.wstring() + L"\" run-preview \"" + config.wstring() + L"\"";
    const bool started = output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE &&
        CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, app.directory.c_str(), &startup, &process);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (started) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    SetWindowTextW(app.message, started ? L"Agent launched. See agent.log for device status."
                                       : L"Could not launch the agent in this folder.");
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM parameter, LPARAM payload) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(payload)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->device = widget(window, L"STATIC", L"LogiNative", 0, 20, 16, 410, 22);
        app->info = widget(window, L"STATIC", L"", 0, 20, 40, 410, 22);
        for (unsigned i = 0; i < std::size(settings); ++i) {
            const int top = 74 + static_cast<int>(i) * 34;
            widget(window, L"STATIC", settings[i].title, 0, 20, top + 4, 260, 24);
            app->fields[i] = widget(window, L"EDIT", L"", WS_TABSTOP | ES_NUMBER,
                                     304, top, 110, 26, first_field + i, WS_EX_CLIENTEDGE);
            SendMessageW(app->fields[i], EM_SETLIMITTEXT, 5, 0);
        }
        widget(window, L"BUTTON", L"Refresh", WS_TABSTOP, 20, 287, 118, 30, refresh);
        app->apply_button = widget(window, L"BUTTON", L"Apply", WS_TABSTOP,
                                    296, 287, 118, 30, apply);
        widget(window, L"BUTTON", L"Edit mappings", WS_TABSTOP, 20, 334, 118, 30, edit);
        widget(window, L"BUTTON", L"Start takeover", WS_TABSTOP, 158, 334, 118, 30, start);
        widget(window, L"BUTTON", L"Stop / restore", WS_TABSTOP, 296, 334, 118, 30, stop);
        widget(window, L"BUTTON", L"Preview Ring", WS_TABSTOP, 20, 377, 118, 30, preview);
        app->message = widget(window, L"STATIC", L"", 0, 20, 422, 410, 42);
        run_io(*app, false);
        return 0;
    }
    if (app && message == completed) {
        app->work.join();
        app->baseline = app->values;
        SetWindowTextW(app->device, app->name.c_str());
        SetWindowTextW(app->info, app->details.c_str());
        SetWindowTextW(app->message, app->result.c_str());
        for (unsigned i = 0; i < app->fields.size(); ++i) {
            const auto value = app->baseline[i];
            SetWindowTextW(app->fields[i], value ? std::to_wstring(*value).c_str() : L"");
            EnableWindow(app->fields[i], value.has_value());
        }
        EnableWindow(app->apply_button, app->baseline[0].has_value());
        return 0;
    }
    if (app && message == WM_COMMAND && HIWORD(parameter) == BN_CLICKED) {
        switch (LOWORD(parameter)) {
        case refresh: run_io(*app, false); return 0;
        case apply: run_io(*app, true); return 0;
        case edit: {
            edit_mappings(window, app->directory / L"mappings.ini");
            return 0;
        }
        case preview:
            EnableWindow(window, FALSE);
            preview_ring((app->directory / L"mappings.ini").c_str());
            EnableWindow(window, TRUE);
            SetForegroundWindow(window);
            return 0;
        case start: start_remaps(*app); return 0;
        case stop:
            SetWindowTextW(app->message, stop_agent() == 0 ? L"Stop requested. See agent.log."
                                                         : L"No LogiNative agent found.");
            return 0;
        }
    }
    if (app && message == WM_DESTROY) {
        if (app->work.joinable()) app->work.join();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, parameter, payload);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    App app;
    wchar_t module[4096]{};
    const auto size = GetModuleFileNameW(nullptr, module, std::size(module));
    if (!size || size >= std::size(module)) return 1;
    app.directory = std::filesystem::path(module).parent_path();
    WNDCLASSW window{};
    window.lpfnWndProc = window_proc;
    window.hInstance = instance;
    window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    window.lpszClassName = L"LogiNativeSettings";
    if (!RegisterClassW(&window)) return 1;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, px(434), px(469)};
    AdjustWindowRect(&bounds, style, FALSE);
    const auto hwnd = CreateWindowW(window.lpszClassName, L"LogiNative Settings", style,
                                     CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                     bounds.bottom - bounds.top, nullptr, nullptr, instance, &app);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
