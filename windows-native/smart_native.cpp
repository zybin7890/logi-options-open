#include "smart_native.hpp"
#include "remap.hpp"
#include "native_icons.hpp"
#include "native_typography.hpp"
#include "smart_steps_layout.hpp"
#include "smart_system_macros.hpp"
#include "smart_templates.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <commctrl.h>
#include <commdlg.h>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <uxtheme.h>
#include <vector>

namespace {

constexpr std::wstring_view marker = L"# Smart Action ";
constexpr int canvas_width = 1494;
constexpr COLORREF background = RGB(25, 25, 25);
constexpr COLORREF surface = RGB(0, 0, 0);
constexpr COLORREF foreground = RGB(251, 251, 251);
constexpr COLORREF accent = RGB(0, 234, 208);

struct Choice {
    const wchar_t* name;
    const wchar_t* code;
};

constexpr Choice mouse_buttons[] = {
    {L"中键", L"0052"}, {L"模式切换按钮", L"00C4"}, {L"前进按钮", L"0056"},
    {L"后退按钮", L"0053"}, {L"手势按钮", L"00C3"},
};
constexpr Choice system_actions[] = {
    {L"播放/暂停", L"PlayPause"}, {L"操作中心", L"Win+A"}, {L"撤销", L"Ctrl+Z"}, {L"此 PC", L"ThisPC"},
    {L"打开网页", L"@OpenWebPage"}, {L"打开文件", L"@OpenFile"},
    {L"打开文件夹", L"@OpenFolder"}, {L"打开应用程序", L"@OpenApplication"},
    {L"打印", L"Ctrl+P"}, {L"复制", L"Ctrl+C"}, {L"关闭窗口", L"CloseWindow"},
    {L"关闭浏览器标签页", L"Ctrl+W"}, {L"关机", L"Shutdown"}, {L"后退", L"Alt+Left"},
    {L"计算器", L"Open(calc.exe)"}, {L"剪切", L"Ctrl+X"}, {L"截屏", L"ScreenCapture(Screen,File)"},
    {L"亮度降低", L"BrightnessDown"}, {L"亮度升高", L"BrightnessUp"},
    {L"麦克风静音/取消静音（系统）", L"MicrophoneMute"}, {L"屏幕截图", L"Win+Shift+S"},
    {L"前进", L"Alt+Right"}, {L"全屏截图", L"PrintScreen"}, {L"任务视图", L"Win+Tab"},
    {L"上一个", L"PreviousTrack"}, {L"设置", L"Win+I"}, {L"输入语言", L"Win+Space"},
    {L"刷新", L"F5"}, {L"睡眠", L"Sleep"}, {L"搜索", L"Win+S"}, {L"锁定", L"Lock"},
    {L"听写", L"Win+H"}, {L"下一个", L"NextTrack"}, {L"显示/隐藏桌面", L"Win+D"},
    {L"新建浏览器标签页", L"Ctrl+T"}, {L"扬声器静音/取消静音", L"Mute"},
    {L"音量降低", L"VolumeDown"}, {L"音量升高", L"VolumeUp"}, {L"应用程式菜单", L"Apps"},
    {L"右 Ctrl", L"RightCtrl"}, {L"粘贴", L"Ctrl+V"}, {L"重做", L"Ctrl+Y"},
    {L"主页（浏览器）", L"BrowserHome"}, {L"桌面向右", L"Win+Ctrl+Right"}, {L"桌面向左", L"Win+Ctrl+Left"},
    {L"最大化窗口", L"Maximize"}, {L"最小化窗口", L"Minimize"},
    {L"Delete", L"Delete"}, {L"End", L"End"}, {L"Home", L"Home"}, {L"Insert", L"Insert"},
    {L"Page down", L"PageDown"}, {L"Page up", L"PageUp"},
    {L"Windows 高对比度", L"HighContrast"}, {L"Windows Copilot", L"Win+Shift+F23"},
};
constexpr Choice legacy_system_actions[]{
    {L"开始菜单", L"Win"}, {L"文件资源管理器", L"Win+E"}, {L"切换应用", L"Alt+Tab"},
    {L"保存全屏截图", L"Win+PrintScreen"}, {L"汉字转换（韩文）", L"Hanja"},
    {L"电子邮件", L"Mail"}, {L"媒体播放器", L"Media"}, {L"停止播放", L"MediaStop"},
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
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    const auto size = file.tellg();
    if (size <= 0) return std::string{};
    std::string result(static_cast<std::size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    file.read(result.data(), size);
    return result;
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
    bool enabled{true};
    std::wstring source_document{};
    std::wstring description{};
    unsigned usage_count{};
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
        entry.enabled = field(json, L"enabled") != L"false";
        entry.source_document = json_string(field(json, L"sourceDocument")).value_or(L"");
        entry.description = json_string(field(json, L"description")).value_or(
            json_string(field(entry.source_document, L"description")).value_or(L""));
        auto usage_sv = field(json, L"usageCount");
        if (usage_sv.empty()) usage_sv = field(entry.source_document, L"usageCount");
        for (const wchar_t ch : usage_sv) {
            if (ch < L'0' || ch > L'9') { entry.usage_count = 0; break; }
            entry.usage_count = entry.usage_count * 10u + static_cast<unsigned>(ch - L'0');
        }
        if ((!entry.enabled && !entry.sequence.empty()) ||
            (!entry.trigger.key.empty() && config.get(entry.trigger) == entry.sequence)) result.push_back(std::move(entry));
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
        if (field(item, L"type") != L"\"keys\"" && !field(item, L"value").starts_with(L'[')) {
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

std::wstring decoded_path(std::wstring_view hex) {
    if (hex.size() % 2) return {};
    std::string bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1; };
        const int high = digit(hex[i]), low = digit(hex[i + 1]);
        if (high < 0 || low < 0) return {};
        bytes += static_cast<char>(high * 16 + low);
    }
    return wide(bytes);
}

std::wstring path_hex(std::wstring_view path) {
    std::wstring result;
    constexpr wchar_t hex[]{L"0123456789abcdef"};
    for (const unsigned char byte : utf8(path)) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

std::wstring open_action(std::wstring_view path) {
    if (path.empty() || path.find_first_of(L"\r\n") != path.npos || path.find(L'\0') != path.npos) return {};
    int depth = 0;
    for (const auto ch : path) {
        if (ch == L'(') ++depth;
        if (ch == L')' && --depth < 0) return {};
    }
    if (depth) return {};
    const auto result = L"Open(" + std::wstring(path) + L")";
    return split_steps(result).size() == 1 ? result : L"";
}

struct ApplicationIdentity { const wchar_t* id; const wchar_t* name; const wchar_t* executable; const wchar_t* location; };
constexpr ApplicationIdentity application_identities[]{
    {L"application_id_google_chrome", L"Google Chrome", L"chrome.exe", L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe"},
    {L"application_id_microsoft_edge_chromium", L"Microsoft Edge", L"msedge.exe", L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe"},
    {L"application_id_vscode", L"Visual Studio Code", L"Code.exe", L"%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe"},
    {L"application_id_wps", L"WPS Office", L"ksolaunch.exe", L"%LOCALAPPDATA%\\Kingsoft\\WPS Office\\ksolaunch.exe"},
    {L"application_id_firefox", L"Mozilla Firefox", L"firefox.exe", L"%ProgramFiles%\\Mozilla Firefox\\firefox.exe"},
    {L"application_id_zoom", L"Zoom", L"Zoom.exe", L"%APPDATA%\\Zoom\\bin\\Zoom.exe"},
    {L"application_id_spotifymusic", L"Spotify", L"Spotify.exe", L"%APPDATA%\\Spotify\\Spotify.exe"},
    {L"application_id_microsoft_excel", L"Microsoft Excel", L"EXCEL.EXE", L""},
    {L"application_id_microsoft_word", L"Microsoft Word", L"WINWORD.EXE", L""},
    {L"application_id_microsoft_powerpoint", L"Microsoft PowerPoint", L"POWERPNT.EXE", L""},
    {L"application_id_microsoft_teams", L"Microsoft Teams", L"ms-teams.exe", L"%LOCALAPPDATA%\\Microsoft\\Teams\\current\\Teams.exe"},
    {L"application_id_we_chat", L"WeChat", L"WeChat.exe", L"%ProgramFiles%\\Tencent\\WeChat\\WeChat.exe"},
};
constexpr std::wstring_view desktop_application_id = L"420fd454-0c36-499d-bde4-146823b16147";

std::wstring expanded_path(std::wstring_view value) {
    std::wstring result(value);
    if (result.size() >= 2 && result.front() == L'"' && result.back() == L'"') result = result.substr(1, result.size() - 2);
    if (result.find(L'%') == result.npos) return result;
    const auto count = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
    if (!count) return {};
    std::wstring expanded(count, L'\0');
    if (!ExpandEnvironmentStringsW(result.c_str(), expanded.data(), count)) return {};
    expanded.pop_back();
    return expanded;
}

bool existing_application(std::wstring_view path) {
    std::error_code error;
    return std::filesystem::path(path).is_absolute() && same(std::filesystem::path(path).extension().wstring(), L".exe") &&
        std::filesystem::is_regular_file(path, error);
}

std::wstring registered_application(std::wstring_view executable) {
    for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
            wchar_t path[32768]{};
            DWORD bytes = sizeof(path);
            const auto key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + std::wstring(executable);
            if (RegGetValueW(hive, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view,
                            nullptr, path, &bytes) == ERROR_SUCCESS) {
                const auto expanded = expanded_path(path);
                if (existing_application(expanded)) return expanded;
            }
        }
    }
    return {};
}

std::wstring resolve_application_id(std::wstring_view id) {
    struct Fallback { const wchar_t* id; const wchar_t* path; };
    static constexpr Fallback fallbacks[]{
        {L"application_id_google_chrome", L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe"},
        {L"application_id_google_chrome", L"%LOCALAPPDATA%\\Google\\Chrome\\Application\\chrome.exe"},
        {L"application_id_microsoft_edge_chromium", L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe"},
        {L"application_id_vscode", L"%ProgramFiles%\\Microsoft VS Code\\Code.exe"},
        {L"application_id_wps", L"%ProgramFiles(x86)%\\Kingsoft\\WPS Office\\ksolaunch.exe"},
        {L"application_id_firefox", L"%ProgramFiles(x86)%\\Mozilla Firefox\\firefox.exe"},
        {L"application_id_zoom", L"%ProgramFiles%\\Zoom\\bin\\Zoom.exe"},
        {L"application_id_zoom", L"%ProgramFiles(x86)%\\Zoom\\bin\\Zoom.exe"},
        {L"application_id_microsoft_teams", L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\ms-teams.exe"},
        {L"application_id_spotifymusic", L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\Spotify.exe"},
        {L"application_id_we_chat", L"%ProgramFiles(x86)%\\Tencent\\WeChat\\WeChat.exe"},
        {L"application_id_we_chat", L"%ProgramFiles%\\Tencent\\Weixin\\Weixin.exe"},
    };
    for (const auto& item : application_identities) if (item.id == id) {
        if (const auto path = registered_application(item.executable); !path.empty()) return path;
        if (const auto path = expanded_path(item.location); existing_application(path)) return path;
        for (const auto& fb : fallbacks) if (fb.id == id) {
            if (const auto path = expanded_path(fb.path); existing_application(path)) return path;
        }
        wchar_t found[MAX_PATH]{};
        if (item.executable && SearchPathW(nullptr, item.executable, nullptr, std::size(found), found, nullptr))
            return found;
    }
    return {};
}

std::wstring application_identity_name(std::wstring_view id) {
    for (const auto& item : application_identities) if (item.id == id) return item.name;
    return L"应用程序";
}

const Choice* system_choice(std::wstring_view step) {
    // Previously saved explicit shortcuts remain executable unchanged. Their
    // labels still resolve after the menu adopts the original native operation.
    if (step == L"Alt+F4") step = L"CloseWindow";
    else if (step == L"Open(shell:MyComputerFolder)") step = L"ThisPC";
    else if (step == L"Alt+Home") step = L"BrowserHome";
    else if (step == L"Shift+F10") step = L"Apps";
    for (const auto& item : system_actions) if (step == item.code) return &item;
    for (const auto& item : legacy_system_actions) if (step == item.code) return &item;
    if (step.starts_with(L"Open(") && split_steps(step).size() == 1) {
        const auto target = inner(step);
        std::error_code error;
        const auto kind = UrlIsW(target.c_str(), URLIS_URL) ? L"@OpenWebPage" :
            std::filesystem::is_directory(target, error) ? L"@OpenFolder" :
            same(std::filesystem::path(target).extension().wstring(), L".exe") ? L"@OpenApplication" : L"@OpenFile";
        for (const auto& item : system_actions) if (item.code == std::wstring_view(kind)) return &item;
    }
    return nullptr;
}

int step_type(std::wstring_view step) {
    if (system_choice(step)) return 3;
    if (step.starts_with(L"Open(") || step.starts_with(L"AppForeground(") || step.starts_with(L"AppClose(") ||
        step.starts_with(L"AppMaximize(") || step.starts_with(L"AppMinimize(")) return 0;
    if (step.starts_with(L"Text(") || step.starts_with(L"TextHex(")) return 2;
    if (step.starts_with(L"Wait(") && split_steps(step).size() == 1) return 4;
    return 1;
}

bool template_pending(std::wstring_view step) { return step.starts_with(L"TemplatePending("); }

bool complete_steps(const std::vector<std::wstring>& steps) {
    return !steps.empty() && std::ranges::none_of(steps, template_pending);
}

std::wstring step_label(std::wstring_view step) {
    if (template_pending(step)) {
        const auto separator = step.find(L'|');
        return L"请选择 · " + std::wstring(separator == step.npos ? L"应用程序或文件" : step.substr(separator + 1, step.size() - separator - 2));
    }
    switch (step_type(step)) {
    case 0: {
        const auto parts = split_steps(step);
        const auto path = parts.front().starts_with(L"Open(") ? inner(parts.front()) : decoded_path(inner(parts.front()));
        return L"应用程序 · " + std::filesystem::path(path).filename().wstring();
    }
    case 2: {
        auto value = decoded_text(step);
        if (auto at = value.find_first_of(L"\r\n"); at != value.npos) value.erase(at);
        if (value.size() > 36) value = value.substr(0, 36) + L"…";
        return L"文本 · " + value;
    }
    case 3:
        if (const auto item = system_choice(step)) return L"系统 · " + std::wstring(item->name);
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
        case 2: json += quote(decoded_text(step)); break;
        case 3: json += quote(step); break;
        case 4: json += quote(std::to_wstring(std::wcstoul(inner(step).c_str(), nullptr, 10) / 1000.0)); break;
        case 0: case 1: {
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

std::wstring entry_metadata(const Entry& entry, const std::vector<std::wstring>& steps) {
    return std::wstring(marker) + L"{\"name\":" + quote(entry.name) + L",\"tag\":" + quote(entry.tag) +
        L",\"description\":" + quote(entry.description) +
        L",\"trigger\":{\"section\":" + quote(entry.trigger.section) + L",\"key\":" + quote(entry.trigger.key) +
        L"},\"steps\":" + steps_metadata(steps) + L",\"sequence\":" + quote(entry.sequence) +
        L",\"previous\":" + (entry.previous ? quote(*entry.previous) : L"null") +
        L",\"enabled\":" + (entry.enabled ? L"true" : L"false") +
        L",\"usageCount\":" + std::to_wstring(entry.usage_count) +
        (entry.source_document.empty() ? L"" : L",\"sourceDocument\":" + quote(entry.source_document)) + L"}";
}

std::wstring text_step(std::wstring_view value) {
    std::wstring step = L"TextHex(";
    for (auto ch : value) {
        wchar_t code[5]{};
        std::swprintf(code, std::size(code), L"%04x", static_cast<unsigned>(ch));
        step += code;
    }
    return step + L")";
}

std::optional<unsigned> unsigned_value(std::wstring_view value) {
    if (value.empty() || value.size() > 9) return {};
    unsigned result = 0;
    for (auto ch : value) {
        if (ch < L'0' || ch > L'9') return {};
        result = result * 10 + ch - L'0';
    }
    return result;
}

std::wstring hid_key(unsigned code) {
    if (code >= 4 && code <= 29) return std::wstring(1, static_cast<wchar_t>(L'A' + code - 4));
    if (code >= 30 && code <= 38) return std::wstring(1, static_cast<wchar_t>(L'1' + code - 30));
    if (code >= 58 && code <= 69) return L"F" + std::to_wstring(code - 57);
    if (code >= 104 && code <= 115) return L"F" + std::to_wstring(code - 91);
    switch (code) {
    case 39: return L"0"; case 40: return L"Enter"; case 41: return L"Esc";
    case 42: return L"Backspace"; case 43: return L"Tab"; case 44: return L"Space";
    case 70: return L"PrintScreen"; case 74: return L"Home"; case 75: return L"PageUp";
    case 76: return L"Delete"; case 77: return L"End"; case 78: return L"PageDown";
    case 79: return L"Right"; case 80: return L"Left"; case 81: return L"Down"; case 82: return L"Up";
    case 224: return L"LeftCtrl"; case 225: return L"LeftShift"; case 226: return L"LeftAlt"; case 227: return L"LeftWin";
    case 228: return L"RightCtrl"; case 229: return L"RightShift"; case 230: return L"RightAlt"; case 231: return L"RightWin";
    case 45: return L"189"; case 46: return L"187"; case 47: return L"219"; case 48: return L"221";
    case 49: return L"220"; case 51: return L"186"; case 52: return L"222"; case 53: return L"192";
    case 54: return L"188"; case 55: return L"190"; case 56: return L"191"; case 57: return L"CapsLock";
    case 71: return L"145"; case 72: return L"19"; case 73: return L"Insert";
    case 83: return L"NumLock"; case 84: return L"111"; case 85: return L"106";
    case 86: return L"109"; case 87: return L"107";
    case 89: return L"97"; case 90: return L"98"; case 91: return L"99"; case 92: return L"100";
    case 93: return L"101"; case 94: return L"102"; case 95: return L"103"; case 96: return L"104";
    case 97: return L"105"; case 98: return L"96"; case 99: return L"110"; case 101: return L"93";
    default: return {};
    }
}

std::optional<unsigned> key_hid(std::wstring_view key) {
    if (key == L"Ctrl") return 224;
    if (key == L"Shift") return 225;
    if (key == L"Alt") return 226;
    if (key == L"Win") return 227;
    for (unsigned code = 4; code <= 231; ++code)
        if (const auto name = hid_key(code); !name.empty() && name == key) return code;
    return {};
}

std::optional<std::wstring> import_system_macro(std::wstring_view macro) {
    const auto type = json_string(field(macro, L"type")).value_or(L"");
    for (const auto& entry : smart_system::macros) {
        if (entry.type != type || json_string(field(field(macro, entry.object), entry.member)).value_or(L"") != entry.value) continue;
        if (entry.code == L"ScrollLeft" || entry.code == L"ScrollRight") {
            const auto speed = field(field(macro, entry.object), L"current_tilt_speed");
            if (!speed.empty() && speed != L"0") return {};
        }
        return std::wstring(entry.code);
    }
    if (type == L"MOUSE") {
        const auto mouse = field(macro, L"mouse");
        if (json_string(field(mouse, L"action")).value_or(L"") == L"BUTTON" && unsigned_value(field(mouse, L"hidUsage")) == 3)
            return L"MouseMiddle";
    }
    if (type == L"APP") {
        const auto app = field(macro, L"app");
        const auto arguments = array_items(field(app, L"argumentList"));
        if (same(json_string(field(app, L"executablePath")).value_or(L""), L"explorer.exe") && arguments.size() == 1 &&
            same(json_string(arguments.front()).value_or(L""), L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")) return L"ThisPC";
    }
    if (type == L"SCREEN_CAPTURE") {
        const auto capture = field(macro, L"screenCapture");
        const auto area = json_string(field(capture, L"areatocapture")).value_or(L"");
        const auto destination = json_string(field(capture, L"destinationfolder")).value_or(L"");
        if ((area == L"CAPTURE_SCREEN" || area == L"CAPTURE_WINDOW") &&
            (destination == L"CAPTURE_FILE" || destination == L"CAPTURE_CLIPBOARD"))
            return std::wstring(L"ScreenCapture(") + (area == L"CAPTURE_SCREEN" ? L"Screen," : L"Window,") +
                (destination == L"CAPTURE_FILE" ? L"File)" : L"Clipboard)");
    }
    return {};
}

std::optional<std::wstring> export_system_macro(std::wstring_view code) {
    for (const auto& entry : smart_system::macros) {
        if (entry.code == code)
            return L"{\"type\":" + quote(entry.type) + L"," + quote(entry.object) + L":{" +
                   quote(entry.member) + L":" + quote(entry.value) + L"}}";
    }
    if (code == L"MouseMiddle") return L"{\"type\":\"MOUSE\",\"mouse\":{\"action\":\"BUTTON\",\"hidUsage\":3}}";
    if (code == L"ThisPC") return LR"({"type":"APP","app":{"executablePath":"explorer.exe","argumentList":["::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"]}})";
    for (const auto area : {L"Screen", L"Window"})
        for (const auto destination : {L"File", L"Clipboard"})
            if (code == std::wstring(L"ScreenCapture(") + area + L"," + destination + L")")
                return L"{\"type\":\"SCREEN_CAPTURE\",\"screenCapture\":{\"areatocapture\":" +
                    quote(std::wstring_view(area) == L"Screen" ? L"CAPTURE_SCREEN" : L"CAPTURE_WINDOW") +
                    L",\"destinationfolder\":" + quote(std::wstring_view(destination) == L"File" ? L"CAPTURE_FILE" : L"CAPTURE_CLIPBOARD") + L"}}";
    return {};
}

std::optional<std::vector<std::wstring>> import_cards(std::wstring_view document, std::wstring& error) {
    std::vector<std::wstring> steps;
    if (!field(document, L"version").empty() || !field(document, L"schemaVersion").empty()) {
        error = L"此文件声明了尚未支持的格式版本，未导入。"; return {};
    }
    for (const auto card : array_items(field(document, L"cards"))) {
        if (json_string(field(card, L"attribute")).value_or(L"") != L"MACRO_PLAYBACK") {
            error = L"此文件包含未知卡片类型，未导入。"; return {};
        }
        const auto macro = field(card, L"macro");
        const auto type = json_string(field(macro, L"type")).value_or(L"");
        const auto application_id = json_string(field(card, L"applicationId")).value_or(L"");
        const auto application_path = application_id.empty() || application_id == desktop_application_id ? L"" : resolve_application_id(application_id);
        if (!application_id.empty() && application_id != desktop_application_id && application_path.empty()) {
            error = L"无法定位此宏指定的应用程序：" + application_id + L"。未导入，未作用于当前窗口。"; return {};
        }
        if (const auto system = import_system_macro(macro)) {
            steps.push_back(*system);
        } else if (type == L"DELAY") {
            const auto delay = unsigned_value(field(field(macro, L"delay"), L"durationMs"));
            if (!delay || *delay > 30000) { error = L"延迟必须在 0 到 30 秒之间。"; return {}; }
            steps.push_back(L"Wait(" + std::to_wstring(*delay) + L")");
        } else if (type == L"TEXT_BLOCK") {
            const auto text = json_string(field(field(macro, L"textBlock"), L"text"));
            if (!text || text->empty() || text->size() > 1000 || text->find(L'\0') != text->npos) {
                error = L"文本操作必须包含 1 到 1000 个字符。"; return {};
            }
            steps.push_back(text_step(*text));
        } else if (type == L"KEYSTROKE") {
            const auto stroke = field(macro, L"keystroke");
            const auto code = unsigned_value(field(stroke, L"code"));
            std::wstring chord;
            for (const auto modifier : array_items(field(stroke, L"modifiers"))) {
                const auto key = unsigned_value(modifier);
                if (!key || *key < 224 || *key > 231) { error = L"此宏使用了当前尚不支持的修饰键。"; return {}; }
                chord += hid_key(*key) + L"+";
            }
            if ((!code && field(stroke, L"code").empty()) || code == 0) {
                if (chord.empty()) { error = L"此击键尚未设置按键，未导入。"; return {}; }
                chord.pop_back();
                steps.push_back(std::move(chord));
            } else {
                if (!code || hid_key(*code).empty()) { error = L"此宏包含尚不支持的键码。"; return {}; }
                steps.push_back(chord + hid_key(*code));
            }
        } else if (type == L"APP_WINDOWS_MANAGEMENT") {
            const auto app = field(macro, L"appWindowsManagement");
            const auto action = json_string(field(app, L"action")).value_or(L"");
            const auto path = json_string(field(app, L"path")).value_or(application_path);
            const auto target = path.empty() ? application_path : path;
            if (!std::filesystem::path(target).is_absolute()) { error = L"此应用窗口操作缺少可用的绝对路径。"; return {}; }
            const auto code = action == L"CLOSE" ? L"AppClose" : action == L"BRING_TO_FOREGROUND" ? L"AppForeground" :
                action == L"MAXIMIZE" ? L"AppMaximize" : action == L"MINIMIZE" ? L"AppMinimize" : L"";
            if (!*code) { error = L"未知应用窗口操作：" + action; return {}; }
            steps.push_back(std::wstring(code) + L"(" + path_hex(target) + L")");
        } else if (type == L"OPEN_WEB_PAGE" || type == L"OPEN_FILE_FOLDER") {
            const auto object = field(macro, type == L"OPEN_WEB_PAGE" ? L"open_web_page" : L"open_file_folder");
            const auto path = json_string(field(object, type == L"OPEN_WEB_PAGE" ? L"url" : L"path"));
            if (!path || path->empty()) { error = L"此应用操作没有可用的文件路径。"; return {}; }
            auto action = open_action(*path);
            if (action.empty()) { error = L"此路径含有无法无损表示的动作分隔符，未导入。"; return {}; }
            steps.push_back(std::move(action));
        } else if (type == L"INPUT_SEQUENCE") {
            std::vector<unsigned> held;
            std::vector<std::wstring> sequence;
            const auto input = field(macro, L"inputSequence");
            // The original recorder materializes each delay and forces the first
            // component to 21 ms. Its defaultDelay/useDefaultDelay are edit options;
            // sourceDocument retains them without rewriting the recorded timings.
            for (const auto list : array_items(field(input, L"componentLists"))) {
                for (const auto component : array_items(field(list, L"components"))) {
                    if (const auto delay_json = field(component, L"delay"); !delay_json.empty()) {
                        const auto delay = unsigned_value(field(delay_json, L"durationMs"));
                        if (!delay || *delay > 30000) { error = L"击键延迟必须在 0 到 30 秒之间。"; return {}; }
                        sequence.push_back(L"Wait(" + std::to_wstring(*delay) + L")");
                        continue;
                    }
                    const auto keyboard = field(component, L"keyboard");
                    const auto code = unsigned_value(field(keyboard, L"hidUsage"));
                    if (!code || hid_key(*code).empty()) { error = L"此输入序列包含尚不支持的事件。"; return {}; }
                    const bool down = field(keyboard, L"isDown") == L"true";
                    const bool exists = std::ranges::find(held, *code) != held.end();
                    if (down == exists) { error = L"输入序列的按下与松开事件不匹配。"; return {}; }
                    if (down) held.push_back(*code); else std::erase(held, *code);
                    sequence.push_back(std::wstring(down ? L"KeyDown(" : L"KeyUp(") + hid_key(*code) + L")");
                }
            }
            if (!held.empty() || sequence.empty()) { error = L"击键序列不完整。"; return {}; }
            steps.push_back(sequence_text(sequence));
        } else {
            error = L"此宏包含尚未实现的操作：" + (type.empty() ? L"未知类型" : type) + L"。没有导入任何内容。";
            return {};
        }
        if (!application_path.empty() && type != L"APP_WINDOWS_MANAGEMENT" && type != L"OPEN_FILE_FOLDER" && type != L"OPEN_WEB_PAGE")
            steps.back() = L"AppForeground(" + path_hex(application_path) + L");" + steps.back();
    }
    if (steps.empty() || split_steps(sequence_text(steps)).size() > 1024) {
        error = L"宏必须包含 1 到 1024 个操作，按下、抬起与间隔均计入。"; return {};
    }
    return steps;
}

enum class TemplateTarget { none, application, file_or_folder, unsupported };
struct TemplateCard {
    std::wstring card, group, label, error, step;
    TemplateTarget target{TemplateTarget::none};
};

std::wstring suggested_template_application(std::wstring_view name) {
    struct Hint { const wchar_t* name; const wchar_t* executable; const wchar_t* path; };
    constexpr Hint hints[]{
        {L"qqmusic", L"QQMusic.exe", L""},
        {L"Microsoft OneDrive", L"OneDrive.exe", L"%LOCALAPPDATA%\\Microsoft\\OneDrive\\OneDrive.exe"},
        {L"Microsoft Outlook", L"OUTLOOK.EXE", L""},
        {L"Slack", L"slack.exe", L"%LOCALAPPDATA%\\slack\\slack.exe"},
    };
    for (const auto& item : hints) if (same(item.name, name)) {
        if (const auto path = registered_application(item.executable); !path.empty()) return path;
        const auto path = expanded_path(item.path);
        if (existing_application(path)) return path;
    }
    return {};
}

TemplateCard prepare_template_card(std::wstring_view card, std::wstring_view selected_path = {}) {
    TemplateCard result; result.card = card;
    const auto macro = field(card, L"macro");
    const auto type = json_string(field(macro, L"type")).value_or(L"");
    const auto id = json_string(field(card, L"applicationId")).value_or(L"");
    const bool desktop = id.empty() || id == desktop_application_id;
    const auto object = field(macro, type == L"OPEN_FILE_FOLDER" ? L"open_file_folder" : L"appWindowsManagement");
    const auto suggested_name = json_string(field(object, L"appName")).value_or(L"");
    result.group = id == L"DYNAMIC_APPLICATION_ID_PLACEHOLDER" ? L"dynamic:" + suggested_name : id;
    result.label = suggested_name.empty() ? application_identity_name(id) : suggested_name;
    std::wstring application_path;
    if (!desktop) application_path = selected_path.empty() ? resolve_application_id(id) : std::wstring(selected_path);
    std::wstring normalized_macro(macro);
    if (type == L"APP_WINDOWS_MANAGEMENT" || type == L"OPEN_FILE_FOLDER") {
        auto path = selected_path.empty() ? expanded_path(json_string(field(object, L"path")).value_or(L"")) : std::wstring(selected_path);
        if (path.empty()) path = application_path;
        if (path.empty() && !suggested_name.empty()) path = suggested_template_application(suggested_name);
        const bool application = type == L"APP_WINDOWS_MANAGEMENT" || !suggested_name.empty() || !desktop;
        std::error_code error;
        const bool valid = application ? existing_application(path) : std::filesystem::exists(path, error);
        if (!valid) {
            result.target = application ? TemplateTarget::application : TemplateTarget::file_or_folder;
            if (!application) result.label = L"文件或文件夹";
            return result;
        }
        if (type == L"APP_WINDOWS_MANAGEMENT") {
            const auto action = json_string(field(object, L"action")).value_or(L"");
            normalized_macro = L"{\"type\":\"APP_WINDOWS_MANAGEMENT\",\"appWindowsManagement\":{\"path\":" + quote(path) + L",\"action\":" + quote(action) + L"}}";
        } else normalized_macro = L"{\"type\":\"OPEN_FILE_FOLDER\",\"open_file_folder\":{\"path\":" + quote(path) + L"}}";
    } else if (!desktop && !existing_application(application_path)) {
        result.target = TemplateTarget::application;
        return result;
    }
    const auto document = L"{\"cards\":[{\"attribute\":\"MACRO_PLAYBACK\",\"macro\":" + normalized_macro + L"}]}";
    const auto steps = import_cards(document, result.error);
    if (!steps || steps->size() != 1) { result.target = TemplateTarget::unsupported; return result; }
    result.step = steps->front();
    if (!desktop && type != L"APP_WINDOWS_MANAGEMENT" && type != L"OPEN_FILE_FOLDER" && type != L"OPEN_WEB_PAGE")
        result.step = L"AppForeground(" + path_hex(application_path) + L");" + result.step;
    return result;
}

std::wstring template_placeholder(size_t index, const TemplateCard& card) {
    return L"TemplatePending(" + std::to_wstring(index) + L"|" + card.label + L")";
}

bool resolve_template_target(std::vector<TemplateCard>& cards, std::vector<std::wstring>& steps,
                             size_t index, std::wstring_view path, std::wstring& error) {
    if (index >= cards.size()) return false;
    auto replacement_cards = cards;
    auto replacement_steps = steps;
    const auto group = cards[index].group;
    bool changed = false;
    for (size_t i = 0; i < cards.size(); ++i) {
        if (i != index && (group.empty() || cards[i].group != group)) continue;
        const auto pending = template_placeholder(i, cards[i]);
        const auto row = std::ranges::find(replacement_steps, pending);
        if (row == replacement_steps.end()) continue;
        auto resolved = prepare_template_card(cards[i].card, path);
        if (resolved.target != TemplateTarget::none || resolved.step.empty()) {
            error = resolved.error.empty() ? L"请选择存在且有效的应用程序、文件或文件夹。" : resolved.error;
            return false;
        }
        *row = resolved.step;
        replacement_cards[i] = std::move(resolved);
        changed = true;
    }
    if (!changed) return false;
    cards = std::move(replacement_cards);
    steps = std::move(replacement_steps);
    return true;
}

std::optional<std::wstring> export_cards(const Entry& entry) {
    std::wstring cards = L"[";
    std::wstring components;
    std::vector<unsigned> held;
    const auto append_card = [&](const std::wstring& macro) {
        if (cards.size() > 1) cards += L',';
        cards += L"{\"attribute\":\"MACRO_PLAYBACK\",\"macro\":" + macro + L"}";
    };
    const auto flush_sequence = [&] {
        if (components.empty()) return;
        append_card(L"{\"type\":\"INPUT_SEQUENCE\",\"inputSequence\":{\"componentLists\":[{\"components\":[" + components +
                    L"]}],\"defaultDelay\":21,\"useDefaultDelay\":false}}");
        components.clear();
    };
    for (const auto& step : split_steps(entry.sequence)) {
        std::wstring macro;
        if (step.starts_with(L"KeyDown(") || step.starts_with(L"KeyUp(")) {
            const auto code = key_hid(inner(step));
            if (!code) return {};
            const bool down = step.starts_with(L"KeyDown(");
            const bool exists = std::ranges::find(held, *code) != held.end();
            if (down == exists) return {};
            if (down) held.push_back(*code); else std::erase(held, *code);
            if (!components.empty()) components += L',';
            components += L"{\"keyboard\":{\"hidUsage\":" + std::to_wstring(*code) +
                L",\"displayName\":" + quote(hid_key(*code)) +
                (step.starts_with(L"KeyDown(") ? L",\"isDown\":true}}" : L",\"isDown\":false}}");
            continue;
        }
        if (step.starts_with(L"Wait(") && !components.empty()) {
            components += L",{\"delay\":{\"durationMs\":" + inner(step) + L"}}";
            continue;
        }
        if (!held.empty()) return {};
        flush_sequence();
        if (const auto system = export_system_macro(step))
            macro = *system;
        else if (step.starts_with(L"Text(") || step.starts_with(L"TextHex("))
            macro = L"{\"type\":\"TEXT_BLOCK\",\"textBlock\":{\"text\":" + quote(decoded_text(step)) + L"}}";
        else if (step.starts_with(L"Wait("))
            macro = L"{\"type\":\"DELAY\",\"delay\":{\"durationMs\":" + inner(step) + L"}}";
        else if (step.starts_with(L"AppForeground(") || step.starts_with(L"AppClose(") ||
                 step.starts_with(L"AppMaximize(") || step.starts_with(L"AppMinimize(")) {
            const auto action = step.starts_with(L"AppForeground(") ? L"BRING_TO_FOREGROUND" : step.starts_with(L"AppClose(") ? L"CLOSE" :
                step.starts_with(L"AppMaximize(") ? L"MAXIMIZE" : L"MINIMIZE";
            const auto path = decoded_path(inner(step));
            if (!std::filesystem::path(path).is_absolute()) return {};
            macro = L"{\"type\":\"APP_WINDOWS_MANAGEMENT\",\"appWindowsManagement\":{\"action\":" + quote(action) +
                L",\"path\":" + quote(path) + L",\"appName\":" + quote(std::filesystem::path(path).stem().wstring()) + L"}}";
        } else if (step.starts_with(L"Open(")) {
            const auto target = inner(step);
            macro = UrlIsW(target.c_str(), URLIS_URL)
                ? L"{\"type\":\"OPEN_WEB_PAGE\",\"open_web_page\":{\"url\":" + quote(target) + L"}}"
                : L"{\"type\":\"OPEN_FILE_FOLDER\",\"open_file_folder\":{\"path\":" + quote(target) + L"}}";
        }
        else {
            std::wstring_view remaining = step;
            std::vector<unsigned> codes;
            while (!remaining.empty()) {
                const auto separator = remaining.find(L'+');
                const auto code = key_hid(trim(remaining.substr(0, separator)));
                if (!code) return {};
                codes.push_back(*code);
                if (separator == remaining.npos) break;
                remaining.remove_prefix(separator + 1);
            }
            if (codes.empty()) return {};
            const bool modifier_only = std::ranges::all_of(codes, [](unsigned code) { return code >= 224 && code <= 231; });
            std::wstring modifiers = L"[";
            for (size_t i = 0; i < codes.size() - (modifier_only ? 0 : 1); ++i) {
                if (codes[i] < 224) return {};
                if (i) modifiers += L',';
                modifiers += std::to_wstring(codes[i]);
            }
            macro = L"{\"type\":\"KEYSTROKE\",\"keystroke\":{\"code\":" + std::to_wstring(modifier_only ? 0 : codes.back()) +
                L",\"modifiers\":" + modifiers + L"]}}";
        }
        append_card(macro);
    }
    if (!held.empty()) return {};
    flush_sequence();
    return cards + L']';
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

enum class Detail { none, shortcut, device, delay, keys, system, application, application_action, ring, name, tag };

struct Application {
    std::wstring name, path;
    HICON icon{};
};

struct PreviewResult { std::atomic<DWORD> exit_code{STILL_ACTIVE}; };

constexpr Choice browser_actions[]{
    {L"打开应用程序", L"Open"}, {L"关闭标签页", L"Ctrl+W"}, {L"关闭应用程序", L"AppClose"},
    {L"上一个标签页（左）", L"Ctrl+Shift+Tab"}, {L"刷新页面", L"Ctrl+R"}, {L"下一个标签页（右）", L"Ctrl+Tab"},
    {L"新建浏览器标签页", L"Ctrl+T"}, {L"新建浏览器窗口", L"Ctrl+N"}, {L"置于前台", L"AppForeground"},
    {L"重新打开已关闭的标签页", L"Ctrl+Shift+T"}, {L"最大化应用程序", L"AppMaximize"}, {L"最小化应用程序", L"AppMinimize"},
};
constexpr Choice generic_application_actions[]{
    {L"打开应用程序", L"Open"}, {L"关闭应用程序", L"AppClose"}, {L"置于前台", L"AppForeground"},
    {L"最大化应用程序", L"AppMaximize"}, {L"最小化应用程序", L"AppMinimize"},
};

std::span<const Choice> application_actions(const Application& application) {
    const auto executable = std::filesystem::path(application.path).filename().wstring();
    if (same(executable, L"chrome.exe") || same(executable, L"firefox.exe") || same(executable, L"msedge.exe")) return browser_actions;
    return generic_application_actions;
}

std::wstring application_action(const Application& application, std::wstring_view code) {
    if (code == L"Open") return open_action(application.path);
    const auto path = path_hex(application.path);
    if (code.starts_with(L"App")) return std::wstring(code) + L"(" + path + L")";
    return L"AppForeground(" + path + L");" + std::wstring(code);
}

struct Editor {
    std::filesystem::path directory;
    std::string initial;
    Config config{L""};
    std::vector<Entry> entries;
    std::vector<Entry> template_entries;
    std::vector<TemplateCard> template_cards;
    bool templates_view{};
    int template_target{-1};
    std::vector<size_t> visible_entries;
    std::optional<Entry> original;
    std::wstring description;
    std::vector<std::wstring> steps;
    std::vector<std::wstring> recorded_steps;
    std::vector<std::wstring> profiles;
    std::vector<std::wstring> ring_items;
    Trigger trigger;
    HFONT font{}, heading{}, title{}, card_title{}, input_font{}, step_font{}, step_number_font{};
    HBRUSH backdrop{}, fill{};
    HWND window{}, list{}, filter{}, empty{}, name{}, name_label{}, tag{}, tag_label{}, status{};
    HWND trigger_type{}, trigger_profile{}, trigger_choice{}, trigger_key{}, trigger_summary{};
    HWND action_type{}, action_value{}, action_choice{}, action_title{}, browse{}, record{}, fixed_delay{};
    HWND header_title{}, text_edit{}, text_value{}, text_counter{}, text_notice{};
    std::vector<HWND> text_hidden_controls;
    HWND sort{};
    std::vector<HWND> cards, card_menu;
    std::vector<std::wstring> filter_tags;
    std::vector<HWND> tag_buttons;
    size_t selected_tag{};
    int tag_rows{1};
    int cards_scroll{}, card_menu_entry{-1};
    bool show_education{true};
    Gdiplus::PrivateFontCollection symbols;
    bool symbols_loaded{};
    HWND if_heading{}, then_heading{};
    HWND step_ghost{};
    int editor_scroll{}, editor_scroll_max{}, steps_offset{}, steps_top{}, steps_bottom{};
    int step_hover{-1}, step_press{-1}, step_drag_index{-1};
    bool step_remove_hover{}, step_press_remove{}, step_dragging{};
    ULONGLONG step_pressed_at{};
    POINT step_pick{}, step_pointer{};
    std::vector<std::wstring> step_drag_original;
    std::array<HWND, 5> trigger_categories{}, action_categories{};
    Detail detail{Detail::none};
    std::vector<HWND> detail_hidden, detail_controls;
    HWND detail_value{}, detail_list{}, detail_confirm{}, detail_cancel{}, detail_status{}, detail_fixed{}, detail_delay{}, detail_record{}, detail_advanced{};
    HWND detail_description{}, detail_name_counter{}, detail_description_counter{};
    HWND system_parameter{}, system_browse{};
    int system_parameter_row{-1};
    std::vector<std::wstring> recorded_down;
    std::wstring application_step;
    int detail_choice{-1}, detail_profile{}, detail_hover{-1};
    std::vector<Application> applications;
    std::vector<size_t> detail_items;
    std::vector<HWND> application_tiles;
    int application_scroll{}, selected_application{-1};
    IStream* device_stream{};
    std::unique_ptr<Gdiplus::Image> device_image;
    std::array<IStream*, 3> app_streams{};
    std::array<std::unique_ptr<Gdiplus::Image>, 3> app_images;
    ULONGLONG last_stroke{};
    int dpi{96};
    int window_width{};
    int edit_index{-1};
    bool editing{}, recording{}, trigger_open{}, action_open{}, text_editing{}, closing{};
    unsigned long long hint_revision{}, reload_revision{}, reload_hint_revision{};
    std::wstring reload_message;
    std::optional<SmartReloadRequest> reload_active, reload_pending;
    ReloadResult reload_result;
    std::thread reload_worker;
    HANDLE reload_cancel{};
    std::shared_ptr<PreviewResult> preview_result;
    ~Editor() {
        if (reload_cancel) SetEvent(reload_cancel);
        if (reload_worker.joinable()) reload_worker.join();
        if (reload_cancel) CloseHandle(reload_cancel);
        device_image.reset();
        if (device_stream) device_stream->Release();
        for (size_t i = 0; i < app_images.size(); ++i) {
            app_images[i].reset();
            if (app_streams[i]) app_streams[i]->Release();
        }
        for (const auto& app : applications) if (app.icon) DestroyIcon(app.icon);
    }
};

const std::vector<Entry>& displayed_entries(const Editor& editor) {
    return editor.templates_view ? editor.template_entries : editor.entries;
}

void load_templates(Editor& editor) {
    if (!editor.template_entries.empty()) return;
    for (const auto& definition : smart_templates::definitions) {
        Entry entry;
        entry.name = definition.name; entry.description = definition.description; entry.tag = definition.categories;
        std::vector<std::wstring> steps;
        for (const auto card : array_items(definition.cards)) {
            const auto prepared = prepare_template_card(card);
            steps.push_back(prepared.target == TemplateTarget::none ? prepared.step : template_placeholder(steps.size(), prepared));
        }
        entry.sequence = sequence_text(steps);
        entry.metadata = entry_metadata(entry, steps);
        editor.template_entries.push_back(std::move(entry));
    }
}

enum : WORD {
    close_id = 100, create_id, list_id, edit_id, remove_id, filter_id, back_id,
    trigger_type_id, trigger_profile_id, trigger_choice_id, trigger_key_id, apply_trigger_id,
    action_type_id, action_value_id, action_choice_id, apply_step_id, step_edit_id,
    step_remove_id, step_up_id, step_down_id, browse_id, preview_id, save_id, record_id, fixed_delay_id,
    minimize_id, manage_id, trigger_menu_id, action_menu_id, search_frame_id, add_tag_id, name_id,
    text_edit_id, text_value_id,
    import_id, sort_id, menu_rename_id, menu_tag_id, menu_duplicate_id, menu_export_id, menu_remove_id,
    education_id, education_text_id, education_close_id, all_tags_id,
    detail_confirm_id, detail_cancel_id, detail_value_id, detail_list_id, detail_record_id,
    detail_fixed_id, detail_delay_id, detail_browse_id, detail_status_id, detail_advanced_id,
    detail_description_id,
    system_parameter_id, system_browse_id, templates_id,
};
constexpr WORD trigger_category_id = 200;
constexpr WORD action_category_id = 210;
constexpr WORD device_choice_id = 230;
constexpr WORD device_profile_id = 250;
constexpr WORD tag_filter_id = 400;
constexpr WORD application_tile_id = 1000;
constexpr WORD card_id = 10000;
constexpr UINT append_key = WM_APP + 7;
constexpr UINT reload_finished = WM_APP + 8;
constexpr UINT_PTR preview_timer = 98;

void open_detail(Editor& editor, Detail detail);
void close_detail(Editor& editor);
void record_key(Editor& editor, std::wstring_view chord);
void add_step(Editor& editor);
void save(Editor& editor);
void update_trigger(Editor& editor, bool close);
bool contains_text(std::wstring_view text, std::wstring_view query);
void fill_steps(Editor& editor, int index);
void layout_editor(Editor& editor);

int px(const Editor& editor, int logical) {
    if (editor.window_width) return MulDiv(logical, editor.window_width, canvas_width);
    return MulDiv(logical, editor.dpi, 96);
}

int smart_text(HDC dc, const wchar_t* value, int length, RECT* rect, UINT flags) {
    const auto text = std::wstring_view(value, length < 0 ? std::wcslen(value) : static_cast<size_t>(length));
    const auto font = reinterpret_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
    return native_typography::drawText(dc, font, GetTextColor(dc), text, *rect, flags) ? rect->bottom - rect->top : 0;
}

void smart_card_text(HDC dc, const wchar_t* value, RECT rect, UINT flags, bool active) {
    if (active) { smart_text(dc, value, -1, &rect, flags); return; }
    // Original .macros-card.deactivated text uses opacity:.6. Rasterizing a new
    // gray ClearType glyph is different: its edge contrast is recomputed and
    // looks embossed. Keep the active glyph coverage and composite it instead.
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || !*value) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* storage{};
    const auto memory = CreateCompatibleDC(dc);
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
    if (!memory || !bitmap) { if (memory) DeleteDC(memory); if (bitmap) DeleteObject(bitmap); return; }
    const auto previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, dc, rect.left, rect.top, SRCCOPY);
    GdiFlush();
    auto* pixels = static_cast<DWORD*>(storage);
    const std::vector<DWORD> backdrop(pixels, pixels + static_cast<size_t>(width) * height);
    const auto font = reinterpret_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
    native_typography::drawText(memory, font, GetTextColor(dc), value, {0, 0, width, height}, flags);
    GdiFlush();
    for (size_t i = 0; i < backdrop.size(); ++i) {
        DWORD blended = pixels[i] & 0xff000000u;
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const auto ink = (pixels[i] >> shift) & 0xffu, under = (backdrop[i] >> shift) & 0xffu;
            blended |= ((ink * 3 + under * 2 + 2) / 5) << shift;
        }
        pixels[i] = blended;
    }
    BitBlt(dc, rect.left, rect.top, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory);
}

LRESULT CALLBACK static_text(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                             UINT_PTR id, DWORD_PTR) {
    if (message == WM_SETTEXT) {
        const auto result = DefSubclassProc(window, message, parameter, payload);
        // STATIC can paint synchronously inside WM_SETTEXT. Finish every text
        // update through our background clear and DirectWrite paint path.
        if (IsWindowVisible(window))
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return result;
    }
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(parameter);
        RECT rect{}; GetClientRect(window, &rect);
        const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(window), WM_CTLCOLORSTATIC,
            reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(window)));
        if (brush) FillRect(dc, &rect, brush);
        const auto length = GetWindowTextLengthW(window);
        std::wstring value(length + 1, L'\0');
        value.resize(GetWindowTextW(window, value.data(), length + 1));
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        const UINT flags = DT_WORDBREAK | DT_NOPREFIX |
            ((style & SS_TYPEMASK) == SS_CENTER ? DT_CENTER : (style & SS_TYPEMASK) == SS_RIGHT ? DT_RIGHT : DT_LEFT);
        native_typography::drawText(dc, font, GetTextColor(dc), value, rect, flags);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, static_text, id);
    return DefSubclassProc(window, message, parameter, payload);
}

LRESULT CALLBACK edit_selection(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                                UINT_PTR id, DWORD_PTR) {
    const bool writable = !(GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY);
    if (writable && message == WM_KEYDOWN && parameter == L'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(window, EM_SETSEL, 0, -1);
        return 0;
    }
    if (writable && message == WM_CHAR && parameter == 1) return 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, edit_selection, id);
    return DefSubclassProc(window, message, parameter, payload);
}

HWND widget(Editor& editor, const wchar_t* type, const wchar_t* text, DWORD style,
            int x, int y, int w, int h, WORD id = 0) {
    const auto child = CreateWindowW(type, text, WS_CHILD | WS_VISIBLE | style,
                                    px(editor, x), px(editor, y), px(editor, w), px(editor, h),
                                    editor.window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                    GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(same(type, L"EDIT") ? editor.input_font : editor.font), FALSE);
    if (same(type, L"STATIC") && (style & SS_TYPEMASK) != SS_OWNERDRAW)
        SetWindowSubclass(child, static_text, 5, 0);
    if (same(type, L"EDIT")) SetWindowSubclass(child, edit_selection, 6, 0);
    return child;
}

HWND label(Editor& editor, const wchar_t* text, int x, int y, int width, bool heading = false) {
    const auto handle = widget(editor, L"STATIC", text, 0, x, y, width, heading ? 34 : 22);
    if (heading) SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(editor.heading), FALSE);
    return handle;
}

HWND button(Editor& editor, const wchar_t* text, int x, int y, int w, WORD id, int height = 34) {
    return widget(editor, L"BUTTON", text, WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW, x, y, w, height, id);
}

void smart_symbol(Editor& editor, HDC dc, wchar_t code, int x, int y, int size, COLORREF color) {
    if (!editor.symbols_loaded) return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::Font font(L"5thAug2026", static_cast<float>(size), Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel, &editor.symbols);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(&code, 1, &font, Gdiplus::RectF(static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(size), static_cast<float>(size)), &format, &brush);
}

wchar_t action_symbol(std::wstring_view step) {
    constexpr wchar_t icons[]{0xe98c, 0xe959, 0xe998, 0xe979, 0xe999};
    return icons[step_type(step)];
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
        smart_text(dc, text.data(), static_cast<int>(length), &content,
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
        EnableWindow(save, complete_steps(editor.steps));
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

int logical(const Editor& editor, int pixels) {
    return MulDiv(pixels, canvas_width, editor.window_width);
}

void draw_step(Editor& editor, HDC dc, int index, int y, bool hovered, bool ghost = false) {
    if (index < 0 || static_cast<size_t>(index) >= editor.steps.size()) return;
    const RECT row{0, y, px(editor, 500), y + px(editor, smart_steps::row_height)};
    FillRect(dc, &row, hovered || ghost ? editor.backdrop : editor.fill);
    if (editor.step_dragging && index == editor.step_drag_index && !ghost) return;
    const bool handle = editor.steps.size() > 1 && (hovered || ghost);
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (handle) {
            const auto color = ghost ? accent : foreground;
            Gdiplus::SolidBrush dots(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
            for (int column = 0; column < 2; ++column)
                for (int dot = 0; dot < 3; ++dot)
                    graphics.FillEllipse(&dots, px(editor, 32 + column * 8), y + px(editor, 17 + dot * 8), px(editor, 4), px(editor, 4));
        } else {
            Gdiplus::Pen border(Gdiplus::Color(255, 66, 66, 66), static_cast<float>(std::max(1, px(editor, 1))));
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 25, 25, 25));
            graphics.FillEllipse(&fill, px(editor, 20), y + px(editor, 8), px(editor, 40), px(editor, 40));
            graphics.DrawEllipse(&border, px(editor, 20), y + px(editor, 8), px(editor, 40), px(editor, 40));
        }
        if (hovered && !ghost)
            draw_native_icon(graphics, L"close", px(editor, 456), y + px(editor, 18), px(editor, 20),
                             editor.step_remove_hover ? accent : foreground);
    }
    const auto previous = SelectObject(dc, editor.step_number_font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, foreground);
    if (!handle) {
        const auto number = std::to_wstring(index + 1);
        RECT number_box{px(editor, 20), y + px(editor, 8), px(editor, 60), y + px(editor, 48)};
        smart_text(dc, number.c_str(), -1, &number_box, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    smart_symbol(editor, dc, action_symbol(editor.steps[index]), px(editor, 70), y + px(editor, 16), px(editor, 24), foreground);
    SelectObject(dc, editor.step_font);
    auto label = step_label(editor.steps[index]);
    if (const auto separator = label.find(L" · "); separator != label.npos && !template_pending(editor.steps[index])) label.erase(0, separator + 3);
    RECT text{px(editor, 107), y, px(editor, 445), y + px(editor, smart_steps::row_height)};
    smart_text(dc, label.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previous);
}

LRESULT CALLBACK step_ghost_proc(HWND window, UINT message, WPARAM parameter, LPARAM payload) {
    auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        editor = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(payload)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT && editor) {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        draw_step(*editor, dc, editor->step_drag_index, 0, true, true);
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, parameter, payload);
}

void end_step_drag(Editor& editor, bool cancel) {
    const bool was_dragging = editor.step_dragging;
    if (cancel && was_dragging) editor.steps = std::move(editor.step_drag_original);
    editor.step_drag_original.clear();
    editor.step_dragging = false;
    editor.step_press = editor.step_drag_index = -1;
    if (editor.step_ghost) { DestroyWindow(editor.step_ghost); editor.step_ghost = nullptr; }
    if (GetCapture() == editor.list) ReleaseCapture();
    if (cancel && was_dragging) fill_steps(editor, -1);
    else if (editor.list) InvalidateRect(editor.list, nullptr, FALSE);
}

void move_step_ghost(Editor& editor) {
    if (!editor.step_ghost) return;
    POINT cursor{}; GetCursorPos(&cursor);
    SetWindowPos(editor.step_ghost, HWND_TOP, cursor.x - editor.step_pick.x, cursor.y - editor.step_pick.y,
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void begin_step_drag(Editor& editor) {
    editor.step_dragging = true;
    editor.step_drag_index = editor.step_press;
    editor.step_drag_original = editor.steps;
    WNDCLASSW type{};
    type.lpfnWndProc = step_ghost_proc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"LogiNativeSmartStepGhost";
    RegisterClassW(&type);
    editor.step_ghost = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        type.lpszClassName, L"", WS_POPUP, 0, 0, px(editor, 500), px(editor, smart_steps::row_height),
        editor.window, nullptr, type.hInstance, &editor);
    if (editor.step_ghost) SetLayeredWindowAttributes(editor.step_ghost, 0, 204, LWA_ALPHA);
    move_step_ghost(editor);
}

void scroll_editor(Editor& editor, int delta) {
    const int next = std::clamp(editor.editor_scroll + delta, 0, editor.editor_scroll_max);
    if (next == editor.editor_scroll) return;
    editor.editor_scroll = next;
    layout_editor(editor);
}

LRESULT CALLBACK step_surface(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                              UINT_PTR id, DWORD_PTR context) {
    auto& editor = *reinterpret_cast<Editor*>(context);
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const auto target = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(parameter);
        RECT bounds{}; GetClientRect(window, &bounds);
        const auto dc = CreateCompatibleDC(target);
        const auto bitmap = CreateCompatibleBitmap(target, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
        const auto previous = SelectObject(dc, bitmap);
        FillRect(dc, &bounds, editor.fill);
        const int first = std::max(0, editor.steps_offset / smart_steps::row_height);
        const int last = std::min(static_cast<int>(editor.steps.size()),
            (editor.steps_offset + logical(editor, bounds.bottom) + smart_steps::row_height - 1) / smart_steps::row_height);
        for (int index = first; index < last; ++index)
            draw_step(editor, dc, index, px(editor, index * smart_steps::row_height - editor.steps_offset), index == editor.step_hover);
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen border(Gdiplus::Color(255, 45, 45, 45), static_cast<float>(std::max(1, px(editor, 1))));
            const int right = bounds.right - 1;
            const int radius = px(editor, 8);
            graphics.DrawLine(&border, 0, editor.steps_offset == 0 ? radius : 0, 0, bounds.bottom);
            graphics.DrawLine(&border, right, editor.steps_offset == 0 ? radius : 0, right, bounds.bottom);
            if (editor.steps_offset == 0) {
                graphics.DrawArc(&border, 0, 0, radius * 2, radius * 2, 180.0f, 90.0f);
                graphics.DrawArc(&border, right - radius * 2, 0, radius * 2, radius * 2, 270.0f, 90.0f);
                graphics.DrawLine(&border, radius, 0, right - radius, 0);
            }
        }
        BitBlt(target, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        SendMessageW(editor.window, message, parameter, payload); return 0;
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        const int x = static_cast<short>(LOWORD(payload)), y = static_cast<short>(HIWORD(payload));
        const int logical_x = logical(editor, x), logical_y = logical(editor, y);
        RECT bounds{}; GetClientRect(window, &bounds);
        const int hit = x >= 0 && x < bounds.right && y >= 0 && y < bounds.bottom
            ? smart_steps::hit_row(logical_y, editor.steps_offset, static_cast<int>(editor.steps.size())) : -1;
        const bool remove = logical_x >= 445;
        if (message == WM_LBUTTONDOWN && hit >= 0) {
            SetFocus(window); SetCapture(window);
            editor.step_press = hit; editor.step_press_remove = remove;
            editor.step_pressed_at = GetTickCount64();
            editor.step_pointer = {x, y};
            editor.step_pick = {x, y - px(editor, hit * smart_steps::row_height - editor.steps_offset)};
            SendMessageW(window, LB_SETCURSEL, hit, 0);
            return 0;
        }
        if (message == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
            if (editor.step_press >= 0 && !editor.step_press_remove && editor.steps.size() > 1 &&
                !editor.step_dragging && GetTickCount64() - editor.step_pressed_at >= 60 &&
                (x != editor.step_pointer.x || y != editor.step_pointer.y)) begin_step_drag(editor);
            if (editor.step_dragging) {
                move_step_ghost(editor);
                if (hit >= 0 && smart_steps::reorder(editor.steps, editor.step_drag_index, hit)) {
                    editor.step_drag_index = hit;
                    fill_steps(editor, hit);
                    if (editor.step_ghost) InvalidateRect(editor.step_ghost, nullptr, FALSE);
                }
            }
            if (editor.step_hover != hit || editor.step_remove_hover != remove) {
                editor.step_hover = hit; editor.step_remove_hover = remove;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (message == WM_LBUTTONUP) {
            const bool dragged = editor.step_dragging;
            const int pressed = editor.step_press;
            const bool pressed_remove = editor.step_press_remove;
            end_step_drag(editor, false);
            if (!dragged && pressed >= 0 && pressed == hit && pressed_remove == remove) {
                SendMessageW(window, LB_SETCURSEL, hit, 0);
                if (remove || logical_x >= 65)
                    SendMessageW(editor.window, WM_COMMAND, remove ? step_remove_id : step_edit_id, reinterpret_cast<LPARAM>(window));
            }
            return 0;
        }
    }
    if (message == WM_MOUSELEAVE) {
        editor.step_hover = -1; editor.step_remove_hover = false;
        InvalidateRect(window, nullptr, FALSE); return 0;
    }
    if (message == WM_GETDLGCODE) {
        const auto* key = reinterpret_cast<const MSG*>(payload);
        return DefSubclassProc(window, message, parameter, payload) | DLGC_WANTARROWS |
            (key && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE) ? DLGC_WANTMESSAGE : 0);
    }
    if (message == WM_KEYDOWN) {
        if (parameter == VK_ESCAPE && editor.step_press >= 0) { end_step_drag(editor, true); return 0; }
        if (parameter == VK_ESCAPE) { SendMessageW(editor.window, WM_COMMAND, IDCANCEL, 0); return 0; }
        if (parameter == VK_RETURN || parameter == VK_SPACE || parameter == VK_DELETE) {
            SendMessageW(editor.window, WM_COMMAND, parameter == VK_DELETE ? step_remove_id : step_edit_id, reinterpret_cast<LPARAM>(window));
            return 0;
        }
        if (parameter == VK_UP || parameter == VK_DOWN || parameter == VK_HOME || parameter == VK_END) {
            const int count = static_cast<int>(editor.steps.size());
            if (!count) return 0;
            const int current = selected(window, LB_GETCURSEL);
            const int next = parameter == VK_HOME ? 0 : parameter == VK_END ? count - 1 :
                std::clamp(current + (parameter == VK_UP ? -1 : 1), 0, count - 1);
            SendMessageW(window, LB_SETCURSEL, next, 0);
            const int top = editor.steps_top + next * smart_steps::row_height;
            if (top < smart_steps::viewport_top) scroll_editor(editor, top - smart_steps::viewport_top);
            else if (top + smart_steps::row_height > editor.steps_bottom)
                scroll_editor(editor, top + smart_steps::row_height - editor.steps_bottom);
            editor.step_hover = next; editor.step_remove_hover = false;
            InvalidateRect(window, nullptr, FALSE); return 0;
        }
        if (parameter == VK_PRIOR || parameter == VK_NEXT) {
            scroll_editor(editor, (parameter == VK_PRIOR ? -1 : 1) * (editor.steps_bottom - smart_steps::viewport_top - 56));
            return 0;
        }
    }
    if (message == WM_CHAR && (parameter == VK_RETURN || parameter == VK_SPACE || parameter == VK_ESCAPE)) return 0;
    if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(payload) != window && editor.step_press >= 0)
        end_step_drag(editor, true);
    if (message == WM_NCDESTROY) {
        editor.step_press = editor.step_drag_index = -1; editor.step_dragging = false;
        editor.step_drag_original.clear();
        if (editor.step_ghost) { DestroyWindow(editor.step_ghost); editor.step_ghost = nullptr; }
        RemoveWindowSubclass(window, step_surface, id);
    }
    return DefSubclassProc(window, message, parameter, payload);
}

void layout_editor(Editor& editor) {
    if (!editor.editing || editor.text_editing || editor.detail != Detail::none) return;
    RECT client{}; GetClientRect(editor.window, &client);
    const auto layout = smart_steps::layout(static_cast<int>(editor.steps.size()), editor.trigger_open, editor.action_open,
                                            logical(editor, client.bottom), editor.editor_scroll);
    editor.editor_scroll = layout.scroll; editor.editor_scroll_max = layout.max_scroll;
    editor.steps_top = layout.steps; editor.steps_bottom = layout.bottom;
    const auto move = [&](HWND control, int x, int y, int w, int h) {
        if (!control) return;
        MoveWindow(control, px(editor, x), px(editor, y), px(editor, w), px(editor, h), FALSE);
        const auto clip = CreateRectRgn(0, px(editor, std::clamp(smart_steps::viewport_top - y, 0, h)), px(editor, w),
                                        px(editor, std::clamp(layout.bottom - y, 0, h)));
        if (!SetWindowRgn(control, clip, FALSE)) DeleteObject(clip);
    };
    move(editor.if_heading, 511, layout.if_heading, 480, 28);
    move(editor.trigger_summary, 491, layout.trigger, 500, 48);
    move(editor.then_heading, 511, layout.then_heading, 480, 28);
    move(GetDlgItem(editor.window, action_menu_id), 491, layout.add, 500, 48);
    move(GetDlgItem(editor.window, add_tag_id), 491, layout.tag, 150, 30);
    if (editor.list) {
        const int top = std::max(smart_steps::viewport_top, layout.steps);
        const int bottom = std::min(layout.bottom, layout.add);
        editor.steps_offset = top - layout.steps;
        MoveWindow(editor.list, px(editor, 491), px(editor, top), px(editor, 500), px(editor, std::max(1, bottom - top)), FALSE);
        show_control(editor.list, !editor.steps.empty() && top < bottom);
        SendMessageW(editor.list, LB_SETTOPINDEX, std::max(0, editor.steps_offset / smart_steps::row_height), 0);
        InvalidateRect(editor.list, nullptr, FALSE);
    }
    for (size_t i = 0; i < 5; ++i) {
        move(editor.trigger_categories[i], 491, layout.trigger + 48 + static_cast<int>(i) * 44, 500, 44);
        move(editor.action_categories[i], 491, layout.add + 48 + static_cast<int>(i) * 44, 500, 44);
    }
    show_control(editor.name, false);
    show_control(editor.name_label, false);
    show_control(editor.tag, false);
    show_control(editor.tag_label, false);
    // Layout changes move and shrink several sibling windows without painting.
    // Repaint the parent's exposed old bounds as well as each child's new bounds.
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void trigger_visibility(Editor& editor) {
    if (editor.text_editing || editor.detail != Detail::none) return;
    for (const auto control : {editor.trigger_type, editor.trigger_profile, editor.trigger_choice, editor.trigger_key})
        show_control(control, false);
    for (const auto control : editor.trigger_categories) show_control(control, editor.trigger_open);
    layout_editor(editor);
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
    if (editor.text_editing || editor.detail != Detail::none) return;
    for (const auto control : {editor.action_type, editor.action_choice, editor.action_value, editor.text_edit,
                              editor.action_title, editor.browse, editor.record, editor.fixed_delay,
                              GetDlgItem(editor.window, apply_step_id)}) show_control(control, false);
    for (const auto control : editor.action_categories) show_control(control, editor.action_open);
    show_control(GetDlgItem(editor.window, add_tag_id), true);
    layout_editor(editor);
}

void show_action_fields(Editor& editor, std::wstring_view step = {}) {
    if (editor.text_editing || editor.detail != Detail::none) return;
    const int type = selected(editor.action_type);
    action_visibility(editor);
    MoveWindow(editor.action_choice, px(editor, 650), px(editor, 587),
               px(editor, type == 3 ? 341 : 161), px(editor, 280), IsWindowVisible(editor.action_choice));
    MoveWindow(editor.action_value, px(editor, type == 0 ? 819 : 650), px(editor, 587),
               px(editor, type == 0 ? 172 : 341), px(editor, type == 2 || type == 1 ? 44 : 31),
               IsWindowVisible(editor.action_value));
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
        int chosen = -1;
        for (size_t i = 0; i < std::size(system_actions); ++i) {
            add_choice(editor.action_choice, system_actions[i].name);
            if (system_choice(step) == &system_actions[i]) chosen = static_cast<int>(i);
        }
        select(editor.action_choice, chosen);
    } else if (type == 4) {
        value = step.empty() ? L"1000" : inner(step);
        SetWindowTextW(editor.action_title, L"延迟（毫秒，0 到 30000）");
    }
    SetWindowTextW(editor.action_value, value.c_str());
    SetWindowTextW(GetDlgItem(editor.window, apply_step_id), editor.edit_index < 0 ? L"添加操作" : L"更新操作");
}

LRESULT CALLBACK text_cue(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                          UINT_PTR id, DWORD_PTR context) {
    const auto result = DefSubclassProc(window, message, parameter, payload);
    if (message == WM_GETDLGCODE && payload) {
        const auto* key = reinterpret_cast<const MSG*>(payload);
        if (key->message == WM_KEYDOWN &&
            (key->wParam == VK_ESCAPE || (key->wParam == VK_TAB &&
             GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0)))
            return result & ~(DLGC_WANTTAB | DLGC_WANTALLKEYS);
    }
    if (message == WM_PAINT && !GetWindowTextLengthW(window)) {
        const auto dc = GetDC(window);
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        const auto previous = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(177, 177, 177));
        smart_text(dc, context ? reinterpret_cast<const wchar_t*>(context) : L"在此处输入或粘贴文本（最多 1000 个字符）", -1, &bounds,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, text_cue, id);
    return result;
}

void update_text_editor(Editor& editor) {
    const auto length = GetWindowTextLengthW(editor.text_value);
    const auto count = length <= 1000 ? L"剩余字符：" + std::to_wstring(1000 - length)
                                      : L"文本不能超过 1000 个字符";
    SetWindowTextW(editor.text_counter, count.c_str());
    EnableWindow(GetDlgItem(editor.window, apply_step_id), length > 0 && length <= 1000);
}

void open_text_editor(Editor& editor) {
    if (editor.text_editing) return;
    editor.text_editing = true;
    editor.text_hidden_controls.clear();
    const auto confirm = GetDlgItem(editor.window, apply_step_id);
    for (auto child = GetWindow(editor.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto id = GetDlgCtrlID(child);
        if (child == editor.header_title || child == confirm ||
            id == back_id || id == minimize_id || id == close_id) continue;
        if (IsWindowVisible(child)) {
            editor.text_hidden_controls.push_back(child);
            ShowWindow(child, SW_HIDE);
        }
    }
    SetWindowTextW(editor.header_title, L"文本");
    const auto value = read_text(editor.action_value);
    editor.text_value = widget(editor, L"EDIT", value.c_str(),
                               WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                               294, 282, 906, 278, text_value_id);
    SendMessageW(editor.text_value, EM_SETLIMITTEXT, 1000, 0);
    capture_keys(editor.text_value);
    SetWindowSubclass(editor.text_value, text_cue, 2, 0);
    const auto before_confirm = GetWindow(confirm, GW_HWNDPREV);
    SetWindowPos(editor.text_value, before_confirm ? before_confirm : HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (const auto id : {minimize_id, close_id}) {
        const auto control = GetDlgItem(editor.window, id);
        SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) & ~WS_TABSTOP);
    }
    editor.text_notice = label(editor, L"请勿使用 Smart Actions 存储敏感信息", 294, 587, 650);
    editor.text_counter = widget(editor, L"STATIC", L"", SS_RIGHT, 968, 587, 232, 22);
    MoveWindow(confirm, px(editor, 294), px(editor, 630), px(editor, 906), px(editor, 38), TRUE);
    SetWindowTextW(confirm, L"确认");
    show_control(confirm, true);
    update_text_editor(editor);
    SetFocus(editor.text_value);
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void close_text_editor(Editor& editor) {
    editor.text_editing = false;
    show_control(GetDlgItem(editor.window, apply_step_id), false);
    DestroyWindow(editor.text_value);
    DestroyWindow(editor.text_counter);
    DestroyWindow(editor.text_notice);
    editor.text_value = editor.text_counter = editor.text_notice = nullptr;
    for (const auto id : {minimize_id, close_id}) {
        const auto control = GetDlgItem(editor.window, id);
        SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) | WS_TABSTOP);
    }
    for (const auto child : editor.text_hidden_controls) show_control(child, true);
    editor.text_hidden_controls.clear();
    SetWindowTextW(editor.header_title, editor.original || !editor.template_cards.empty() ? L"编辑 Smart Actions" : L"创建 Smart Actions");
    editor.edit_index = -1;
    editor.trigger_open = editor.action_open = false;
    const auto confirm = GetDlgItem(editor.window, apply_step_id);
    EnableWindow(confirm, TRUE);
    MoveWindow(confirm, px(editor, 1008), px(editor, 586), px(editor, 114), px(editor, 35), TRUE);
    trigger_visibility(editor);
    show_action_fields(editor);
    SetFocus(GetDlgItem(editor.window, action_menu_id));
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

HWND detail_widget(Editor& editor, const wchar_t* type, const wchar_t* text, DWORD style,
                   int x, int y, int width, int height, WORD id = 0) {
    const auto control = widget(editor, type, text, style, x, y, width, height, id);
    editor.detail_controls.push_back(control);
    return control;
}

HWND detail_button(Editor& editor, const wchar_t* text, int x, int y, int width, int height, WORD id) {
    return detail_widget(editor, L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, x, y, width, height, id);
}

void fill_key_records(Editor& editor) {
    if (editor.detail != Detail::keys || !editor.detail_list) return;
    SendMessageW(editor.detail_list, LB_RESETCONTENT, 0, 0);
    for (const auto& step : editor.recorded_steps) {
        const auto title = step.starts_with(L"Wait(") ? step_label(step) : step;
        SendMessageW(editor.detail_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.c_str()));
    }
    const bool ready = !editor.recorded_steps.empty() && !editor.recording;
    show_control(editor.detail_confirm, ready);
    EnableWindow(editor.detail_confirm, ready);
    MoveWindow(editor.detail_record, px(editor, 413), px(editor, 642),
               px(editor, ready ? 327 : 668), px(editor, 36), TRUE);
    SetWindowTextW(editor.detail_record, editor.recording ? L"停止录制" : ready ? L"重新录制" : L"开始录制");
    show_control(editor.detail_status, editor.recorded_steps.empty());
    EnableWindow(editor.detail_fixed, !editor.recording);
    EnableWindow(editor.detail_delay, !editor.recording);
    EnableWindow(editor.detail_advanced, !editor.recording);
}

std::wstring event_key(WPARAM key, LPARAM flags) {
    if (key == VK_CONTROL) return flags & (1L << 24) ? L"RightCtrl" : L"LeftCtrl";
    if (key == VK_MENU) return flags & (1L << 24) ? L"RightAlt" : L"LeftAlt";
    if (key == VK_SHIFT) return MapVirtualKeyW((flags >> 16) & 255, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? L"RightShift" : L"LeftShift";
    if (key == VK_LWIN) return L"LeftWin";
    if (key == VK_RWIN) return L"RightWin";
    if (const auto name = key_name(key); !name.empty()) return name;
    return key >= 8 && key <= 254 ? std::to_wstring(key) : L"";
}

void finish_recording(Editor& editor) {
    // Complete only keys captured by this editor. These are metadata, not injected input.
    for (auto key = editor.recorded_down.rbegin(); key != editor.recorded_down.rend(); ++key)
        editor.recorded_steps.push_back(L"KeyUp(" + *key + L")");
    editor.recorded_down.clear();
    editor.recording = false;
    fill_key_records(editor);
}

LRESULT CALLBACK keys_capture(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                              UINT_PTR id, DWORD_PTR context) {
    auto& editor = *reinterpret_cast<Editor*>(context);
    if (editor.recording && message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (editor.recording && (down || up) && SendMessageW(editor.detail_advanced, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        const auto key = event_key(parameter, payload);
        if (!key.empty()) {
            const auto held = std::ranges::find(editor.recorded_down, key);
            if (down && held == editor.recorded_down.end()) {
                if (editor.recorded_steps.size() + editor.recorded_down.size() * 2 + 4 > 1024) { finish_recording(editor); return 0; }
                editor.recorded_down.push_back(key);
                record_key(editor, L"KeyDown(" + key + L")");
            } else if (up && held != editor.recorded_down.end()) {
                if (editor.recorded_steps.size() >= 1023) { finish_recording(editor); return 0; }
                editor.recorded_down.erase(held);
                record_key(editor, L"KeyUp(" + key + L")");
            }
        }
        return 0;
    }
    if (editor.recording && down) {
        auto key = key_name(parameter);
        if (!key.empty() && !(payload & (1L << 30))) {
            std::wstring chord;
            if (GetKeyState(VK_CONTROL) < 0) chord += L"Ctrl+";
            if (GetKeyState(VK_MENU) < 0 || (payload & (1L << 29))) chord += L"Alt+";
            if (GetKeyState(VK_SHIFT) < 0) chord += L"Shift+";
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) chord += L"Win+";
            record_key(editor, chord + key);
        }
        return 0;
    }
    if (editor.recording && up) return 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, keys_capture, id);
    return DefSubclassProc(window, message, parameter, payload);
}

void add_application(Editor& editor, std::wstring name, std::wstring path) {
    if (path.empty() || std::ranges::any_of(editor.applications, [&](const auto& app) { return same(app.path, path); })) return;
    SHFILEINFOW info{};
    SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON);
    editor.applications.push_back({std::move(name), std::move(path), info.hIcon});
}

void discover_applications(Editor& editor) {
    if (!editor.applications.empty()) return;
    static std::vector<std::pair<std::wstring, std::wstring>> cached_entries;
    if (cached_entries.empty()) {
        const auto append_cached = [&](std::wstring name, std::wstring path) {
            if (path.empty() || std::ranges::any_of(cached_entries, [&](const auto& item) { return same(item.second, path); })) return;
            cached_entries.emplace_back(std::move(name), std::move(path));
        };
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (const auto folder : {CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS}) {
            wchar_t directory[MAX_PATH]{};
            if (FAILED(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, directory))) continue;
            const std::filesystem::path root(directory);
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
                 it != end; it.increment(error)) {
                if (error) { error.clear(); continue; }
                if (!same(it->path().extension().wstring(), L".lnk")) continue;
                IShellLinkW* shortcut = nullptr;
                if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut)))) continue;
                IPersistFile* file = nullptr;
                if (SUCCEEDED(shortcut->QueryInterface(IID_PPV_ARGS(&file)))) {
                    if (SUCCEEDED(file->Load(it->path().c_str(), STGM_READ))) {
                        wchar_t target[MAX_PATH]{};
                        if (SUCCEEDED(shortcut->GetPath(target, std::size(target), nullptr, SLGP_RAWPATH)) &&
                            same(std::filesystem::path(target).extension().wstring(), L".exe"))
                            append_cached(it->path().stem().wstring(), target);
                    }
                    file->Release();
                }
                shortcut->Release();
            }
        }
        for (const auto& app : applications) {
            wchar_t path[MAX_PATH]{};
            if (SearchPathW(nullptr, app.code, nullptr, std::size(path), path, nullptr)) append_cached(app.name, path);
        }
        for (const auto& identity : application_identities)
            if (const auto path = resolve_application_id(identity.id); !path.empty()) append_cached(identity.name, path);
        std::sort(cached_entries.begin(), cached_entries.end(), [](const auto& a, const auto& b) {
            return CompareStringOrdinal(a.first.c_str(), -1, b.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
    for (const auto& [name, path] : cached_entries) add_application(editor, name, path);
}

void layout_application_tiles(Editor& editor) {
    const int rows = static_cast<int>((editor.application_tiles.size() + 3) / 4);
    editor.application_scroll = std::clamp(editor.application_scroll, 0, std::max(0, rows * 142 - 625));
    for (size_t i = 0; i < editor.application_tiles.size(); ++i) {
        const int y = 207 + static_cast<int>(i / 4) * 142 - editor.application_scroll;
        const auto control = editor.application_tiles[i];
        MoveWindow(control, px(editor, 458 + static_cast<int>(i % 4) * 144), px(editor, y), px(editor, 144), px(editor, 142), FALSE);
        const auto clip = CreateRectRgn(0, px(editor, std::max(0, 207 - y)), px(editor, 144), px(editor, std::min(142, 832 - y)));
        if (!SetWindowRgn(control, clip, FALSE)) DeleteObject(clip);
        show_control(control, y < 832 && y + 142 > 207);
    }
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

std::wstring_view selected_system_code(const Editor& editor) {
    const int index = selected(editor.detail_list, LB_GETCURSEL);
    return index >= 0 && static_cast<size_t>(index) < editor.detail_items.size()
        ? std::wstring_view(system_actions[editor.detail_items[index]].code) : std::wstring_view{};
}

bool valid_system_parameter(std::wstring_view kind, std::wstring_view path) {
    if (!kind.starts_with(L"@Open") || open_action(path).empty()) return false;
    if (kind == L"@OpenWebPage") return UrlIsW(std::wstring(path).c_str(), URLIS_URL);
    std::error_code error;
    if (kind == L"@OpenFolder") return std::filesystem::is_directory(path, error);
    return std::filesystem::is_regular_file(path, error) &&
        (kind != L"@OpenApplication" || same(std::filesystem::path(path).extension().wstring(), L".exe"));
}

void clear_system_parameter(Editor& editor) {
    if (editor.system_parameter) DestroyWindow(editor.system_parameter);
    if (editor.system_browse) DestroyWindow(editor.system_browse);
    editor.system_parameter = editor.system_browse = nullptr;
    editor.system_parameter_row = -1;
}

void layout_system_parameter(Editor& editor) {
    if (!editor.system_parameter || editor.system_parameter_row < 0) return;
    RECT row{}, bounds{};
    if (SendMessageW(editor.detail_list, LB_GETITEMRECT, editor.system_parameter_row, reinterpret_cast<LPARAM>(&row)) == LB_ERR) return;
    GetClientRect(editor.detail_list, &bounds);
    const int x = px(editor, 16), y = row.top + px(editor, 53);
    const int browse_width = editor.system_browse ? px(editor, 82) : 0;
    SetWindowPos(editor.system_parameter, HWND_TOP, x, y, std::max(px(editor, 40), static_cast<int>(bounds.right) - x * 2 - browse_width),
                 px(editor, 26), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (editor.system_browse)
        SetWindowPos(editor.system_browse, HWND_TOP, bounds.right - x - browse_width + px(editor, 6), y - px(editor, 3),
                     browse_width - px(editor, 6), px(editor, 32), SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void select_system_parameter(Editor& editor) {
    if (editor.detail != Detail::system) return;
    clear_system_parameter(editor);
    for (size_t i = 0; i < editor.detail_items.size(); ++i)
        SendMessageW(editor.detail_list, LB_SETITEMHEIGHT, i, px(editor, 38));
    const auto code = selected_system_code(editor);
    if (!code.starts_with(L"@Open")) {
        EnableWindow(editor.detail_confirm, !code.empty());
        InvalidateRect(editor.detail_list, nullptr, TRUE);
        return;
    }
    editor.system_parameter_row = selected(editor.detail_list, LB_GETCURSEL);
    SendMessageW(editor.detail_list, LB_SETITEMHEIGHT, editor.system_parameter_row, px(editor, 100));
    RECT row{}, viewport{};
    SendMessageW(editor.detail_list, LB_GETITEMRECT, editor.system_parameter_row, reinterpret_cast<LPARAM>(&row));
    GetClientRect(editor.detail_list, &viewport);
    if (row.bottom > viewport.bottom) SendMessageW(editor.detail_list, LB_SETTOPINDEX, editor.system_parameter_row, 0);
    std::wstring value;
    if (editor.edit_index >= 0 && static_cast<size_t>(editor.edit_index) < editor.steps.size()) {
        const auto& step = editor.steps[editor.edit_index];
        if (const auto choice = system_choice(step); choice && choice->code == code && step.starts_with(L"Open(")) value = inner(step);
    }
    editor.system_parameter = CreateWindowW(L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 1, 1, editor.detail_list, reinterpret_cast<HMENU>(system_parameter_id), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(editor.system_parameter, WM_SETFONT, reinterpret_cast<WPARAM>(editor.input_font), FALSE);
    SendMessageW(editor.system_parameter, EM_SETLIMITTEXT, 2048, 0);
    SendMessageW(editor.system_parameter, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(code == L"@OpenWebPage" ? L"输入 URL" : L"选择路径"));
    SetWindowSubclass(editor.system_parameter, edit_selection, 6, 0);
    if (code != L"@OpenWebPage") {
        editor.system_browse = CreateWindowW(L"BUTTON", L"浏览", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, editor.detail_list, reinterpret_cast<HMENU>(system_browse_id), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(editor.system_browse, WM_SETFONT, reinterpret_cast<WPARAM>(editor.font), FALSE);
    }
    layout_system_parameter(editor);
    EnableWindow(editor.detail_confirm, valid_system_parameter(code, value));
    RedrawWindow(editor.detail_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void browse_system_parameter(Editor& editor) {
    const auto kind = selected_system_code(editor);
    if (!kind.starts_with(L"@Open") || kind == L"@OpenWebPage") return;
    native_typography::Com<IFileOpenDialog> picker;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                               __uuidof(IFileOpenDialog), reinterpret_cast<void**>(picker.put())))) return;
    FILEOPENDIALOGOPTIONS options{};
    picker->GetOptions(&options);
    picker->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       (kind == L"@OpenFolder" ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    picker->SetTitle(kind == L"@OpenFolder" ? L"选择文件夹" : kind == L"@OpenApplication" ? L"选择应用程序" : L"选择文件");
    if (kind == L"@OpenApplication") {
        const COMDLG_FILTERSPEC filter{L"应用程序 (*.exe)", L"*.exe"};
        picker->SetFileTypes(1, &filter);
    }
    if (FAILED(picker->Show(editor.window))) return;
    native_typography::Com<IShellItem> item;
    PWSTR path{};
    if (SUCCEEDED(picker->GetResult(item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        SetWindowTextW(editor.system_parameter, path);
        CoTaskMemFree(path);
    }
}

void fill_detail_choices(Editor& editor) {
    const auto query = read_text(editor.detail_value);
    editor.detail_items.clear();
    if (editor.detail == Detail::application) {
        for (const auto control : editor.application_tiles) {
            std::erase(editor.detail_controls, control);
            DestroyWindow(control);
        }
        editor.application_tiles.clear();
        editor.application_tiles.push_back(detail_button(editor, L"添加应用程序", 458, 207, 144, 142, application_tile_id));
        for (size_t i = 0; i < editor.applications.size(); ++i) {
            if (!contains_text(editor.applications[i].name, query)) continue;
            editor.detail_items.push_back(i);
            editor.application_tiles.push_back(detail_button(editor, editor.applications[i].name.c_str(), 0, 0, 144, 142,
                                                              static_cast<WORD>(application_tile_id + editor.detail_items.size())));
        }
        editor.application_scroll = 0;
        layout_application_tiles(editor);
    } else if ((editor.detail == Detail::system || editor.detail == Detail::application_action) && editor.detail_list) {
        clear_system_parameter(editor);
        SendMessageW(editor.detail_list, LB_RESETCONTENT, 0, 0);
        const std::span<const Choice> choices = editor.detail == Detail::system ? std::span<const Choice>(system_actions) :
            application_actions(editor.applications[editor.selected_application]);
        for (size_t i = 0; i < choices.size(); ++i) {
            if (editor.template_target >= 0 && editor.detail == Detail::system &&
                choices[i].code != std::wstring_view(L"@OpenFile") && choices[i].code != std::wstring_view(L"@OpenFolder")) continue;
            if (!contains_text(choices[i].name, query)) continue;
            editor.detail_items.push_back(i);
            SendMessageW(editor.detail_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choices[i].name));
        }
        EnableWindow(editor.detail_confirm, FALSE);
    }
}

void update_name_detail(Editor& editor) {
    if (editor.detail != Detail::name || !editor.detail_value) return;
    const auto name = read_text(editor.detail_value);
    const auto description = read_text(editor.detail_description);
    SetWindowTextW(editor.detail_name_counter, (std::to_wstring(name.size()) + L"/24").c_str());
    SetWindowTextW(editor.detail_description_counter, (std::to_wstring(description.size()) + L"/80").c_str());
    EnableWindow(editor.detail_confirm, !trim(name).empty() && name.size() <= 24 && description.size() <= 80);
}

LRESULT CALLBACK choice_hover(HWND window, UINT message, WPARAM parameter, LPARAM payload,
                              UINT_PTR id, DWORD_PTR context) {
    auto& editor = *reinterpret_cast<Editor*>(context);
    if (message == WM_COMMAND || message == WM_DRAWITEM || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
        return SendMessageW(editor.window, message, parameter, payload);
    if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
        const auto hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, payload);
        const int index = message == WM_MOUSELEAVE || HIWORD(hit) ? -1 : LOWORD(hit);
        if (editor.detail_hover != index) {
            editor.detail_hover = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track); }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, choice_hover, id);
    const auto result = DefSubclassProc(window, message, parameter, payload);
    if (message == WM_VSCROLL || message == WM_MOUSEWHEEL || message == WM_KEYDOWN || message == WM_SIZE)
        layout_system_parameter(editor);
    return result;
}

void open_detail(Editor& editor, Detail detail) {
    editor.trigger_open = editor.action_open = false;
    trigger_visibility(editor);
    action_visibility(editor);
    editor.detail = detail;
    editor.detail_hover = -1;
    editor.detail_hidden.clear();
    editor.detail_controls.clear();
    editor.detail_value = editor.detail_list = editor.detail_confirm = editor.detail_cancel = editor.detail_status = nullptr;
    editor.detail_fixed = editor.detail_delay = editor.detail_record = editor.detail_advanced = nullptr;
    editor.detail_description = editor.detail_name_counter = editor.detail_description_counter = nullptr;
    editor.system_parameter = editor.system_browse = nullptr; editor.system_parameter_row = -1;
    for (auto child = GetWindow(editor.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto id = GetDlgCtrlID(child);
        if (child == editor.header_title || id == back_id || id == minimize_id || id == close_id) continue;
        if (IsWindowVisible(child)) { editor.detail_hidden.push_back(child); ShowWindow(child, SW_HIDE); }
    }
    const wchar_t* title = detail == Detail::shortcut ? L"快捷键" : detail == Detail::device ? L"选择按钮" :
        detail == Detail::delay ? L"延迟" : detail == Detail::keys ? L"击键" : detail == Detail::system ? L"系统操作" :
        detail == Detail::application ? L"应用程序" : detail == Detail::application_action ? L"应用程序操作" : detail == Detail::name ? L"命名 Smart Action" :
        detail == Detail::tag ? L"添加标签" : L"Actions Ring";
    if (detail != Detail::ring && detail != Detail::name) SetWindowTextW(editor.header_title, title);
    if (detail == Detail::shortcut) {
        detail_widget(editor, L"STATIC", L"按住一个或多个修饰键（Ctrl、Alt 或 Shift），然后按下另一个键。",
                       SS_LEFT, 584, 326, 326, 40);
        editor.detail_value = detail_widget(editor, L"EDIT", same(editor.trigger.section, L"hotkeys") ? editor.trigger.key.c_str() : L"",
                                            WS_TABSTOP | ES_READONLY, 600, 394, 294, 24, trigger_key_id);
        capture_keys(editor.detail_value);
        SendMessageW(editor.detail_value, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"按下按键组合"));
        editor.detail_confirm = detail_button(editor, L"确认", 584, 492, 326, 47, detail_confirm_id);
        editor.detail_cancel = detail_button(editor, L"取消", 584, 551, 326, 47, detail_cancel_id);
        EnableWindow(editor.detail_confirm, !read_text(editor.detail_value).empty());
    } else if (detail == Detail::delay) {
        const auto milliseconds = unsigned_value(read_text(editor.action_value)).value_or(1000);
        wchar_t value[32]{};
        std::swprintf(value, std::size(value), L"%.3f", milliseconds / 1000.0);
        std::wstring seconds = value;
        while (seconds.size() > 3 && seconds.ends_with(L'0') && seconds[seconds.size() - 2] != L'.') seconds.pop_back();
        editor.detail_value = detail_widget(editor, L"EDIT", seconds.c_str(), WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
                                            662, 355, 138, 25, detail_value_id);
        SendMessageW(editor.detail_value, EM_SETLIMITTEXT, 8, 0);
        detail_widget(editor, L"STATIC", L"s", SS_LEFT, 808, 355, 24, 25);
        editor.detail_confirm = detail_button(editor, L"确认", 584, 424, 326, 47, detail_confirm_id);
        editor.detail_cancel = detail_button(editor, L"取消", 584, 483, 326, 48, detail_cancel_id);
    } else if (detail == Detail::device) {
        editor.detail_choice = -1;
        editor.detail_profile = 0;
        for (size_t i = 0; i < editor.profiles.size(); ++i) if (same(editor.profiles[i], editor.trigger.section)) editor.detail_profile = static_cast<int>(i);
        struct Place { int x, y, width, height; const wchar_t* text; };
        constexpr Place positions[]{{787,241,85,65,L"中键\n滚轮按钮"},{807,301,130,65,L"顶部按钮"},
            {577,330,85,65,L"前进\n前进按钮"},{669,446,85,65,L"后退\n后退按钮"},{520,369,89,84,L"手势\n拇指按钮"}};
        for (size_t i = 0; i < std::size(positions); ++i) {
            const auto& place = positions[i];
            detail_button(editor, place.text, place.x, place.y, place.width, place.height, static_cast<WORD>(device_choice_id + i));
            if (same(editor.trigger.key, mouse_buttons[i].code)) editor.detail_choice = static_cast<int>(i);
        }
        for (size_t i = 0; i < editor.profiles.size(); ++i)
            detail_button(editor, profile_name(editor.profiles[i]).c_str(),
                           1293 - static_cast<int>(editor.profiles.size() > 3 ? editor.profiles.size() - 3 : 0) * 48 + static_cast<int>(i) * 48,
                           38, 48, 48, static_cast<WORD>(device_profile_id + i));
        editor.detail_confirm = detail_button(editor, L"确认触发器", 570, 764, 354, 40, detail_confirm_id);
        EnableWindow(editor.detail_confirm, editor.detail_choice >= 0);
    } else if (detail == Detail::keys) {
        editor.detail_list = detail_widget(editor, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                              LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                           413, 257, 668, 288, detail_list_id);
        SendMessageW(editor.detail_list, LB_SETITEMHEIGHT, 0, px(editor, 40));
        SetWindowSubclass(editor.detail_list, keys_capture, 3, reinterpret_cast<DWORD_PTR>(&editor));
        editor.detail_status = detail_widget(editor, L"STATIC", L"您的击键将显示在此处。按“开始录制”开始。", SS_LEFT,
                                             421, 269, 640, 28, detail_status_id);
        editor.detail_advanced = detail_widget(editor, L"BUTTON", L"高级", WS_TABSTOP | BS_AUTOCHECKBOX,
                                                421, 574, 140, 28, detail_advanced_id);
        SendMessageW(editor.detail_advanced, BM_SETCHECK,
                     std::ranges::any_of(editor.recorded_steps, [](const auto& step) { return step.starts_with(L"KeyDown("); }) ? BST_CHECKED : BST_UNCHECKED, 0);
        editor.detail_fixed = detail_widget(editor, L"BUTTON", L"固定延迟", WS_TABSTOP | BS_AUTOCHECKBOX,
                                             589, 574, 160, 28, detail_fixed_id);
        SendMessageW(editor.detail_fixed, BM_SETCHECK, BST_CHECKED, 0);
        editor.detail_delay = detail_widget(editor, L"EDIT", L"0.021", WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
                                             755, 575, 55, 25, detail_delay_id);
        detail_widget(editor, L"STATIC", L"s", SS_LEFT, 814, 575, 20, 25);
        editor.detail_record = detail_button(editor, L"开始录制", 413, 642, 668, 36, detail_record_id);
        editor.detail_confirm = detail_button(editor, L"确认", 754, 642, 327, 36, detail_confirm_id);
        fill_key_records(editor);
    } else if (detail == Detail::system || detail == Detail::application_action) {
        editor.detail_value = detail_widget(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                                            579, 270, 336, 25, detail_value_id);
        SendMessageW(editor.detail_value, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"搜索"));
        editor.detail_list = detail_widget(editor, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN | LBS_NOTIFY |
                                               LBS_NOINTEGRALHEIGHT | (detail == Detail::system ? LBS_OWNERDRAWVARIABLE : LBS_OWNERDRAWFIXED) | LBS_HASSTRINGS,
                                           563, 338, 368, 254, detail_list_id);
        SetWindowTheme(editor.detail_list, L"DarkMode_Explorer", nullptr);
        SendMessageW(editor.detail_list, LB_SETITEMHEIGHT, 0, px(editor, 38));
        if (detail == Detail::system)
            SetWindowLongPtrW(editor.detail_list, GWL_EXSTYLE, GetWindowLongPtrW(editor.detail_list, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
        SetWindowSubclass(editor.detail_list, choice_hover, 4, reinterpret_cast<DWORD_PTR>(&editor));
        editor.detail_confirm = detail_button(editor, L"确认", 563, 604, 368, 40, detail_confirm_id);
        editor.detail_cancel = detail_button(editor, L"取消", 563, 654, 368, 40, detail_cancel_id);
        if (detail == Detail::application_action) {
            const auto title = editor.applications[editor.selected_application].name + L" 操作";
            SetWindowTextW(editor.header_title, title.c_str());
        }
        fill_detail_choices(editor);
        if (detail == Detail::system && editor.edit_index >= 0) {
            const auto existing = static_cast<size_t>(selected(editor.action_choice));
            if (const auto found = std::ranges::find(editor.detail_items, existing); found != editor.detail_items.end()) {
                SendMessageW(editor.detail_list, LB_SETCURSEL, found - editor.detail_items.begin(), 0);
                select_system_parameter(editor);
            }
        }
    } else if (detail == Detail::application) {
        discover_applications(editor);
        editor.detail_value = detail_widget(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                                            586, 152, 322, 25, detail_value_id);
        SendMessageW(editor.detail_value, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"搜索"));
        fill_detail_choices(editor);
    } else if (detail == Detail::ring) {
        const auto title_label = detail_widget(editor, L"STATIC", L"Actions Ring", SS_CENTER, 579, 262, 336, 36);
        SendMessageW(title_label, WM_SETFONT, reinterpret_cast<WPARAM>(editor.heading), FALSE);
        detail_widget(editor, L"STATIC", L"保存后，在 Actions Ring 的 Smart Actions 操作库中找到此动作，将它分配到一个位置。",
                      SS_LEFT, 579, 438, 336, 76);
        editor.detail_confirm = detail_button(editor, L"确认", 579, 550, 336, 48, detail_confirm_id);
    } else if (detail == Detail::name) {
        const auto heading = detail_widget(editor, L"STATIC", editor.original ? L"重命名 Smart Actions" : L"保存 Smart Actions",
                                          SS_CENTER, 528, 280, 440, 24);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(editor.heading), FALSE);
        detail_widget(editor, L"STATIC", L"命名 Smart Actions 并添加描述。", SS_CENTER, 528, 312, 440, 20);
        const auto value = read_text(editor.name);
        editor.detail_value = detail_widget(editor, L"EDIT", value.c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                                           540, 368, 354, 24, detail_value_id);
        SendMessageW(editor.detail_value, EM_SETLIMITTEXT, 24, 0);
        SendMessageW(editor.detail_value, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"无标题"));
        editor.detail_description = detail_widget(editor, L"EDIT", editor.description.c_str(),
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 540, 428, 416, 44, detail_description_id);
        SendMessageW(editor.detail_description, EM_SETLIMITTEXT, 80, 0);
        SetWindowSubclass(editor.detail_description, text_cue, 2, reinterpret_cast<DWORD_PTR>(L"说明"));
        editor.detail_name_counter = detail_widget(editor, L"STATIC", L"", SS_RIGHT, 901, 371, 55, 20);
        editor.detail_description_counter = detail_widget(editor, L"STATIC", L"", SS_RIGHT, 901, 473, 55, 20);
        editor.detail_cancel = detail_button(editor, L"取消", 528, 520, 212, 48, detail_cancel_id);
        editor.detail_confirm = detail_button(editor, L"确认", 756, 520, 212, 48, detail_confirm_id);
        update_name_detail(editor);
    } else if (detail == Detail::tag) {
        const auto value = read_text(editor.tag);
        editor.detail_value = detail_widget(editor, L"EDIT", value.c_str(), WS_TABSTOP | ES_AUTOHSCROLL,
                                            584, 358, 326, 32, detail_value_id);
        SendMessageW(editor.detail_value, EM_SETLIMITTEXT, 24, 0);
        editor.detail_confirm = detail_button(editor, L"确认", 584, 424, 326, 47, detail_confirm_id);
        editor.detail_cancel = detail_button(editor, L"取消", 584, 483, 326, 48, detail_cancel_id);
    }
    SetFocus(editor.detail_value ? editor.detail_value : editor.detail_list ? editor.detail_list : GetDlgItem(editor.window, back_id));
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void close_detail(Editor& editor) {
    editor.recording = false;
    editor.recorded_down.clear();
    clear_system_parameter(editor);
    editor.detail = Detail::none;
    for (const auto control : editor.detail_controls) DestroyWindow(control);
    editor.detail_controls.clear();
    editor.application_tiles.clear();
    editor.detail_value = editor.detail_list = editor.detail_confirm = editor.detail_cancel = editor.detail_status = nullptr;
    editor.detail_fixed = editor.detail_delay = editor.detail_record = editor.detail_advanced = nullptr;
    editor.detail_description = editor.detail_name_counter = editor.detail_description_counter = nullptr;
    for (const auto control : editor.detail_hidden) show_control(control, true);
    editor.detail_hidden.clear();
    SetWindowTextW(editor.header_title, editor.original || !editor.template_cards.empty() ? L"编辑 Smart Actions" : L"创建 Smart Actions");
    editor.trigger_open = editor.action_open = false;
    trigger_visibility(editor);
    action_visibility(editor);
    SetFocus(GetDlgItem(editor.window, action_menu_id));
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool complete_template_target(Editor& editor, std::wstring_view path) {
    if (editor.template_target < 0) return false;
    std::wstring error;
    if (!resolve_template_target(editor.template_cards, editor.steps, static_cast<size_t>(editor.template_target), path, error)) {
        if (editor.detail_status) SetWindowTextW(editor.detail_status, error.c_str());
        return false;
    }
    editor.template_target = -1;
    const int row = editor.edit_index;
    editor.edit_index = -1;
    close_detail(editor);
    fill_steps(editor, row);
    hint(editor, complete_steps(editor.steps) ? L"模板所需内容已设置，可保存或试运行。" : L"已设置目标。请继续填写其余标有“请选择”的步骤。");
    return true;
}

void open_template_target(Editor& editor, std::wstring_view step) {
    const auto separator = step.find(L'|');
    if (!template_pending(step) || separator == step.npos) return;
    const auto index = unsigned_value(step.substr(16, separator - 16));
    if (!index || *index >= editor.template_cards.size()) return;
    const auto& card = editor.template_cards[*index];
    if (card.target == TemplateTarget::unsupported) { hint(editor, card.error.c_str()); return; }
    editor.template_target = static_cast<int>(*index);
    open_detail(editor, card.target == TemplateTarget::application ? Detail::application : Detail::system);
    SetWindowTextW(editor.header_title, (L"选择 " + card.label).c_str());
}

void confirm_detail(Editor& editor) {
    const auto detail = editor.detail;
    if (detail == Detail::shortcut) {
        const auto chord = read_text(editor.detail_value);
        if (chord.empty()) return;
        close_detail(editor);
        select(editor.trigger_type, 1);
        SetWindowTextW(editor.trigger_key, chord.c_str());
        update_trigger(editor, true);
    } else if (detail == Detail::device) {
        const int choice = editor.detail_choice, profile = editor.detail_profile;
        if (choice < 0 || profile < 0 || static_cast<size_t>(profile) >= editor.profiles.size()) return;
        close_detail(editor);
        select(editor.trigger_type, 0);
        select(editor.trigger_profile, profile);
        show_trigger_fields(editor);
        select(editor.trigger_choice, choice);
        update_trigger(editor, true);
    } else if (detail == Detail::ring) {
        close_detail(editor);
        editor.trigger = {L"ring-library", L""};
        SetWindowTextW(editor.trigger_summary, L"Actions Ring");
        trigger_visibility(editor); action_visibility(editor); update_save(editor);
    } else if (detail == Detail::delay) {
        const auto value = read_text(editor.detail_value);
        wchar_t* end = nullptr;
        const double seconds = std::wcstod(value.c_str(), &end);
        if (value.empty() || end != value.c_str() + value.size() || !std::isfinite(seconds) || seconds < 0 || seconds > 30) {
            EnableWindow(editor.detail_confirm, FALSE);
            return;
        }
        const auto delay = std::to_wstring(static_cast<unsigned>(std::round(seconds * 1000.0)));
        select(editor.action_type, 4);
        SetWindowTextW(editor.action_value, delay.c_str());
        add_step(editor);
    } else if (detail == Detail::keys) {
        if (editor.recording || editor.recorded_steps.empty()) return;
        select(editor.action_type, 1);
        add_step(editor);
    } else if (detail == Detail::system) {
        const int index = selected(editor.detail_list, LB_GETCURSEL);
        if (index < 0 || static_cast<size_t>(index) >= editor.detail_items.size()) return;
        const auto choice = editor.detail_items[index];
        std::wstring parameter_action;
        if (std::wstring_view(system_actions[choice].code).starts_with(L"@Open")) {
            const auto path = read_text(editor.system_parameter);
            if (!valid_system_parameter(system_actions[choice].code, path)) return;
            if (editor.template_target >= 0) { complete_template_target(editor, path); return; }
            parameter_action = open_action(path);
        }
        close_detail(editor);
        select(editor.action_type, 3);
        show_action_fields(editor);
        select(editor.action_choice, static_cast<int>(choice));
        editor.application_step = std::move(parameter_action);
        add_step(editor);
    } else if (detail == Detail::application_action) {
        const auto selected_action = selected(editor.detail_list, LB_GETCURSEL);
        if (selected_action < 0 || static_cast<size_t>(selected_action) >= editor.detail_items.size() || editor.selected_application < 0) return;
        const auto& app = editor.applications[editor.selected_application];
        const auto path = app.path;
        const auto sequence = application_action(app, application_actions(app)[editor.detail_items[selected_action]].code);
        close_detail(editor);
        select(editor.action_type, 0);
        show_action_fields(editor);
        SetWindowTextW(editor.action_value, path.c_str());
        editor.application_step = sequence;
        add_step(editor);
    } else if (detail == Detail::name || detail == Detail::tag) {
        const auto value = read_text(editor.detail_value);
        if (detail == Detail::name) {
            const auto description = read_text(editor.detail_description);
            if (trim(value).empty() || value.size() > 24 || description.size() > 80) return;
            editor.description = description;
        }
        close_detail(editor);
        SetWindowTextW(detail == Detail::name ? editor.name : editor.tag, value.c_str());
        if (detail == Detail::name) save(editor);
        else SetWindowTextW(GetDlgItem(editor.window, add_tag_id), value.empty() ? L"＋ 添加标签" : value.c_str());
    }
}

void fill_steps(Editor& editor, int index = -1) {
    SendMessageW(editor.list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < editor.steps.size(); ++i) {
        auto name = std::to_wstring(i + 1) + L".  " + step_label(editor.steps[i]);
        SendMessageW(editor.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    if (index >= 0) SendMessageW(editor.list, LB_SETCURSEL, index, 0);
    action_visibility(editor);
    if (index >= 0 && !editor.step_dragging) {
        const int row_top = editor.steps_top + index * smart_steps::row_height;
        if (row_top < smart_steps::viewport_top) scroll_editor(editor, row_top - smart_steps::viewport_top);
        else if (row_top + smart_steps::row_height > editor.steps_bottom)
            scroll_editor(editor, row_top + smart_steps::row_height - editor.steps_bottom);
    }
    if (editor.step_hover >= static_cast<int>(editor.steps.size())) editor.step_hover = -1;
    if (const auto preview = GetDlgItem(editor.window, preview_id))
        EnableWindow(preview, complete_steps(editor.steps));
    update_save(editor);
}

bool contains_text(std::wstring_view text, std::wstring_view query) {
    if (query.empty()) return true;
    return std::ranges::search(text, query, [](wchar_t left, wchar_t right) {
        return CompareStringOrdinal(&left, 1, &right, 1, TRUE) == CSTR_EQUAL;
    }).begin() != text.end();
}

void close_card_menu(Editor& editor) {
    for (const auto control : editor.card_menu) DestroyWindow(control);
    editor.card_menu.clear();
    editor.card_menu_entry = -1;
}

void place_card_control(Editor& editor, HWND control, int x, int y, int width, int height) {
    const int top = (editor.show_education ? 315 : 193) + (editor.tag_rows - 1) * 40;
    MoveWindow(control, px(editor, x), px(editor, y), px(editor, width), px(editor, height), FALSE);
    const auto clip = CreateRectRgn(0, px(editor, std::max(0, top - y)), px(editor, width),
                                    px(editor, std::min(height, 755 - y)));
    if (!SetWindowRgn(control, clip, FALSE)) DeleteObject(clip);
    show_control(control, y < 755 && y + height > top);
}

void layout_cards(Editor& editor) {
    const int top = (editor.show_education ? 315 : 193) + (editor.tag_rows - 1) * 40;
    const int columns = MulDiv(editor.window_width, 96, editor.dpi) <= 1130 ? 2 : 3;
    const int grid_width = columns == 3 ? 840 : 550;
    const int left = (canvas_width + 238 - grid_width) / 2;
    const int rows = static_cast<int>((editor.visible_entries.size() + columns - 1) / columns);
    editor.cards_scroll = std::clamp(editor.cards_scroll, 0, std::max(0, rows * 350 - 30 - (755 - top)));
    for (size_t index = 0; index < editor.visible_entries.size(); ++index) {
        const int x = left + static_cast<int>(index % columns) * 290;
        const int y = top + static_cast<int>(index / columns) * 350 - editor.cards_scroll;
        place_card_control(editor, editor.cards[index * 3], x, y, 260, 320);
        place_card_control(editor, editor.cards[index * 3 + 1], x + 20, y + 280, 170, 24);
        place_card_control(editor, editor.cards[index * 3 + 2], x + 214, y + 18, 28, 32);
        if (editor.templates_view) show_control(editor.cards[index * 3 + 2], false);
    }
    RedrawWindow(editor.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void fill_records(Editor& editor) {
    close_card_menu(editor);
    for (const auto control : editor.cards) DestroyWindow(control);
    editor.cards.clear();
    editor.visible_entries.clear();
    const auto filter = read_text(editor.filter);
    const auto tag_filter = editor.selected_tag < editor.filter_tags.size() ? editor.filter_tags[editor.selected_tag] : L"";
    const auto& entries = displayed_entries(editor);
    const auto tag_tokens = [](std::wstring_view raw) {
        std::vector<std::wstring> tokens;
        size_t pos = 0;
        while (pos < raw.size()) {
            const auto next = raw.find_first_of(L",，、/|", pos);
            auto tok = trim(raw.substr(pos, next == raw.npos ? next : next - pos));
            if (!tok.empty()) tokens.emplace_back(tok);
            if (next == raw.npos) break;
            pos = next + 1;
        }
        return tokens;
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        auto title = entry.name + L"  ·  " + entry.description + L"  ·  " + trigger_name(entry.trigger);
        if (!entry.tag.empty()) title += L"  ·  " + entry.tag;
        if (!contains_text(title, filter)) continue;
        if (!tag_filter.empty() && tag_filter != L"全部") {
            const auto tokens = tag_tokens(entry.tag);
            const bool matched = std::ranges::any_of(tokens, [&](const auto& t) { return same(t, tag_filter); }) ||
                (editor.templates_view && contains_text(entry.tag, tag_filter));
            if (!matched) continue;
        }
        editor.visible_entries.push_back(i);
    }
    const int order = selected(editor.sort);
    if (order == 1) std::stable_sort(editor.visible_entries.begin(), editor.visible_entries.end(), [&](size_t a, size_t b) {
        const auto& left = entries[a].name;
        const auto& right = entries[b].name;
        return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                     static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    });
    else if (order == 2) std::stable_sort(editor.visible_entries.begin(), editor.visible_entries.end(), [&](size_t a, size_t b) {
        return entries[a].enabled > entries[b].enabled;
    });
    else if (order == 3) std::stable_sort(editor.visible_entries.begin(), editor.visible_entries.end(), [&](size_t a, size_t b) {
        return entries[a].usage_count > entries[b].usage_count;
    });
    else if (!editor.templates_view) std::reverse(editor.visible_entries.begin(), editor.visible_entries.end());
    const bool populated = !editor.visible_entries.empty();
    for (size_t i = 0; i < editor.visible_entries.size(); ++i) {
        const auto& entry = entries[editor.visible_entries[i]];
        editor.cards.push_back(button(editor, entry.name.c_str(), 0, 0, 260,
                                       static_cast<WORD>(card_id + i * 3), 320));
        editor.cards.push_back(button(editor, editor.templates_view ? L"获取" : entry.enabled ? L"停用" : L"激活", 0, 0, 170,
                                       static_cast<WORD>(card_id + i * 3 + 1), 24));
        editor.cards.push_back(button(editor, L"更多", 0, 0, 28,
                                       static_cast<WORD>(card_id + i * 3 + 2), 32));
        // Win32 creates child windows at the bottom of the sibling Z order.
        // These owner-drawn controls overlap the card's full-size background.
        SetWindowPos(editor.cards[i * 3 + 1], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(editor.cards[i * 3 + 2], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetWindowTextW(editor.empty, filter.empty() ? L"点击「创建」以添加您的第一个 Smart Action。"
                                              : L"未找到匹配的 Smart Actions。");
    show_control(editor.empty, !populated);
    layout_cards(editor);
    hint(editor, L"");
}

void window_header(Editor& editor, const wchar_t* caption) {
    button(editor, L"返回", 43, 35, 40, back_id, 42);
    editor.header_title = widget(editor, L"STATIC", caption, SS_LEFT, 86, 30, 830, 60);
    if (editor.title) SendMessageW(editor.header_title, WM_SETFONT, reinterpret_cast<WPARAM>(editor.title), FALSE);
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
        smart_text(dc, L"搜索", 2, &bounds, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(window, nullptr, TRUE);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, filter_cue, id);
    return result;
}

void list_screen(Editor& editor) {
    editor.editing = false;
    editor.list = nullptr;
    editor.filter = editor.status = nullptr;
    editor.cards.clear();
    editor.card_menu.clear();
    editor.cards_scroll = 0;
    window_header(editor, L"Smart Actions");
    load_templates(editor);
    button(editor, L"＋ 创建", 1361, 37, 88, create_id, 48);
    button(editor, L"导入", 1248, 37, 88, import_id, 48);
    button(editor, L"模板", 50, 414, 86, templates_id, 40);
    button(editor, L"管理", 50, 462, 86, manage_id, 40);
    widget(editor, L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS, 933, 37, 280, 49, search_frame_id);
    editor.filter = widget(editor, L"EDIT", L"", WS_TABSTOP | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
                           948, 51, 227, 23, filter_id);
    SetWindowPos(editor.filter, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowSubclass(editor.filter, filter_cue, 1, 0);
    if (editor.show_education) {
        widget(editor, L"STATIC", L"", SS_OWNERDRAW, 446, 122, 840, 100, education_id);
        widget(editor, L"STATIC", editor.templates_view ? L"在此选项卡上，您可以找到 Smart Actions 模板。选择“获取”，选择所需应用程序或文件，再按需要自定义操作并保存。" : L"在此选项卡上，可以管理您的 Smart Actions。使用“创建”按钮添加动作，设置触发器并组合操作。您也可以导入或导出 Smart Actions 文件，与朋友和同事分享自己的动作。",
               SS_LEFT, 466, 137, 770, 73, education_text_id);
        button(editor, L"关闭提示", 1248, 126, 32, education_close_id, 32);
    }
    const int filter_top = editor.show_education ? 242 : 120;
    editor.filter_tags = {L"全部"};
    editor.tag_buttons.clear();
    editor.selected_tag = 0;
    editor.tag_rows = 1;
    if (editor.templates_view) editor.filter_tags = {L"全部", L"生产力", L"休闲", L"AI", L"会议", L"设计师", L"开发者", L"热门"};
    else for (const auto& entry : editor.entries) {
        size_t pos = 0;
        while (pos < entry.tag.size()) {
            const auto next = entry.tag.find_first_of(L",，、/|", pos);
            const auto tok = std::wstring(trim(std::wstring_view(entry.tag).substr(pos, next == entry.tag.npos ? next : next - pos)));
            if (!tok.empty() && std::ranges::none_of(editor.filter_tags, [&](const auto& existing) { return same(existing, tok); }))
                editor.filter_tags.push_back(tok);
            if (next == entry.tag.npos) break;
            pos = next + 1;
        }
    }
    const auto dc = GetDC(editor.window);
    int tag_x = 446, tag_y = filter_top + 8;
    for (size_t i = 0; i < editor.filter_tags.size(); ++i) {
        const auto measured = native_typography::measureText(dc, editor.font, editor.filter_tags[i]).Width;
        const int width = i == 0 ? 54 : std::clamp(MulDiv(static_cast<int>(std::ceil(measured)), canvas_width, editor.window_width) + 24, 54, 250);
        if (tag_x + width > 1036) { tag_x = 446; tag_y += 40; ++editor.tag_rows; }
        editor.tag_buttons.push_back(button(editor, editor.filter_tags[i].c_str(), tag_x, tag_y, width,
                                             static_cast<WORD>(tag_filter_id + i), 32));
        tag_x += width + 8;
    }
    ReleaseDC(editor.window, dc);
    editor.sort = combo(editor, 1118, filter_top, 168, sort_id);
    for (const auto title : {L"最近的", L"字母顺序", L"激活状态", L"最常用"}) add_choice(editor.sort, title);
    SendMessageW(editor.sort, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(editor, 46));
    select(editor.sort, 0);
    editor.empty = widget(editor, L"STATIC", L"", SS_CENTER, 446, 687, 840, 25);
    editor.status = label(editor, L"", 446, 784, 815);
    fill_records(editor);
}

void editor_screen(Editor& editor) {
    editor.editing = true;
    editor.trigger_open = editor.action_open = false;
    editor.trigger_categories.fill(nullptr);
    editor.action_categories.fill(nullptr);
    editor.if_heading = editor.then_heading = nullptr;
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
    editor.if_heading = label(editor, L"如果…", 511, 325, 480, true);
    editor.trigger_summary = button(editor, editor.trigger.key.empty() ? L"添加触发器" :
                                    trigger_name(editor.trigger).c_str(), 491, 360, 500, trigger_menu_id, 50);
    constexpr const wchar_t* trigger_labels[]{L"应用程序", L"设备", L"Actions Ring", L"系统", L"快捷键"};
    for (size_t i = 0; i < 5; ++i) {
        editor.trigger_categories[i] = button(editor, trigger_labels[i], 491, 409 + static_cast<int>(i) * 44,
                                               500, static_cast<WORD>(trigger_category_id + i), 44);
        if (i == 0 || i == 3) EnableWindow(editor.trigger_categories[i], FALSE);
    }
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
    editor.then_heading = label(editor, L"那么…", 511, 454, 480, true);
    button(editor, L"添加操作", 491, 490, 500, action_menu_id, 48);
    for (size_t i = 0; i < 5; ++i)
        editor.action_categories[i] = button(editor, step_names[i], 491, 538 + static_cast<int>(i) * 44,
                                              500, static_cast<WORD>(action_category_id + i), 44);
    button(editor, editor.original && !editor.original->tag.empty() ? editor.original->tag.c_str() : L"＋ 添加标签",
           491, 571, 150, add_tag_id, 30);
    editor.list = widget(editor, L"LISTBOX", L"操作步骤", WS_TABSTOP | LBS_NOTIFY |
                         LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                         491, 552, 500, 134, list_id);
    SendMessageW(editor.list, LB_SETITEMHEIGHT, 0, px(editor, smart_steps::row_height));
    SetWindowSubclass(editor.list, step_surface, 7, reinterpret_cast<DWORD_PTR>(&editor));
    editor.action_type = combo(editor, 491, 587, 150, action_type_id);
    for (auto title : step_names) add_choice(editor.action_type, title);
    editor.action_title = label(editor, L"", 650, 553, 341);
    editor.action_choice = combo(editor, 650, 587, 161, action_choice_id);
    editor.action_value = widget(editor, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                                 650, 587, 341, 44, action_value_id);
    editor.text_edit = button(editor, L"编辑文本", 650, 587, 341, text_edit_id, 44);
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
    editor.template_cards.clear();
    editor.template_target = -1;
    editor.original = std::move(entry);
    editor.description = editor.original ? editor.original->description : L"";
    editor.trigger = editor.original ? editor.original->trigger : Trigger{};
    editor.steps = editor.original ? entry_steps(*editor.original) : std::vector<std::wstring>{};
    editor.edit_index = -1;
    editor.editor_scroll = 0; editor.step_hover = -1;
    clear_widgets(editor.window);
    editor_screen(editor);
    InvalidateRect(editor.window, nullptr, TRUE);
    SetFocus(editor.trigger_summary);
}

void begin_template(Editor& editor, size_t index) {
    if (index >= std::size(smart_templates::definitions)) return;
    begin(editor);
    const auto& definition = smart_templates::definitions[index];
    SetWindowTextW(editor.name, std::wstring(definition.name).c_str());
    SetWindowTextW(editor.tag, std::wstring(definition.categories).c_str());
    SetWindowTextW(GetDlgItem(editor.window, add_tag_id), std::wstring(definition.categories).c_str());
    SetWindowTextW(editor.header_title, L"编辑 Smart Actions");
    editor.description = definition.description;
    for (const auto card : array_items(definition.cards)) {
        auto prepared = prepare_template_card(card);
        editor.steps.push_back(prepared.target == TemplateTarget::none ? prepared.step : template_placeholder(editor.template_cards.size(), prepared));
        editor.template_cards.push_back(std::move(prepared));
    }
    fill_steps(editor);
    hint(editor, complete_steps(editor.steps) ? L"模板已载入草稿，可编辑后保存。" : L"点击标有“请选择”的步骤设置应用程序或文件，完成后才能保存或试运行。");
}

void show_list(Editor& editor) {
    editor.entries = records(editor.config);
    clear_widgets(editor.window);
    list_screen(editor);
    InvalidateRect(editor.window, nullptr, TRUE);
    SetFocus(editor.cards.empty() ? editor.filter : editor.cards.front());
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
    const auto value = read_text(editor.text_editing ? editor.text_value : editor.action_value);
    std::wstring step;
    switch (type) {
    case 0: {
        const auto path = trim(value);
        if (path.empty() || path.find_first_of(L"\r\n") != path.npos) {
            hint(editor, L"请选择程序，或填写一行程序路径。");
            return;
        }
        step = editor.application_step.empty() ? open_action(path) : editor.application_step;
        editor.application_step.clear();
        if (step.empty()) { hint(editor, L"程序路径包含无法表示的动作分隔符。"); return; }
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
        if (std::wstring_view(system_actions[option].code).starts_with(L"@Open")) {
            if (editor.application_step.empty()) { hint(editor, L"请先选择有效的打开目标。"); return; }
            step = std::move(editor.application_step);
            editor.application_step.clear();
        } else step = system_actions[option].code;
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
    if (split_steps(sequence_text(steps)).size() > 1024) {
        hint(editor, L"最多可添加 1024 个操作，按键事件及间隔也计入。");
        return;
    }
    editor.steps = std::move(steps);
    editor.edit_index = -1;
    if (editor.text_editing) close_text_editor(editor);
    if (editor.detail != Detail::none) close_detail(editor);
    fill_steps(editor, index);
    show_action_fields(editor);
    hint(editor, L"操作已加入。");
}

void record_key(Editor& editor, std::wstring_view chord) {
    if (!editor.recording) editor.recorded_steps.clear();
    if (editor.recorded_steps.size() >= 1023) return;
    const auto now = GetTickCount64();
    if (editor.recording && editor.last_stroke) {
        const auto fixed = editor.detail == Detail::keys ? editor.detail_fixed : editor.fixed_delay;
        const auto value = editor.detail == Detail::keys ? read_text(editor.detail_delay) : L"0.021";
        const auto delay = SendMessageW(fixed, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? static_cast<ULONGLONG>(std::clamp(std::wcstod(value.c_str(), nullptr), 0.0, 30.0) * 1000.0 + 0.5)
            : std::min<ULONGLONG>(30000, now - editor.last_stroke);
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
    fill_key_records(editor);
}

void toggle_recording(Editor& editor) {
    if (editor.recording) {
        finish_recording(editor);
        SetWindowTextW(editor.record, L"开始录制");
        return;
    }
    if (editor.detail == Detail::keys && !editor.recording) {
        const auto text = read_text(editor.detail_delay);
        wchar_t* end = nullptr;
        const auto seconds = std::wcstod(text.c_str(), &end);
        if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(seconds) || seconds < 0 || seconds > 30) {
            SetWindowTextW(editor.detail_status, L"请输入 0 到 30 秒之间的固定延迟。");
            show_control(editor.detail_status, true);
            return;
        }
    }
    editor.recording = !editor.recording;
    SetWindowTextW(editor.record, editor.recording ? L"停止录制" : L"开始录制");
    if (editor.recording) {
        editor.recorded_steps.clear();
        editor.recorded_down.clear();
        editor.last_stroke = 0;
        SetWindowTextW(editor.action_value, L"");
        SetFocus(editor.detail == Detail::keys ? editor.detail_list : editor.action_value);
        hint(editor, L"录制中。完成后点击停止录制。");
    } else hint(editor, L"已停止录制。点击添加操作。");
    fill_key_records(editor);
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

void toggle_entry(Editor& editor, size_t index) {
    if (index >= editor.entries.size()) return;
    const auto old = editor.entries[index];
    if (old.trigger.key.empty()) {
        begin(editor, old);
        hint(editor, L"选择触发器并保存后即可激活。");
        return;
    }
    auto entry = old;
    auto config = editor.config;
    restore(config, old);
    entry.enabled = !old.enabled;
    if (entry.enabled) {
        const auto current = config.get(entry.trigger);
        if (current != entry.previous && current != entry.sequence &&
            MessageBoxW(editor.window, L"触发器的动作已更改。激活此 Smart Action 并替换当前动作？",
                        L"Smart Actions", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        entry.previous = current;
        config.set(entry.trigger, entry.sequence);
    }
    config.lines.push_back(entry_metadata(entry, entry_steps(old)));
    if (commit(editor, std::move(config))) {
        show_list(editor);
        show_reload_notice(editor);
    }
}

void duplicate_entry(Editor& editor, size_t index) {
    if (index >= editor.entries.size()) return;
    auto entry = editor.entries[index];
    const auto steps = entry_steps(entry);
    entry.name += L"（副本）";
    entry.trigger = {};
    entry.previous.reset();
    entry.enabled = false;
    auto config = editor.config;
    config.lines.push_back(entry_metadata(entry, steps));
    if (commit(editor, std::move(config))) show_list(editor);
}

void replace_document_string(std::wstring& document, std::wstring_view key, std::wstring_view value) {
    if (const auto existing = field(document, key); !existing.empty())
        document.replace(static_cast<size_t>(existing.data() - document.data()), existing.size(), quote(value));
    else if (const auto start = document.find(L'{'); start != document.npos)
        document.insert(start + 1, quote(key) + L":" + quote(value) + L",");
}

std::optional<std::wstring> export_document(const Entry& entry, std::wstring_view identifier) {
    const auto cards = export_cards(entry);
    if (!cards) return {};
    auto json = L"{\"id\":" + quote(identifier) + L",\"name\":" + quote(entry.name) +
        L",\"description\":" + quote(entry.description) + L",\"platform\":\"WIN\",\"originType\":\"CUSTOM\",\"state\":\"ACTIVE\","
        L"\"lastEditTimestamp\":\"0\",\"usageCount\":0,\"predefinedIdRef\":\"\",\"iconPath\":\"\","
        L"\"restrictedRegions\":[],\"specificRegions\":[],\"categories\":[],\"customCategories\":{\"categories\":[]},\"cards\":" + *cards + L"}";
    if (!entry.source_document.empty()) {
        std::wstring error;
        const auto source_steps = import_cards(entry.source_document, error);
        if (source_steps && sequence_text(*source_steps) == entry.sequence) {
            json = entry.source_document;
            replace_document_string(json, L"name", entry.name);
            replace_document_string(json, L"description", entry.description);
        }
    }
    return json;
}

void export_entry(Editor& editor, const Entry& entry) {
    const auto cards = export_cards(entry);
    if (!cards) { hint(editor, L"此宏含有尚不能准确转换为 Options+ 文件格式的动作，未导出。"); return; }
    wchar_t filename[MAX_PATH]{};
    auto name = entry.name;
    for (auto& ch : name) if (ch < 32 || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos) ch = L'_';
    name += L".json";
    std::wcsncpy(filename, name.c_str(), std::size(filename) - 1);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = editor.window;
    dialog.lpstrFilter = L"Smart Actions (*.json)\0*.json\0\0";
    dialog.lpstrDefExt = L"json";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = std::size(filename);
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    GUID id{};
    wchar_t identifier[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, identifier, std::size(identifier))) {
        hint(editor, L"无法创建导出标识。"); return;
    }
    const auto json = export_document(entry, std::wstring_view(identifier + 1, 36));
    hint(editor, json && write_bytes(filename, utf8(*json)) ? L"已导出 Smart Action。文件不包含设备触发器。" : L"导出文件写入失败。");
}

void import_entry(Editor& editor) {
    wchar_t filename[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = editor.window;
    dialog.lpstrFilter = L"Smart Actions (*.json)\0*.json\0\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = std::size(filename);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    const auto bytes = read_file(filename);
    if (!bytes) { hint(editor, L"无法读取导入文件。"); return; }
    auto json = wide(*bytes);
    if (json.starts_with(L'\ufeff')) json.erase(json.begin());
    if (json_string(field(json, L"platform")).value_or(L"WIN") != L"WIN") {
        hint(editor, L"此文件不是 Windows Smart Action，未导入。"); return;
    }
    std::wstring error;
    const auto steps = import_cards(json, error);
    if (!steps) { hint(editor, error.c_str()); return; }
    const auto name = json_string(field(json, L"name"));
    if (!name || name->empty() || name->size() > 60) { hint(editor, L"宏名称必须包含 1 到 60 个字符。"); return; }
    const auto sequence = sequence_text(*steps);
    const auto temporary = temp_path(editor.directory);
    if (temporary.empty()) { hint(editor, L"无法验证导入动作。"); return; }
    const bool valid = write_bytes(temporary, utf8(L"[hotkeys]\nCtrl+Alt+F10 = " + sequence + L"\n")) &&
                       check_mappings(temporary.c_str());
    DeleteFileW(temporary.c_str());
    if (!valid) { hint(editor, L"导入文件包含无法执行的动作，原配置已保留。"); return; }
    Entry entry{*name, {}, sequence, {L"ring-library", L""}, {}, {}, false, json};
    entry.description = json_string(field(json, L"description")).value_or(L"");
    auto config = editor.config;
    config.lines.push_back(entry_metadata(entry, *steps));
    if (commit(editor, std::move(config))) {
        show_list(editor);
        hint(editor, L"已导入。请打开该卡片，选择触发器后保存并激活。");
    }
}

void open_card_menu(Editor& editor, size_t visible) {
    close_card_menu(editor);
    if (visible >= editor.visible_entries.size()) return;
    editor.card_menu_entry = static_cast<int>(editor.visible_entries[visible]);
    RECT anchor{};
    GetWindowRect(editor.cards[visible * 3 + 2], &anchor);
    MapWindowPoints(nullptr, editor.window, reinterpret_cast<POINT*>(&anchor), 2);
    const int x = MulDiv(anchor.right, canvas_width, editor.window_width) - 168;
    const int y = std::min(590, MulDiv(anchor.bottom, canvas_width, editor.window_width));
    const wchar_t* labels[]{L"重命名", L"添加标签", L"重复", L"导出", L"移除"};
    for (unsigned i = 0; i < std::size(labels); ++i) {
        auto control = button(editor, labels[i], x, y + static_cast<int>(i) * 32, 168,
                              static_cast<WORD>(menu_rename_id + i), 32);
        SetWindowPos(control, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        editor.card_menu.push_back(control);
    }
    SetFocus(editor.card_menu.front());
}

void save(Editor& editor) {
    if (!complete_steps(editor.steps)) { hint(editor, L"请先为模板选择所需应用程序或文件，或移除未设置的步骤。"); return; }
    const auto raw_name = read_text(editor.name);
    const auto name = trim(raw_name);
    if (name.empty()) {
        open_detail(editor, Detail::name);
        return;
    }
    if (editor.trigger.key.empty()) editor.trigger.section = L"ring-library";
    if (editor.steps.empty()) { hint(editor, L"请添加至少一个操作。"); return; }
    auto config = editor.config;
    const bool same_original = editor.original && same_trigger(editor.original->trigger, editor.trigger);
    if (!editor.trigger.key.empty() && !same_original && std::ranges::any_of(editor.entries, [&](const Entry& item) {
            return !item.metadata.empty() && same_trigger(item.trigger, editor.trigger);
        })) {
        hint(editor, L"触发器已有 Smart Action。请在管理中编辑该动作。");
        return;
    }
    if (editor.original) restore(config, *editor.original);
    const auto previous = config.get(editor.trigger);
    const bool enabled = !editor.trigger.key.empty() &&
        (!editor.original || editor.original->trigger.key.empty() || editor.original->enabled);
    if (enabled && !same_original && previous &&
        MessageBoxW(editor.window, L"此触发器已有动作。替换后，移除本 Smart Action 时会恢复原动作。继续？",
                    L"Smart Actions", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    const auto sequence = sequence_text(editor.steps);
    if (enabled) config.set(editor.trigger, sequence);
    Entry entry{std::wstring(name), read_text(editor.tag), sequence, editor.trigger, previous, {}, enabled};
    entry.description = editor.description;
    if (editor.original) {
        entry.source_document = editor.original->source_document;
        entry.usage_count = editor.original->usage_count;
    }
    config.lines.push_back(entry_metadata(entry, editor.steps));
    if (commit(editor, std::move(config))) {
        editor.templates_view = false;
        show_list(editor);
        show_reload_notice(editor);
    }
}

void preview(Editor& editor) {
    if (editor.preview_result && editor.preview_result->exit_code.load() == STILL_ACTIVE) return;
    if (!complete_steps(editor.steps)) { hint(editor, L"请先完成所有操作的设置。"); return; }
    if (editor.original) {
        for (auto& item : editor.entries) {
            if (same(item.name, editor.original->name) && same_trigger(item.trigger, editor.original->trigger)) {
                ++item.usage_count;
                break;
            }
        }
        ++editor.original->usage_count;
    }
    const auto path = temp_path(editor.directory);
    if (path.empty()) { hint(editor, L"无法创建临时试运行文件。"); return; }
    const auto text = utf8(L"[hotkeys]\nCtrl+Alt+F10 = " + sequence_text(editor.steps) + L"\n");
    if (!write_bytes(path, text) || play_actions(path.c_str(), true) != 0) {
        DeleteFileW(path.c_str());
        hint(editor, L"当前动作无法试运行。");
        return;
    }
    editor.preview_result = std::make_shared<PreviewResult>();
    const auto result = editor.preview_result;
    std::thread([path, result] {
        Sleep(3000);
        const int exit_code = play_actions(path.c_str(), false);
        DeleteFileW(path.c_str());
        result->exit_code.store(static_cast<DWORD>(exit_code));
    }).detach();
    SetTimer(editor.window, preview_timer, 100, nullptr);
    EnableWindow(GetDlgItem(editor.window, preview_id), FALSE);
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
    if (item.hwndItem == editor.detail_list && (editor.detail == Detail::system || editor.detail == Detail::application_action)) {
        FillRect(dc, &bounds, editor.fill);
        if (bounds.bottom - bounds.top > px(editor, 38)) {
            const auto brush = CreateSolidBrush(RGB(51,51,51));
            const RECT parameters{bounds.left, bounds.top + px(editor, 38), bounds.right, bounds.bottom};
            FillRect(dc, &parameters, brush); DeleteObject(brush);
            bounds.bottom = bounds.top + px(editor, 38);
        }
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto color = selected || static_cast<int>(item.itemID) == editor.detail_hover ? accent : foreground;
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), static_cast<float>(px(editor, 1)));
        graphics.DrawEllipse(&pen, bounds.left + px(editor, 16), bounds.top + px(editor, 10), px(editor, 18), px(editor, 18));
        if (selected) {
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(accent), GetGValue(accent), GetBValue(accent)));
            graphics.FillEllipse(&brush, bounds.left + px(editor, 20), bounds.top + px(editor, 14), px(editor, 10), px(editor, 10));
        }
        bounds.left += px(editor, 50);
        const auto font = SelectObject(dc, editor.font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
        smart_text(dc, title.c_str(), static_cast<int>(length), &bounds, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, font);
        return;
    }
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
    smart_text(dc, title.data(), static_cast<int>(length), &bounds,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

void draw_application_tile(Editor& editor, const DRAWITEMSTRUCT& item) {
    const auto dc = item.hDC;
    FillRect(dc, &item.rcItem, editor.fill);
    const auto pen = CreatePen(PS_SOLID, 1, RGB(42,42,42));
    const auto old_pen = SelectObject(dc, pen);
    MoveToEx(dc, item.rcItem.right - 1, 0, nullptr); LineTo(dc, item.rcItem.right - 1, item.rcItem.bottom);
    MoveToEx(dc, 0, item.rcItem.bottom - 1, nullptr); LineTo(dc, item.rcItem.right, item.rcItem.bottom - 1);
    SelectObject(dc, old_pen); DeleteObject(pen);
    if (item.CtlID == application_tile_id) smart_symbol(editor, dc, 0xe903, px(editor, 48), px(editor, 32), px(editor, 48), accent);
    else {
        const size_t index = item.CtlID - application_tile_id - 1;
        if (index < editor.detail_items.size()) {
            const auto& app = editor.applications[editor.detail_items[index]];
            if (app.icon) DrawIconEx(dc, px(editor, 48), px(editor, 32), app.icon, px(editor, 48), px(editor, 48), 0, nullptr, DI_NORMAL);
        }
    }
    RECT title{px(editor, 8), px(editor, 96), px(editor, 136), px(editor, 134)};
    const auto font = SelectObject(dc, editor.font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, foreground);
    const auto text = read_text(item.hwndItem);
    smart_text(dc, text.c_str(), -1, &title, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, font);
    if (item.itemState & ODS_FOCUS) {
        const auto focus_pen = CreatePen(PS_SOLID, std::max(1, px(editor, 1)), accent);
        const auto previous = SelectObject(dc, focus_pen);
        const auto brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, 2, 2, item.rcItem.right - 2, item.rcItem.bottom - 2, px(editor, 8), px(editor, 8));
        SelectObject(dc, brush); SelectObject(dc, previous); DeleteObject(focus_pen);
    }
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
    smart_text(item.hDC, text.data(), static_cast<int>(length), &area,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(item.hDC, font);
}

void draw_macro_card(Editor& editor, const DRAWITEMSTRUCT& item) {
    const size_t index = (item.CtlID - card_id) / 3;
    const unsigned part = (item.CtlID - card_id) % 3;
    if (index >= editor.visible_entries.size()) return;
    const auto& entry = displayed_entries(editor)[editor.visible_entries[index]];
    const auto dc = item.hDC;
    RECT box = item.rcItem;
    const auto ink = entry.enabled ? foreground : RGB(151, 151, 151);
    // The rounded card leaves rectangular child-window corners uncovered.
    // Clear those pixels before drawing the card, including focus-only redraws.
    if (part == 0) FillRect(dc, &box, editor.backdrop);
    const auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, part == 1 ? RGB(15, 15, 15) : surface);
    SetDCPenColor(dc, part == 0 ? background : part == 1 ? RGB(15, 15, 15) : surface);
    if (part == 0) {
        RoundRect(dc, box.left, box.top, box.right, box.bottom, px(editor, 16), px(editor, 16));
        RECT footer{box.left + 1, box.bottom - px(editor, 56), box.right - 1, box.bottom - 1};
        SetDCBrushColor(dc, RGB(15, 15, 15));
        SetDCPenColor(dc, RGB(15, 15, 15));
        Rectangle(dc, footer.left, footer.top, footer.right, footer.bottom);
        const auto trigger_icon = same(entry.trigger.section, L"ring") || same(entry.trigger.section, L"ring-library") ? 0xe997 :
            same(entry.trigger.section, L"hotkeys") ? 0xe959 : 0xe966;
        smart_symbol(editor, dc, static_cast<wchar_t>(trigger_icon), box.left + px(editor, 20),
                     box.top + px(editor, 20), px(editor, 28), ink);
        SetDCPenColor(dc, RGB(170, 170, 170));
        MoveToEx(dc, box.left + px(editor, 60), box.top + px(editor, 24), nullptr);
        LineTo(dc, box.left + px(editor, 60), box.top + px(editor, 44));
        const auto steps = entry_steps(entry);
        for (size_t n = 0; n < std::min<size_t>(steps.size(), 3); ++n)
            smart_symbol(editor, dc, action_symbol(steps[n]), box.left + px(editor, 70 + static_cast<int>(n) * 33),
                         box.top + px(editor, 20), px(editor, 28), ink);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, foreground);
        auto old_font = SelectObject(dc, editor.card_title);
        RECT title{box.left + px(editor, 20), box.top + px(editor, 90), box.right - px(editor, 20), box.top + px(editor, 174)};
        smart_card_text(dc, entry.name.c_str(), title,
                        DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX, entry.enabled);
        SelectObject(dc, editor.font);
        const auto description = entry.description.empty() ?
            (entry.trigger.key.empty() ? L"尚未选择触发器" : trigger_name(entry.trigger)) : entry.description;
        RECT details{box.left + px(editor, 20), box.top + px(editor, 188), box.right - px(editor, 20), box.top + px(editor, 230)};
        smart_card_text(dc, description.c_str(), details,
                        DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX, entry.enabled);
        if (!entry.tag.empty()) {
            details.top = box.top + px(editor, 236);
            details.bottom = box.top + px(editor, 260);
            SetTextColor(dc, RGB(198, 198, 198));
            smart_card_text(dc, entry.tag.c_str(), details, DT_SINGLELINE | DT_END_ELLIPSIS, entry.enabled);
        }
        if (steps.size() > 3) {
            const auto extra = L"+" + std::to_wstring(steps.size() - 3);
            RECT count{box.left + px(editor, 168), box.top + px(editor, 20), box.left + px(editor, 210), box.top + px(editor, 48)};
            smart_card_text(dc, extra.c_str(), count, DT_SINGLELINE | DT_VCENTER, entry.enabled);
        }
        SelectObject(dc, old_font);
    } else if (part == 1) {
        FillRect(dc, &box, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (editor.templates_view) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, accent);
            const auto font = SelectObject(dc, editor.font);
            smart_text(dc, L"获取", -1, &box, DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, font);
            SelectObject(dc, old_pen); SelectObject(dc, old_brush);
            return;
        }
        SetDCBrushColor(dc, entry.enabled ? accent : RGB(102, 102, 102));
        SetDCPenColor(dc, entry.enabled ? accent : RGB(102, 102, 102));
        RoundRect(dc, 0, px(editor, 4), px(editor, 28), px(editor, 20), px(editor, 16), px(editor, 16));
        SetDCBrushColor(dc, surface);
        SetDCPenColor(dc, surface);
        const int thumb = px(editor, entry.enabled ? 14 : 2);
        Ellipse(dc, thumb, px(editor, 6), thumb + px(editor, 12), px(editor, 18));
        RECT caption = box;
        caption.left += px(editor, 36);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        const auto font = SelectObject(dc, editor.font);
        smart_text(dc, entry.enabled ? L"激活" : L"停用", -1, &caption, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, font);
    } else {
        FillRect(dc, &box, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        smart_symbol(editor, dc, 0xe945, 0, 0, px(editor, 28), foreground);
    }
    if (item.itemState & ODS_FOCUS) {
        SetDCPenColor(dc, accent);
        SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, box.left + 1, box.top + 1, box.right - 1, box.bottom - 1, px(editor, 14), px(editor, 14));
    }
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void draw_button(Editor& editor, const DRAWITEMSTRUCT& item) {
    const auto id = item.CtlID;
    const bool education = id == education_id || id == education_close_id;
    const bool menu = id >= menu_rename_id && id <= menu_remove_id;
    const bool trigger_category = id >= trigger_category_id && id < trigger_category_id + 5;
    const bool action_category = id >= action_category_id && id < action_category_id + 5;
    const bool category = trigger_category || action_category;
    const bool device_choice = id >= device_choice_id && id < device_choice_id + 5;
    const bool profile = id >= device_profile_id && id < device_profile_id + editor.profiles.size();
    const bool tag_filter = id >= tag_filter_id && id < tag_filter_id + editor.filter_tags.size();
    const bool header = id == back_id || id == close_id || id == minimize_id;
    const bool highlight = id == create_id || id == save_id || (id == manage_id && !editor.templates_view) || (id == templates_id && editor.templates_view) ||
        (id == apply_step_id && editor.text_editing) || id == detail_confirm_id ||
        (device_choice && static_cast<int>(id - device_choice_id) == editor.detail_choice) ||
        (tag_filter && id - tag_filter_id == editor.selected_tag);
    const bool card = id == trigger_menu_id || id == action_menu_id;
    const bool expanded_card = (id == trigger_menu_id && editor.trigger_open) || (id == action_menu_id && editor.action_open);
    const bool name_cancel = editor.detail == Detail::name && id == detail_cancel_id;
    const bool search = id == search_frame_id;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = education || expanded_card ? accent : name_cancel ? surface : profile ? background : menu || category ? surface : disabled ? RGB(30, 30, 30)
        : highlight ? (pressed ? RGB(4, 186, 171) : accent)
        : header ? (pressed ? RGB(55, 55, 55) : background)
        : search || card ? (pressed ? RGB(37, 37, 37) : surface)
        : id == preview_id && disabled ? RGB(30, 30, 30) : RGB(37, 37, 37);
    const COLORREF ink = education ? surface : expanded_card ? foreground : disabled ? RGB(130, 130, 130)
        : highlight ? RGB(14, 28, 27) :
        (id == trigger_menu_id && editor.trigger_open) || (id == action_menu_id && editor.action_open) ? accent : foreground;
    const auto& bounds = item.rcItem;
    FillRect(item.hDC, &bounds, editor.backdrop);
    if (fill != background) {
        const auto brush = CreateSolidBrush(fill);
        const auto pen = CreatePen(PS_SOLID, 1, name_cancel ? RGB(102,102,102) : (search || card) ? RGB(45, 45, 45) : fill);
        const auto old_brush = SelectObject(item.hDC, brush);
        const auto old_pen = SelectObject(item.hDC, pen);
        const auto radius = px(editor, card ? 16 : category ? 0 : 8);
        RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
        if (id == action_menu_id && !editor.steps.empty())
            Rectangle(item.hDC, bounds.left, bounds.top, bounds.right, bounds.top + radius / 2);
        SelectObject(item.hDC, old_pen);
        SelectObject(item.hDC, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }
    if (id == education_id) return;
    if (id == education_close_id) {
        Gdiplus::Graphics graphics(item.hDC);
        draw_native_icon(graphics, L"close", px(editor, 7), px(editor, 7), px(editor, 18), surface);
        return;
    }
    if (profile) {
        const auto index = id - device_profile_id;
        const auto& name = editor.profiles[index];
        Gdiplus::Graphics graphics(item.hDC);
        const int image_index = name.find(L"firefox") != name.npos ? 0 : name.find(L"chrome") != name.npos ? 1 :
            name.find(L"wps") != name.npos ? 2 : -1;
        if (same(name, L"global")) smart_symbol(editor, item.hDC, 0xe953, px(editor, 10), px(editor, 10), px(editor, 28), foreground);
        else if (image_index >= 0 && editor.app_images[image_index])
            graphics.DrawImage(editor.app_images[image_index].get(), px(editor, 10), px(editor, 10), px(editor, 28), px(editor, 28));
        else {
            RECT text = bounds;
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, foreground);
            smart_text(item.hDC, profile_name(name).c_str(), -1, &text, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        if (static_cast<int>(index) == editor.detail_profile) {
            const auto pen = CreatePen(PS_SOLID, std::max(1, px(editor, 2)), accent);
            const auto previous = SelectObject(item.hDC, pen);
            MoveToEx(item.hDC, px(editor, 8), bounds.bottom - px(editor, 3), nullptr);
            LineTo(item.hDC, bounds.right - px(editor, 8), bounds.bottom - px(editor, 3));
            SelectObject(item.hDC, previous);
            DeleteObject(pen);
        }
        return;
    }
    if ((item.itemState & ODS_FOCUS) && !disabled) {
        RECT focus = bounds;
        const int inset = std::max(1, px(editor, 3));
        InflateRect(&focus, -inset, -inset);
        const int radius = std::max(px(editor, 4), px(editor, card ? 18 : 9) - inset * 2);
        const auto old_brush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
        const auto pen = CreatePen(PS_SOLID, std::max(1, px(editor, 1)), accent);
        const auto old_pen = SelectObject(item.hDC, pen);
        if (highlight) {
            const auto contrast = CreatePen(PS_SOLID, std::max(3, px(editor, 3)), background);
            SelectObject(item.hDC, contrast);
            RoundRect(item.hDC, focus.left, focus.top, focus.right, focus.bottom, radius, radius);
            SelectObject(item.hDC, pen);
            DeleteObject(contrast);
        }
        RoundRect(item.hDC, focus.left, focus.top, focus.right, focus.bottom, radius, radius);
        SelectObject(item.hDC, old_pen);
        SelectObject(item.hDC, old_brush);
        DeleteObject(pen);
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
    if (category) {
        constexpr wchar_t triggers[]{0xe98c, 0xe966, 0xe997, 0xe979, 0xe959};
        constexpr wchar_t actions[]{0xe98c, 0xe959, 0xe998, 0xe979, 0xe999};
        smart_symbol(editor, item.hDC, trigger_category ? triggers[id - trigger_category_id] : actions[id - action_category_id],
                     px(editor, 20), px(editor, 7), px(editor, 30), ink);
        area.left += px(editor, 65);
    }
    if (menu) {
        constexpr wchar_t icons[]{0xe94a, 0xe903, 0xe948, 0xe99b, 0xe980};
        smart_symbol(editor, item.hDC, icons[id - menu_rename_id], px(editor, 10), px(editor, 5), px(editor, 22), foreground);
        area.left += px(editor, 40);
    }
    if (card || device_choice) { area.left += px(editor, device_choice ? 12 : 20); area.right -= px(editor, device_choice ? 10 : 0); }
    if (id == action_menu_id) {
        Gdiplus::Graphics graphics(item.hDC);
        draw_native_icon(graphics, L"add", px(editor, 20), px(editor, 14), px(editor, 20), ink);
        area.left = bounds.left + px(editor, 55);
    }
    if (id == manage_id) area.left += px(editor, 42);
    auto title = read_text(item.hwndItem);
    smart_text(item.hDC, title.c_str(), static_cast<int>(title.size()), &area,
              (device_choice ? DT_WORDBREAK : DT_SINGLELINE) | (card || category || device_choice || menu || id == manage_id ? DT_LEFT : DT_CENTER) |
              DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(item.hDC, old_font);
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
    if (message == WM_TIMER && parameter == preview_timer && editor->preview_result) {
        const auto code = editor->preview_result->exit_code.load();
        if (code == STILL_ACTIVE) return 0;
        KillTimer(window, preview_timer);
        editor->preview_result.reset();
        EnableWindow(GetDlgItem(window, preview_id), complete_steps(editor->steps));
        hint(*editor, code == 0 ? L"试运行完成。" : code == 4 ? L"试运行已取消：目标窗口发生切换。" :
            code == 2 ? L"试运行失败：动作无效或尚不支持。" : L"试运行失败：应用窗口操作或输入发送未成功，后续动作已停止。");
        return 0;
    }
    if (message == WM_ACTIVATE && LOWORD(parameter) == WA_INACTIVE) {
        if (editor->recording) finish_recording(*editor);
        if (editor->step_press >= 0) end_step_drag(*editor, true);
    }
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
        if (editor->detail != Detail::none && editor->detail != Detail::device) {
            RECT panel = editor->detail == Detail::shortcut ? RECT{552,290,942,630} :
                editor->detail == Detail::keys ? RECT{389,234,1105,718} :
                editor->detail == Detail::system || editor->detail == Detail::application_action ? RECT{531,226,963,726} :
                editor->detail == Detail::application ? RECT{457,122,1037,850} :
                editor->detail == Detail::ring ? RECT{547,223,947,626} :
                editor->detail == Detail::name ? RECT{495,251,999,597} : RECT{552,309,942,563};
            const auto brush = CreateSolidBrush(surface);
            const auto old_brush = SelectObject(dc, brush);
            const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, px(*editor, panel.left), px(*editor, panel.top), px(*editor, panel.right), px(*editor, panel.bottom),
                      px(*editor, 16), px(*editor, 16));
            if (editor->detail == Detail::name) {
                const auto pen = CreatePen(PS_SOLID, std::max(1, px(*editor, 1)), RGB(102,102,102));
                SelectObject(dc, pen);
                SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                RoundRect(dc, px(*editor, 528), px(*editor, 356), px(*editor, 968), px(*editor, 404), px(*editor, 8), px(*editor, 8));
                RoundRect(dc, px(*editor, 528), px(*editor, 416), px(*editor, 968), px(*editor, 496), px(*editor, 8), px(*editor, 8));
                SelectObject(dc, old_pen);
                DeleteObject(pen);
            } else if (editor->detail == Detail::shortcut || editor->detail == Detail::delay || editor->detail == Detail::tag) {
                const int top = editor->detail == Detail::shortcut ? 380 : 344;
                const auto pen = CreatePen(PS_SOLID, std::max(1, px(*editor, 1)), accent);
                SelectObject(dc, pen);
                SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                RoundRect(dc, px(*editor, 584), px(*editor, top), px(*editor, 910), px(*editor, top + 48), px(*editor, 8), px(*editor, 8));
                SelectObject(dc, old_pen);
                DeleteObject(pen);
            } else SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(brush);
            if (editor->detail == Detail::ring) {
                Gdiplus::Graphics graphics(dc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::SolidBrush slot(Gdiplus::Color(255, 65, 65, 65));
                Gdiplus::Pen outline(Gdiplus::Color(255, 0, 234, 208), static_cast<float>(px(*editor, 2)));
                for (int i = 0; i < 8; ++i) {
                    const double angle = i * 3.141592653589793 / 4.0;
                    const int x = 747 + static_cast<int>(std::sin(angle) * 46);
                    const int y = 363 - static_cast<int>(std::cos(angle) * 46);
                    graphics.FillEllipse(&slot, px(*editor, x - 13), px(*editor, y - 13), px(*editor, 26), px(*editor, 26));
                }
                graphics.DrawEllipse(&outline, px(*editor, 731), px(*editor, 347), px(*editor, 32), px(*editor, 32));
            }
            if (editor->detail == Detail::system || editor->detail == Detail::application_action || editor->detail == Detail::application) {
                const RECT search = editor->detail == Detail::application ? RECT{570,140,924,188} : RECT{563,258,931,306};
                const auto search_pen = CreatePen(PS_SOLID, std::max(1, px(*editor, 1)), RGB(75,75,75));
                const auto previous = SelectObject(dc, search_pen);
                const auto previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                RoundRect(dc, px(*editor, search.left), px(*editor, search.top), px(*editor, search.right), px(*editor, search.bottom), px(*editor, 8), px(*editor, 8));
                SelectObject(dc, previous_brush); SelectObject(dc, previous); DeleteObject(search_pen);
            }
            if (editor->detail == Detail::keys) {
                const auto pen = CreatePen(PS_SOLID, std::max(1, px(*editor, 1)), editor->recording ? accent : RGB(65,65,65));
                const auto previous = SelectObject(dc, pen);
                MoveToEx(dc, px(*editor, 421), px(*editor, 562), nullptr); LineTo(dc, px(*editor, 1087), px(*editor, 562));
                if (editor->recording) {
                    const auto previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                    Rectangle(dc, px(*editor, 412), px(*editor, 256), px(*editor, 1082), px(*editor, 547));
                    SelectObject(dc, previous_brush);
                }
                SelectObject(dc, previous); DeleteObject(pen);
            }
        }
        if (editor->detail == Detail::device) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            if (editor->device_image) graphics.DrawImage(editor->device_image.get(), px(*editor, 618), px(*editor, 224), px(*editor, 200), px(*editor, 322));
            constexpr POINT markers[]{{759,274},{778,334},{686,362},{707,417},{633,410}};
            for (size_t i = 0; i < std::size(markers); ++i) {
                const auto color = static_cast<int>(i) == editor->detail_choice ? accent : foreground;
                Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), static_cast<float>(px(*editor, 2)));
                graphics.DrawEllipse(&pen, px(*editor, markers[i].x - 6), px(*editor, markers[i].y - 6), px(*editor, 12), px(*editor, 12));
            }
        }
        if (editor->text_editing) {
            const auto brush = CreateSolidBrush(surface);
            const auto pen = CreatePen(PS_SOLID, std::max(1, px(*editor, 1)), RGB(45, 45, 45));
            const auto old_brush = SelectObject(dc, brush);
            const auto old_pen = SelectObject(dc, pen);
            RoundRect(dc, px(*editor, 270), px(*editor, 260), px(*editor, 1224), px(*editor, 691),
                      px(*editor, 18), px(*editor, 18));
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(pen);
            DeleteObject(brush);
        }
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
        if (editor->editing && item.hwndItem == editor->list) InvalidateRect(editor->list, nullptr, FALSE);
        else if (!editor->editing && item.CtlID >= card_id) draw_macro_card(*editor, item);
        else if (editor->detail == Detail::application && item.CtlID >= application_tile_id) draw_application_tile(*editor, item);
        else if (item.CtlType == ODT_LISTBOX) draw_list_row(*editor, item);
        else if (item.CtlType == ODT_COMBOBOX) draw_choice_row(*editor, item);
        else draw_button(*editor, item);
        return TRUE;
    }
    if (message == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(payload);
        if (item.CtlType == ODT_COMBOBOX) { item.itemHeight = px(*editor, 34); return TRUE; }
        if (item.CtlType == ODT_LISTBOX && item.CtlID == detail_list_id && editor->detail == Detail::system) {
            item.itemHeight = px(*editor, 38); return TRUE;
        }
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        const auto dc = reinterpret_cast<HDC>(parameter);
        if (editor->detail != Detail::none && std::ranges::find(editor->detail_controls, reinterpret_cast<HWND>(payload)) != editor->detail_controls.end()) {
            SetTextColor(dc, foreground);
            SetBkColor(dc, surface);
            return reinterpret_cast<LRESULT>(editor->fill);
        }
        if (GetDlgCtrlID(reinterpret_cast<HWND>(payload)) == education_text_id) {
            SetTextColor(dc, surface);
            SetBkColor(dc, accent);
            SetDCBrushColor(dc, accent);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        SetTextColor(dc, foreground);
        const bool static_label = message == WM_CTLCOLORSTATIC &&
            reinterpret_cast<HWND>(payload) != editor->trigger_key &&
            reinterpret_cast<HWND>(payload) != editor->action_value &&
            reinterpret_cast<HWND>(payload) != editor->text_counter &&
            reinterpret_cast<HWND>(payload) != editor->text_notice;
        SetBkColor(dc, static_label ? background : surface);
        return reinterpret_cast<LRESULT>(static_label ? editor->backdrop : editor->fill);
    }
    if (message == reload_finished) { finish_reload(*editor);return 0; }
    if (message == WM_MOUSEWHEEL && !editor->editing) {
        close_card_menu(*editor);
        editor->cards_scroll -= GET_WHEEL_DELTA_WPARAM(parameter) * 90 / WHEEL_DELTA;
        layout_cards(*editor);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && editor->detail == Detail::application) {
        editor->application_scroll -= GET_WHEEL_DELTA_WPARAM(parameter) * 90 / WHEEL_DELTA;
        layout_application_tiles(*editor);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && editor->editing && editor->detail == Detail::none && !editor->text_editing) {
        scroll_editor(*editor, -GET_WHEEL_DELTA_WPARAM(parameter) * 90 / WHEEL_DELTA);
        return 0;
    }
    if (message == WM_LBUTTONDOWN && !editor->editing) close_card_menu(*editor);
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
        if (editor->detail != Detail::none) {
            if (id == back_id || id == IDCANCEL || id == detail_cancel_id) {
                const bool application_back = editor->detail == Detail::application_action;
                close_detail(*editor);
                editor->template_target = -1;
                if (application_back) open_detail(*editor, Detail::application);
                else editor->edit_index = -1;
            } else if (id == detail_confirm_id) confirm_detail(*editor);
            else if (id == system_browse_id) browse_system_parameter(*editor);
            else if (id == system_parameter_id && notification == EN_CHANGE && editor->system_parameter)
                EnableWindow(editor->detail_confirm, valid_system_parameter(selected_system_code(*editor), read_text(editor->system_parameter)));
            else if (id == detail_record_id) toggle_recording(*editor);
            else if (editor->detail == Detail::device && id >= device_choice_id && id < device_choice_id + 5) {
                editor->detail_choice = id - device_choice_id;
                EnableWindow(editor->detail_confirm, TRUE);
                RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            } else if (editor->detail == Detail::device && id >= device_profile_id && id < device_profile_id + editor->profiles.size()) {
                editor->detail_profile = id - device_profile_id;
                RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            } else if (editor->detail == Detail::application && id >= application_tile_id) {
                if (id == application_tile_id) {
                    wchar_t filename[MAX_PATH]{};
                    OPENFILENAMEW dialog{};
                    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
                    dialog.lpstrFilter = L"应用程序 (*.exe)\0*.exe\0\0"; dialog.lpstrFile = filename;
                    dialog.nMaxFile = std::size(filename); dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameW(&dialog)) {
                        add_application(*editor, std::filesystem::path(filename).stem().wstring(), filename);
                        if (editor->template_target >= 0) { complete_template_target(*editor, filename); return 0; }
                        const auto found = std::ranges::find_if(editor->applications, [&](const auto& app) { return same(app.path, filename); });
                        editor->selected_application = static_cast<int>(found - editor->applications.begin());
                        close_detail(*editor); open_detail(*editor, Detail::application_action);
                    }
                } else {
                    const size_t index = id - application_tile_id - 1;
                    if (index < editor->detail_items.size()) {
                        editor->selected_application = static_cast<int>(editor->detail_items[index]);
                        if (editor->template_target >= 0) {
                            complete_template_target(*editor, editor->applications[editor->selected_application].path);
                            return 0;
                        }
                        close_detail(*editor); open_detail(*editor, Detail::application_action);
                    }
                }
            } else if (id == detail_list_id && notification == LBN_SELCHANGE && editor->detail_confirm) {
                if (editor->detail == Detail::system) select_system_parameter(*editor);
                else EnableWindow(editor->detail_confirm, selected(editor->detail_list, LB_GETCURSEL) >= 0 && !editor->recording);
            } else if ((id == detail_value_id || id == detail_description_id || id == trigger_key_id) && notification == EN_CHANGE && editor->detail_value) {
                if (editor->detail == Detail::name) update_name_detail(*editor);
                else if (editor->detail == Detail::system || editor->detail == Detail::application || editor->detail == Detail::application_action) {
                    if (editor->detail == Detail::application || editor->detail_list) fill_detail_choices(*editor);
                } else if (editor->detail_confirm) {
                    const auto text = read_text(editor->detail_value);
                    bool valid = !trim(text).empty();
                    if (editor->detail == Detail::delay) {
                        wchar_t* end = nullptr;
                        const auto seconds = std::wcstod(text.c_str(), &end);
                        valid = !text.empty() && end == text.c_str() + text.size() && std::isfinite(seconds) && seconds >= 0 && seconds <= 30;
                    }
                    EnableWindow(editor->detail_confirm, valid || editor->detail == Detail::tag);
                }
            }
            return 0;
        }
        if (!editor->editing) {
            if (id == templates_id || id == manage_id) {
                editor->templates_view = id == templates_id;
                show_list(*editor);
                return 0;
            }
            if (id == filter_id && notification == EN_CHANGE) {
                if (editor->filter && editor->status) { editor->cards_scroll = 0; fill_records(*editor); }
                return 0;
            }
            if (id == sort_id && notification == CBN_SELCHANGE) { editor->cards_scroll = 0; fill_records(*editor); return 0; }
            if (id >= tag_filter_id && id < tag_filter_id + editor->filter_tags.size()) {
                editor->selected_tag = id - tag_filter_id;
                for (const auto control : editor->tag_buttons) InvalidateRect(control, nullptr, FALSE);
                editor->cards_scroll = 0; fill_records(*editor); return 0;
            }
            if (id == education_close_id) { editor->show_education = false; show_list(*editor); return 0; }
            if (id == import_id) { import_entry(*editor); return 0; }
            if (id == create_id) { begin(*editor); return 0; }
            if (id == back_id || id == IDCANCEL) {
                if (!editor->card_menu.empty()) close_card_menu(*editor);
                else close_editor(*editor);
                return 0;
            }
            if (id >= card_id) {
                const auto visible = static_cast<size_t>((id - card_id) / 3);
                if (visible >= editor->visible_entries.size()) return 0;
                const auto entry = editor->visible_entries[visible];
                if (editor->templates_view) {
                    if ((id - card_id) % 3 != 2) begin_template(*editor, entry);
                    return 0;
                }
                switch ((id - card_id) % 3) {
                case 0: close_card_menu(*editor); begin(*editor, editor->entries[entry]); break;
                case 1: close_card_menu(*editor); toggle_entry(*editor, entry); break;
                case 2: open_card_menu(*editor, visible); break;
                }
                return 0;
            }
            if (id >= menu_rename_id && id <= menu_remove_id && editor->card_menu_entry >= 0) {
                const auto entry_index = static_cast<size_t>(editor->card_menu_entry);
                const auto item = editor->entries[entry_index];
                close_card_menu(*editor);
                if (id == menu_rename_id || id == menu_tag_id) {
                    begin(*editor, item);
                    open_detail(*editor, id == menu_rename_id ? Detail::name : Detail::tag);
                } else if (id == menu_duplicate_id) duplicate_entry(*editor, entry_index);
                else if (id == menu_export_id) export_entry(*editor, item);
                else if (id == menu_remove_id && MessageBoxW(window, L"移除此 Smart Action？", L"Smart Actions",
                                                            MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                    auto config = editor->config;
                    restore(config, item);
                    if (commit(*editor, std::move(config))) { show_list(*editor); show_reload_notice(*editor); }
                }
                return 0;
            }
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
        if (id >= trigger_category_id && id < trigger_category_id + 5) {
            if (id == trigger_category_id + 1) open_detail(*editor, Detail::device);
            else if (id == trigger_category_id + 4) open_detail(*editor, Detail::shortcut);
            else if (id == trigger_category_id + 2) open_detail(*editor, Detail::ring);
            return 0;
        }
        if (id >= action_category_id && id < action_category_id + 5) {
            const int type = id - action_category_id;
            editor->action_open = false;
            select(editor->action_type, type);
            show_action_fields(*editor);
            action_visibility(*editor);
            if (type == 2) open_text_editor(*editor);
            else open_detail(*editor, type == 0 ? Detail::application : type == 1 ? Detail::keys : type == 3 ? Detail::system : Detail::delay);
            return 0;
        }
        switch (id) {
        case name_id:
            if (notification == EN_CHANGE) update_save(*editor);
            break;
        case back_id: case IDCANCEL:
            if (editor->text_editing) close_text_editor(*editor);
            else show_list(*editor);
            break;
        case add_tag_id:
            open_detail(*editor, Detail::tag);
            break;
        case trigger_menu_id:
            editor->trigger_open = !editor->trigger_open;
            if (editor->trigger_open) editor->action_open = false;
            trigger_visibility(*editor);
            action_visibility(*editor);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            break;
        case action_menu_id:
            editor->action_open = !editor->action_open;
            if (editor->action_open) editor->trigger_open = false;
            trigger_visibility(*editor);
            action_visibility(*editor);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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
        case text_edit_id: open_text_editor(*editor); break;
        case text_value_id:
            if (notification == EN_CHANGE && editor->text_editing) update_text_editor(*editor);
            break;
        case action_type_id:
            if (notification == CBN_SELCHANGE) {
                show_action_fields(*editor);
                if (selected(editor->action_type) == 2) open_text_editor(*editor);
            }
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
                if (template_pending(step)) { open_template_target(*editor, step); break; }
                select(editor->action_type, step_type(step));
                show_action_fields(*editor, step);
                if (step_type(step) == 2) open_text_editor(*editor);
                else open_detail(*editor, step_type(step) == 0 ? Detail::application : step_type(step) == 1 ? Detail::keys :
                                  step_type(step) == 3 ? Detail::system : Detail::delay);
            }
            break;
        case step_remove_id:
            if (index >= 0) {
                editor->steps.erase(editor->steps.begin() + index);
                editor->edit_index = -1;
                fill_steps(*editor, std::min(index, static_cast<int>(editor->steps.size()) - 1));
                show_action_fields(*editor);
            }
            break;
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

std::vector<SmartLibraryItem> read_smart_library(const std::filesystem::path& mapping) {
    const auto bytes = read_file(mapping);
    if (!bytes) return {};
    std::vector<SmartLibraryItem> result;
    for (const auto& entry : records(Config(wide(*bytes))))
        if (entry.trigger.section == L"ring-library" && entry.trigger.key.empty() && !entry.sequence.empty())
            result.push_back({entry.name, entry.sequence});
    return result;
}

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
    editor.dpi = static_cast<int>(GetDpiForWindow(owner));
    editor.font = CreateFontW(-px(editor, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, native_typography::nativeFamily(FW_NORMAL));
    editor.input_font = CreateFontW(-px(editor, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    editor.heading = CreateFontW(-px(editor, 21), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, native_typography::nativeFamily(FW_SEMIBOLD));
    editor.title = CreateFontW(-px(editor, 36), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, native_typography::nativeFamily(FW_BOLD));
    editor.card_title = CreateFontW(-px(editor, 24), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, native_typography::nativeFamily(FW_BOLD));
    editor.step_font = CreateFontW(-px(editor, 14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, native_typography::nativeFamily(FW_BOLD));
    editor.step_number_font = CreateFontW(-px(editor, 12), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, native_typography::nativeFamily(FW_BOLD));
    editor.backdrop = CreateSolidBrush(background);
    editor.fill = CreateSolidBrush(surface);
    const auto instance = GetModuleHandleW(nullptr);
    if (const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(107), RT_RCDATA)) {
        const auto data = LockResource(LoadResource(instance, resource));
        editor.symbols_loaded = editor.symbols.AddMemoryFont(data, SizeofResource(instance, resource)) == Gdiplus::Ok;
    }
    const auto load_image = [&](WORD id, IStream*& stream, std::unique_ptr<Gdiplus::Image>& image) {
        const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource) return;
        const auto data = static_cast<const BYTE*>(LockResource(LoadResource(instance, resource)));
        stream = SHCreateMemStream(data, SizeofResource(instance, resource));
        if (!stream) return;
        image.reset(Gdiplus::Image::FromStream(stream));
        if (image && image->GetLastStatus() != Gdiplus::Ok) image.reset();
    };
    load_image(101, editor.device_stream, editor.device_image);
    for (WORD index = 0; index < 3; ++index) load_image(104 + index, editor.app_streams[index], editor.app_images[index]);
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
            if ((editor.text_editing || editor.detail != Detail::none) && !editor.recording &&
                msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                const auto focused = GetFocus();
                if (IsWindowEnabled(focused) &&
                    (focused == GetDlgItem(window, apply_step_id) || focused == GetDlgItem(window, back_id) ||
                     focused == editor.detail_confirm || focused == editor.detail_cancel || focused == editor.detail_record)) {
                    SendMessageW(focused, BM_CLICK, 0, 0);
                    continue;
                }
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
    DeleteObject(editor.input_font);
    DeleteObject(editor.heading);
    DeleteObject(editor.title);
    DeleteObject(editor.card_title);
    DeleteObject(editor.step_font);
    DeleteObject(editor.step_number_font);
    DeleteObject(editor.backdrop);
    DeleteObject(editor.fill);
}
