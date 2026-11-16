#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace options_plus_import {

struct Snapshot {
    bool found{};
    std::size_t db_bytes{};
    unsigned profile_count{};
    std::optional<bool> use_system_theme;
    std::optional<bool> battery_notify;
    std::optional<bool> high_res_dpi;
    std::optional<double> pointer_value;
    std::optional<bool> scroll_inverted;
    std::optional<bool> scroll_smooth;
    std::optional<bool> smartshift_enabled;
    std::optional<bool> smartshift_ratchet;
    std::optional<unsigned> smartshift_sensitivity;
    std::optional<bool> thumb_inverted;
    std::optional<bool> thumb_smooth;
    std::wstring error;
};

namespace detail {

inline std::optional<std::size_t> find_key_value_start(std::string_view text, std::string_view key,
                                                       std::size_t from = 0) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto pos = text.find(needle, from);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += needle.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
        ++pos;
    if (pos >= text.size() || text[pos] != ':') return std::nullopt;
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
        ++pos;
    return pos;
}

inline std::string_view extract_brace_block(std::string_view text, std::string_view key) {
    const auto start_opt = find_key_value_start(text, key);
    if (!start_opt || *start_opt >= text.size() || text[*start_opt] != '{') return {};
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (std::size_t i = *start_opt; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_str) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == '{') ++depth;
        else if (ch == '}') {
            if (--depth == 0) return text.substr(*start_opt, i - *start_opt + 1);
        }
    }
    return {};
}

inline std::optional<bool> read_bool(std::string_view text, std::string_view key) {
    const auto pos = find_key_value_start(text, key);
    if (!pos) return std::nullopt;
    const auto rest = text.substr(*pos);
    if (rest.starts_with("true")) return true;
    if (rest.starts_with("false")) return false;
    return std::nullopt;
}

inline std::optional<double> read_number(std::string_view text, std::string_view key) {
    const auto pos = find_key_value_start(text, key);
    if (!pos) return std::nullopt;
    std::size_t end = *pos;
    while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
                                 text[end] == '.' || text[end] == '-' || text[end] == '+'))
        ++end;
    if (end == *pos) return std::nullopt;
    const std::string num(text.substr(*pos, end - *pos));
    char* ptr = nullptr;
    const double val = std::strtod(num.c_str(), &ptr);
    if (!ptr || ptr == num.c_str()) return std::nullopt;
    return val;
}

inline std::optional<std::string> read_string(std::string_view text, std::string_view key) {
    const auto pos = find_key_value_start(text, key);
    if (!pos || *pos >= text.size() || text[*pos] != '"') return std::nullopt;
    std::string out;
    bool esc = false;
    for (std::size_t i = *pos + 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (esc) {
            out.push_back(ch);
            esc = false;
        } else if (ch == '\\') {
            esc = true;
        } else if (ch == '"') {
            return out;
        } else {
            out.push_back(ch);
        }
    }
    return std::nullopt;
}

} // namespace detail

inline Snapshot parse_settings_json(std::string_view json) {
    Snapshot snap;
    if (json.empty()) return snap;
    snap.found = true;
    snap.db_bytes = json.size();

    std::size_t pos = 0;
    while ((pos = json.find("\"profile-", pos)) != std::string_view::npos) {
        ++snap.profile_count;
        pos += 9;
    }

    snap.use_system_theme = detail::read_bool(json, "use_system_theme");
    if (const auto bat = detail::extract_brace_block(json, "battery_notify"); !bat.empty()) {
        snap.battery_notify = true;
    }

    if (const auto ptr = detail::extract_brace_block(json, "pointerSpeed"); !ptr.empty()) {
        snap.high_res_dpi = detail::read_bool(ptr, "highResolutionSensorActive");
        snap.pointer_value = detail::read_number(ptr, "value");
    }

    if (const auto scroll = detail::extract_brace_block(json, "mouseScrollWheelSettings"); !scroll.empty()) {
        if (const auto dir = detail::read_string(scroll, "dir"))
            snap.scroll_inverted = (*dir != "STANDARD");
        snap.scroll_smooth = detail::read_bool(scroll, "isSmooth");
        if (const auto ss = detail::extract_brace_block(scroll, "smartshift"); !ss.empty()) {
            snap.smartshift_enabled = detail::read_bool(ss, "isEnabled");
            if (const auto mode = detail::read_string(ss, "mode"))
                snap.smartshift_ratchet = (*mode == "RATCHET");
            if (const auto sens = detail::read_number(ss, "sensitivity"))
                snap.smartshift_sensitivity = static_cast<unsigned>(std::clamp(std::lround(*sens), 0l, 100l));
        }
    }

    if (const auto thumb = detail::extract_brace_block(json, "mouseThumbWheelSettings"); !thumb.empty()) {
        if (const auto dir = detail::read_string(thumb, "dir"))
            snap.thumb_inverted = (*dir != "STANDARD");
        snap.thumb_smooth = detail::read_bool(thumb, "isSmooth");
    }

    return snap;
}

inline unsigned dpi_from_options_plus(double ratio, bool high_res) {
    const double clamped = std::clamp(ratio, 0.0, 1.0);
    const double max_dpi = high_res ? 8000.0 : 4000.0;
    const double raw = 200.0 + clamped * (max_dpi - 200.0);
    const unsigned stepped = static_cast<unsigned>(std::lround(raw / 50.0) * 50.0);
    return std::clamp(stepped, 200u, high_res ? 8000u : 4000u);
}

inline std::filesystem::path default_db_path() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring_view(buf, n)) / L"LogiOptionsPlus" / L"settings.db";
}

inline Snapshot load_from_db(const std::filesystem::path& db_path = default_db_path()) {
    Snapshot out;
    if (db_path.empty() || !std::filesystem::exists(db_path)) {
        out.error = L"未找到原版 Logi Options+ 数据库 (settings.db)。";
        return out;
    }

    HMODULE sqlite = LoadLibraryW(L"winsqlite3.dll");
    if (!sqlite) {
        out.error = L"系统缺少 winsqlite3.dll，无法读取 settings.db。";
        return out;
    }

    struct sqlite3;
    struct sqlite3_stmt;
    using OpenV2Fn = int(__stdcall*)(const char*, sqlite3**, int, const char*);
    using CloseV2Fn = int(__stdcall*)(sqlite3*);
    using PrepareV2Fn = int(__stdcall*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    using StepFn = int(__stdcall*)(sqlite3_stmt*);
    using ColumnBlobFn = const void*(__stdcall*)(sqlite3_stmt*, int);
    using ColumnBytesFn = int(__stdcall*)(sqlite3_stmt*, int);
    using FinalizeFn = int(__stdcall*)(sqlite3_stmt*);

    auto open_v2 = reinterpret_cast<OpenV2Fn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_open_v2")));
    auto close_v2 = reinterpret_cast<CloseV2Fn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_close_v2")));
    auto prepare_v2 = reinterpret_cast<PrepareV2Fn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_prepare_v2")));
    auto step_fn = reinterpret_cast<StepFn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_step")));
    auto col_blob = reinterpret_cast<ColumnBlobFn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_column_blob")));
    auto col_bytes = reinterpret_cast<ColumnBytesFn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_column_bytes")));
    auto finalize_fn = reinterpret_cast<FinalizeFn>(reinterpret_cast<void*>(GetProcAddress(sqlite, "sqlite3_finalize")));

    if (!open_v2 || !close_v2 || !prepare_v2 || !step_fn || !col_blob || !col_bytes || !finalize_fn) {
        FreeLibrary(sqlite);
        out.error = L"winsqlite3.dll 导出符号不完整。";
        return out;
    }

    const auto u8_wide = db_path.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, u8_wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8_path(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, u8_wide.c_str(), -1, u8_path.data(), n, nullptr, nullptr);

    sqlite3* db = nullptr;
    constexpr int SQLITE_OPEN_READONLY = 1;
    constexpr int SQLITE_ROW = 100;
    if (open_v2(u8_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != 0 || !db) {
        if (db) close_v2(db);
        FreeLibrary(sqlite);
        out.error = L"无法以只读方式打开 settings.db（可能被锁定）。";
        return out;
    }

    sqlite3_stmt* stmt = nullptr;
    std::string json;
    if (prepare_v2(db, "SELECT file FROM data ORDER BY _id DESC LIMIT 1", -1, &stmt, nullptr) == 0 && stmt) {
        if (step_fn(stmt) == SQLITE_ROW) {
            const void* ptr = col_blob(stmt, 0);
            const int len = col_bytes(stmt, 0);
            if (ptr && len > 0) json.assign(static_cast<const char*>(ptr), static_cast<std::size_t>(len));
        }
        finalize_fn(stmt);
    }
    close_v2(db);
    FreeLibrary(sqlite);

    if (json.empty()) {
        out.error = L"settings.db 中未找到有效的配置记录。";
        return out;
    }
    return parse_settings_json(json);
}

} // namespace options_plus_import
