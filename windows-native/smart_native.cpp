#include "smart_native.hpp"
#include "remap.hpp"
#include "native_icons.hpp"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <uxtheme.h>
#include <vector>

namespace {

constexpr std::wstring_view marker = L"# Smart Action ";
constexpr int canvas_width = 1494;
constexpr COLORREF background = RGB(26, 26, 26);
constexpr COLORREF surface = RGB(15, 15, 15);
constexpr COLORREF foreground = RGB(239, 239, 239);
constexpr COLORREF accent = RGB(0, 226, 207);

struct Choice {
    const wchar_t* name;
    const wchar_t* code;
};

constexpr Choice mouse_buttons[] = {
    {L"中键", L"0052"}, {L"模式切换按钮", L"00C4"}, {L"前进按钮", L"0056"},
    {L"后退按钮", L"0053"}, {L"手势按钮", L"00C3"},
};
constexpr Choice system_actions[] = {
    {L"锁定", L"Lock"}, {L"屏幕截图", L"Win+Shift+S"}, {L"截屏（按系统设置）", L"PrintScreen"},
    {L"任务视图", L"Win+Tab"}, {L"显示/隐藏桌面", L"Win+D"}, {L"文件资源管理器", L"Win+E"},
    {L"开始菜单", L"Win"}, {L"音量降低", L"VolumeDown"}, {L"音量升高", L"VolumeUp"},
    {L"扬声器静音/取消静音", L"Mute"}, {L"播放/暂停", L"PlayPause"}, {L"下一个", L"NextTrack"},
    {L"上一个", L"PreviousTrack"}, {L"切换应用", L"Alt+Tab"}, {L"复制", L"Ctrl+C"},
    {L"粘贴", L"Ctrl+V"},
    {L"操作中心", L"Win+A"}, {L"撤销", L"Ctrl+Z"}, {L"此 PC", L"Open(shell:MyComputerFolder)"},
    {L"打印", L"Ctrl+P"}, {L"关闭窗口", L"Alt+F4"}, {L"关闭浏览器标签页", L"Ctrl+W"},
    {L"后退", L"Alt+Left"}, {L"计算器", L"Open(calc.exe)"}, {L"剪切", L"Ctrl+X"},
    {L"前进", L"Alt+Right"}, {L"全屏截图", L"Win+PrintScreen"}, {L"设置", L"Win+I"},
    {L"输入语言", L"Win+Space"}, {L"刷新", L"F5"}, {L"搜索", L"Win+S"},
    {L"新建浏览器标签页", L"Ctrl+T"}, {L"应用程式菜单", L"Shift+F10"}, {L"重做", L"Ctrl+Y"},
    {L"主页（浏览器）", L"Alt+Home"}, {L"桌面向右", L"Win+Ctrl+Right"}, {L"桌面向左", L"Win+Ctrl+Left"},
    {L"最大化窗口", L"Maximize"}, {L"最小化窗口", L"Minimize"},
    {L"Delete", L"Delete"}, {L"End", L"End"}, {L"Home", L"Home"},
    {L"Page down", L"PageDown"}, {L"Page up", L"PageUp"},
};
constexpr Choice applications[] = {
    {L"记事本", L"notepad.exe"}, {L"计算器", L"calc.exe"}, {L"文件资源管理器", L"explorer.exe"},
};
constexpr const wchar_t* trigger_names[]{L"设备", L"快捷键", L"Actions Ring"};
constexpr const wchar_t* step_names[]{L"应用程序", L"击键", L"文本", L"系统", L"延迟"};

std::wstring_view trim(std::wstring_view text) {
    const auto start = text.find_first_not_of(L" \t\r\n");
    return start == text.npos ? std::wstring_view{} :
        text.substr(start, text.find_last_not_of(L" \t\r\n") - start + 1);
}

bool same(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::wstring wide(std::string_view bytes) {
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                  static_cast<int>(bytes.size()), text.data(), count);
    return text;
}

std::string utf8(std::wstring_view text) {
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                  static_cast<int>(text.size()), bytes.data(), count, nullptr, nullptr);
    return bytes;
}

struct Trigger {
    std::wstring section, key;
};

bool same_trigger(const Trigger& a, const Trigger& b) {
    return same(a.section, b.section) && same(a.key, b.key);
}

struct Entry {
    std::wstring name, tag, sequence;
    Trigger trigger;
    std::optional<std::wstring> previous;
    std::wstring metadata;
};

struct Config {
    std::vector<std::wstring> lines;

    explicit Config(std::wstring_view text) {
        if (text.starts_with(L'\ufeff')) text.remove_prefix(1);
        while (!text.empty()) {
            const auto end = text.find(L'\n');
            auto line = text.substr(0, end);
            if (line.ends_with(L'\r')) line.remove_suffix(1);
            lines.emplace_back(line);
            if (end == text.npos) break;
            text.remove_prefix(end + 1);
        }
    }

    static std::wstring_view section(std::wstring_view line) {
        line = trim(line);
        if (line.size() < 3 || !line.starts_with(L'[') || !line.ends_with(L']')) return {};
        return trim(line.substr(1, line.size() - 2));
    }

    static std::wstring_view key(std::wstring_view line) {
        line = trim(line);
        if (line.empty() || line.starts_with(L'#') || line.starts_with(L';')) return {};
        const auto equals = line.find(L'=');
        return equals == line.npos ? std::wstring_view{} : trim(line.substr(0, equals));
    }

    std::optional<std::wstring> get(const Trigger& trigger) const {
        bool active = false;
        for (const auto& line : lines) {
            if (auto name = section(line); !name.empty()) active = same(name, trigger.section);
            else if (auto label = key(line); active && !label.empty() && same(label, trigger.key))
                return std::wstring(trim(std::wstring_view(line).substr(line.find(L'=') + 1)));
        }
        return std::nullopt;
    }

    void set(const Trigger& trigger, const std::optional<std::wstring>& value) {
        auto start = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            const auto name = section(lines[i]);
            if (name.empty()) {
                if (start == lines.size() || key(lines[i]).empty() || !same(key(lines[i]), trigger.key))
                    continue;
                if (value) lines[i] = trigger.key + L" = " + *value;
                else lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
            if (start != lines.size()) {
                if (value) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i), trigger.key + L" = " + *value);
                return;
            }
            if (same(name, trigger.section)) start = i;
        }
        if (!value) return;
        if (start == lines.size()) {
            if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
            lines.push_back(L"[" + trigger.section + L"]");
        }
        lines.push_back(trigger.key + L" = " + *value);
    }

    std::wstring text() const {
        std::wstring result;
        for (const auto& line : lines) result += line + L"\r\n";
        return result;
    }
};

// Only top-level JSON fields in the existing "# Smart Action" comment are needed.
size_t quoted_end(std::wstring_view text, size_t start) {
    for (size_t i = start + 1; i < text.size(); ++i) {
        if (text[i] == L'\\') ++i;
        else if (text[i] == L'"') return i + 1;
    }
    return text.npos;
}

std::wstring_view field(std::wstring_view json, std::wstring_view name) {
    if (!json.starts_with(L'{')) return {};
    size_t i = 1;
    while (i < json.size()) {
        i = json.find_first_not_of(L" \t\r\n,", i);
        if (i == json.npos || json[i] != L'"') return {};
        const auto end = quoted_end(json, i);
        if (end == json.npos) return {};
        const auto label = json.substr(i + 1, end - i - 2);
        i = json.find_first_not_of(L" \t\r\n", end);
        if (i == json.npos || json[i] != L':') return {};
        i = json.find_first_not_of(L" \t\r\n", i + 1);
        if (i == json.npos) return {};
        const auto start = i;
        unsigned depth = 0;
        while (i < json.size()) {
            const auto ch = json[i];
            if (ch == L'"') {
                i = quoted_end(json, i);
                if (i == json.npos) return {};
                continue;
            }
            if (ch == L'{' || ch == L'[') ++depth;
            if ((ch == L',' || ch == L'}') && !depth) break;
            if ((ch == L'}' || ch == L']') && depth) --depth;
            ++i;
        }
        if (label == name) return trim(json.substr(start, i - start));
    }
    return {};
}

std::optional<std::wstring> json_string(std::wstring_view value) {
    if (value.size() < 2 || !value.starts_with(L'"') || !value.ends_with(L'"')) return std::nullopt;
    std::wstring result;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        auto ch = value[i];
        if (ch == L'\\') {
            ch = value[++i];
            switch (ch) {
            case L'n': ch = L'\n'; break;
            case L'r': ch = L'\r'; break;
            case L't': ch = L'\t'; break;
            case L'b': ch = L'\b'; break;
            case L'f': ch = L'\f'; break;
            case L'u': {
                if (i + 4 >= value.size() - 1) return std::nullopt;
                wchar_t digits[]{value[i + 1], value[i + 2], value[i + 3], value[i + 4], 0};
                wchar_t* end = nullptr;
                ch = static_cast<wchar_t>(std::wcstoul(digits, &end, 16));
                if (end != digits + 4) return std::nullopt;
                i += 4;
                break;
            }
            default: break;
            }
        }
        result.push_back(ch);
    }
    return result;
}

std::wstring quote(std::wstring_view value) {
    std::wstring result = L"\"";
    for (const auto ch : value) {
        if (ch == L'\\' || ch == L'"') result.push_back(L'\\');
        if (ch < 0x20) {
            wchar_t code[7]{};
            std::swprintf(code, std::size(code), L"\\u%04x", static_cast<unsigned>(ch));
            result += code;
        } else result.push_back(ch);
    }
    return result + L"\"";
}

std::vector<Entry> records(const Config& config) {
    std::vector<Entry> result;
    for (const auto& line : config.lines) {
        if (!line.starts_with(marker)) continue;
        const auto json = std::wstring_view(line).substr(marker.size());
        const auto trigger = field(json, L"trigger");
        Entry entry;
        entry.name = json_string(field(json, L"name")).value_or(L"Smart Action");
        entry.tag = json_string(field(json, L"tag")).value_or(L"");
        entry.trigger.section = json_string(field(trigger, L"section")).value_or(L"");
        entry.trigger.key = json_string(field(trigger, L"key")).value_or(L"");
        entry.sequence = json_string(field(json, L"sequence")).value_or(L"");
        entry.previous = json_string(field(json, L"previous"));
        entry.metadata = line;
        if (!entry.trigger.key.empty() && config.get(entry.trigger) == entry.sequence) result.push_back(std::move(entry));
    }
    bool hotkeys = false;
    for (const auto& line : config.lines) {
        if (auto name = Config::section(line); !name.empty()) {
            hotkeys = same(name, L"hotkeys");
        } else if (auto key = Config::key(line); hotkeys && !key.empty()) {
            Trigger trigger{L"hotkeys", std::wstring(key)};
            if (std::ranges::none_of(result, [&](const Entry& entry) { return same_trigger(entry.trigger, trigger); }))
                result.push_back({std::wstring(key), {}, config.get(trigger).value_or(L""), trigger, {}, {}});
        }
    }
    return result;
}

std::wstring profile_name(const std::wstring& section) {
    if (same(section, L"global")) return L"全局设置";
    return std::filesystem::path(section).filename().wstring();
}

std::wstring trigger_name(const Trigger& trigger) {
    if (trigger.key.empty()) return L"请选择触发器";
    if (same(trigger.section, L"hotkeys")) return trigger.key;
    if (same(trigger.section, L"ring")) return L"Actions Ring · " + trigger.key;
    for (auto item : mouse_buttons)
        if (same(trigger.key, item.code)) return std::wstring(item.name) + L" · " + profile_name(trigger.section);
    return trigger.key + L" · " + profile_name(trigger.section);
}

std::vector<std::wstring> split_steps(std::wstring_view sequence) {
    std::vector<std::wstring> steps;
    unsigned depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < sequence.size(); ++i) {
        if (sequence[i] == L'(') ++depth;
        if (sequence[i] == L')' && depth) --depth;
        if (sequence[i] == L';' && !depth) {
            steps.emplace_back(trim(sequence.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < sequence.size()) steps.emplace_back(trim(sequence.substr(start)));
    return steps;
}

std::vector<std::wstring_view> array_items(std::wstring_view array) {
    std::vector<std::wstring_view> result;
    if (array.size() < 2 || !array.starts_with(L'[') || !array.ends_with(L']')) return result;
    array.remove_prefix(1);
    array.remove_suffix(1);
    unsigned depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < array.size(); ++i) {
        const auto ch = array[i];
        if (ch == L'"') {
            const auto end = quoted_end(array, i);
            if (end == array.npos) return {};
            i = end - 1;
        } else if (ch == L'{' || ch == L'[') ++depth;
        else if ((ch == L'}' || ch == L']') && depth) --depth;
        else if (ch == L',' && !depth) {
            result.push_back(trim(array.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (const auto last = trim(array.substr(start)); !last.empty()) result.push_back(last);
    return result;
}

std::wstring sequence_text(const std::vector<std::wstring>& steps) {
    std::wstring sequence;
    for (const auto& step : steps) {
        if (!sequence.empty()) sequence += L';';
        sequence += step;
    }
    return sequence;
}

std::vector<std::wstring> entry_steps(const Entry& entry) {
    auto steps = split_steps(entry.sequence);
    if (entry.metadata.empty()) return steps;
    const auto json = std::wstring_view(entry.metadata).substr(marker.size());
    const auto stored = array_items(field(json, L"steps"));
    if (stored.empty()) return steps;
    std::vector<std::wstring> grouped;
    size_t used = 0;
    for (const auto item : stored) {
        if (used == steps.size()) return steps;
        if (field(item, L"type") != L"\"keys\"") {
            grouped.push_back(steps[used++]);
            continue;
        }
        std::vector<std::wstring> keys;
        for (auto value : array_items(field(item, L"value"))) {
            auto key = json_string(value);
            if (!key || used == steps.size() || *key != steps[used++]) return steps;
            keys.push_back(std::move(*key));
        }
        if (keys.empty()) return steps;
        grouped.push_back(sequence_text(keys));
    }
    return used == steps.size() ? grouped : steps;
}

std::wstring inner(std::wstring_view step) {
    const auto start = step.find(L'(');
    return start == step.npos || !step.ends_with(L')') ? std::wstring{} :
        std::wstring(step.substr(start + 1, step.size() - start - 2));
}

std::wstring decoded_text(std::wstring_view step) {
    const auto code = inner(step);
    if (!step.starts_with(L"TextHex(")) return code;
    std::wstring text;
    for (size_t i = 0; i + 4 <= code.size(); i += 4)
        text.push_back(static_cast<wchar_t>(std::wcstoul(code.substr(i, 4).c_str(), nullptr, 16)));
    return text;
}

int step_type(std::wstring_view step) {
    for (const auto item : system_actions) if (step == item.code) return 3;
    if (step.starts_with(L"Open(")) return 0;
    if (step.starts_with(L"Text(") || step.starts_with(L"TextHex(")) return 2;
    if (step.starts_with(L"Wait(")) return 4;
    return 1;
}

std::wstring step_label(std::wstring_view step) {
    switch (step_type(step)) {
    case 0: return L"应用程序 · " + inner(step);
    case 2: {
        auto value = decoded_text(step);
        if (auto at = value.find_first_of(L"\r\n"); at != value.npos) value.erase(at);
        if (value.size() > 36) value = value.substr(0, 36) + L"…";
        return L"文本 · " + value;
    }
    case 3:
        for (auto item : system_actions) if (step == item.code) return L"系统 · " + std::wstring(item.name);
        break;
    case 4: return L"延迟 · " + inner(step) + L" 毫秒";
    }
    std::wstring label = L"击键 · ";
    bool first = true;
    for (const auto& chord : split_steps(step)) {
        if (chord.starts_with(L"Wait(")) continue;
        if (!first) label += L" → ";
        label += chord;
        first = false;
    }
    return label;
}

std::wstring steps_metadata(const std::vector<std::wstring>& steps) {
    constexpr const wchar_t* types[]{L"app", L"keys", L"text", L"system", L"wait"};
    std::wstring json = L"[";
    for (const auto& step : steps) {
        if (json.size() > 1) json += L',';
        const int type = step_type(step);
        json += L"{\"type\":" + quote(types[type]) + L",\"value\":";
        switch (type) {
        case 0: json += quote(inner(step)); break;
        case 2: json += quote(decoded_text(step)); break;
        case 3: json += quote(step); break;
        case 4: json += quote(std::to_wstring(std::wcstoul(inner(step).c_str(), nullptr, 10) / 1000.0)); break;
        case 1: {
            json += L'[';
            bool first = true;
            for (const auto& key : split_steps(step)) {
                if (!first) json += L',';
                json += quote(key);
                first = false;
            }
            json += L']';
            break;
        }
        }
        json += L'}';
    }
    return json + L']';
}

bool write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return static_cast<bool>(file);
}

std::filesystem::path temp_path(const std::filesystem::path& directory) {
    wchar_t buffer[MAX_PATH]{};
    if (!GetTempFileNameW(directory.c_str(), L"LNA", 0, buffer)) return {};
    return buffer;
}

struct SmartReloadRequest {
    unsigned long long revision{};
    ReloadFingerprint fingerprint{};
    std::string bytes;
};

struct Editor {
    std::filesystem::path directory;
    std::string initial;
    Config config{L""};
    std::vector<Entry> entries;
    std::vector<size_t> visible_entries;
    std::optional<Entry> original;
    std::vector<std::wstring> steps;
    std::vector<std::wstring> recorded_steps;
    std::vector<std::wstring> profiles;
    std::vector<std::wstring> ring_items;
    Trigger trigger;
    HFONT font{}, heading{}, title{};
    HBRUSH backdrop{}, fill{};
    HWND window{}, list{}, filter{}, empty{}, name{}, name_label{}, tag{}, tag_label{}, status{};
    HWND trigger_type{}, trigger_profile{}, trigger_choice{}, trigger_key{}, trigger_summary{};
    HWND action_type{}, action_value{}, action_choice{}, action_title{}, browse{}, record{}, fixed_delay{};
    ULONGLONG last_stroke{};
    int dpi{96};
    int window_width{};
    int edit_index{-1};
    bool editing{}, recording{}, trigger_open{}, action_open{}, closing{};
    unsigned long long hint_revision{}, reload_revision{}, reload_hint_revision{};
    std::wstring reload_message;
    std::optional<SmartReloadRequest> reload_active, reload_pending;
    ReloadResult reload_result;
    std::thread reload_worker;
    HANDLE reload_cancel{};
    ~Editor() {
        if (reload_cancel) SetEvent(reload_cancel);
        if (reload_worker.joinable()) reload_worker.join();
        if (reload_cancel) CloseHandle(reload_cancel);
    }
};

enum : WORD {
    close_id = 100, create_id, list_id, edit_id, remove_id, filter_id, back_id,
    trigger_type_id, trigger_profile_id, trigger_choice_id, trigger_key_id, apply_trigger_id,
    action_type_id, action_value_id, action_choice_id, apply_step_id, step_edit_id,
    step_remove_id, step_up_id, step_down_id, browse_id, preview_id, save_id, record_id, fixed_delay_id,
    minimize_id, manage_id, trigger_menu_id, action_menu_id, search_frame_id, add_tag_id, name_id,
};
constexpr UINT append_key = WM_APP + 7;
constexpr UINT reload_finished = WM_APP + 8;

int px(const Editor& editor, int logical) {
    if (editor.window_width) return MulDiv(logical, editor.window_width, canvas_width);
    return MulDiv(logical, editor.dpi, 96);
}

HWND widget(Editor& editor, const wchar_t* type, const wchar_t* text, DWORD style,
            int x, int y, int w, int h, WORD id = 0) {
    const auto child = CreateWindowW(type, text, WS_CHILD | WS_VISIBLE | style,
                                    px(editor, x), px(editor, y), px(editor, w), px(editor, h),
                                    editor.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                    GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(editor.font), FALSE);
    return child;
}

HWND label(Editor& editor, const wchar_t* text, int x, int y, int width, bool heading = false) {
    const auto handle = widget(editor, L"STATIC", text, 0, x, y, width, heading ? 34 : 22);
    if (heading) SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(editor.heading), FALSE);
    return handle;
}

HWND button(Editor& editor, const wchar_t* text, int x, int y, int w, WORD id, int height = 34) {
    return widget(editor, L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, x, y, w, height, id);
}

void paint_choice(Editor& editor, HWND window, HDC dc) {
    RECT box{};
    GetClientRect(window, &box);
    FillRect(dc, &box, editor.backdrop);
    const bool focused = GetFocus() == window;
    const auto brush = CreateSolidBrush(RGB(37, 37, 37));
    const auto pen = CreatePen(PS_SOLID, std::max(1, px(editor, 1)), focused ? accent : RGB(65, 65, 65));
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, px(editor, 8), px(editor, 8));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    const auto index = SendMessageW(window, CB_GETCURSEL, 0, 0);
    const auto length = index == CB_ERR ? CB_ERR : SendMessageW(window, CB_GETLBTEXTLEN, index, 0);
    if (length != CB_ERR) {
        std::wstring text(length + 1, L'\0');
        SendMessageW(window, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        RECT content = box;
        content.left += px(editor, 12);
        content.right -= px(editor, 30);
        const auto previous = SelectObject(dc, editor.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground);
        DrawTextW(dc, text.data(), static_cast<int>(length), &content,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, previous);
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen arrow(Gdiplus::Color(255, 215, 215, 215), static_cast<float>(std::max(1, px(editor, 1))));
    const float x = static_cast<float>(box.right - px(editor, 17));
    const float y = (box.top + box.bottom) / 2.0f;
    const float half = static_cast<float>(px(editor, 4));
    graphics.DrawLine(&arrow, x - half, y - half / 2, x, y + half / 2);
    graphics.DrawLine(&arrow, x, y + half / 2, x + half, y - half / 2);
}

LRESULT CALLBACK choice_control(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                                UINT_PTR id, DWORD_PTR context) {
    auto& editor = *reinterpret_cast<Editor*>(context);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        paint_choice(editor, window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        paint_choice(editor, window, reinterpret_cast<HDC>(parameter));
        return 0;
    }
    const auto result = DefSubclassProc(window, message, parameter, payload);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SETCURSEL ||
        message == CB_RESETCONTENT || message == CB_SHOWDROPDOWN || message == WM_LBUTTONUP ||
        message == WM_KEYUP) InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, choice_control, id);
    return result;
}

HWND combo(Editor& editor, int x, int y, int w, WORD id) {
    const auto control = widget(editor, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, x, y, w, 280, id);
    SetWindowTheme(control, L"", L"");
    SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(editor, 32));
    SendMessageW(control, CB_SETITEMHEIGHT, 0, px(editor, 34));
    SendMessageW(control, CB_SETDROPPEDWIDTH, px(editor, std::max(w, 220)), 0);
    SetWindowSubclass(control, choice_control, 1, reinterpret_cast<DWORD_PTR>(&editor));
    return control;
}

std::wstring read_text(HWND window) {
    const auto size = GetWindowTextLengthW(window);
    std::wstring text(size + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), size + 1));
    return text;
}

void add_choice(HWND box, std::wstring_view text) {
    const std::wstring copy(text);
    SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(copy.c_str()));
}

int selected(HWND window, UINT message = CB_GETCURSEL) {
    return static_cast<int>(SendMessageW(window, message, 0, 0));
}

void select(HWND box, int index) {
    SendMessageW(box, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

void hint(Editor& editor, const wchar_t* message) {
    ++editor.hint_revision;
    SetWindowTextW(editor.status, message);
}

const wchar_t* reload_notice(ReloadStatus status) {
    switch (status) {
    case ReloadStatus::applied: return L"已保存，后台已应用此配置。";
    case ReloadStatus::not_running: return L"已保存；后台未运行，尚未应用。";
    case ReloadStatus::unsupported: return L"已保存；当前后台版本不支持应用确认。";
    case ReloadStatus::configuration_mismatch: return L"已保存；后台使用另一份配置，尚未应用。";
    case ReloadStatus::superseded: return L"配置版本已变化，未确认本次配置仍在应用。";
    case ReloadStatus::invalid_mapping: return L"后台拒绝此配置；上一份有效映射保留。";
    case ReloadStatus::read_failed: return L"后台无法读取此配置；尚未应用。";
    case ReloadStatus::hotkey_failed: return L"快捷键注册失败；未应用此配置。";
    case ReloadStatus::hardware_failed: return L"设备未确认新映射；后台正在恢复上一份配置。";
    case ReloadStatus::device_unavailable: return L"已保存；设备未就绪，尚未应用。";
    case ReloadStatus::stopped: return L"后台已停止或重启；本次应用未确认。";
    case ReloadStatus::busy: return L"已保存；后台忙，尚未确认应用。";
    case ReloadStatus::timeout: return L"已保存；等待后台确认超时，应用结果未知。";
    case ReloadStatus::cancelled: return L"已停止等待；配置已保存，应用结果未确认。";
    default: return L"已保存；无法取得后台应用结果。";
    }
}

void launch_reload(Editor& editor) {
    if (editor.reload_active || !editor.reload_pending) return;
    if (!editor.reload_cancel) editor.reload_cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!editor.reload_cancel) {
        editor.reload_pending.reset();
        editor.reload_message=reload_notice(ReloadStatus::ipc_error);
        return;
    }
    editor.reload_active=std::move(editor.reload_pending);
    editor.reload_pending.reset();
    const auto request=*editor.reload_active;
    const auto path=editor.directory/L"mappings.ini";
    const auto window=editor.window;
    const auto cancel=editor.reload_cancel;
    editor.reload_worker=std::thread([&editor,request,path,window,cancel] {
        const auto deadline=GetTickCount64()+4000;
        ReloadResult result;
        for (;;) {
            const auto now=GetTickCount64();
            if (now>=deadline) {result.status=ReloadStatus::timeout;break;}
            result=reload_agent_confirmed(path.c_str(),request.revision,request.fingerprint,
                                          static_cast<unsigned>(deadline-now),cancel);
            if (result.status!=ReloadStatus::busy) break;
            if (WaitForSingleObject(cancel,100)==WAIT_OBJECT_0) {
                result.status=ReloadStatus::cancelled;break;
            }
        }
        editor.reload_result=result;
        PostMessageW(window,reload_finished,0,0);
    });
}

void queue_reload(Editor& editor,const std::string& bytes) {
    SmartReloadRequest request{++editor.reload_revision,{},bytes};
    editor.reload_pending.reset();
    if (!mapping_fingerprint(bytes,request.fingerprint)) {
        editor.reload_message=L"已保存；无法确认配置版本，尚未请求应用。";
        return;
    }
    editor.reload_pending=std::move(request);
    editor.reload_message=L"已保存，等待后台确认应用…";
    launch_reload(editor);
}

void show_reload_notice(Editor& editor) {
    hint(editor,editor.reload_message.c_str());
    editor.reload_hint_revision=editor.hint_revision;
}

void finish_reload(Editor& editor) {
    if (!editor.reload_active) return;
    if (editor.reload_worker.joinable()) editor.reload_worker.join();
    const auto request=std::move(*editor.reload_active);
    editor.reload_active.reset();
    auto result=editor.reload_result;
    if (request.revision==editor.reload_revision && request.bytes==editor.initial) {
        if (read_file(editor.directory/L"mappings.ini")!=request.bytes ||
            (result.status==ReloadStatus::applied &&
            (result.revision!=request.revision || result.fingerprint!=request.fingerprint ||
             !is_current_reload_result(result)))) result.status=ReloadStatus::superseded;
        editor.reload_message=reload_notice(result.status);
        if (editor.hint_revision==editor.reload_hint_revision) show_reload_notice(editor);
    }
    launch_reload(editor);
    if (editor.closing && !editor.reload_active && !editor.reload_pending)
        DestroyWindow(editor.window);
}

void close_editor(Editor& editor) {
    editor.closing=true;
    if (!editor.reload_active && !editor.reload_pending) DestroyWindow(editor.window);
    // A normal close lets the latest saved version finish its bounded submission.
    // The UI loop stays responsive; WM_DESTROY cancels only an abnormal early exit.
}

void update_save(Editor& editor) {
    if (const auto save = GetDlgItem(editor.window, save_id))
        EnableWindow(save, !editor.trigger.key.empty() && !editor.steps.empty() &&
                           !trim(read_text(editor.name)).empty());
}

std::wstring key_name(WPARAM vk) {
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9'))
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_SNAPSHOT: return L"PrintScreen";
    default: return {};
    }
}

LRESULT CALLBACK key_capture(HWND window, UINT message, WPARAM parameter, LPARAM payload) {
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, L"LogiNativeKeyEdit"));
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        if (GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY) {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            const bool alt = GetKeyState(VK_MENU) < 0 || (payload & (1L << 29));
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            const bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
            const bool shortcut = GetDlgCtrlID(window) == trigger_key_id;
            auto key = key_name(parameter);
            if (!key.empty() && !(payload & (1L << 30)) &&
                (!shortcut || (!win && (ctrl || alt || shift)))) {
                std::wstring chord;
                if (ctrl) chord += L"Ctrl+";
                if (alt) chord += L"Alt+";
                if (shift) chord += L"Shift+";
                if (win) chord += L"Win+";
                chord += key;
                if (shortcut) SetWindowTextW(window, chord.c_str());
                else SendMessageW(GetParent(window), append_key, 0, reinterpret_cast<LPARAM>(chord.c_str()));
            }
            return 0;
        }
        if (message == WM_KEYDOWN && parameter == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    if (message == WM_GETDLGCODE && payload && (GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY)) {
        const auto* key = reinterpret_cast<const MSG*>(payload);
        if (key->wParam != VK_TAB || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
            return DLGC_WANTALLKEYS;
    }
    if ((message == WM_CHAR || message == WM_SYSCHAR) &&
        (GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY)) return 0;
    if (message == WM_CHAR && parameter == 1) return 0;
    if (message == WM_NCDESTROY) RemovePropW(window, L"LogiNativeKeyEdit");
    return original ? CallWindowProcW(original, window, message, parameter, payload) :
        DefWindowProcW(window, message, parameter, payload);
}

void capture_keys(HWND control) {
    const auto prior = SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(key_capture));
    SetPropW(control, L"LogiNativeKeyEdit", reinterpret_cast<HANDLE>(prior));
}

void show_control(HWND window, bool visible) {
    if (window) ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
}

void trigger_visibility(Editor& editor) {
    const int type = selected(editor.trigger_type);
    show_control(editor.trigger_type, editor.trigger_open);
    show_control(editor.trigger_profile, editor.trigger_open && type == 0);
    show_control(editor.trigger_choice, editor.trigger_open && type != 1);
    show_control(editor.trigger_key, editor.trigger_open && type == 1);
    if (editor.trigger_choice)
        MoveWindow(editor.trigger_choice, px(editor, type == 2 ? 629 : 803), px(editor, 417),
                   px(editor, type == 2 ? 362 : 188), px(editor, 280), TRUE);
}

void show_trigger_fields(Editor& editor) {
    const int type = selected(editor.trigger_type);
    trigger_visibility(editor);
    SendMessageW(editor.trigger_choice, CB_RESETCONTENT, 0, 0);
    if (type == 0) {
        int chosen = 0;
        for (size_t i = 0; i < std::size(mouse_buttons); ++i) {
            add_choice(editor.trigger_choice, mouse_buttons[i].name);
            if (same(editor.trigger.key, mouse_buttons[i].code)) chosen = static_cast<int>(i);
        }
        select(editor.trigger_choice, chosen);
    }
    if (type == 2) {
        int chosen = 0;
        for (size_t i = 0; i < editor.ring_items.size(); ++i) {
            add_choice(editor.trigger_choice, editor.ring_items[i]);
            if (same(editor.trigger.key, editor.ring_items[i])) chosen = static_cast<int>(i);
        }
        select(editor.trigger_choice, chosen);
    }
}

void action_visibility(Editor& editor) {
    const int type = selected(editor.action_type);
    show_control(editor.action_type, editor.action_open);
    show_control(editor.action_choice, editor.action_open && (type == 0 || type == 3));
    show_control(editor.action_value, editor.action_open && type != 3);
    show_control(editor.action_title, editor.action_open);
    show_control(editor.browse, editor.action_open && type == 0);
    show_control(editor.record, editor.action_open && type == 1);
    show_control(editor.fixed_delay, editor.action_open && type == 1);
    show_control(GetDlgItem(editor.window, apply_step_id), editor.action_open);
    show_control(GetDlgItem(editor.window, add_tag_id), !editor.action_open && editor.steps.empty());
    const int top = editor.action_open ? 648 : 552;
    if (editor.list)
        MoveWindow(editor.list, px(editor, 491), px(editor, top), px(editor, 500), px(editor, 134), TRUE);
    show_control(editor.list, !editor.steps.empty());
    const WORD row_controls[]{step_edit_id, step_up_id, step_down_id, step_remove_id};
    for (unsigned i = 0; i < std::size(row_controls); ++i) {
        const auto control = GetDlgItem(editor.window, row_controls[i]);
        if (!control) continue;
        MoveWindow(control, px(editor, 1008), px(editor, top + static_cast<int>(i) * 34),
                   px(editor, 114), px(editor, 29), TRUE);
        show_control(control, !editor.steps.empty());
    }
}

void show_action_fields(Editor& editor, std::wstring_view step = {}) {
    const int type = selected(editor.action_type);
    action_visibility(editor);
    MoveWindow(editor.action_choice, px(editor, 650), px(editor, 587),
               px(editor, type == 3 ? 341 : 161), px(editor, 280), TRUE);
    MoveWindow(editor.action_value, px(editor, type == 0 ? 819 : 650), px(editor, 587),
               px(editor, type == 0 ? 172 : 341), px(editor, type == 2 || type == 1 ? 44 : 31), TRUE);
    editor.recording = false;
    editor.last_stroke = 0;
    editor.recorded_steps.clear();
    SetWindowTextW(editor.record, L"开始录制");
    SendMessageW(editor.action_value, EM_SETREADONLY, type == 1, 0);
    SendMessageW(editor.action_value, EM_SETLIMITTEXT, type == 4 ? 5 : 1000, 0);
    SendMessageW(editor.action_choice, CB_RESETCONTENT, 0, 0);
    SetWindowTextW(editor.action_title, L"");
    std::wstring value;
    if (type == 0) {
        for (auto item : applications) add_choice(editor.action_choice, item.name);
        value = step.empty() ? L"" : inner(step);
        SetWindowTextW(editor.action_title, L"选择程序，或填写程序路径");
    } else if (type == 1) {
        value = step;
        editor.recorded_steps = split_steps(step);
        SetWindowTextW(editor.action_title, L"按键，或录制一串击键");
    } else if (type == 2) {
        value = decoded_text(step);
        SetWindowTextW(editor.action_title, L"输入文本（最多 1000 字）");
    } else if (type == 3) {
        int chosen = 0;
        for (size_t i = 0; i < std::size(system_actions); ++i) {
            add_choice(editor.action_choice, system_actions[i].name);
            if (step == system_actions[i].code) chosen = static_cast<int>(i);
        }
        select(editor.action_choice, chosen);
    } else if (type == 4) {
        value = step.empty() ? L"1000" : inner(step);
        SetWindowTextW(editor.action_title, L"延迟（毫秒，0 到 30000）");
    }
    SetWindowTextW(editor.action_value, value.c_str());
    SetWindowTextW(GetDlgItem(editor.window, apply_step_id), editor.edit_index < 0 ? L"添加操作" : L"更新操作");
}

void fill_steps(Editor& editor, int index = -1) {
    SendMessageW(editor.list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < editor.steps.size(); ++i) {
        auto name = std::to_wstring(i + 1) + L".  " + step_label(editor.steps[i]);
        SendMessageW(editor.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    if (index >= 0) SendMessageW(editor.list, LB_SETCURSEL, index, 0);
    action_visibility(editor);
    if (const auto preview = GetDlgItem(editor.window, preview_id))
        EnableWindow(preview, !editor.steps.empty());
    update_save(editor);
}

bool contains_text(std::wstring_view text, std::wstring_view query) {
    if (query.empty()) return true;
    return std::ranges::search(text, query, [](wchar_t left, wchar_t right) {
        return CompareStringOrdinal(&left, 1, &right, 1, TRUE) == CSTR_EQUAL;
    }).begin() != text.end();
}

void fill_records(Editor& editor) {
    SendMessageW(editor.list, LB_RESETCONTENT, 0, 0);
    editor.visible_entries.clear();
    const auto filter = read_text(editor.filter);
    for (size_t i = 0; i < editor.entries.size(); ++i) {
        const auto& entry = editor.entries[i];
        auto title = entry.name + L"  ·  " + trigger_name(entry.trigger);
        if (!entry.tag.empty()) title += L"  ·  " + entry.tag;
        if (!contains_text(title, filter)) continue;
        SendMessageW(editor.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.c_str()));
        editor.visible_entries.push_back(i);
    }
    const bool populated = !editor.visible_entries.empty();
    if (populated) {
        const int rows = static_cast<int>(std::min<std::size_t>(editor.visible_entries.size(), 9));
        MoveWindow(editor.list, px(editor, 446), px(editor, 170), px(editor, 840), px(editor, rows * 54), TRUE);
        SendMessageW(editor.list, LB_SETCURSEL, 0, 0);
    }
    SetWindowTextW(editor.empty, filter.empty() ? L"点击「创建」以添加您的第一个 Smart Action。"
                                              : L"未找到匹配的 Smart Actions。");
    show_control(editor.list, populated);
    show_control(editor.empty, !populated);
    show_control(GetDlgItem(editor.window, edit_id), populated);
    show_control(GetDlgItem(editor.window, remove_id), populated);
    InvalidateRect(editor.window, nullptr, TRUE);
    hint(editor, L"");
}

void window_header(Editor& editor, const wchar_t* caption) {
    button(editor, L"返回", 43, 35, 40, back_id, 42);
    const auto title = widget(editor, L"STATIC", caption, SS_LEFT, 86, 30, 830, 60);
    if (editor.title) SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(editor.title), FALSE);
    button(editor, L"最小化", 1388, 1, 52, minimize_id, 34);
    button(editor, L"关闭", 1440, 1, 52, close_id, 34);
}

LRESULT CALLBACK filter_cue(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                           UINT_PTR id, DWORD_PTR) {
    const auto result = DefSubclassProc(window, message, parameter, payload);
    if (message == WM_PAINT && !GetWindowTextLengthW(window)) {
        const auto dc = GetDC(window);
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        const auto previous = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(177, 177, 177));
        DrawTextW(dc, L"搜索", 2, &bounds, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(window, nullptr, TRUE);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, filter_cue, id);
    return result;
}

void list_screen(Editor& editor) {
    editor.editing = false;
    window_header(editor, L"Smart Actions");
    button(editor, L"＋ 创建", 1361, 37, 88, create_id, 48);
    widget(editor, L"STATIC", L"管理", SS_OWNERDRAW, 50, 462, 86, 40, manage_id);
    widget(editor, L"STATIC", L"", SS_OWNERDRAW, 933, 37, 280, 49, search_frame_id);
    editor.filter = widget(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                           948, 51, 227, 23, filter_id);
    SetWindowPos(editor.filter, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowSubclass(editor.filter, filter_cue, 1, 0);
    editor.list = widget(editor, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                         LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                         446, 170, 840, 525, list_id);
    SendMessageW(editor.list, LB_SETITEMHEIGHT, 0, px(editor, 54));
    editor.empty = widget(editor, L"STATIC", L"", SS_CENTER, 446, 687, 840, 25);
    editor.status = label(editor, L"", 446, 784, 815);
    button(editor, L"编辑", 446, 716, 112, edit_id, 41);
    button(editor, L"移除", 570, 716, 112, remove_id, 41);
    fill_records(editor);
}

void editor_screen(Editor& editor) {
    editor.editing = true;
    editor.trigger_open = editor.action_open = false;
    window_header(editor, editor.original ? L"编辑 Smart Actions" : L"创建 Smart Actions");
    editor.name_label = label(editor, L"名称", 491, 236, 52);
    editor.name = widget(editor, L"EDIT", editor.original ? editor.original->name.c_str() : L"",
                         WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 545, 232, 225, 29, name_id);
    SendMessageW(editor.name, EM_SETLIMITTEXT, 60, 0);
    editor.tag_label = label(editor, L"标签", 789, 236, 49);
    editor.tag = widget(editor, L"EDIT", editor.original ? editor.original->tag.c_str() : L"",
                        WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 841, 232, 150, 29);
    SendMessageW(editor.tag, EM_SETLIMITTEXT, 40, 0);
    for (const auto control : {editor.tag_label, editor.tag})
        show_control(control, editor.original.has_value());
    label(editor, L"如果…", 511, 321, 480, true);
    editor.trigger_summary = button(editor, editor.trigger.key.empty() ? L"添加触发器" :
                                    trigger_name(editor.trigger).c_str(), 491, 360, 500, trigger_menu_id, 50);
    editor.trigger_type = combo(editor, 491, 417, 130, trigger_type_id);
    for (auto title : trigger_names) add_choice(editor.trigger_type, title);
    editor.trigger_profile = combo(editor, 629, 417, 166, trigger_profile_id);
    editor.trigger_choice = combo(editor, 803, 417, 188, trigger_choice_id);
    editor.trigger_key = widget(editor, L"EDIT", same(editor.trigger.section, L"hotkeys") ? editor.trigger.key.c_str() : L"",
                               WS_TABSTOP | WS_BORDER | ES_READONLY, 629, 417, 362, 34, trigger_key_id);
    SendMessageW(editor.trigger_key, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"按下 Ctrl、Alt 或 Shift 与一个主键"));
    capture_keys(editor.trigger_key);
    int profile = 0;
    for (size_t i = 0; i < editor.profiles.size(); ++i) {
        add_choice(editor.trigger_profile, profile_name(editor.profiles[i]));
        if (same(editor.profiles[i], editor.trigger.section)) profile = static_cast<int>(i);
    }
    select(editor.trigger_profile, profile);
    select(editor.trigger_type, same(editor.trigger.section, L"hotkeys") ? 1 : same(editor.trigger.section, L"ring") ? 2 : 0);
    show_trigger_fields(editor);
    label(editor, L"那么…", 511, 466, 480, true);
    button(editor, L"添加操作", 491, 505, 500, action_menu_id, 42);
    button(editor, L"＋ 添加标签", 491, 571, 91, add_tag_id, 30);
    editor.list = widget(editor, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                         LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                         491, 552, 500, 134, list_id);
    SendMessageW(editor.list, LB_SETITEMHEIGHT, 0, px(editor, 33));
    button(editor, L"编辑操作", 1008, 552, 114, step_edit_id, 29);
    button(editor, L"上移", 1008, 586, 114, step_up_id, 29);
    button(editor, L"下移", 1008, 620, 114, step_down_id, 29);
    button(editor, L"移除操作", 1008, 654, 114, step_remove_id, 29);
    editor.action_type = combo(editor, 491, 587, 150, action_type_id);
    for (auto title : step_names) add_choice(editor.action_type, title);
    editor.action_title = label(editor, L"", 650, 553, 341);
    editor.action_choice = combo(editor, 650, 587, 161, action_choice_id);
    editor.action_value = widget(editor, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                                 650, 587, 341, 44, action_value_id);
    capture_keys(editor.action_value);
    editor.browse = button(editor, L"浏览…", 1008, 550, 114, browse_id, 29);
    editor.record = button(editor, L"开始录制", 1008, 550, 114, record_id, 29);
    editor.fixed_delay = widget(editor, L"BUTTON", L"固定 21 毫秒", WS_TABSTOP | BS_AUTOCHECKBOX,
                                491, 553, 150, 23, fixed_delay_id);
    SendMessageW(editor.fixed_delay, BM_SETCHECK, BST_CHECKED, 0);
    button(editor, L"添加操作", 1008, 586, 114, apply_step_id, 35);
    editor.status = label(editor, L"", 491, 792, 760);
    button(editor, L"试试看", 1290, 721, 169, preview_id, 42);
    button(editor, L"保存", 1290, 770, 169, save_id, 40);
    select(editor.action_type, 2);
    show_action_fields(editor);
    fill_steps(editor);
}

void clear_widgets(HWND window) {
    while (const auto child = GetWindow(window, GW_CHILD)) DestroyWindow(child);
}

void begin(Editor& editor, std::optional<Entry> entry = std::nullopt) {
    editor.original = std::move(entry);
    editor.trigger = editor.original ? editor.original->trigger : Trigger{};
    editor.steps = editor.original ? entry_steps(*editor.original) : std::vector<std::wstring>{};
    editor.edit_index = -1;
    clear_widgets(editor.window);
    editor_screen(editor);
    InvalidateRect(editor.window, nullptr, TRUE);
    SetFocus(editor.original ? editor.name : editor.trigger_summary);
}

void show_list(Editor& editor) {
    editor.entries = records(editor.config);
    clear_widgets(editor.window);
    list_screen(editor);
    InvalidateRect(editor.window, nullptr, TRUE);
    SetFocus(editor.visible_entries.empty() ? editor.filter : editor.list);
}

void update_trigger(Editor& editor, bool close = true) {
    const auto type = selected(editor.trigger_type);
    const auto option = selected(editor.trigger_choice);
    if (type == 0) {
        const auto profile = selected(editor.trigger_profile);
        if (option < 0 || profile < 0) return;
        editor.trigger = {editor.profiles[profile], mouse_buttons[option].code};
    } else if (type == 1) {
        auto chord = read_text(editor.trigger_key);
        if (chord.empty()) { hint(editor, L"请在快捷键输入框按下组合键。"); return; }
        editor.trigger = {L"hotkeys", std::move(chord)};
    } else if (type == 2) {
        if (option < 0) {
            editor.trigger = {};
            SetWindowTextW(editor.trigger_summary, L"添加触发器");
            update_save(editor);
            hint(editor, L"请先在 Actions Ring 中设置一个可用位置。");
            return;
        }
        editor.trigger = {L"ring", editor.ring_items[option]};
    }
    SetWindowTextW(editor.trigger_summary, trigger_name(editor.trigger).c_str());
    if (close) {
        editor.trigger_open = false;
        trigger_visibility(editor);
        SetFocus(editor.trigger_summary);
        RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    update_save(editor);
    hint(editor, L"已选触发器。");
}

void add_step(Editor& editor) {
    const auto type = selected(editor.action_type);
    const auto option = selected(editor.action_choice);
    const auto value = read_text(editor.action_value);
    std::wstring step;
    switch (type) {
    case 0: {
        const auto path = trim(value);
        if (path.empty() || path.find_first_of(L"\r\n") != path.npos) {
            hint(editor, L"请选择程序，或填写一行程序路径。");
            return;
        }
        step = L"Open(" + std::wstring(path) + L")";
        break;
    }
    case 1:
        if (editor.recorded_steps.empty()) { hint(editor, L"请先录入一个按键或组合键。"); return; }
        step = sequence_text(editor.recorded_steps);
        break;
    case 2:
        if (value.empty()) { hint(editor, L"请输入要发送的文本。"); return; }
        step = L"TextHex(";
        for (auto ch : value) {
            wchar_t code[5]{};
            std::swprintf(code, std::size(code), L"%04x", static_cast<unsigned>(ch));
            step += code;
        }
        step += L")";
        break;
    case 3:
        if (option < 0) return;
        step = system_actions[option].code;
        break;
    case 4: {
        wchar_t* end = nullptr;
        const auto delay = std::wcstoul(value.c_str(), &end, 10);
        if (value.empty() || end != value.c_str() + value.size() || delay > 30000) {
            hint(editor, L"请输入 0 到 30000 之间的整数毫秒。");
            return;
        }
        step = L"Wait(" + std::to_wstring(delay) + L")";
        break;
    }
    default: return;
    }
    auto index = editor.edit_index;
    auto steps = editor.steps;
    if (index >= 0) steps[index] = std::move(step);
    else {
        index = static_cast<int>(editor.steps.size());
        steps.push_back(std::move(step));
    }
    if (split_steps(sequence_text(steps)).size() > 32) {
        hint(editor, L"最多可添加 32 个操作，击键间隔也计入。");
        return;
    }
    editor.steps = std::move(steps);
    editor.edit_index = -1;
    fill_steps(editor, index);
    show_action_fields(editor);
    hint(editor, L"操作已加入。");
}

void record_key(Editor& editor, std::wstring_view chord) {
    if (!editor.recording) editor.recorded_steps.clear();
    if (editor.recorded_steps.size() >= 31) return;
    const auto now = GetTickCount64();
    if (editor.recording && editor.last_stroke) {
        const auto delay = SendMessageW(editor.fixed_delay, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? 21ULL : std::min<ULONGLONG>(30000, now - editor.last_stroke);
        editor.recorded_steps.push_back(L"Wait(" + std::to_wstring(delay) + L")");
    }
    editor.recorded_steps.emplace_back(chord);
    editor.last_stroke = now;
    std::wstring label;
    for (const auto& step : editor.recorded_steps) {
        if (step.starts_with(L"Wait(")) continue;
        if (!label.empty()) label += L" → ";
        label += step;
    }
    SetWindowTextW(editor.action_value, label.c_str());
}

void toggle_recording(Editor& editor) {
    editor.recording = !editor.recording;
    SetWindowTextW(editor.record, editor.recording ? L"停止录制" : L"开始录制");
    if (editor.recording) {
        editor.recorded_steps.clear();
        editor.last_stroke = 0;
        SetWindowTextW(editor.action_value, L"");
        SetFocus(editor.action_value);
        hint(editor, L"录制中。完成后点击停止录制。");
    } else hint(editor, L"已停止录制。点击添加操作。");
}

void browse_application(Editor& editor) {
    const auto library = LoadLibraryW(L"comdlg32.dll");
    if (!library) return;
    const auto address = GetProcAddress(library, "GetOpenFileNameW");
    decltype(&GetOpenFileNameW) picker{};
    static_assert(sizeof(picker) == sizeof(address));
    std::memcpy(&picker, &address, sizeof(picker));
    wchar_t filename[MAX_PATH]{};
    OPENFILENAMEW data{};
    data.lStructSize = sizeof(data);
    data.hwndOwner = editor.window;
    data.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件\0*.*\0";
    data.lpstrFile = filename;
    data.nMaxFile = std::size(filename);
    data.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (picker && picker(&data)) SetWindowTextW(editor.action_value, filename);
    FreeLibrary(library);
}

void restore(Config& config, const Entry& entry) {
    if (config.get(entry.trigger) == entry.sequence) config.set(entry.trigger, entry.previous);
    if (!entry.metadata.empty()) std::erase(config.lines, entry.metadata);
}

bool commit(Editor& editor, Config config) {
    const auto target = editor.directory / L"mappings.ini";
    if (read_file(target) != editor.initial) {
        hint(editor, L"配置已在别处更改。关闭此窗口再打开即可读取。");
        return false;
    }
    const auto bytes = utf8(config.text());
    const auto temporary = temp_path(editor.directory);
    if (temporary.empty()) { hint(editor, L"无法在程序目录写入配置。"); return false; }
    const bool valid = !bytes.empty() && write_bytes(temporary, bytes) && check_mappings(temporary.c_str());
    const bool current = valid && read_file(target)==editor.initial;
    const bool done = current && MoveFileExW(temporary.c_str(), target.c_str(),
                                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!done) {
        DeleteFileW(temporary.c_str());
        hint(editor, !valid ? L"动作或触发器无效，原配置已保留。" :
             !current ? L"配置已在别处更改，本次保存已取消。" : L"无法替换配置文件。");
        return false;
    }
    editor.initial = bytes;
    editor.config = std::move(config);
    queue_reload(editor,bytes);
    return true;
}

void save(Editor& editor) {
    const auto raw_name = read_text(editor.name);
    const auto name = trim(raw_name);
    if (name.empty()) {
        for (const auto control : {editor.name_label, editor.name, editor.tag_label, editor.tag})
            show_control(control, true);
        hint(editor, L"请输入名称。");
        SetFocus(editor.name);
        return;
    }
    if (editor.trigger.key.empty()) { hint(editor, L"请先选择触发器。"); return; }
    if (editor.steps.empty()) { hint(editor, L"请添加至少一个操作。"); return; }
    auto config = editor.config;
    const bool same_original = editor.original && same_trigger(editor.original->trigger, editor.trigger);
    if (!same_original && std::ranges::any_of(editor.entries, [&](const Entry& item) {
            return !item.metadata.empty() && same_trigger(item.trigger, editor.trigger);
        })) {
        hint(editor, L"触发器已有 Smart Action。请在管理中编辑该动作。");
        return;
    }
    if (editor.original) restore(config, *editor.original);
    const auto previous = config.get(editor.trigger);
    if (!same_original && previous &&
        MessageBoxW(editor.window, L"此触发器已有动作。替换后，移除本 Smart Action 时会恢复原动作。继续？",
                    L"Smart Actions", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    const auto sequence = sequence_text(editor.steps);
    config.set(editor.trigger, sequence);
    config.lines.push_back(std::wstring(marker) + L"{\"name\":" + quote(name) +
        L",\"tag\":" + quote(read_text(editor.tag)) + L",\"trigger\":{\"section\":" +
        quote(editor.trigger.section) + L",\"key\":" + quote(editor.trigger.key) +
        L"},\"steps\":" + steps_metadata(editor.steps) + L",\"sequence\":" + quote(sequence) + L",\"previous\":" +
        (previous ? quote(*previous) : L"null") + L"}");
    if (commit(editor, std::move(config))) {
        show_list(editor);
        show_reload_notice(editor);
    }
}

void preview(Editor& editor) {
    if (editor.steps.empty()) { hint(editor, L"请添加至少一个操作。"); return; }
    const auto path = temp_path(editor.directory);
    if (path.empty()) { hint(editor, L"无法创建临时试运行文件。"); return; }
    const auto text = utf8(L"[hotkeys]\nCtrl+Alt+F10 = " + sequence_text(editor.steps) + L"\n");
    if (!write_bytes(path, text) || play_actions(path.c_str(), true) != 0) {
        DeleteFileW(path.c_str());
        hint(editor, L"当前动作无法试运行。");
        return;
    }
    const auto exe = editor.directory / L"logi-cli.exe";
    auto command = L"\"" + exe.wstring() + L"\" play-actions \"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, editor.directory.c_str(), &startup, &process)) {
        DeleteFileW(path.c_str());
        hint(editor, L"未能启动试运行。");
        return;
    }
    CloseHandle(process.hThread);
    std::thread([handle = process.hProcess, path] {
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        DeleteFileW(path.c_str());
    }).detach();
    hint(editor, L"3 秒后试运行。可切到目标窗口。");
}

void draw_list_row(Editor& editor, const DRAWITEMSTRUCT& item) {
    const auto dc = item.hDC;
    auto bounds = item.rcItem;
    FillRect(dc, &bounds, editor.backdrop);
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto length = SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0);
    if (length == LB_ERR) return;
    std::wstring title(length + 1, L'\0');
    if (SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(title.data())) == LB_ERR)
        return;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const auto fill = selected ? accent : surface;
    const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, fill);
    RoundRect(dc, bounds.left, bounds.top + 1, bounds.right, bounds.bottom - 1, px(editor, 8), px(editor, 8));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    bounds.left += px(editor, 16);
    const auto old_font = SelectObject(dc, editor.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? RGB(14, 28, 27) : foreground);
    DrawTextW(dc, title.data(), static_cast<int>(length), &bounds,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

void draw_choice_row(Editor& editor, const DRAWITEMSTRUCT& item) {
    const auto selected = (item.itemState & ODS_SELECTED) != 0;
    const auto fill = CreateSolidBrush(selected ? RGB(39, 65, 62) : RGB(37, 37, 37));
    FillRect(item.hDC, &item.rcItem, fill);
    DeleteObject(fill);
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    if (length == CB_ERR) return;
    std::wstring text(length + 1, L'\0');
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data()));
    auto area = item.rcItem;
    area.left += px(editor, 12);
    area.right -= px(editor, 10);
    const auto font = SelectObject(item.hDC, editor.font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, selected ? accent : foreground);
    DrawTextW(item.hDC, text.data(), static_cast<int>(length), &area,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(item.hDC, font);
}

void draw_button(Editor& editor, const DRAWITEMSTRUCT& item) {
    const auto id = item.CtlID;
    const bool header = id == back_id || id == close_id || id == minimize_id;
    const bool highlight = id == create_id || id == save_id || id == manage_id;
    const bool card = id == trigger_menu_id || id == action_menu_id;
    const bool search = id == search_frame_id;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = disabled ? RGB(30, 30, 30)
        : highlight ? (pressed ? RGB(4, 186, 171) : accent)
        : header ? (pressed ? RGB(55, 55, 55) : background)
        : search || card ? (pressed ? RGB(37, 37, 37) : surface)
        : id == preview_id && disabled ? RGB(30, 30, 30) : RGB(37, 37, 37);
    const COLORREF ink = disabled ? RGB(130, 130, 130)
        : highlight ? RGB(14, 28, 27) : foreground;
    const auto& bounds = item.rcItem;
    FillRect(item.hDC, &bounds, editor.backdrop);
    if (fill != background) {
        const auto brush = CreateSolidBrush(fill);
        const auto pen = CreatePen(PS_SOLID, 1, (search || card) ? RGB(45, 45, 45) : fill);
        const auto old_brush = SelectObject(item.hDC, brush);
        const auto old_pen = SelectObject(item.hDC, pen);
        const auto radius = px(editor, card ? 18 : 9);
        RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
        SelectObject(item.hDC, old_pen);
        SelectObject(item.hDC, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }
    if (header || id == manage_id || search) {
        const auto icon = id == back_id ? L"back" : id == close_id ? L"close"
            : id == minimize_id ? L"minimize" : search ? L"search" : L"settings";
        const float size = static_cast<float>(px(editor, id == back_id ? 23 : header ? 30 : 21));
        const float left = header ? (bounds.left + bounds.right - size) / 2.0f
            : search ? bounds.right - px(editor, 16) - size : bounds.left + px(editor, 12);
        Gdiplus::Graphics graphics(item.hDC);
        draw_native_icon(graphics, icon, left, (bounds.top + bounds.bottom - size) / 2.0f, size, ink);
        if (header || search) return;
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, ink);
    const auto old_font = SelectObject(item.hDC, editor.font);
    RECT area = bounds;
    if (card) area.left += px(editor, 20);
    if (id == manage_id) area.left += px(editor, 42);
    auto title = read_text(item.hwndItem);
    DrawTextW(item.hDC, title.c_str(), static_cast<int>(title.size()), &area,
              DT_SINGLELINE | (card || id == manage_id ? DT_LEFT : DT_CENTER) |
              DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(item.hDC, old_font);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = bounds;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM parameter, LPARAM payload) {
    auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        editor = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(payload)->lpCreateParams);
        editor->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
        list_screen(*editor);
        return 0;
    }
    if (!editor) return DefWindowProcW(window, message, parameter, payload);
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(parameter), &client, editor->backdrop);
        return TRUE;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, editor->backdrop);
        if (!editor->editing && editor->visible_entries.empty()) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush circle(Gdiplus::Color(255,36,52,51));
            graphics.FillEllipse(&circle, px(*editor, 773), px(*editor, 444),
                                 px(*editor, 186), px(*editor, 186));
            draw_native_icon(graphics, L"smart", px(*editor, 812), px(*editor, 482),
                             px(*editor, 108), accent);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(payload);
        if (item.CtlType == ODT_LISTBOX) draw_list_row(*editor, item);
        else if (item.CtlType == ODT_COMBOBOX) draw_choice_row(*editor, item);
        else draw_button(*editor, item);
        return TRUE;
    }
    if (message == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(payload);
        if (item.CtlType == ODT_COMBOBOX) { item.itemHeight = px(*editor, 34); return TRUE; }
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        const auto dc = reinterpret_cast<HDC>(parameter);
        SetTextColor(dc, foreground);
        const bool static_label = message == WM_CTLCOLORSTATIC &&
            reinterpret_cast<HWND>(payload) != editor->trigger_key &&
            reinterpret_cast<HWND>(payload) != editor->action_value;
        SetBkColor(dc, static_label ? background : surface);
        return reinterpret_cast<LRESULT>(static_label ? editor->backdrop : editor->fill);
    }
    if (message == reload_finished) { finish_reload(*editor);return 0; }
    if (message == WM_DESTROY && editor->reload_cancel) SetEvent(editor->reload_cancel);
    if (message == WM_CLOSE) {
        close_editor(*editor);
        return 0;
    }
    if (message == append_key && editor->editing) {
        record_key(*editor, reinterpret_cast<const wchar_t*>(payload));
        return 0;
    }
    if (message == WM_COMMAND) {
        const auto id = LOWORD(parameter);
        const auto notification = HIWORD(parameter);
        if (id == close_id) { close_editor(*editor); return 0; }
        if (id == minimize_id) {
            const auto owner = GetWindow(window, GW_OWNER);
            ShowWindow(owner ? owner : window, SW_MINIMIZE);
            return 0;
        }
        if (!editor->editing) {
            if (id == filter_id && notification == EN_CHANGE) {
                if (editor->filter && editor->list && editor->status) fill_records(*editor);
                return 0;
            }
            if (id == create_id) { begin(*editor); return 0; }
            if (id == back_id || id == IDCANCEL) { close_editor(*editor); return 0; }
            const bool edit = id == edit_id || (id == list_id && notification == LBN_DBLCLK);
            if (!edit && id != remove_id) return 0;
            const auto index = selected(editor->list, LB_GETCURSEL);
            if (index < 0 || static_cast<size_t>(index) >= editor->visible_entries.size()) return 0;
            const auto entry_index = editor->visible_entries[index];
            if (entry_index >= editor->entries.size()) return 0;
            const auto item = editor->entries[entry_index];
            if (edit) {
                begin(*editor, item);
            } else if (MessageBoxW(window, L"移除此 Smart Action？", L"Smart Actions",
                                  MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                auto config = editor->config;
                restore(config, item);
                if (commit(*editor, std::move(config))) {
                    show_list(*editor);
                    show_reload_notice(*editor);
                }
            }
            return 0;
        }
        const auto index = selected(editor->list, LB_GETCURSEL);
        switch (id) {
        case name_id:
            if (notification == EN_CHANGE) update_save(*editor);
            break;
        case back_id: case IDCANCEL: show_list(*editor); break;
        case add_tag_id:
            show_control(editor->tag_label, true);
            show_control(editor->tag, true);
            SetFocus(editor->tag);
            break;
        case trigger_menu_id:
            editor->trigger_open = !editor->trigger_open;
            if (editor->trigger_open) editor->action_open = false;
            trigger_visibility(*editor);
            action_visibility(*editor);
            break;
        case action_menu_id:
            editor->action_open = !editor->action_open;
            if (editor->action_open) editor->trigger_open = false;
            trigger_visibility(*editor);
            action_visibility(*editor);
            break;
        case trigger_type_id:
            if (notification == CBN_SELCHANGE) {
                show_trigger_fields(*editor);
                if (selected(editor->trigger_type) != 1 || !read_text(editor->trigger_key).empty())
                    update_trigger(*editor, false);
                else {
                    editor->trigger = {};
                    SetWindowTextW(editor->trigger_summary, L"添加触发器");
                    update_save(*editor);
                    SetFocus(editor->trigger_key);
                }
            }
            break;
        case trigger_profile_id:
            if (notification == CBN_SELCHANGE) update_trigger(*editor, false);
            break;
        case trigger_choice_id:
            if (notification == CBN_SELCHANGE) update_trigger(*editor, false);
            else if (notification == CBN_SELENDOK) update_trigger(*editor);
            break;
        case trigger_key_id:
            if (notification == EN_CHANGE && editor->trigger_open && !read_text(editor->trigger_key).empty())
                update_trigger(*editor);
            break;
        case apply_trigger_id: update_trigger(*editor); break;
        case action_type_id:
            if (notification == CBN_SELCHANGE) show_action_fields(*editor);
            break;
        case action_choice_id:
            if (notification == CBN_SELCHANGE && selected(editor->action_type) == 0) {
                const auto app = selected(editor->action_choice);
                if (app >= 0) SetWindowTextW(editor->action_value, applications[app].code);
            }
            break;
        case apply_step_id: add_step(*editor); break;
        case list_id:
            if (notification != LBN_DBLCLK) break;
            [[fallthrough]];
        case step_edit_id:
            if (index >= 0) {
                editor->edit_index = index;
                editor->action_open = true;
                editor->trigger_open = false;
                trigger_visibility(*editor);
                const auto& step = editor->steps[index];
                select(editor->action_type, step_type(step));
                show_action_fields(*editor, step);
            }
            break;
        case step_remove_id:
            if (index >= 0) {
                editor->steps.erase(editor->steps.begin() + index);
                editor->edit_index = -1;
                fill_steps(*editor);
                show_action_fields(*editor);
            }
            break;
        case step_up_id: case step_down_id: {
            const int next = index + (id == step_up_id ? -1 : 1);
            if (index >= 0 && next >= 0 && static_cast<size_t>(next) < editor->steps.size()) {
                std::swap(editor->steps[index], editor->steps[next]);
                editor->edit_index = -1;
                fill_steps(*editor, next);
                show_action_fields(*editor);
            }
            break;
        }
        case browse_id: browse_application(*editor); break;
        case record_id: toggle_recording(*editor); break;
        case preview_id: preview(*editor); break;
        case save_id: save(*editor); break;
        }
        return 0;
    }
    return DefWindowProcW(window, message, parameter, payload);
}

} // namespace

void show_smart_actions(HWND owner, const std::filesystem::path& directory) {
    const auto bytes = read_file(directory / L"mappings.ini");
    if (!bytes) {
        MessageBoxW(owner, L"无法读取 mappings.ini。", L"Smart Actions", MB_OK | MB_ICONERROR);
        return;
    }
    Editor editor;
    editor.directory = directory;
    editor.initial = *bytes;
    editor.config = Config(wide(*bytes));
    editor.entries = records(editor.config);
    editor.profiles.emplace_back(L"global");
    bool ring = false;
    for (const auto& line : editor.config.lines) {
        if (const auto section = Config::section(line); !section.empty()) {
            ring = same(section, L"ring");
            if (section.size() >= 4 && same(section.substr(section.size() - 4), L".exe") &&
                std::ranges::none_of(editor.profiles, [&](const auto& known) { return same(known, section); }))
                editor.profiles.emplace_back(section);
        } else if (const auto key = Config::key(line); ring && !key.empty()) {
            const auto value = trim(std::wstring_view(line).substr(line.find(L'=') + 1));
            if (!value.starts_with(L"Folder(")) editor.ring_items.emplace_back(key);
        }
    }
    RECT parent{};
    if (!GetWindowRect(owner, &parent)) return;
    const int width = parent.right - parent.left, height = parent.bottom - parent.top;
    if (width <= 0 || height <= 0) return;
    editor.window_width = width;
    editor.font = CreateFontW(-px(editor, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    editor.heading = CreateFontW(-px(editor, 21), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    editor.title = CreateFontW(-px(editor, 36), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    editor.backdrop = CreateSolidBrush(background);
    editor.fill = CreateSolidBrush(surface);
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"LogiNativeSmartActions";
    RegisterClassW(&type);
    constexpr DWORD style = WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN;
    const auto window = CreateWindowExW(0, type.lpszClassName, L"Smart Actions", style,
        parent.left, parent.top, width, height, owner, nullptr, instance, &editor);
    if (window) {
        EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOW);
        MSG msg{};
        while (IsWindow(window)) {
            const auto event = GetMessageW(&msg, nullptr, 0, 0);
            if (event <= 0) {
                if (event == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (!IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (IsWindow(window)) DestroyWindow(window);
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    DeleteObject(editor.font);
    DeleteObject(editor.heading);
    DeleteObject(editor.title);
    DeleteObject(editor.backdrop);
    DeleteObject(editor.fill);
}
