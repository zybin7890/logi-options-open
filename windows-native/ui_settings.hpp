#pragma once

#include <windows.h>
#include <atomic>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ui_settings {

enum class LogLevel : unsigned { off, error, info, debug };
inline constexpr const wchar_t* level_names[]{L"off",L"error",L"info",L"debug"};
inline std::atomic<LogLevel> cached_level{LogLevel::info};
inline std::atomic<ULONGLONG> level_due{};
inline LogLevel log_level_now() {
    const auto now=GetTickCount64();
    auto next=level_due.load(std::memory_order_relaxed);
    if(now>=next && level_due.compare_exchange_strong(next,now+250)) {
        DWORD raw{},size=sizeof(raw);
        const auto result=RegGetValueW(HKEY_CURRENT_USER,L"Software\\LogiNative",L"LogLevel",
            RRF_RT_REG_DWORD,nullptr,&raw,&size);
        cached_level.store(result==ERROR_SUCCESS && raw<=3?static_cast<LogLevel>(raw):LogLevel::info);
    }
    return cached_level.load(std::memory_order_relaxed);
}
inline bool set_log_level(LogLevel level) {
    const DWORD raw=static_cast<DWORD>(level);
    if(RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\LogiNative",L"LogLevel",REG_DWORD,&raw,sizeof(raw))!=ERROR_SUCCESS)return false;
    cached_level.store(level);
    level_due.store(0);
    return true;
}

enum class Theme { system, dark, light };
enum class Language { zh_cn, en_us };
enum class RingSize { small, medium, large };
enum class FlowEdge { right, left, top, bottom };

struct FlowPeerConfig {
    std::wstring id;
    std::wstring name;
    std::wstring host;
    unsigned port{59835};
    FlowEdge edge{FlowEdge::right};
    unsigned host_slot{2};
    unsigned screen_width{1920};
    unsigned screen_height{1080};
    bool operator==(const FlowPeerConfig&) const = default;
};

struct Settings {
    Theme theme{Theme::dark};
    Language language{Language::zh_cn};
    RingSize ring_size{RingSize::medium};
    bool notify_low_battery{true};
    bool notify_locks{true};
    bool notify_mic{true};
    bool close_to_tray{true};
    bool flow_enabled{false};
    bool flow_share_clipboard{true};
    bool flow_hold_ctrl{false};
    bool flow_edge_corners{true};
    bool flow_link_keyboard{false};
    std::wstring flow_pin{L"482910"};
    unsigned flow_port{59835};
    std::vector<FlowPeerConfig> flow_peers;
    bool operator==(const Settings&) const = default;
};

inline bool system_prefers_light() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return false;
}

inline bool effective_light(Theme theme) {
    return theme == Theme::light || (theme == Theme::system && system_prefers_light());
}

inline float ring_scale(RingSize size) {
    switch (size) {
    case RingSize::small: return 0.82f;
    case RingSize::large: return 1.22f;
    default: return 1.0f;
    }
}

inline std::filesystem::path file_path(const std::filesystem::path& mapping) {
    auto path = mapping;
    path.replace_filename(L"ui.settings");
    return path;
}

inline std::wstring utf8_to_wide(std::string_view input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), out.data(), count);
    return out;
}

inline std::string wide_to_utf8(std::wstring_view input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                        out.data(), count, nullptr, nullptr);
    return out;
}

inline std::wstring sanitize_field(std::wstring_view value) {
    std::wstring out;
    for (wchar_t ch : value) {
        if (ch != L'|' && ch != L'\r' && ch != L'\n') out.push_back(ch);
    }
    return out;
}

inline std::wstring edge_name(FlowEdge edge) {
    switch (edge) {
    case FlowEdge::left: return L"left";
    case FlowEdge::top: return L"top";
    case FlowEdge::bottom: return L"bottom";
    default: return L"right";
    }
}

inline FlowEdge parse_edge(std::wstring_view value) {
    if (value == L"left") return FlowEdge::left;
    if (value == L"top") return FlowEdge::top;
    if (value == L"bottom") return FlowEdge::bottom;
    return FlowEdge::right;
}

inline Settings load(const std::filesystem::path& mapping) {
    Settings result;
    std::ifstream input(file_path(mapping), std::ios::binary);
    if (!input) return result;
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        if (first) {
            first = false;
            if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto wide = utf8_to_wide(line);
        const auto eq = wide.find(L'=');
        if (eq == std::wstring::npos) continue;
        const auto key = wide.substr(0, eq);
        const auto val = wide.substr(eq + 1);
        if (key == L"theme") {
            result.theme = val == L"light" ? Theme::light : val == L"system" ? Theme::system : Theme::dark;
        } else if (key == L"language") {
            result.language = val == L"en-US" ? Language::en_us : Language::zh_cn;
        } else if (key == L"ring.size") {
            result.ring_size = val == L"small" ? RingSize::small : val == L"large" ? RingSize::large : RingSize::medium;
        } else if (key == L"notify.low_battery") {
            result.notify_low_battery = val != L"0";
        } else if (key == L"notify.locks") {
            result.notify_locks = val != L"0";
        } else if (key == L"notify.mic") {
            result.notify_mic = val != L"0";
        } else if (key == L"tray.close_to_tray") {
            result.close_to_tray = val != L"0";
        } else if (key == L"flow.enabled") {
            result.flow_enabled = val == L"1";
        } else if (key == L"flow.share_clipboard") {
            result.flow_share_clipboard = val != L"0";
        } else if (key == L"flow.hold_ctrl") {
            result.flow_hold_ctrl = val == L"1";
        } else if (key == L"flow.edge_corners") {
            result.flow_edge_corners = val != L"0";
        } else if (key == L"flow.link_keyboard") {
            result.flow_link_keyboard = val == L"1";
        } else if (key == L"flow.pin" && !val.empty()) {
            result.flow_pin = sanitize_field(val);
        } else if (key == L"flow.port") {
            const auto port = static_cast<unsigned>(_wtoi(val.c_str()));
            if (port >= 1024 && port <= 65535) result.flow_port = port;
        } else if (key == L"flow.peer") {
            std::wistringstream stream(val);
            std::wstring parts[7];
            for (auto& part : parts) std::getline(stream, part, L'|');
            if (!parts[1].empty() && !parts[2].empty()) {
                FlowPeerConfig peer;
                peer.id = parts[0].empty() ? parts[1] : parts[0];
                peer.name = parts[1];
                peer.host = parts[2];
                const unsigned port = static_cast<unsigned>(_wtoi(parts[3].c_str()));
                if (port >= 1024 && port <= 65535) peer.port = port;
                peer.edge = parse_edge(parts[4]);
                const unsigned slot = static_cast<unsigned>(_wtoi(parts[5].c_str()));
                if (slot >= 1 && slot <= 3) peer.host_slot = slot;
                if (!parts[6].empty()) {
                    const auto x = parts[6].find(L'x');
                    if (x != std::wstring::npos) {
                        const unsigned w = static_cast<unsigned>(_wtoi(parts[6].substr(0, x).c_str()));
                        const unsigned h = static_cast<unsigned>(_wtoi(parts[6].substr(x + 1).c_str()));
                        if (w >= 640 && h >= 480) { peer.screen_width = w; peer.screen_height = h; }
                    }
                }
                result.flow_peers.push_back(std::move(peer));
            }
        }
    }
    return result;
}

inline bool save(const std::filesystem::path& mapping, const Settings& settings) {
    const auto path = file_path(mapping);
    const auto temp = path.wstring() + L".tmp";
    std::wostringstream out;
    out << L"theme=" << (settings.theme == Theme::light ? L"light" : settings.theme == Theme::system ? L"system" : L"dark") << L"\n";
    out << L"language=" << (settings.language == Language::en_us ? L"en-US" : L"zh-CN") << L"\n";
    out << L"ring.size=" << (settings.ring_size == RingSize::small ? L"small" : settings.ring_size == RingSize::large ? L"large" : L"medium") << L"\n";
    out << L"notify.low_battery=" << (settings.notify_low_battery ? L"1" : L"0") << L"\n";
    out << L"notify.locks=" << (settings.notify_locks ? L"1" : L"0") << L"\n";
    out << L"notify.mic=" << (settings.notify_mic ? L"1" : L"0") << L"\n";
    out << L"tray.close_to_tray=" << (settings.close_to_tray ? L"1" : L"0") << L"\n";
    out << L"flow.enabled=" << (settings.flow_enabled ? L"1" : L"0") << L"\n";
    out << L"flow.share_clipboard=" << (settings.flow_share_clipboard ? L"1" : L"0") << L"\n";
    out << L"flow.hold_ctrl=" << (settings.flow_hold_ctrl ? L"1" : L"0") << L"\n";
    out << L"flow.edge_corners=" << (settings.flow_edge_corners ? L"1" : L"0") << L"\n";
    out << L"flow.link_keyboard=" << (settings.flow_link_keyboard ? L"1" : L"0") << L"\n";
    out << L"flow.pin=" << sanitize_field(settings.flow_pin) << L"\n";
    out << L"flow.port=" << settings.flow_port << L"\n";
    for (const auto& peer : settings.flow_peers) {
        out << L"flow.peer=" << sanitize_field(peer.id) << L"|" << sanitize_field(peer.name) << L"|"
            << sanitize_field(peer.host) << L"|" << peer.port << L"|" << edge_name(peer.edge) << L"|"
            << peer.host_slot << L"|" << peer.screen_width << L"x" << peer.screen_height << L"\n";
    }
    const auto utf8 = wide_to_utf8(out.str());
    {
        std::ofstream file(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        if (!file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()))) {
            DeleteFileW(temp.c_str());
            return false;
        }
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temp.c_str());
    return false;
}

inline const wchar_t* tr(Language lang, const wchar_t* zh, const wchar_t* en) {
    return lang == Language::en_us ? en : zh;
}

using FlowPollFn = unsigned (*)(const Settings& settings, std::wstring& peer_name);
using FlowStopFn = void (*)();

inline FlowPollFn& flow_poll_hook() {
    static FlowPollFn fn{};
    return fn;
}

inline FlowStopFn& flow_stop_hook() {
    static FlowStopFn fn{};
    return fn;
}

} // namespace ui_settings
