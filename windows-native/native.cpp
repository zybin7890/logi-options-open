#include "mouse.hpp"
#include "remap.hpp"
#include "smart_native.hpp"
#include "cli_entry.hpp"
#include "../first_run.hpp"
#include "lazy_win32.hpp"
#include "configuration_backup.hpp"
#include "config_sync_engine.hpp"
#include "config_sync_drive.hpp"
#include "config_sync_onedrive.hpp"
#include "native_dpi.hpp"
#include "native_smartshift.hpp"
#include "agent_log.hpp"
#include "tray_recovery.hpp"
#include "ui_settings.hpp"
#include "windows_autostart.hpp"
#include "flow.hpp"
#include "pairing.hpp"
#include "notifications_hud.hpp"
#include "options_plus_import.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "native_icons.hpp"
#include "native_typography.hpp"
#include "ring_metadata.hpp"
#include "native_ring_editor.hpp"
#include "ring_profile_package.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <malloc.h>
#include <map>
#include <span>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Text = std::wstring;
using Section = std::map<Text, Text>;
using Config = std::map<Text, Section>;

inline COLORREF background = RGB(25,25,25), panel = RGB(0,0,0);
inline COLORREF raised = RGB(36,36,36), white = RGB(251,251,251);
inline COLORREF muted = RGB(198,198,198), teal = RGB(0,234,208);
inline COLORREF line_color = RGB(53,53,53), green = RGB(133,222,83);
constexpr int client_width = 1494, client_height = 848, pane_x = 995, pane_header = 69;
int layout_dpi = 96;
bool ring_typography{};
constexpr unsigned min_thumb_speed = 10, default_thumb_speed = 100, max_thumb_speed = 200;
constexpr auto thumb_speed_key = L"thumb.speed";
constexpr UINT finished = WM_APP + 1;
constexpr UINT assign_ring = WM_APP + 2;
constexpr UINT reload_finished = WM_APP + 3;
constexpr UINT swap_ring_slots = WM_APP + 4;
constexpr UINT tray_callback = WM_APP + 20;
constexpr UINT show_existing_window = WM_APP + 21;
enum Page { home, buttons, pointer, easy, flow, settings, ring, pairing };
enum Id {
    back = 10, smart, ring_page, new_profile, reload, begin, end, advanced, ring_preview,
    ring_add, close_pane, apply, find_action, dpi_range, field, direction_left, direction_right,
    remove_profile, link_action, thumb_speed, replace_ring, flip_primary, profile_choice,
    delete_profile, ring_folder, gesture_preset, apply_thumb_speed,
    minimize_ui, close_ui, more_actions, custom_actions, ring_library, ring_search, ring_custom,
    ring_all, ring_undo, ring_redo, ring_commit, gesture_presets_open, gesture_overview, profiles_previous, profiles_next,
    export_config, import_config, ring_menu, ring_cut, ring_paste, ring_clear, ring_browse,
    ring_replace, dpi_extended, ring_folder_back, smartshift_toggle, wheel_standard, wheel_reverse,
    wheel_free, wheel_ratchet, wheel_smooth, wheel_smooth_all, thumb_smooth,
    open_pairing = 90, pairing_scan, pairing_bolt_lock, pairing_open_bt, pairing_tab_bolt, pairing_tab_bt,
    ring_size_small, ring_size_medium, ring_size_large,
    settings_theme_system, settings_theme_dark, settings_theme_light,
    settings_lang_zh, settings_lang_en,
    settings_notify_battery, settings_notify_locks, settings_notify_mic, settings_notify_test,
    settings_check_fw, settings_reset_defaults,
    settings_autostart, settings_close_to_tray, settings_import_options_plus,
    pointer_precision, pointer_dpi_800, pointer_dpi_1600, pointer_dpi_3200, pointer_dpi_5000,
    tray_open, tray_refresh, tray_host_1, tray_host_2, tray_host_3, tray_toggle_flow, tray_toggle_trial, tray_exit,
    flow_toggle_enabled, flow_toggle_clip, flow_toggle_ctrl, flow_toggle_corners,
    flow_edge_left, flow_edge_right, flow_edge_top, flow_edge_bottom,
    flow_slot_1, flow_slot_2, flow_slot_3,
    flow_discover, flow_save_peer, flow_remove_peer, flow_ping_peer, flow_loopback_test, flow_reset_all,
    flow_edit_name, flow_edit_host, flow_edit_port, flow_edit_pin,
    action_param_browse, action_param_apply, action_param_edit,
    nav_first = 100, profiles_first = 200, pins_first = 400, hosts_first = 500, rings_first = 600,
    gesture_rows_first = 700,
    ring_icon_edit=1800,ring_settings,ring_adaptive,ring_profiles_menu,ring_profile_new,ring_profile_copy,
    ring_profile_rename,ring_profile_delete,ring_profile_export,ring_profile_import,ring_application_add,
    ring_profile_name,ring_profile_accept,ring_application_menu,ring_profile_first=1900,ring_application_first=2100,
    ring_click_type_first=2300,ring_click_modifier_first=2310,
    ring_profile_cancel=2400,ring_profile_context_first=2500,
    ring_create_general=2700,ring_create_application,ring_create_import,ring_create_browse,ring_create_blank,
    ring_create_search,ring_create_previous,ring_create_next,ring_create_app_first=2800,
    flow_peer_first = 3100,
    log_level_first=3400, cloud_settings=3410, cloud_dav,cloud_google,cloud_onedrive,cloud_endpoint,cloud_identity,
    cloud_password,cloud_automatic,cloud_save,cloud_run,cloud_authorize,cloud_local,cloud_remote,cloud_disconnect,
};
struct Point { const wchar_t* key; const wchar_t* title; int x, y, align, width; };
constexpr Point button_points[] = {
    {L"0052",L"滚轮按钮",71,15,1,86},{L"00C4",L"顶部按钮",81,34,1,86},
    {L"thumb",L"拇指滚轮",55,51,1,132},{L"0056",L"前进按钮",35,43,-1,86},
    {L"0053",L"后退按钮",45,60,0,86},{L"00C3",L"拇指按钮",8,58,-1,92},
};
constexpr Point pointer_points[] = {
    {L"wheel",L"滚轮",73,16,1,148},{L"dpi",L"指针速度",83,54,1,135},
    {L"thumb",L"拇指滚轮",55,51,-1,132},
};
struct Action { const wchar_t* code; const wchar_t* name; };
constexpr Action actions[] = {
    {L"MouseMiddle",L"中键"},{L"MouseForward",L"前进"},{L"MouseBack",L"后退"},
    {L"WheelMode",L"切换滚轮模式"},{L"Lock",L"锁定"},{L"Win+Tab",L"任务视图"},
    {L"Win+D",L"显示或隐藏桌面"},{L"Alt+Tab",L"切换应用程序"},
    {L"Maximize",L"最大化窗口"},{L"Minimize",L"最小化窗口"},{L"CloseWindow",L"关闭窗口"},
    {L"ThisPC",L"此PC"},{L"Ctrl+C",L"复制"},{L"Ctrl+V",L"粘贴"},{L"Ctrl+X",L"剪切"},
    {L"Ctrl+Z",L"撤销"},{L"Ctrl+Y",L"重做"},{L"Ctrl+F",L"查找"},
    {L"Ctrl+Tab",L"上一个/下一个标签页"},{L"Ctrl+Shift+Tab",L"上一个"},
    {L"Ctrl+W",L"关闭标签页"},{L"Ctrl+T",L"新建浏览器标签页"},{L"Ctrl+Shift+T",L"重新打开标签页"},
    {L"VolumeUp",L"音量升高"},{L"VolumeDown",L"音量降低"},{L"Mute",L"扬声器静音/取消静音"},
    {L"MicrophoneMute",L"麦克风静音"},{L"PlayPause",L"播放/暂停"},
    {L"NextTrack",L"下一个"},{L"PreviousTrack",L"上一个"},
    {L"BrightnessUp",L"亮度升高"},{L"BrightnessDown",L"亮度降低"},
    {L"Win+Ctrl+Left",L"桌面向左"},{L"Win+Ctrl+Right",L"桌面向右"},
    {L"Win",L"开始菜单"},{L"Win+E",L"文件资源管理器"},
    {L"Win+Shift+S",L"截屏"},{L"ScreenCapture(Screen,File)",L"屏幕截图"},{L"PrintScreen",L"全屏截图"},
    {L"ScrollLeft",L"向左滚动"},{L"ScrollRight",L"向右滚动"},
    {L"ZoomIn",L"放大"},{L"ZoomOut",L"缩小"},{L"ZoomReset",L"重置缩放"},
    {L"Win+.",L"表情符号菜单"},{L"TextHex(d83dde00)",L"表情符号"},
    {L"Win+H",L"听写"},{L"Win+A",L"操作中心"},
    {L"Win+Shift+F23",L"Windows Copilot"},{L"Win+Space",L"输入语言"},{L"RControl",L"右Ctrl"},
    {L"Open(calc.exe)",L"计算器"},{L"Open()",L"打开应用程序 / 文件 / 文件夹 / 网页"},
    {L"DpiToggle(1000,5000)",L"调整指针速度"},{L"AdvancedClick(Single,0)",L"高级点击"},
    {L"Widget(spotlight)",L"Spotlight 聚焦"},
    {L"Ring",L"Actions Ring"},{L"None",L"无动作"},
};
struct RingAction { const wchar_t* category; const wchar_t* code; const wchar_t* name; };
constexpr RingAction ring_actions[] = {
    {L"媒体和音量",L"Mute",L"切换静音"},
    {L"媒体和音量",L"VolumeUp",L"音量上升"},{L"媒体和音量",L"VolumeDown",L"音量下降"},
    {L"媒体和音量",L"PlayPause",L"播放/暂停"},
    {L"媒体和音量",L"NextTrack",L"下一曲目"},{L"媒体和音量",L"PreviousTrack",L"上一曲目"},
    {L"媒体和音量",L"MediaStop",L"停止"},
    {L"媒体",L"PlayPause",L"播放/暂停"},{L"媒体",L"NextTrack",L"下一曲目"},
    {L"媒体",L"PreviousTrack",L"上一曲目"},{L"媒体",L"MediaStop",L"停止"},
    {L"打开",L"Open()",L"打开应用程序、文件或网址"},
    {L"打开",L"Win+E",L"文件资源管理器"},{L"打开",L"Open(notepad.exe)",L"记事本"},
    {L"打开",L"Open(calc.exe)",L"计算器"},
    {L"打开",L"ThisPC",L"此电脑"},{L"打开",L"Mail",L"邮件"},{L"打开",L"Media",L"媒体播放器"},{L"打开",L"BrowserHome",L"浏览器主页"},
    {L"导航",L"Alt+Tab",L"切换应用"},{L"导航",L"Ctrl+Tab",L"下一个标签页"},
    {L"导航",L"Ctrl+Shift+Tab",L"上一个标签页"},{L"导航",L"Ctrl+T",L"打开新标签页"},
    {L"导航",L"Ctrl+W",L"关闭标签页"},{L"导航",L"Ctrl+Shift+T",L"重新打开标签页"},
    {L"导航",L"Win+Ctrl+Left",L"向左切换虚拟桌面"},{L"导航",L"Win+Ctrl+Right",L"向右切换虚拟桌面"},
    {L"实用程序",L"Win+Shift+S",L"截屏"},{L"实用程序",L"ScreenCapture(Screen,File)",L"屏幕截图"},
    {L"实用程序",L"PrintScreen",L"全屏截图"},{L"实用程序",L"ZoomIn",L"放大"},
    {L"实用程序",L"ZoomOut",L"缩小"},{L"实用程序",L"ZoomReset",L"重置缩放"},
    {L"实用程序",L"RotateLeft",L"向左旋转"},{L"实用程序",L"RotateRight",L"向右旋转"},
    {L"系统",L"BrightnessUp",L"亮度升高"},{L"系统",L"BrightnessDown",L"亮度降低"},
    {L"系统",L"MicrophoneMute",L"麦克风静音"},{L"系统",L"CloseWindow",L"关闭窗口"},
    {L"系统",L"Sleep",L"睡眠"},{L"系统",L"LogOff",L"注销"},{L"系统",L"Shutdown",L"关机"},
    {L"系统",L"Win+Tab",L"任务视图"},{L"系统",L"Win+D",L"显示或隐藏桌面"},
    {L"系统",L"Maximize",L"最大化窗口"},{L"系统",L"Minimize",L"最小化窗口"},
    {L"系统",L"Lock",L"锁定"},{L"系统",L"Win",L"开始菜单"},
    {L"系统",L"Win+A",L"操作中心"},{L"系统",L"Win+H",L"听写"},
    {L"系统",L"Win+.",L"表情符号菜单"},{L"系统",L"Win+Space",L"输入语言"},
    {L"系统",L"Win+Shift+F23",L"Windows Copilot"},
    {L"鼠标",L"MouseMiddle",L"中键"},{L"鼠标",L"MouseBack",L"后退"},
    {L"鼠标",L"MouseForward",L"前进"},{L"鼠标",L"ScrollLeft",L"向左滚动"},
    {L"鼠标",L"ScrollRight",L"向右滚动"},
    {L"鼠标",L"AdvancedClick(Single,0)",L"高级点击"},{L"鼠标",L"DpiToggle(1000,5000)",L"切换指针速度"},
    {L"键盘",L"@shortcut",L"键盘快捷键"},{L"键盘",L"TextHex()",L"文本"},{L"键盘",L"TextHex(d83dde00)",L"表情符号"},
    {L"键盘",L"Ctrl+C",L"复制"},{L"键盘",L"Ctrl+V",L"粘贴"},
    {L"键盘",L"Ctrl+X",L"剪切"},{L"键盘",L"Ctrl+Z",L"撤销"},
    {L"键盘",L"Ctrl+Y",L"重做"},{L"键盘",L"Ctrl+F",L"查找"},{L"键盘",L"RControl",L"右Ctrl"},
    {L"日期和时间",L"DateTime(date)",L"当前日期"},{L"日期和时间",L"DateTime(time)",L"当前时间"},
    {L"日期和时间",L"DateTime(datetime)",L"日期和时间"},{L"日期和时间",L"DateTime(day_of_year)",L"一年中的第几天"},
    {L"日期和时间",L"DateTime(week_number)",L"周数"},{L"日期和时间",L"DateTime(month)",L"当前月份"},
    {L"小部件",L"Widget(clock)",L"时钟小部件"},{L"小部件",L"Widget(stopwatch)",L"秒表计时器"},
    {L"小部件",L"Widget(spotlight)",L"Spotlight 聚焦"},{L"小部件",L"Widget(system)",L"系统状态面板"},
    {L"Easy-Switch",L"HostSwitch(1)",L"切换到主机 1"},
    {L"Easy-Switch",L"HostSwitch(2)",L"切换到主机 2"},
    {L"Easy-Switch",L"HostSwitch(3)",L"切换到主机 3"},
    {L"高级",L"@folder",L"文件夹"},{L"高级",L"None",L"无动作"},
};
struct RingRow { Text title; int action{-1}; bool expanded{}; };
enum class RingValueKind { none, shortcut, open, text, sequence, folder, advanced_click, dpi_toggle };
struct WheelAction { const wchar_t* name; const wchar_t* left; const wchar_t* right; };
constexpr WheelAction wheel_actions[] = {
    {L"音量升高/降低",L"VolumeDown",L"VolumeUp"},
    {L"标签页导航",L"Ctrl+Shift+Tab",L"Ctrl+Tab"},
    {L"水平滚动",L"ScrollLeft",L"ScrollRight"},
    {L"缩放",L"ZoomOut",L"ZoomIn"},
    {L"上一曲/下一曲",L"PreviousTrack",L"NextTrack"},
    {L"切换虚拟桌面",L"Win+Ctrl+Left",L"Win+Ctrl+Right"},
    {L"前进/后退",L"MouseBack",L"MouseForward"},
    {L"应用导航",L"Alt+Shift+Tab",L"Alt+Tab"},
    {L"无动作",L"None",L"None"},
};
constexpr const wchar_t* hardware_keys[]{L"dpi",L"smartshift",L"ratchet",L"hires",L"invert",L"thumb-invert"};
constexpr std::array<unsigned,4> pointer_presets{800,1600,3200,5000};
constexpr const wchar_t* gesture_suffix[]{L"",L".left",L".right",L".up",L".down"};
constexpr const wchar_t* gesture_titles[]{L"点击",L"向左",L"向右",L"向上",L"向下"};
struct GesturePreset { const wchar_t* title; std::array<const wchar_t*,5> rules; };
constexpr GesturePreset gesture_presets[] = {
    {L"虚拟桌面",{L"Win+Tab",L"Win+Ctrl+Left",L"Win+Ctrl+Right",L"Win",L"Win+D"}},
    {L"媒体控制",{L"PlayPause",L"PreviousTrack",L"NextTrack",L"VolumeUp",L"VolumeDown"}},
    {L"媒体控制（连续）",{L"GestureMedia",L"PreviousTrack",L"NextTrack",L"VolumeUp",L"VolumeDown"}},
    {L"窗口管理",{L"Alt+Tab",L"Win+Left",L"Win+Right",L"Maximize",L"Win+D"}},
    {L"应用程序导航",{L"GestureAppNav",L"Alt+Shift+Tab",L"Alt+Tab",L"Win",L"Win+D"}},
    {L"缩放/旋转",{L"GestureZoomRotate",L"RotateLeft",L"RotateRight",L"ZoomIn",L"ZoomOut"}},
    {L"平移",{L"Pan",L"None",L"None",L"None",L"None"}},
    {L"排列窗口",{L"Alt+Tab",L"Win+Left",L"Win+Right",L"Maximize",L"Minimize"}},
    {L"自定义",{L"None",L"None",L"None",L"None",L"None"}},
};

int px(int number) { return MulDiv(number, layout_dpi, 96); }
double css_ease(double progress) {
    // CSS ease: cubic-bezier(.25,.1,.25,1); solve the time coordinate first.
    if(progress<=0 || progress>=1)return std::clamp(progress,0.0,1.0);
    double low=0,high=1;
    for(unsigned i=0;i<20;++i) {
        const double u=(low+high)/2,one=1-u;
        const double time=.75*u*one*one+.75*u*u*one+u*u*u;
        if(time<progress)low=u;else high=u;
    }
    const double u=(low+high)/2,one=1-u;
    return .3*u*one*one+3*u*u*one+u*u*u;
}
int fitted_dpi(const RECT& work, UINT dpi) {
    return std::clamp(std::min(MulDiv(work.right-work.left-18,96,client_width),
                               MulDiv(work.bottom-work.top-18,96,client_height)),
                      48,static_cast<int>(dpi));
}

Text from_utf8(std::string_view value) {
    if (value.starts_with("\xEF\xBB\xBF")) value.remove_prefix(3);
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    Text text(size, L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), text.data(), size);
    return text;
}
std::string to_utf8(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    return bytes;
}
std::wstring_view trim(std::wstring_view line) {
    const auto first = line.find_first_not_of(L" \r\t");
    if (first == line.npos) return {};
    return line.substr(first, line.find_last_not_of(L" \r\t") - first + 1);
}
Text read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    file.read(bytes.data(), size);
    return from_utf8(bytes);
}
std::string read_binary(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    file.read(bytes.data(), size);
    return bytes;
}
std::vector<Text> lines(std::wstring_view text) {
    std::vector<Text> result;
    while (!text.empty()) {
        const auto end = text.find(L'\n');
        auto line = text.substr(0, end);
        if (line.ends_with(L'\r')) line.remove_suffix(1);
        result.emplace_back(line);
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return result;
}
std::wstring_view section_name(std::wstring_view line) {
    line = trim(line);
    return line.starts_with(L'[') && line.ends_with(L']') ? trim(line.substr(1, line.size()-2)) : L"";
}
std::pair<Text,Text> entry(std::wstring_view line) {
    line = trim(line);
    if (line.starts_with(L'#') || line.starts_with(L';')) return {};
    const auto equals = line.find(L'=');
    if (equals == line.npos) return {};
    return {Text(trim(line.substr(0,equals))), Text(trim(line.substr(equals+1)))};
}
Config parse(std::wstring_view text) {
    Config result;
    Text section = L"global";
    for (const auto& line : lines(text)) {
        if (auto name = section_name(line); !name.empty()) {
            section = name;
            result.try_emplace(section);
        } else if (auto [key,value] = entry(line); !key.empty()) {
            result[section][key] = value;
        }
    }
    return result;
}
Text change_section(std::wstring_view text, const Text& name, const Section& changes,
                    bool remove = false) {
    auto source = lines(text);
    auto first = std::ranges::find_if(source,[&](const Text& line){return section_name(line)==name;});
    Text result;
    const auto add = [&](const Text& line) { result += line + L"\n"; };
    for (auto line = source.begin(); line != first; ++line) add(*line);
    if (first == source.end()) {
        if (!remove) add(L"\n[" + name + L"]");
    } else if (!remove) add(*first);
    auto next = first;
    if (next != source.end()) {
        ++next;
        while (next != source.end() && section_name(*next).empty()) {
            if (!remove && !changes.contains(entry(*next).first)) add(*next);
            ++next;
        }
    }
    if (!remove) {
        for (const auto& [key,value]:changes) if (!value.empty()) add(key + L" = " + value);
    }
    for (; next != source.end(); ++next) add(*next);
    return result;
}

struct Device {
    Text name, detail;
    Text serial, firmware, receiver_firmware;
    bool thumb_cached{};
    ULONGLONG dpi_sampled_at{};
    std::optional<unsigned> battery;
    std::array<std::optional<unsigned>,std::size(hardware_keys)> values{};
    std::vector<unsigned> dpi;
    std::vector<logi::HostSlot> hosts;
};
struct Picture {
    HINSTANCE instance{};
    int resource_id{};
    IStream* stream{};
    std::unique_ptr<Gdiplus::Image> image;
    std::unique_ptr<Gdiplus::Bitmap> cached;
    int cached_w{}, cached_h{};
    ~Picture() { unload(); }
    void load(HINSTANCE inst, int resource) {
        instance = inst;
        resource_id = resource;
    }
    void unload() {
        cached.reset();
        image.reset();
        if (stream) { stream->Release(); stream = nullptr; }
        cached_w = cached_h = 0;
    }
    Gdiplus::Image* ensure() {
        if (image) return image.get();
        if (!instance || !resource_id) return nullptr;
        const auto item = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
        if (!item) return nullptr;
        const DWORD length = SizeofResource(instance,item);
        const auto bytes = static_cast<const BYTE*>(LockResource(LoadResource(instance,item)));
        if (!bytes || !length) return nullptr;
        stream = SHCreateMemStream(bytes,length);
        if (stream) {
            image.reset(Gdiplus::Image::FromStream(stream));
            if (image && image->GetLastStatus() != Gdiplus::Ok) unload();
        }
        return image.get();
    }
    Gdiplus::Image* scaled(int w, int h) {
        if (cached && cached_w == w && cached_h == h) return cached.get();
        auto* base = ensure();
        if (!base || w <= 0 || h <= 0) return base;
        auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(bmp.get());
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        if (g.DrawImage(base, 0, 0, w, h) != Gdiplus::Ok) return base;
        cached = std::move(bmp);
        cached_w = w;
        cached_h = h;
        // Release the full-resolution source PNG stream & image once the exact scaled bitmap is cached.
        image.reset();
        if (stream) { stream->Release(); stream = nullptr; }
        return cached.get();
    }
};
enum class Look {
    filled, plain, glyph, tab, profile, hotspot, device, slot, section, badge, switch_toggle, ring_slot, ring_tag, tool, ring_menu_item, radio,
    gesture_row, gesture_header, gesture_picker, ring_creation_card,ring_application_card
};
struct ButtonSkin {
    Look look = Look::filled;
    bool active = false;
    std::wstring_view icon;
    Text caption, secondary;
    int title_lines = 1;
    HICON application_icon{};
    Gdiplus::Image* original_icon{};
    std::optional<RingIcon> ring_icon{};
};
struct MovingControl { HWND window; RECT from,to; };
struct BindingEdit { Text section; Section changes; };
struct RingEdit { Text section; std::size_t index; Text name,code; };
struct ReloadEdit {
    fs::path path;
    std::string bytes;
    ReloadFingerprint fingerprint{};
    unsigned long long revision{},notice_revision{};
    ULONGLONG deadline{};
};
struct ReloadCompletion { ReloadEdit edit; ReloadResult result; };
enum class ReloadViewState { idle, pending, applied, unconfirmed };
struct CloudJob {
    enum class ApplyStage { prepare, probe, reload, settings } apply_stage{ApplyStage::prepare};
    loo_backup::Configuration download;
    unsigned long long apply_reload_revision{},apply_dpi_revision{};
    std::array<unsigned long long,6> apply_setting_revision{};
    config_sync::Options options;
    std::vector<unsigned char> local;
    config_sync::SyncResult result;
    std::wstring error;
    bool authorization{};
    bool cloud_confirmed{};
    std::atomic<bool> finished{};
};
struct App {
    config_sync::Options cloud_options,cloud_draft;
    std::shared_ptr<CloudJob> cloud_job;
    std::wstring cloud_status,cloud_secret_draft;
    bool cloud_open{},cloud_conflict{},cloud_applying{};
    ULONGLONG cloud_deadline{};
    std::optional<config_sync::Mode> cloud_queued;
    ULONGLONG cloud_queue_deadline{};
    HWND window{}, note{}, input{}, slider{}, speed_value{}, dpi_value{}, sensitivity_value{}, action_list{}, search{}, ring_value{}, save_action{};
    std::array<HWND,5> gesture{};
    std::array<HWND,5> hardware{};
    std::vector<HWND> children;
    std::map<HWND,ButtonSkin> button_skins;
    std::map<Text,HICON> application_icons;
    HWND tips{};
    std::size_t profile_offset{};
    bool has_frame{},animating{},rendered_editing{},rendering{};
    Page rendered_page{home};
    ULONGLONG animation_start{};
    RECT visual_image{},animation_from{},animation_to{};
    int visual_pane{client_width},pane_from{client_width},pane_to{client_width};
    std::vector<MovingControl> moving;
    HBRUSH background_brush{CreateSolidBrush(background)}, panel_brush{CreateSolidBrush(panel)},
           field_brush{CreateSolidBrush(raised)};
    HFONT normal{}, heading{}, bold{}, subheading{}, small{}, caption{};
    unsigned font_generation{};
    fs::path directory;
    Text source, selected{L"global"}, message;
    unsigned long long notice_revision{}, startup_notice_revision{};
    unsigned long long reload_revision{},io_notice_revision{};
    bool reload_busy{};
    HANDLE reload_cancel{};
    std::thread reload_worker;
    std::optional<ReloadEdit> pending_reload;
    std::optional<ReloadCompletion> reload_completion;
    std::optional<ReloadResult> last_reload_result;
    ReloadViewState reload_state{ReloadViewState::idle};
    Text ring_section{L"ring"};
    Text ring_application{L"global"};
    ring_metadata::Settings ring_ui;
    std::map<Text,std::unique_ptr<Gdiplus::Image>> ring_images;
    RingFonts ring_fonts;
    bool ring_settings_open{},ring_profiles_open{},ring_applications_open{};
    int ring_profile_edit{};
    Text ring_profile_target;
    int ring_creation_stage{};std::size_t ring_creation_page{};
    Text ring_creation_application,ring_creation_query;
    struct RingApplication {Text name,path;bool supported{};};
    std::vector<RingApplication> ring_catalog;
    std::vector<std::size_t> ring_catalog_visible;
    std::vector<Text> ring_registry_cache;
    std::vector<Text> ring_profile_sections,ring_applications;
    RingIcon ring_clipboard_icon;
    std::vector<Text> ring_navigation;
    Text ring_category;
    Text ring_name,ring_code,ring_query;
    RECT ring_editor{};
    POINT ring_menu_anchor{};
    RECT ring_menu_bounds{};
    std::vector<Text> ring_undo_stack,ring_redo_stack;
    std::vector<RingRow> ring_rows;
    std::vector<SmartLibraryItem> ring_smart_actions;
    std::optional<RingEdit> ring_clipboard;
    std::map<Text,std::vector<std::pair<Text,Text>>> ring_copied_folders;
    std::map<Text,std::array<RingIcon,8>> ring_copied_icons;
    HWND ring_list{}, ring_filter{}, ring_choice{};
    HWND ring_value2{};
    bool ring_advanced{},ring_searching{},ring_menu_open{};
    RingValueKind ring_value_kind{RingValueKind::none};
    int ring_drag_action{-1};
    POINT ring_drag_start{};
    bool ring_dragging{};
    Config config;
    std::vector<Text> profiles;
    std::vector<Action> visible;
    Page page{home};
    int pane_index{-1};
    bool busy{}, native{}, official{}, waiting_agent{}, waiting_stop{}, all_actions{}, show_custom{};
    ULONGLONG official_checked_at{};
    HDC paint_dc{};
    HBITMAP paint_bitmap{};
    int paint_w{}, paint_h{};
    bool gesture_panel{}, choosing_preset{};
    int gesture_direction{-1};
    HANDLE started{};
    unsigned timer_checks{};
    Device device, next_device;
    std::optional<logi::Mouse> connection;
    std::map<unsigned,unsigned> pending_values;
    unsigned long long dpi_revision{}, io_dpi_revision{};
    std::optional<unsigned> desired_dpi;
    std::optional<bool> dpi_mode;
    bool dpi_unconfirmed{}, io_writes_dpi{}, work_dpi_failed{};
    ui_settings::Settings ui;
    HWND flow_name_edit{}, flow_host_edit{}, flow_port_edit{}, flow_pin_edit{}, action_param_box{};
    std::size_t selected_flow_peer{};
    std::vector<flow::DiscoveredPeer> discovered_peers;
    std::vector<pairing::ReceiverSlotInfo> bolt_slots;
    std::vector<pairing::BluetoothDeviceInfo> bt_devices;
    bool pairing_tab_bluetooth{};
    bool bolt_pairing_open{};
    int ring_slot_drag_from{-1};
    POINT ring_slot_drag_start{};
    bool ring_slot_dragging{};
    std::vector<std::pair<unsigned,unsigned>> work_unattempted;
    Text dpi_failure_notice;
    unsigned long long setting_serial{};
    std::array<unsigned long long,6> setting_revision{},io_setting_revision{};
    std::array<std::optional<unsigned>,6> io_setting_values;
    std::array<std::optional<unsigned>,6> desired_setting;
    std::array<bool,6> setting_unconfirmed{};
    std::array<Text,6> setting_failure_notice;
    Text setting_last_notice;
    std::array<std::optional<SettingResult>,6> work_settings;
    std::array<unsigned,6> work_setting_last{};
    std::optional<unsigned> pending_thumb_speed;
    std::optional<unsigned> pending_sensitivity;
    std::optional<BindingEdit> pending_binding;
    std::optional<RingEdit> pending_ring;
    bool partial_io{}, io_included_hosts{}, closing{};
    std::thread worker;
    Text work_message;
    Picture side, front, bottom, firefox_icon, chrome_icon, wps_icon;
    std::unique_ptr<Gdiplus::PrivateFontCollection> symbols;
    bool symbols_loaded{};
    bool ui_active{};
    ULONG_PTR gdiplus_token{};
    HINSTANCE instance{};
    NOTIFYICONDATAW tray_icon{};
    bool tray_added{};
    bool force_exit{};
    bool ending_session{};
    bool record_recovery_start{};
    HICON tray_hicon{};
    UINT taskbar_created_msg{};
    std::thread agent_thread;
    std::atomic<bool> agent_thread_running{};
    ~App() {
        if (agent_thread.joinable()) {
            stop_agent();
            agent_thread.join();
        }
        if (reload_cancel) SetEvent(reload_cancel);
        if (reload_worker.joinable()) reload_worker.join();
        if (reload_cancel) CloseHandle(reload_cancel);
        if (worker.joinable()) worker.join();
        if (started) CloseHandle(started);
        if (paint_bitmap) DeleteObject(paint_bitmap);
        if (paint_dc) DeleteDC(paint_dc);
        for (const auto& [name,icon]:application_icons) if (icon) DestroyIcon(icon);
        for (auto font : {normal,heading,bold,subheading,small,caption}) if (font) DeleteObject(font);
        for (auto brush : {background_brush,panel_brush,field_brush}) DeleteObject(brush);
        side.unload(); front.unload(); bottom.unload();
        firefox_icon.unload(); chrome_icon.unload(); wps_icon.unload();
        ring_images.clear();
        ring_fonts.faces.clear();
        symbols.reset();
        native_typography::shutdown();
        if (gdiplus_token) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            gdiplus_token = 0;
        }
    }
    fs::path mapping() const { return directory / L"mappings.ini"; }
};

void apply_ui_theme(App& app) {
    const bool light = ui_settings::effective_light(app.ui.theme);
    if (light) {
        background = RGB(243,244,246);
        panel = RGB(255,255,255);
        raised = RGB(229,231,235);
        white = RGB(20,22,26);
        muted = RGB(105,112,122);
        teal = RGB(0,153,136);
        line_color = RGB(212,216,222);
    } else {
        background = RGB(25,25,25);
        panel = RGB(0,0,0);
        raised = RGB(36,36,36);
        white = RGB(251,251,251);
        muted = RGB(198,198,198);
        teal = RGB(0,234,208);
        line_color = RGB(53,53,53);
    }
    for (auto brush : {app.background_brush, app.panel_brush, app.field_brush})
        if (brush) DeleteObject(brush);
    app.background_brush = CreateSolidBrush(background);
    app.panel_brush = CreateSolidBrush(panel);
    app.field_brush = CreateSolidBrush(raised);
    if (app.window && app.ui_active) {
        const BOOL dark = light ? FALSE : TRUE;
        DwmSetWindowAttribute(app.window, 20, &dark, sizeof(dark));
    }
}
Text local_computer_name() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD n = static_cast<DWORD>(std::size(buf));
    return GetComputerNameW(buf, &n) && n ? Text(buf, n) : Text(L"本机");
}
Text control_text(HWND control);
void notice(App& app, Text message);
void persist_ui_settings(App& app) {
    if (!ui_settings::save(app.mapping(), app.ui))
        notice(app, L"无法写入外观与功能配置文件。");
}
void refresh_pairing_inventory(App& app) {
    app.bolt_slots.clear();
    if (const auto connected = logi::connect()) {
        if (auto* ch = connected->channel.get())
            app.bolt_slots = pairing::scan_bolt_receiver_slots(*ch);
    }
    app.bt_devices = pairing::enumerate_bluetooth_devices();
}
void read_flow_edits(App& app) {
    if (app.page != flow) return;
    if (!app.flow_name_edit || !app.flow_host_edit || !app.flow_port_edit || !app.flow_pin_edit) return;
    const auto name = Text(trim(control_text(app.flow_name_edit)));
    const auto host = Text(trim(control_text(app.flow_host_edit)));
    const auto port_str = Text(trim(control_text(app.flow_port_edit)));
    const auto pin_str = Text(trim(control_text(app.flow_pin_edit)));
    if (!pin_str.empty()) app.ui.flow_pin = pin_str;
    if (const auto p = logi::number(port_str); p && *p > 0 && *p <= 65535) app.ui.flow_port = *p;
    if (!app.ui.flow_peers.empty() && app.selected_flow_peer < app.ui.flow_peers.size()) {
        auto& peer = app.ui.flow_peers[app.selected_flow_peer];
        if (!name.empty()) peer.name = name;
        peer.host = host;
        if (const auto p = logi::number(port_str); p && *p > 0 && *p <= 65535) peer.port = *p;
    } else if (!host.empty() || !name.empty()) {
        ui_settings::FlowPeerConfig peer;
        peer.id = L"peer-1";
        peer.name = name.empty() ? L"远程电脑" : name;
        peer.host = host;
        peer.port = app.ui.flow_port;
        app.ui.flow_peers.push_back(std::move(peer));
        app.selected_flow_peer = 0;
    }
}
Text run_flow_loopback_test(const ui_settings::Settings& ui) {
    flow::Controller server;
    bool entered = false;
    std::wstring got_clip;
    std::wstring got_peer;
    flow::Controller::Callbacks cb;
    cb.on_edge_enter = [&](const flow::EdgeEvent& ev) {
        entered = true;
        got_peer = ev.peer_name;
        got_clip = ev.clipboard_text;
    };
    const auto pin = to_utf8(ui.flow_pin.empty() ? L"LOGI2026" : ui.flow_pin);
    if (!server.start(0, pin, "loopback-local", "LogiNative-Loopback", 1920, 1080, std::move(cb), false))
        return L"Flow 本地回路自检失败：无法启动 TCP 监听。";
    const unsigned port = server.bound_port();
    flow::Controller client;
    const bool ok_ping = client.ping_peer(L"127.0.0.1", port, pin, 1000);
    ui_settings::FlowPeerConfig self_peer;
    self_peer.id = L"loopback";
    self_peer.name = L"本机测试节点";
    self_peer.host = L"127.0.0.1";
    self_peer.port = port;
    self_peer.edge = ui_settings::FlowEdge::right;
    self_peer.host_slot = 1;
    const bool ok_edge = client.send_edge_switch(self_peer, 0.5, L"LogiNative-Flow-ClipTest", pin, 1000);
    Sleep(30);
    server.stop();
    if (!ok_ping || !ok_edge || !entered || got_clip != L"LogiNative-Flow-ClipTest")
        return L"Flow 本地回路自检未通过（认证或切屏回调异常）。";
    return L"Flow 本地回路自检通过（127.0.0.1:" + std::to_wstring(port) + L" HMAC-SHA256 认证与剪贴板同步正常）。";
}

bool windows_pointer_precision() {
    int params[3]{};
    return SystemParametersInfoW(SPI_GETMOUSE, 0, params, 0) && params[2] != 0;
}

bool set_windows_pointer_precision(bool enabled) {
    int params[3]{6, 10, enabled ? 1 : 0};
    SystemParametersInfoW(SPI_GETMOUSE, 0, params, 0);
    params[2] = enabled ? 1 : 0;
    return SystemParametersInfoW(SPI_SETMOUSE, 0, params, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
}

Text build_tray_tip(const App& app) {
    Text tip = L"LogiNative · ";
    tip += app.device.name.empty() ? L"MX Master 3S" : app.device.name;
    if (app.device.battery) tip += L" · " + std::to_wstring(*app.device.battery) + L"%";
    if (app.device.values[0]) tip += L" · " + std::to_wstring(*app.device.values[0]) + L" DPI";
    if (tip.size() >= 127) tip.resize(126);
    return tip;
}

void ensure_tray_icon(App& app, bool modify_only = false) {
    if (!app.window || !app.tray_hicon) return;
    app.tray_icon = {};
    app.tray_icon.cbSize = sizeof(NOTIFYICONDATAW);
    app.tray_icon.hWnd = app.window;
    app.tray_icon.uID = 1;
    app.tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.tray_icon.uCallbackMessage = tray_callback;
    app.tray_icon.hIcon = app.tray_hicon;
    const auto tip = build_tray_tip(app);
    wcsncpy_s(app.tray_icon.szTip, tip.c_str(), _TRUNCATE);
    if (!app.tray_added && !modify_only) {
        app.tray_added = Shell_NotifyIconW(NIM_ADD, &app.tray_icon) != FALSE;
    } else if (app.tray_added) {
        Shell_NotifyIconW(NIM_MODIFY, &app.tray_icon);
    }
}

void remove_tray_icon(App& app) {
    if (!app.tray_added) return;
    Shell_NotifyIconW(NIM_DELETE, &app.tray_icon);
    app.tray_added = false;
}

void create_fonts(App& app);
void render(App& app);

void trim_working_set() {
    _heapmin();
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

bool ensure_ui_resources(App& app) {
    if (app.ui_active) return true;
    lazy_win32::init_common_controls(ICC_BAR_CLASSES | ICC_STANDARD_CLASSES);
    if (!app.gdiplus_token) {
        Gdiplus::GdiplusStartupInput graphics;
        if (Gdiplus::GdiplusStartup(&app.gdiplus_token, &graphics, nullptr) != Gdiplus::Ok) return false;
    }
    native_typography::initialize();
    if (!app.normal) create_fonts(app);
    const auto instance = app.instance ? app.instance : GetModuleHandleW(nullptr);
    app.side.load(instance, 101);
    app.front.load(instance, 102);
    app.bottom.load(instance, 103);
    app.firefox_icon.load(instance, 104);
    app.chrome_icon.load(instance, 105);
    app.wps_icon.load(instance, 106);
    if (!app.symbols_loaded) {
        if (const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(107), RT_RCDATA)) {
            const auto data = LockResource(LoadResource(instance, resource));
            app.symbols = std::make_unique<Gdiplus::PrivateFontCollection>();
            app.symbols_loaded = app.symbols->AddMemoryFont(data, SizeofResource(instance, resource)) == Gdiplus::Ok;
            if (!app.symbols_loaded) app.symbols.reset();
        }
    }
    app.ui_active = true;
    if (app.window) {
        apply_ui_theme(app);
        const DWORD corners = 2;
        DwmSetWindowAttribute(app.window, 33, &corners, sizeof(corners));
    }
    return true;
}

void release_ui_resources(App& app) {
    if (app.window) KillTimer(app.window, 6);
    app.ui_active = false;
    app.animating = false;
    app.moving.clear();
    app.has_frame = false;
    app.note = app.input = app.slider = app.speed_value = app.dpi_value = app.sensitivity_value =
        app.action_list = app.search = app.ring_value = app.save_action = nullptr;
    app.ring_list = app.ring_filter = app.ring_choice = app.ring_value2 = nullptr;
    app.flow_name_edit = app.flow_host_edit = app.flow_port_edit = app.flow_pin_edit = app.action_param_box = nullptr;
    app.tips = nullptr;
    app.gesture = {};
    app.hardware = {};
    for (const auto child : app.children) DestroyWindow(child);
    app.children.clear();
    app.children.shrink_to_fit();
    app.button_skins.clear();
    app.visible.clear();
    app.visible.shrink_to_fit();
    app.ring_catalog.clear();
    app.ring_catalog.shrink_to_fit();
    app.ring_catalog_visible.clear();
    app.ring_catalog_visible.shrink_to_fit();
    app.ring_registry_cache.clear();
    app.ring_registry_cache.shrink_to_fit();
    app.ring_profile_sections.clear();
    app.ring_profile_sections.shrink_to_fit();
    app.ring_applications.clear();
    app.ring_applications.shrink_to_fit();
    app.ring_rows.clear();
    app.ring_rows.shrink_to_fit();
    app.ring_smart_actions.clear();
    app.ring_smart_actions.shrink_to_fit();
    if (app.ring_undo_stack.size() > 8) {
        app.ring_undo_stack.erase(app.ring_undo_stack.begin(), app.ring_undo_stack.end() - 8);
    }
    app.ring_undo_stack.shrink_to_fit();
    app.ring_redo_stack.shrink_to_fit();
    app.bolt_slots.clear();
    app.bolt_slots.shrink_to_fit();
    app.bt_devices.clear();
    app.bt_devices.shrink_to_fit();
    app.discovered_peers.clear();
    app.discovered_peers.shrink_to_fit();
    if (app.paint_bitmap) { DeleteObject(app.paint_bitmap); app.paint_bitmap = nullptr; }
    if (app.paint_dc) { DeleteDC(app.paint_dc); app.paint_dc = nullptr; }
    app.paint_w = app.paint_h = 0;
    app.side.unload();
    app.front.unload();
    app.bottom.unload();
    app.firefox_icon.unload();
    app.chrome_icon.unload();
    app.wps_icon.unload();
    app.ring_images.clear();
    app.ring_fonts.faces.clear();
    for (const auto& [name, icon] : app.application_icons) if (icon) DestroyIcon(icon);
    app.application_icons.clear();
    app.symbols.reset();
    app.symbols_loaded = false;
    for (auto* f : {&app.normal, &app.heading, &app.bold, &app.subheading, &app.small, &app.caption}) {
        if (*f) { DeleteObject(*f); *f = nullptr; }
    }
    native_typography::shutdown();
    if (app.gdiplus_token) {
        Gdiplus::GdiplusShutdown(app.gdiplus_token);
        app.gdiplus_token = 0;
    }
    CoFreeUnusedLibraries();
    if (!app.busy) app.connection.reset();
    trim_working_set();
}

void update_agents(App& app, bool force);
void read_config(App& app);
void io(App& app, std::vector<std::pair<unsigned,unsigned>> changes, unsigned host,bool only_dpi=false);

void restore_from_tray(App& app) {
    if (!app.window) return;
    const bool reopening=!app.ui_active || IsIconic(app.window) || !IsWindowVisible(app.window);
    update_agents(app, false);
    read_config(app);
    if (!ensure_ui_resources(app)) return;
    if (app.children.empty()) render(app);
    ShowWindow(app.window, IsIconic(app.window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(app.window);
    if (!app.busy && !app.waiting_agent && (reopening || !app.device.battery)) io(app, {}, 0);
}

void show_tray_menu(App& app) {
    if (!app.window) return;
    update_agents(app, false);
    const auto tr = [&](const wchar_t* zh, const wchar_t* en) { return ui_settings::tr(app.ui.language, zh, en); };
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HMENU hosts = CreatePopupMenu();
    for (unsigned slot = 1; slot <= 3; ++slot) {
        const auto found = std::ranges::find(app.device.hosts, slot, &logi::HostSlot::slot);
        const bool current = found != app.device.hosts.end() && found->current;
        const bool paired = found != app.device.hosts.end() && found->paired.value_or(false);
        Text label_text = Text(tr(L"通道 ", L"Host ")) + std::to_wstring(slot);
        if (current) label_text += tr(L" (当前)", L" (Active)");
        else if (!paired) label_text += tr(L" (未配对)", L" (Empty)");
        AppendMenuW(hosts, MF_STRING | (paired && !current && !app.native && !app.official ? MF_ENABLED : MF_GRAYED),
                    tray_host_1 + static_cast<int>(slot - 1), label_text.c_str());
    }
    AppendMenuW(menu, MF_STRING, tray_open, tr(L"打开 LogiNative 主界面", L"Open LogiNative"));
    AppendMenuW(menu, MF_STRING, tray_refresh, tr(L"刷新设备状态", L"Refresh Device"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hosts), tr(L"Easy-Switch 主机切换", L"Easy-Switch Host"));
    AppendMenuW(menu, MF_STRING | (app.ui.flow_enabled ? MF_CHECKED : MF_UNCHECKED),
                tray_toggle_flow, tr(L"启用 LogiNative Flow", L"Enable Flow"));
    AppendMenuW(menu, MF_STRING, tray_toggle_trial,
                app.native ? tr(L"停止设备服务", L"Stop Device Service") : tr(L"启动设备服务", L"Start Device Service"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, tray_exit, tr(L"退出 LogiNative", L"Exit LogiNative"));

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(app.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, app.window, nullptr);
    PostMessageW(app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

HFONT font(int size, int weight = FW_NORMAL) {
    return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,native_typography::nativeFamily(weight));
}
void create_fonts(App& app) {
    ++app.font_generation;
    app.normal=font(16);
    app.heading=font(36,FW_BOLD);
    app.bold=font(16,FW_SEMIBOLD);
    app.subheading=font(20);
    app.small=font(14);
    app.caption=font(13,FW_BOLD);
}
LRESULT CALLBACK edit_selection(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_KEYDOWN && wparam=='A' && GetKeyState(VK_CONTROL)<0) {
        SendMessageW(window,EM_SETSEL,0,-1);return 0;
    }
    if(message==WM_CHAR && wparam==1) return 0;
    if(message==WM_NCDESTROY) lazy_win32::remove_window_subclass(window,edit_selection,id);
    return lazy_win32::def_subclass_proc(window,message,wparam,lparam);
}
HWND add(App& app, const wchar_t* kind, const Text& title, DWORD style, int x,int y,int w,int h,
         int id = 0, HFONT use_font = nullptr, DWORD extension = 0) {
    auto control = CreateWindowExW(extension,kind,title.c_str(),WS_CHILD|WS_VISIBLE|(app.page==ring?WS_CLIPSIBLINGS:0)|style,
                                  px(x),px(y),px(w),px(h),app.window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr),nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(use_font ? use_font : app.normal),FALSE);
    if(_wcsicmp(kind,L"EDIT")==0) lazy_win32::set_window_subclass(control,edit_selection,1,0);
    app.children.push_back(control);
    return control;
}
void text_at(HDC dc,HFONT font,COLORREF color,std::wstring_view value,RECT rect,UINT flags);
LRESULT CALLBACK smooth_label(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if (message==WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc=BeginPaint(window,&paint);
        RECT box{};GetClientRect(window,&box);
        const auto brush=reinterpret_cast<HBRUSH>(SendMessageW(GetParent(window),WM_CTLCOLORSTATIC,
                                                    reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(window)));
        FillRect(dc,&box,brush);
        Text value(GetWindowTextLengthW(window)+1,L'\0');
        GetWindowTextW(window,value.data(),static_cast<int>(value.size()));value.pop_back();
        const auto align=(GetWindowLongPtrW(window,GWL_STYLE)&SS_TYPEMASK)==SS_RIGHT?DT_RIGHT:DT_LEFT;
        text_at(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),GetTextColor(dc),value,box,DT_WORDBREAK|align);
        EndPaint(window,&paint);return 0;
    }
    if (message==WM_NCDESTROY) lazy_win32::remove_window_subclass(window,smooth_label,id);
    return lazy_win32::def_subclass_proc(window,message,wparam,lparam);
}
HWND label(App& app,const Text& value,int x,int y,int width,int height,HFONT use_font = nullptr) {
    const auto control=add(app,L"STATIC",value,SS_LEFT,x,y,width,height,0,use_font);
    lazy_win32::set_window_subclass(control,smooth_label,1,0);
    return control;
}
LRESULT CALLBACK hover_button(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                              UINT_PTR id,DWORD_PTR) {
    const int cid = GetDlgCtrlID(window);
    const bool is_ring_slot = cid >= rings_first && cid < rings_first + 8;
    if (is_ring_slot && message == WM_LBUTTONDOWN && IsWindowEnabled(window)) {
        SetPropW(window, L"LogiNative.SlotDown", reinterpret_cast<HANDLE>(1));
        SetPropW(window, L"LogiNative.SlotDrag", reinterpret_cast<HANDLE>(0));
        SetPropW(window, L"LogiNative.DragX", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(GET_X_LPARAM(lparam))));
        SetPropW(window, L"LogiNative.DragY", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(GET_Y_LPARAM(lparam))));
    }
    if (is_ring_slot && message == WM_MOUSEMOVE && GetPropW(window, L"LogiNative.SlotDown")) {
        const int sx = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(window, L"LogiNative.DragX")));
        const int sy = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(window, L"LogiNative.DragY")));
        const int dx = GET_X_LPARAM(lparam) - sx, dy = GET_Y_LPARAM(lparam) - sy;
        if (dx * dx + dy * dy > px(8) * px(8)) {
            SetPropW(window, L"LogiNative.SlotDrag", reinterpret_cast<HANDLE>(1));
            SetCapture(window);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return 0;
        }
    }
    if (is_ring_slot && message == WM_LBUTTONUP && GetPropW(window, L"LogiNative.SlotDown")) {
        const bool dragged = GetPropW(window, L"LogiNative.SlotDrag") != nullptr;
        RemovePropW(window, L"LogiNative.SlotDown");
        RemovePropW(window, L"LogiNative.SlotDrag");
        if (dragged) {
            ReleaseCapture();
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            HWND parent = GetParent(window);
            MapWindowPoints(window, parent, &pt, 1);
            HWND target = ChildWindowFromPointEx(parent, pt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            const int tid = target ? GetDlgCtrlID(target) : 0;
            if (tid >= rings_first && tid < rings_first + 8 && tid != cid) {
                PostMessageW(parent, swap_ring_slots, static_cast<WPARAM>(cid - rings_first),
                             static_cast<LPARAM>(tid - rings_first));
                return 0;
            }
        }
    }
    if (message==WM_MOUSEMOVE && !GetPropW(window,L"LogiNative.Hover")) {
        TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0};
        if (TrackMouseEvent(&event)) {
            SetPropW(window,L"LogiNative.Hover",reinterpret_cast<HANDLE>(1));
            InvalidateRect(window,nullptr,FALSE);
        }
    } else if (message==WM_MOUSELEAVE || message==WM_NCDESTROY) {
        RemovePropW(window,L"LogiNative.Hover");
        RemovePropW(window,L"LogiNative.SlotDown");
        RemovePropW(window,L"LogiNative.SlotDrag");
        if (message==WM_NCDESTROY) lazy_win32::remove_window_subclass(window,hover_button,id);
        else InvalidateRect(window,nullptr,FALSE);
    }
    return lazy_win32::def_subclass_proc(window,message,wparam,lparam);
}
HWND styled_button(App& app,const Text& title,int id,int x,int y,int width,int height,ButtonSkin skin) {
    auto control = add(app,L"BUTTON",title,BS_OWNERDRAW|WS_TABSTOP,x,y,width,height,id,app.bold);
    app.button_skins.emplace(control,std::move(skin));
    lazy_win32::set_window_subclass(control,hover_button,1,0);
    return control;
}
void tooltip(App& app,HWND control) {
    TOOLINFOW info{};info.cbSize=sizeof(info);
    info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
    info.hwnd=app.window;info.uId=reinterpret_cast<UINT_PTR>(control);
    info.lpszText=const_cast<wchar_t*>(app.button_skins.at(control).caption.c_str());
    SendMessageW(app.tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
}
HICON application_icon(App& app,const Text& path) {
    auto [item,inserted]=app.application_icons.try_emplace(path,nullptr);
    if (inserted && fs::path(path).is_absolute()) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_ICON|SHGFI_LARGEICON)) item->second=info.hIcon;
    }
    return item->second;
}
HWND button(App& app,const Text& title,int id,int x,int y,int width,int height,bool active = false) {
    return styled_button(app,title,id,x,y,width,height,{Look::filled,active,{},{},{}});
}
HWND icon_button(App& app,const Text& title,int id,int x,int y,int width,int height,
                 std::wstring_view icon,Look look=Look::glyph,bool active=false) {
    return styled_button(app,title,id,x,y,width,height,{look,active,icon,{},{}});
}
HWND field_at(App& app,const Text& title,int id,int x,int y,int w,int h,DWORD style = ES_AUTOHSCROLL) {
    return add(app,L"EDIT",title,WS_TABSTOP|style,x,y,w,h,id,nullptr,WS_EX_CLIENTEDGE);
}
Text control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    Text result(length+1,L'\0');
    GetWindowTextW(control,result.data(),length+1);
    result.resize(length);
    return result;
}
void notice(App& app, Text message) {
    ++app.notice_revision;
    app.message = std::move(message);
    if (app.note) SetWindowTextW(app.note,app.message.c_str());
}
void read_config(App& app) {
    auto next = read(app.mapping());
    if (!app.source.empty() && next == app.source) {
        if (std::ranges::find(app.profiles,app.selected)==app.profiles.end()) app.selected=L"global";
        return;
    }
    app.source = std::move(next);
    app.config = parse(app.source);
    app.ring_ui=ring_metadata::read(app.source);
    if(const auto mode=native_dpi::read_mode(app.source)) app.dpi_mode=*mode;
    app.profiles = {L"global"};
    for (const auto& [name,values] : app.config) {
        if (!name.starts_with(L"ring")&&name.size() > 4 && _wcsicmp(name.c_str()+name.size()-4,L".exe")==0)
            app.profiles.push_back(name);
    }
    if (std::ranges::find(app.profiles,app.selected)==app.profiles.end()) app.selected=L"global";
}
Text get(const App& app, const Text& key, bool inherit = true) {
    const auto section = app.config.find(app.selected);
    if (section != app.config.end() && section->second.contains(key)) return section->second.at(key);
    if (inherit) {
        const auto global = app.config.find(L"global");
        if (global != app.config.end() && global->second.contains(key)) return global->second.at(key);
    }
    return {};
}
unsigned read_thumb_speed(const App& app) {
    const auto global = app.config.find(L"global");
    if (global == app.config.end()) return default_thumb_speed;
    const auto setting = global->second.find(thumb_speed_key);
    if (setting == global->second.end()) return default_thumb_speed;
    const auto speed = logi::number(setting->second);
    return speed && *speed >= min_thumb_speed && *speed <= max_thumb_speed ? *speed : default_thumb_speed;
}
bool read_smooth(const App& app,bool thumb=false) {
    const auto global=app.config.find(L"global");
    if(global==app.config.end())return false;
    const auto value=global->second.find(thumb?L"thumb.smooth":L"scroll.smooth");
    return value!=global->second.end() && value->second==L"1";
}
bool read_smooth_all(const App& app) {
    const auto global=app.config.find(L"global");
    if(global==app.config.end())return false;
    const auto value=global->second.find(L"scroll.smooth.all");
    return value!=global->second.end() && value->second==L"1";
}
Text reload_result_notice(ReloadStatus status) {
    switch (status) {
    case ReloadStatus::applied:return L"映射已保存并应用。";
    case ReloadStatus::not_running:return L"已保存；后台未运行，启动后使用此配置。";
    case ReloadStatus::invalid_mapping:return L"已保存，但后台拒绝了配置格式，原映射仍保留。";
    case ReloadStatus::hotkey_failed:return L"已保存，但快捷键注册失败，未应用此配置。";
    case ReloadStatus::hardware_failed:return L"已保存，鼠标映射未能接管，请稍后重试。";
    case ReloadStatus::device_unavailable:return L"已保存；设备未就绪，尚未确认映射应用。";
    case ReloadStatus::unsupported:return L"已保存；当前后台不支持应用确认，请更新后台。";
    case ReloadStatus::configuration_mismatch:return L"已保存；当前后台使用另一份配置，尚未应用。";
    case ReloadStatus::superseded:return L"配置或后台状态已变化，尚未确认当前映射应用。";
    case ReloadStatus::read_failed:return L"已保存，但后台未能读取配置，尚未确认应用。";
    case ReloadStatus::busy:return L"已保存；后台忙，尚未确认映射应用。";
    case ReloadStatus::timeout:return L"已保存；确认超时，映射是否应用尚不确定。";
    case ReloadStatus::cancelled:return L"已保存；已结束等待，尚未确认映射应用。";
    case ReloadStatus::stopped:return L"已保存；后台已停止，尚未确认映射应用。";
    default:return L"已保存，但无法联系后台确认映射应用。";
    }
}
void start_pending_reload(App& app) {
    KillTimer(app.window,8);
    if (app.reload_busy || !app.pending_reload) return;
    auto edit=std::move(*app.pending_reload);
    app.pending_reload.reset();
    if (!app.reload_cancel) app.reload_cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!app.reload_cancel) {
        app.reload_state=ReloadViewState::unconfirmed;
        if (app.notice_revision==edit.notice_revision) notice(app,reload_result_notice(ReloadStatus::ipc_error));
        return;
    }
    ResetEvent(app.reload_cancel);
    if (!edit.deadline) edit.deadline=GetTickCount64()+4000;
    app.reload_busy=true;
    const auto window=app.window;
    const auto cancel=app.reload_cancel;
    app.reload_worker=std::thread([&app,window,cancel,edit=std::move(edit)]() mutable {
        const auto now=GetTickCount64();
        const auto timeout=static_cast<unsigned>(edit.deadline>now?edit.deadline-now:1);
        auto result=reload_agent_confirmed(edit.path.c_str(),edit.revision,edit.fingerprint,timeout,cancel);
        app.reload_completion=ReloadCompletion{std::move(edit),std::move(result)};
        PostMessageW(window,reload_finished,0,0);
    });
}
void queue_reload(App& app,const std::string& bytes) {
    const auto revision=++app.reload_revision;
    app.reload_state=ReloadViewState::pending;
    app.last_reload_result.reset();
    notice(app,L"映射已保存，等待后台应用…");
    ReloadEdit edit{app.mapping(),bytes,{},revision,app.notice_revision};
    if (!mapping_fingerprint(bytes,edit.fingerprint)) {
        app.pending_reload.reset();
        app.reload_state=ReloadViewState::unconfirmed;
        notice(app,L"已保存，但无法确认当前配置版本。");
        return;
    }
    app.pending_reload=std::move(edit);
    start_pending_reload(app);
}
void complete_reload(App& app) {
    if (app.reload_worker.joinable()) app.reload_worker.join();
    app.reload_busy=false;
    if (!app.reload_completion) return;
    auto completion=std::move(*app.reload_completion);
    app.reload_completion.reset();
    auto& edit=completion.edit;
    auto& result=completion.result;
    if (edit.revision==app.reload_revision) {
        // The worker never reads source. Verify the immutable submitted bytes
        // against both the current editor snapshot and the file on completion.
        std::ifstream file(edit.path,std::ios::binary|std::ios::ate);
        std::string bytes;
        if (file) {
            const auto size=file.tellg();
            if (size>0) {
                bytes.resize(static_cast<std::size_t>(size));
                file.seekg(0,std::ios::beg);
                file.read(bytes.data(),size);
            }
        }
        ReloadFingerprint fingerprint{};
        const bool current=file.is_open() && !file.bad() && to_utf8(app.source)==edit.bytes &&
            mapping_fingerprint(bytes,fingerprint) && fingerprint==edit.fingerprint;
        if (!current || (result.status==ReloadStatus::applied &&
            (result.revision!=edit.revision || result.fingerprint!=edit.fingerprint || !is_current_reload_result(result))))
            result.status=ReloadStatus::superseded;
        if (result.status==ReloadStatus::busy && GetTickCount64()<edit.deadline) {
            // Do not release/reuse a processing server slot. Retry only the latest
            // local version, within its original deadline, while keeping the UI live.
            app.pending_reload=std::move(edit);
            SetTimer(app.window,8,100,nullptr);
            return;
        }
        app.last_reload_result=result;
        app.reload_state=result.status==ReloadStatus::applied?ReloadViewState::applied:ReloadViewState::unconfirmed;
        if (app.notice_revision==edit.notice_revision) notice(app,reload_result_notice(result.status));
    }
    start_pending_reload(app);
}
bool save(App& app,const Text& next) {
    if (read(app.mapping()) != app.source) {
        read_config(app);
        notice(app,L"配置已被其他程序修改。请重新打开此项。");
        return false;
    }
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(app.directory.c_str(),L"LNV",0,temporary)) {
        notice(app,L"无法在程序目录写入配置。");
        return false;
    }
    const auto bytes = to_utf8(next);
    std::ofstream output(fs::path{temporary},std::ios::binary);
    output.write(bytes.data(),bytes.size());
    output.close();
    const bool valid = output && !bytes.empty() && check_mappings(temporary);
    const bool current = valid && read(app.mapping()) == app.source;
    const bool saved = current && MoveFileExW(temporary,app.mapping().c_str(),
                                          MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if (!saved) DeleteFileW(temporary);
    if (!saved) {
        if (!valid) notice(app,L"动作格式无效，配置未修改。");
        else if (!current) notice(app,L"配置已被其他程序修改。请重新打开此项。");
        else notice(app,L"无法替换配置文件。");
        return false;
    }
    read_config(app);
    queue_reload(app,bytes);
    return true;
}
void flush_binding_edit(App& app) {
    KillTimer(app.window,5);
    if (!app.pending_binding) return;
    const auto edit=std::move(*app.pending_binding);
    app.pending_binding.reset();
    if (!edit.changes.empty()) save(app,change_section(app.source,edit.section,edit.changes));
}
void normalize_media_edit(const App& app,const Text& key,Section& changes) {
    const auto value=[&](const Text& item) {
        const auto found=changes.find(item);
        return found!=changes.end()?found->second:get(app,item);
    };
    if(value(key)!=L"GestureMedia")return;
    constexpr const wchar_t* expected[]{L"PreviousTrack",L"NextTrack",L"VolumeUp",L"VolumeDown"};
    for(unsigned i=0;i<4;++i)if(value(key+gesture_suffix[i+1])!=expected[i]) {
        changes[key]=L"PlayPause"; // Direction customization returns to ordinary gestures.
        return;
    }
}
void queue_binding_edit(App& app) {
    if (app.pane_index<0 || static_cast<std::size_t>(app.pane_index)>=std::size(button_points)) return;
    BindingEdit edit{app.selected,{}};
    const Text key=button_points[app.pane_index].key;
    if (key==L"thumb") {
        edit.changes[L"thumb.left"]=control_text(app.gesture[0]);
        edit.changes[L"thumb.right"]=control_text(app.gesture[1]);
    } else for (unsigned i=0;i<app.gesture.size();++i) {
        const auto item=key+gesture_suffix[i];
        const auto value=control_text(app.gesture[i]);
        if (value!=get(app,item)) edit.changes[item]=value;
    }
    normalize_media_edit(app,key,edit.changes);
    app.pending_binding=std::move(edit);
    SetTimer(app.window,5,180,nullptr);
}
Text action_name(const Text& code) {
    if (code==L"GestureMedia") return L"播放 / 暂停";
    if (code==L"GestureZoomRotate") return L"重置缩放 (Ctrl+0)";
    if (code==L"GestureAppNav") return L"切换应用程序 (连续)";
    if (code==L"Pan") return L"中键（按住移动以平移）";
    if (code==L"ZoomReset") return L"重置缩放";
    if (code==L"RotateLeft") return L"向左旋转";
    if (code==L"RotateRight") return L"向右旋转";
    if (code==L"Win+Left") return L"窗口靠左";
    if (code==L"Win+Right") return L"窗口靠右";
    if (code==L"Alt+Shift+Tab") return L"上一个应用程序";
    if (code==L"CloseWindow") return L"关闭窗口";
    if (code==L"ThisPC") return L"此PC";
    if (code==L"BrightnessUp") return L"亮度升高";
    if (code==L"BrightnessDown") return L"亮度降低";
    if (code==L"MicrophoneMute") return L"麦克风静音";
    if (code==L"ScreenCapture(Screen,File)") return L"屏幕截图";
    if (code==L"TextHex(d83dde00)") return L"表情符号 😀";
    if (code==L"Win+.") return L"表情符号菜单";
    if (code==L"Win+H") return L"听写";
    if (code==L"Win+A") return L"操作中心";
    if (code==L"Win+Shift+F23") return L"Windows Copilot";
    if (code==L"Win+Space") return L"输入语言";
    if (code==L"RControl") return L"右Ctrl";
    if (code.starts_with(L"Open(") && code.ends_with(L")")) {
        const auto target = code.substr(5, code.size() - 6);
        if (target == L"calc.exe") return L"计算器";
        return target.empty() ? L"打开应用程序 / 文件 / 网页" : L"打开：" + target;
    }
    if (code.starts_with(L"DpiToggle(")) return L"调整指针速度 (" + code.substr(10, code.size() - 11) + L")";
    if (code.starts_with(L"AdvancedClick(")) return L"高级点击 (" + code.substr(14, code.size() - 15) + L")";
    if (code == L"Widget(spotlight)") return L"Spotlight 聚焦";
    if (code == L"Widget(stopwatch)") return L"秒表计时器";
    if (code == L"Widget(clock)") return L"时钟小部件";
    if (code == L"Widget(system)") return L"系统状态面板";
    if (code.starts_with(L"HostSwitch(")) return L"Easy-Switch 通道 " + code.substr(11, code.size() - 12);
    for (const auto& action:actions) if (code == action.code) return action.name;
    return code.empty()?L"未设置":code;
}
Text point_title(const App& app, const Point& point) {
    if (point.key == std::wstring_view{L"thumb"}) {
        const auto left = get(app,L"thumb.left"), right = get(app,L"thumb.right");
        for (const auto& action:wheel_actions) if (left==action.left && right==action.right) return action.name;
        return L"自定义";
    }
    const Text key = point.key;
    std::array<Text,std::size(gesture_suffix)> rules;
    for (std::size_t i=0;i<rules.size();++i) rules[i]=get(app,key+gesture_suffix[i]);
    for (const auto& preset:gesture_presets)
        if (std::equal(rules.begin(),rules.end(),preset.rules.begin())) return L"手势 · " + Text(preset.title);
    if (std::any_of(rules.begin()+1,rules.end(),[](const Text& rule){ return !rule.empty() && rule!=L"None"; }))
        return L"手势 · 自定义";
    return action_name(rules[0]);
}
void update_agents(App& app, bool force = false) {
    const bool was_native = app.native;
    app.native = agent_running();
    const auto now = GetTickCount64();
    if (force || app.native != was_native || !app.official_checked_at || now - app.official_checked_at >= 1000) {
        app.official = options_running();
        app.official_checked_at = now ? now : 1;
    }
}

Device query_device(logi::Mouse& connection, bool include_hosts = false) {
    Device device;
    auto* mouse=&connection;
    device.name = from_utf8(mouse->device.name);
    device.serial = mouse->device.serial;
    device.firmware = mouse->device.firmware;
    device.receiver_firmware = mouse->device.receiver_firmware;
    device.detail = L"HID++ " + std::to_wstring(mouse->device.major) + L"." + std::to_wstring(mouse->device.minor);
    if (const auto battery=mouse->request(mouse->battery,mouse->unified_battery?1:0);
        battery && battery.packet.data()[0]<=100) {
        device.battery=battery.packet.data()[0];
        device.detail = std::to_wstring(battery.packet.data()[0]) + L"%  ·  Logi Bolt";
    }
    device.values[0]=logi::get(*mouse,hardware_keys[0]);
    if(device.values[0])device.dpi_sampled_at=GetTickCount64();
    const auto status=read_agent_settings();
    if (status.available && (status.observed_mask&3u)==3u) {
        device.values[1]=status.observed[0];
        device.values[2]=status.observed[1];
    } else if (const auto shift=mouse->request(mouse->shift,mouse->enhanced_shift?1:0)) {
        const auto d=shift.packet.data();
        device.values[1]=d[1]==255?0u:static_cast<unsigned>(d[1]);
        device.values[2]=d[0]==2?1u:0u;
    }
    if (status.available && (status.observed_mask&12u)==12u) {
        device.values[3]=status.observed[2];
        device.values[4]=status.observed[3];
    } else if (const auto wheel=mouse->request(mouse->wheel,1)) {
        const auto flags=wheel.packet.data()[0];
        device.values[3]=(flags&2)?1u:0u;
        device.values[4]=(flags&4)?1u:0u;
    }
    if (status.available) {
        device.values[5]=(status.observed_mask&16u)?std::optional{status.observed[4]}:std::nullopt;
        device.thumb_cached=(status.observed_mask&16u)!=0;
    } else if (!agent_running()) {
        device.values[5]=logi::get(*mouse,hardware_keys[5]);
    }
    device.dpi=mouse->dpi_choices;
    if (include_hosts) device.hosts=logi::hosts(*mouse);
    return device;
}
void load_cached_device(App& app) {
    if (!app.native) return;
    const auto status = read_agent_status();
    if (!status.available || !status.mappings_ready) return;
    if (app.device.dpi.empty()) {
        for (unsigned d = 200; d <= 8000; d += 50) app.device.dpi.push_back(d);
    }
    auto dpi_path = app.mapping();
    dpi_path.replace_extension(L".dpi");
    std::ifstream input(dpi_path);
    std::string name;
    unsigned dpi_val{};
    if (std::getline(input, name)) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
    }
    if (!name.empty()) {
        app.device.name = from_utf8(name);
        app.device.detail = L"Logi Bolt";
        if (status.settings_ready && (input >> dpi_val) &&
            std::ranges::find(app.device.dpi, dpi_val) != app.device.dpi.end()) {
            app.device.values[0] = dpi_val;
        }
    } else {
        app.device.name = L"MX Master 3S";
        app.device.detail = L"Logi Bolt";
    }
    const auto settings = read_agent_settings();
    if (settings.available) {
        for (unsigned bit = 0; bit < 5; ++bit)
            if (settings.observed_mask & (1u << bit))
                app.device.values[bit + 1] = settings.observed[bit];
        app.device.thumb_cached = (settings.observed_mask & 16u) != 0;
    }
}
Text dpi_result_notice(const DpiCommandResult& result) {
    Text message;
    if (result.applied) {
        message=L"指针速度已生效";
        if (!result.saved) message+=L"，但未能保存供下次启动使用";
    } else if (result.status==DpiCommandStatus::unsupported)
        message=L"当前后台不支持指针速度更新，请更新后台后重试";
    else if (result.status==DpiCommandStatus::configuration_mismatch)
        message=L"当前后台使用另一份配置，未修改指针速度";
    else if (result.status==DpiCommandStatus::timeout)
        message=L"确认超时，指针速度是否生效尚不确定";
    else if (result.status==DpiCommandStatus::busy) message=L"后台忙，请稍后重试";
    else if (result.status==DpiCommandStatus::device_unavailable) message=L"鼠标未连接，请唤醒后重试";
    else if (result.status==DpiCommandStatus::readback_failed) message=L"无法读取指针速度，尚未确认生效";
    else if (result.status==DpiCommandStatus::invalid_value) message=L"鼠标不支持这个指针速度";
    else if (result.status==DpiCommandStatus::write_failed) message=L"指针速度设置失败，请重试";
    else if (result.status==DpiCommandStatus::stopped || result.status==DpiCommandStatus::not_running)
        message=L"后台已停止，指针速度请求已取消";
    else message=L"无法联系后台，指针速度尚未确认";
    if (result.observed_valid) message+=L"；设备读回 "+std::to_wstring(result.observed)+L" DPI";
    return message+L"。";
}
unsigned long long setting_group_revision(const std::array<unsigned long long,6>& revisions,unsigned id) {
    return id<=2?std::max(revisions[1],revisions[2]):id<=4?std::max(revisions[3],revisions[4]):revisions[id];
}
std::optional<unsigned> shown_setting(const App& app,unsigned id) {
    return app.setting_unconfirmed[id] && app.desired_setting[id]?app.desired_setting[id]:app.device.values[id];
}
Text setting_result_notice(const SettingResult& result,bool confirmed) {
    constexpr const wchar_t* names[]{L"SmartShift",L"滚轮模式",L"高分辨率滚动",L"滚动方向",L"拇指滚轮方向"};
    Text message=names[static_cast<unsigned>(result.key)];
    if (confirmed) return message+(result.saved?L"已生效。":L"已生效，但未能保存供下次启动使用。");
    switch (result.status) {
    case SettingStatus::not_running: return message+L"尚未应用：设备服务未运行，请先在「设置」启动服务。";
    case SettingStatus::unsupported: return message+L"尚未应用：当前后台不支持，请更新后台后重试。";
    case SettingStatus::configuration_mismatch: return message+L"尚未应用：后台使用另一份配置。";
    case SettingStatus::busy: return message+L"尚未应用：后台忙，请稍后重试。";
    case SettingStatus::device_unavailable: return message+L"尚未应用：设备未就绪，请唤醒后重试。";
    case SettingStatus::official_conflict: return message+L"尚未应用：请先关闭官方原版后台。";
    case SettingStatus::readback_failed: return message+L"读取确认失败，是否生效尚不确定。";
    case SettingStatus::write_failed: return message+L"设置失败，请重试。";
    case SettingStatus::invalid_value: return message+L"不支持这个值。";
    case SettingStatus::timeout: return message+L"确认超时，是否生效尚不确定。";
    case SettingStatus::stopped: case SettingStatus::cancelled: return message+L"请求已取消，尚未确认生效。";
    default: return message+L"尚未确认，请重试。";
    }
}
// Runs on the UI thread after joining the worker. Actual values may inform the
// next request, but an old ACK must not clear or display over newer user intent.
bool finish_settings(App& app) {
    bool any{},visible{};
    unsigned long long last_revision{};
    app.setting_last_notice.clear();
    for (const unsigned group:{1u,3u,5u}) {
        const auto last=app.work_setting_last[group];
        if (!last) continue;
        any=true;
        const auto& result=*app.work_settings[last];
        const unsigned id=static_cast<unsigned>(result.key)+1;
        const bool valid=id==last && result.revision==app.io_setting_revision[id] &&
            app.io_setting_values[id]==result.requested;
        const bool current=valid && result.applied && (result.observed_mask&(1u<<(id-1))) &&
            result.observed[id-1]==result.requested && is_current_setting_result(result);
        const unsigned affected=group==1?3u:group==3?12u:16u;
        for (unsigned bit=0;bit<5;++bit) if (affected&(1u<<bit)) {
            app.next_device.values[bit+1]=current && (result.observed_mask&(1u<<bit))?
                std::optional{result.observed[bit]}:std::nullopt;
        }
        if (setting_group_revision(app.io_setting_revision,group)!=setting_group_revision(app.setting_revision,group)) continue;
        visible=true;
        for (unsigned field=1;field<6;++field) if (affected&(1u<<(field-1))) {
            const auto& own=app.work_settings[field];
            const auto expected=app.desired_setting[field]?app.desired_setting[field]:app.io_setting_values[field];
            const bool own_valid=!own || (static_cast<unsigned>(own->key)+1==field &&
                own->revision==app.io_setting_revision[field] && app.io_setting_values[field]==own->requested);
            const bool matched=current && own_valid && expected && app.next_device.values[field]==expected;
            app.setting_unconfirmed[field]=expected && (!matched || !result.saved || (own && (!own->applied || !own->saved)));
            Text field_message;
            if (own) field_message=own_valid?setting_result_notice(*own,matched && own->applied):L"设置回执不匹配，尚未确认生效。";
            else if (expected && !matched)
                field_message=Text(field==1?L"SmartShift":field==2?L"滚轮模式":field==3?L"高分辨率":L"滚动方向")+L"期望值尚未确认，请核对当前设置。";
            app.setting_failure_notice[field]=app.setting_unconfirmed[field]?field_message:Text{};
            if (!field_message.empty() && app.io_setting_revision[field]>=last_revision) {
                last_revision=app.io_setting_revision[field];
                app.setting_last_notice=std::move(field_message);
            }
        }
    }
    return !any || visible;
}
Text parameter_result_notice(const App& app) {
    Text message=!app.dpi_failure_notice.empty()?app.dpi_failure_notice:
        !app.setting_last_notice.empty()?app.setting_last_notice:app.work_message;
    constexpr const wchar_t* names[]{L"",L"SmartShift",L"逐行模式",L"高分辨率",L"滚动方向",L"拇指方向"};
    Text pending;
    unsigned count{};
    for (unsigned id=1;id<6;++id) {
        const auto& failure=app.setting_failure_notice[id];
        if (failure.empty() || message==failure) continue;
        if (++count<=2) {
            if (!pending.empty()) pending+=L"、";
            pending+=names[id];
        }
    }
    if (count) {
        message+=L" 未确认："+pending;
        if (count>2) message+=L"等"+std::to_wstring(count)+L"项";
        message+=L"。";
    }
    return message;
}
void io(App& app, std::vector<std::pair<unsigned,unsigned>> changes = {}, unsigned host = 0,bool only_dpi) {
    if (app.busy) {
        for (const auto& [id,value]:changes) app.pending_values[id]=value;
        return;
    }
    if ((!changes.empty() || host) && options_running()) {
        notice(app,L"检测到官方 Logi 后台正在占用设备，请先在「设置」启动设备服务。");
        return;
    }
    if (agent_running() && host) {
        notice(app,L"切换电脑前，请先退出两个鼠标后台。");
        return;
    }
    const bool include_hosts = app.page == easy;
    const bool pointer_read=only_dpi && app.connection.has_value();
    app.busy=true;
    app.partial_io=!changes.empty() && !host;
    app.io_included_hosts=changes.empty() && !host && include_hosts;
    app.io_dpi_revision=app.dpi_revision;
    app.io_setting_revision=app.setting_revision;
    app.io_setting_values={};
    for (const auto& [id,value]:changes) if (id && id<6) app.io_setting_values[id]=value;
    app.work_settings={};
    app.work_setting_last={};
    app.io_writes_dpi=std::ranges::any_of(changes,[](auto item){return item.first==0;});
    notice(app,app.partial_io?L"正在应用设置…":L"正在读取设备…");
    app.io_notice_revision=app.notice_revision;
    app.worker=std::thread([&app, base=app.device, changes=std::move(changes), host, include_hosts, pointer_read]() mutable {
        app.work_message.clear();
        app.work_unattempted.clear();
        app.work_dpi_failed=app.io_writes_dpi;
        if (const auto dpi=std::ranges::find_if(changes,[](auto item){return item.first==0;});dpi!=changes.end()) {
            const bool owned=agent_running();
            const auto result=set_agent_dpi(dpi->second,4000,app.mapping().c_str());
            // An active (including older) daemon owns DPI writes. A failed IPC
            // request must never race its HID handle through a direct fallback.
            if (owned || result.status!=DpiCommandStatus::not_running) {
                base.values[0]=result.observed_valid?std::optional{result.observed}:std::nullopt;
                app.work_dpi_failed=!result.applied || !result.saved;
                app.work_message=dpi_result_notice(result);
                changes.erase(dpi);
                if (!result.applied || !result.saved || (changes.empty() && !host)) {
                    app.work_unattempted=std::move(changes);
                    app.next_device=std::move(base);
                    PostMessageW(app.window,finished,0,0);
                    return;
                }
            }
        }
        // Wheel parameters have one owner, including when no daemon is running.
        // In particular, a failed/unsupported IPC must never fall through to HID.
        for (auto item=changes.begin();item!=changes.end();) {
            const auto [id,value]=*item;
            if (!id || id>=std::size(hardware_keys)) {++item;continue;}
            const auto result=set_agent_setting(static_cast<SettingKey>(id-1),value,
                app.io_setting_revision[id],4000,app.mapping().c_str());
            app.work_settings[id]=result;
            app.work_setting_last[id<=2?1:id<=4?3:id]=id;
            for (unsigned bit=0;bit<5;++bit)
                if (result.observed_mask&(1u<<bit)) base.values[bit+1]=result.observed[bit];
            item=changes.erase(item);
        }
        if (changes.empty() && app.partial_io) {
            app.next_device=std::move(base);
            PostMessageW(app.window,finished,0,0);
            return;
        }
        if(pointer_read) {
            // Reuse the GUI's read handle for a single sensor read while the panel
            // is open. If the handle became invalid, rediscover through the usual path.
            if(const auto observed=logi::get(*app.connection,hardware_keys[0])) {
                base.values[0]=*observed;
                base.dpi_sampled_at=GetTickCount64();
                app.next_device=std::move(base);
                PostMessageW(app.window,finished,0,0);
                return;
            }
        }
        if (changes.empty()) app.connection.reset();
        if (!app.connection) app.connection=logi::connect();
        if (app.connection) {
            auto* mouse=&*app.connection;
            if (!changes.empty() || host) {
                bool success=true;
                bool dpi_saved=true;
                if (host) success=logi::switch_host(*mouse,host);
                for (const auto& [id,value]:changes) {
                    const bool written=logi::set(*mouse,hardware_keys[id],value);
                    base.values[id]=logi::get(*mouse,hardware_keys[id]);
                    if (id==0) app.work_dpi_failed=!written || base.values[id]!=value;
                    if (!written || base.values[id]!=value) {
                        success=false;
                        break;
                    }
                    if (id==0) {
                        dpi_saved=logi::remember_dpi(app.mapping(),*mouse,value);
                        app.work_dpi_failed=!dpi_saved;
                    }
                }
                // SmartShift and ratchet mode share one hardware setting.
                if (std::ranges::any_of(changes,[](auto change){return change.first==1 || change.first==2;})) {
                    if (const auto shift=mouse->request(mouse->shift,mouse->enhanced_shift?1:0)) {
                        const auto d=shift.packet.data();
                        base.values[1]=d[1]==255?0u:static_cast<unsigned>(d[1]);
                        base.values[2]=d[0]==2?1u:0u;
                    } else {
                        base.values[1].reset();
                        base.values[2].reset();
                    }
                }
                app.work_message=success?(host?L"鼠标已接受切换请求。":L"已生效")
                                        :L"设备未确认此次写入，请核对当前值。";
                if (success && !dpi_saved) app.work_message=L"指针速度已生效，但未能保存供下次启动使用。";
                if (!success) app.connection.reset();
            }
            if (!changes.empty() && !host) app.next_device=std::move(base);
            else if (!host) {
                app.next_device=query_device(*app.connection, include_hosts);
                if (!include_hosts && app.next_device.hosts.empty())
                    app.next_device.hosts = std::move(base.hosts);
            }
            else {app.connection.reset();app.next_device=std::move(base);}
        } else {
            base.values[0].reset();
            base.dpi_sampled_at=0;
            app.next_device=std::move(base);
            app.work_message=L"未找到响应的鼠标。唤醒后重试或点刷新。";
        }
        PostMessageW(app.window,finished,0,0);
    });
}
void flush_parameters(App& app) {
    KillTimer(app.window,2);
    if (app.busy || app.pending_values.empty()) return;
    std::vector<std::pair<unsigned,unsigned>> changes;
    for (const auto& [id,value]:app.pending_values)
        if (app.device.values[id]!=value || (id==0?app.dpi_unconfirmed:app.setting_unconfirmed[id]))
            changes.emplace_back(id,value);
    std::ranges::stable_sort(changes,[&](auto left,auto right) {
        return app.setting_revision[left.first]<app.setting_revision[right.first];
    });
    app.pending_values.clear();
    if (!changes.empty()) io(app,std::move(changes));
}
void queue_parameter(App& app,unsigned id,unsigned value,bool typing=false) {
    if (id==0) {
        ++app.dpi_revision;
        app.desired_dpi=value;
        // Even choosing the displayed DPI is an explicit request: the device
        // may have reset or a different client may have changed the sensor.
        app.dpi_unconfirmed=true;
    } else {
        app.setting_revision[id]=++app.setting_serial;
        app.desired_setting[id]=value;
        app.setting_failure_notice[id].clear();
        // Even an equal value is a new intent: an earlier coupled SmartShift /
        // ratchet write may still change it before this request is dispatched.
        app.setting_unconfirmed[id]=true;
    }
    app.pending_values[id]=value;
    if (typing) SetTimer(app.window,2,120,nullptr);
    else flush_parameters(app);
}
unsigned shown_sensitivity(const App& app) {
    return native_smartshift::display(shown_setting(app,1).value_or(0),
        app.pending_sensitivity?app.pending_sensitivity:native_smartshift::read(app.source));
}
bool smartshift_enabled(const App& app) {
    return shown_setting(app,1).value_or(0)!=0 && shown_setting(app,2).value_or(1)!=0;
}
void save_sensitivity(App& app) {
    KillTimer(app.window,9);
    if(!app.pending_sensitivity)return;
    if(app.busy || !app.pending_values.empty()) {SetTimer(app.window,9,120,nullptr);return;}
    // Hardware persistence may have updated the same file while dragging. Merge
    // only this UI comment into the latest configuration after the owner finishes.
    read_config(app);
    const auto value=*app.pending_sensitivity;
    const auto before=app.source;
    if(native_smartshift::read(app.source)==value || save(app,native_smartshift::write(app.source,value)))
        app.pending_sensitivity.reset();
    else if(read(app.mapping())!=before) SetTimer(app.window,9,120,nullptr);
}
std::optional<unsigned> shown_dpi(const App& app) {
    const bool pending=app.pending_values.contains(0) || (app.busy && app.io_writes_dpi);
    return pending && app.desired_dpi?app.desired_dpi:app.device.values[0];
}
void refresh_pointer_dpi(App& app) {
    if(app.page!=pointer || app.busy || app.waiting_agent || app.waiting_stop || !app.pending_values.empty())return;
    // Entering the page and opening its panel often happen in one click sequence.
    if(!app.device.dpi_sampled_at || GetTickCount64()-app.device.dpi_sampled_at>=1000)io(app,{},0,true);
}
bool extended_dpi(App& app) {
    const auto shown=shown_dpi(app);
    if(shown.value_or(0)>4000)app.dpi_mode=true;
    if(!app.dpi_mode)app.dpi_mode=false;
    return *app.dpi_mode;
}
void sync_dpi_control(App& app) {
    if (app.page!=pointer || app.pane_index!=1 || app.busy || !app.pending_values.empty() ||
        !app.dpi_value || !app.slider || GetCapture()==app.slider) return;
    const auto value=app.device.values[0];
    // Programmatic readback must not enqueue another hardware write.
    const bool was_rendering=app.rendering;
    app.rendering=true;
    SetWindowTextW(app.dpi_value,value?(std::to_wstring(*value)+L" DPI").c_str():L"— DPI");
    ShowWindow(app.slider,value?SW_SHOWNA:SW_HIDE);
    if(value)SendMessageW(app.slider,TBM_SETPOS,TRUE,native_dpi::to_percent(*value,extended_dpi(app)));
    for(unsigned i=0;i<pointer_presets.size();++i) {
        const auto control=GetDlgItem(app.window,pointer_dpi_800+static_cast<int>(i));
        if(auto skin=app.button_skins.find(control);skin!=app.button_skins.end() && skin->second.active!=(value==pointer_presets[i])) {
            skin->second.active=value==pointer_presets[i];
            InvalidateRect(control,nullptr,FALSE);
        }
    }
    if(app.hardware[0])SendMessageW(app.hardware[0],BM_SETCHECK,extended_dpi(app)?BST_CHECKED:BST_UNCHECKED,0);
    InvalidateRect(app.slider,nullptr,FALSE);
    app.rendering=was_rendering;
}
void start_agent(App& app) {
    if (app.agent_thread.joinable() && !app.agent_thread_running.load()) app.agent_thread.join();
    update_agents(app);
    if (app.waiting_agent) return;
    const auto begin_wait = [&](HANDLE process) {
        if (app.started) { CloseHandle(app.started); app.started = nullptr; }
        app.started = process;
        app.waiting_agent = true;
        app.waiting_stop = false;
        app.timer_checks = 0;
        SetTimer(app.window, 1, 50, nullptr);
        notice(app, L"正在接管鼠标…");
        app.startup_notice_revision = app.notice_revision;
    };
    if (app.native) {
        if (!options_running()) {
            const auto status = read_agent_status();
            if (status.available && !status.mappings_ready) {
                if (app.agent_thread_running.load() && status.pid == GetCurrentProcessId()) {
                    begin_wait(nullptr);
                } else if (const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, status.pid)) {
                    begin_wait(process);
                }
            }
            return;
        }
        // Reconcile native + official dual-agent conflict completely in-process.
        stop_agent();
        for (int i = 0; i < 50 && agent_running(); ++i) Sleep(50);
        if (app.agent_thread.joinable() && !app.agent_thread_running.load()) app.agent_thread.join();
        update_agents(app);
        if (app.native) return;
    }
    if (!check_mappings(app.mapping().c_str())) { notice(app, L"配置无效，请先检查按键设置。"); return; }
    if (app.agent_thread.joinable()) {
        if (app.agent_thread_running.load()) return;
        app.agent_thread.join();
    }
    app.agent_thread_running.store(true);
    begin_wait(nullptr);
    const auto map_path = app.mapping();
    app.agent_thread = std::thread([&app, map_path] {
        run_agent(map_path.c_str(), true);
        app.agent_thread_running.store(false);
    });
}

bool check_agent_startup(App& app,bool timeout) {
    if (!app.waiting_agent) return false;
    const bool external = app.started != nullptr;
    const bool exited = external ? (WaitForSingleObject(app.started, 0) == WAIT_OBJECT_0) : !app.agent_thread_running.load();
    const DWORD started_pid = external ? GetProcessId(app.started) : GetCurrentProcessId();
    const auto status = read_agent_status();
    const bool same_process = status.available && started_pid && status.pid == started_pid;
    if (app.record_recovery_start && !external && !exited && same_process) {
        app.record_recovery_start = false;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        agent_log::agent_message(agent_log::LogLevel::error, stderr,
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] Tray recovery acquired background ownership after finding no owner. pid=%lu\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, started_pid);
    }
    const bool mappings_ready = same_process && !exited && app.native && status.mappings_ready;
    if (!mappings_ready && !exited && !timeout) return false;
    KillTimer(app.window,1);
    app.waiting_agent=false;
    update_agents(app,true);
    // Startup confirmation is observational. The daemon owns reconnect/recovery;
    // a UI deadline must not terminate that work or a working mapping owner.
    if (app.notice_revision==app.startup_notice_revision) {
        if (mappings_ready) {
            Text message=status.settings_ready && status.ready?L"本地后台已接管鼠标":
                L"映射已接管；启动检查尚未确认鼠标参数已恢复或保存";
            if (app.official) message+=L"；原版后台仍在运行";
            notice(app,message+L"。");
        }
        else notice(app,exited?L"后台已退出，未能接管鼠标。":same_process?
            L"尚未确认本次后台接管，后台将继续重试。":L"尚未确认本次后台接管，请稍后重试。");
    }
    if (mappings_ready && !app.official) {
        if (app.device.name.empty()) load_cached_device(app);
        for (unsigned id=1;id<6;++id)
            if (app.setting_unconfirmed[id] && app.desired_setting[id])
                app.pending_values.try_emplace(id,*app.desired_setting[id]);
        flush_parameters(app);
        // Device labels and Easy-Switch details must not delay takeover or race
        // the daemon's initial HID requests on a second handle.
        if(app.ui_active && !app.busy && !app.device.battery) io(app);
    }
    return true;
}

RECT target_image_area(const App& app) {
    const bool editing=app.pane_index>=0;
    if (app.page==easy) return {489,242,797,720};
    const int x=editing?433:665;
    return {x,205,x+300,686};
}
RECT image_area(const App& app) { return app.animating?app.visual_image:target_image_area(app); }
POINT pin_position(const RECT& image,const Point& pin) {
    return {image.left+(image.right-image.left)*pin.x/100,
            image.top+(image.bottom-image.top)*pin.y/100};
}
std::span<const Point> page_points(Page page) {
    if (page==buttons) return button_points;
    if (page==pointer) return pointer_points;
    return {};
}
int pin_at(const App& app,POINT client) {
    const auto pins=page_points(app.page);
    const auto image=image_area(app);
    for (std::size_t i=0;i<pins.size();++i) {
        const auto position=pin_position(image,pins[i]);
        const auto x=client.x-px(position.x), y=client.y-px(position.y);
        if (x*x+y*y<=px(13)*px(13)) return pins_first+static_cast<int>(i);
    }
    return 0;
}
Text pointer_summary(const App& app,std::size_t index) {
    const auto& values=app.device.values;
    if (index==1) {
        const auto dpi=shown_dpi(app);
        return L"速度：" + (dpi?std::to_wstring(*dpi)+L" DPI":Text(L"—"));
    }
    if (index==2) return L"速度：" + std::to_wstring(read_thumb_speed(app)) +
                         L"%\n滚动方向：" + (!values[5]?L"尚未确认":*values[5]?L"已反转":L"默认");
    return Text(L"滚动方向：")+(values[4].value_or(0)?L"反转":L"标准")+
           L"\n平滑滚动：" + (read_smooth(app)?L"开启":L"关闭")+
           L"\nSmartShift：" + (values[1].value_or(0)&&values[2].value_or(1)?L"开启":L"关闭");
}
int text_width(App& app,std::wstring_view value,HFONT font);
int text_line_height(App& app,HFONT font) {
    struct Entry { HFONT font{}; unsigned generation{}; int dpi{}; bool ring{}; int height{}; };
    static std::array<Entry, 8> cache{};
    static std::size_t next{};
    const bool is_ring = app.page == ring;
    for (const auto& entry : cache)
        if (entry.font == font && entry.generation == app.font_generation &&
            entry.dpi == layout_dpi && entry.ring == is_ring && entry.height > 0)
            return entry.height;
    const int height=static_cast<int>(std::ceil(native_typography::measureText(nullptr,font,L"中文Ag",is_ring).Height*96.0f/layout_dpi));
    cache[next++ % cache.size()] = {font, app.font_generation, layout_dpi, is_ring, height};
    return height;
}
void points(App& app) {
    const auto bounds=image_area(app);
    const auto items=page_points(app.page);
    for (unsigned i=0;i<items.size();++i) {
        const auto& point=items[i];
        const auto [x,y]=pin_position(bounds,point);
        auto title=app.page==buttons?point_title(app,point):Text(point.title);
        auto secondary=app.page==buttons?Text(point.title):pointer_summary(app,i);
        if (title.starts_with(L"手势 · ")) {
            secondary=title.substr(5)+L"\n"+secondary;
            title=L"手势";
        }
        if (title==L"音量升高/降低") title=L"音量升高/\n降低";
        const int title_lines=1+static_cast<int>(std::ranges::count(title,L'\n'));
        const int lines=1+static_cast<int>(std::ranges::count(secondary,L'\n'));
        int required=point.width;
        for (const auto& text: ::lines(title)) required=std::max(required,text_width(app,text,app.bold)+32);
        for (const auto& text: ::lines(secondary)) required=std::max(required,text_width(app,text,app.small)+32);
        const int width=std::min(required,240);
        const int height=21+text_line_height(app,app.bold)*title_lines+text_line_height(app,app.small)*lines;
        const int left=point.align<0?x-width-24:point.align==0?x-width/2+4:x+28;
        const int top=point.align==0?y+29:y-height/2;
        ButtonSkin skin{Look::hotspot,app.pane_index==static_cast<int>(i),{},title,secondary,title_lines};
        if (const auto control=GetDlgItem(app.window,pins_first+i)) {
            app.button_skins[control]=std::move(skin);
            SetWindowTextW(control,(title+L" "+secondary).c_str());
            SetWindowPos(control,nullptr,px(left),px(top),px(width),px(height),SWP_NOZORDER|SWP_NOACTIVATE);
            InvalidateRect(control,nullptr,FALSE);
        } else styled_button(app,title+L" "+secondary,pins_first+i,left,top,width,height,std::move(skin));
    }
}
void fill_actions(App& app) {
    if (!app.action_list) return;
    const auto term=control_text(app.search);
    app.visible.clear();
    SendMessageW(app.action_list,WM_SETREDRAW,FALSE,0);
    SendMessageW(app.action_list,LB_RESETCONTENT,0,0);
    const bool wheel=app.pane_index==2;
    const bool gesture=!wheel && point_title(app,button_points[app.pane_index]).starts_with(L"手势 · ");
    const auto current=wheel?get(app,L"thumb.left"):
        app.gesture_panel && app.choosing_preset?
            (gesture?point_title(app,button_points[app.pane_index]).substr(5):Text(L"自定义")):
        app.gesture_panel && app.gesture_direction>=0?
            get(app,Text(button_points[app.pane_index].key)+gesture_suffix[app.gesture_direction]):
        gesture?Text(L"@gesture"):get(app,button_points[app.pane_index].key);
    const auto append=[&](Action action) {
        app.visible.push_back(action);
        const auto index=SendMessageW(app.action_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(action.name));
        if (current==action.code && (!wheel || std::ranges::any_of(wheel_actions,[&](const auto& item){
            return std::wstring_view(item.name)==action.name && get(app,L"thumb.right")==item.right;
        }))) SendMessageW(app.action_list,LB_SETCURSEL,index,0);
    };
    if (app.gesture_panel && app.choosing_preset) {
        for (const auto& preset:gesture_presets) {
            if (!term.empty() && Text(preset.title).find(term)==Text::npos) continue;
            append({preset.title,preset.title});
        }
    } else if (wheel) {
        for (const auto& action:wheel_actions) {
            if (!term.empty() && Text(action.name).find(term)==Text::npos) continue;
            append({action.left,action.name});
        }
    } else if (term.empty() && !app.all_actions && !app.show_custom && !app.gesture_panel) {
        constexpr const wchar_t* recommendations[][5]={
            {L"MouseMiddle",L"WheelMode",L"Win+Tab",L"Win+D",L""},
            {L"Lock",L"WheelMode",L"Win+Shift+S",L"PrintScreen",L"Win+Tab"},
            {L"",L"",L"",L"",L""},
            {L"MouseForward",L"Ctrl+V",L"VolumeUp",L"Ctrl+Y",L"Win+Tab"},
            {L"MouseBack",L"Ctrl+C",L"VolumeDown",L"Ctrl+Z",L"Win+Tab"},
            {L"Win+Tab",L"Win+D",L"Alt+Tab",L"Win",L"MouseMiddle"},
        };
        for (const auto* code:recommendations[app.pane_index]) {
            const auto action=std::ranges::find_if(actions,[&](const Action& item){
                return std::wstring_view(item.code)==code;
            });
            if (action!=std::end(actions)) append(*action);
        }
        append({L"@gesture",L"手势"});
        append({L"@shortcut",L"键盘快捷键"});
        append({L"Ring",L"显示 Actions Ring"});
    } else {
        for (const auto& action:actions) {
            if (!term.empty() && Text(action.name).find(term)==Text::npos &&
                Text(action.code).find(term)==Text::npos) continue;
            append(action);
        }
    }
    SendMessageW(app.action_list,WM_SETREDRAW,TRUE,0);
    InvalidateRect(app.action_list,nullptr,TRUE);
}
LRESULT CALLBACK search_cue(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                            UINT_PTR id,DWORD_PTR) {
    const auto result=lazy_win32::def_subclass_proc(window,message,wparam,lparam);
    if (message==WM_PAINT && !GetWindowTextLengthW(window) && GetFocus()!=window) {
        const auto dc=GetDC(window);
        RECT rect{};
        GetClientRect(window,&rect);
        native_typography::drawText(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),
                                    muted,L"搜索",rect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        ReleaseDC(window,dc);
    }
    if (message==WM_SETFOCUS || message==WM_KILLFOCUS) InvalidateRect(window,nullptr,TRUE);
    if (message==WM_NCDESTROY) lazy_win32::remove_window_subclass(window,search_cue,id);
    return result;
}
void actions_pane(App& app) {
    constexpr int left=1015,width=458;
    const auto& point=button_points[app.pane_index];
    const bool wheel=app.pane_index==2;
    const int rows=app.show_custom?5:app.all_actions?11:wheel?9:8;
    const int bottom=205+38*rows+12;
    label(app,L"动作",1027,17,240,35,app.subheading);
    app.search=add(app,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,1026,110,398,25,find_action);
    lazy_win32::set_window_subclass(app.search,search_cue,1,0);
    if (app.gesture_panel && !wheel) {
        for (unsigned i=0;i<app.gesture.size();++i) {
            app.gesture[i]=field_at(app,get(app,Text(point.key)+gesture_suffix[i]),field+static_cast<int>(i),0,0,1,1);
            ShowWindow(app.gesture[i],SW_HIDE);
        }
        if (app.choosing_preset || app.gesture_direction>=0) {
            label(app,app.choosing_preset?L"选择手势预设":Text(gesture_titles[app.gesture_direction])+L"的动作",
                  1032,164,400,27,app.bold);
            app.action_list=add(app,L"LISTBOX",L"",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|
                LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,left,205,width,494,link_action);
            lazy_win32::set_window_theme(app.action_list,L"DarkMode_Explorer",nullptr);
            SendMessageW(app.action_list,LB_SETITEMHEIGHT,0,px(38));
            fill_actions(app);
            icon_button(app,L"返回手势",gesture_overview,1030,727,210,38,L"back",Look::tab);
        } else {
            label(app,L"推荐",1032,164,245,27,app.bold);
            styled_button(app,L"手势",0,1015,205,458,46,{Look::gesture_header,true,{},L"手势",{}});
            label(app,L"选择一个预设，或者自定义各个方向的动作。",1032,267,418,35,app.small);
            auto name=point_title(app,point);
            name=name.starts_with(L"手势 · ")?name.substr(5):L"自定义";
            icon_button(app,name,gesture_presets_open,1032,307,417,48,L"chevron",Look::gesture_picker);
            constexpr unsigned order[]{1,2,3,4,0};
            constexpr const wchar_t* row_titles[]{L"点击",L"按住 + 左移",L"按住 + 右移",L"按住 + 上移",L"按住 + 下移"};
            for (unsigned row=0;row<5;++row) {
                const auto i=order[row];
                const auto code=get(app,Text(point.key)+gesture_suffix[i]);
                constexpr const wchar_t* pan_names[]{L"中键",L"向左滚动",L"向右滚动",L"向上滚动",L"向下滚动"};
                const Text description=get(app,point.key)==L"Pan"?pan_names[i]:action_name(code);
                styled_button(app,Text(row_titles[i])+L" "+description,gesture_rows_first+i,
                    1032,369+row*59,417,60,{Look::gesture_row,false,{},row_titles[i],description});
            }
            icon_button(app,L"返回动作列表",custom_actions,1030,708,240,40,L"back",Look::tab);
        }
        return;
    }
    label(app,app.all_actions?L"其他动作":L"推荐",1032,164,245,27,app.bold);
    app.action_list=add(app,L"LISTBOX",L"",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|
                        LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,left,205,width,38*rows,link_action);
    lazy_win32::set_window_theme(app.action_list,L"DarkMode_Explorer",nullptr);
    SendMessageW(app.action_list,LB_SETITEMHEIGHT,0,px(38));
    fill_actions(app);
    if (!app.show_custom) {
        icon_button(app,L"SMART ACTIONS",smart,pane_x,bottom,client_width-pane_x-1,49,L"chevron",Look::section);
        if (!wheel)
            icon_button(app,app.all_actions?L"返回推荐":L"其他动作",more_actions,pane_x,bottom+49,
                        client_width-pane_x-1,49,L"chevron",Look::section);
        if (wheel || app.all_actions)
            icon_button(app,wheel?L"自定义左右动作":L"手势与快捷键",custom_actions,pane_x,
                        bottom+(wheel?49:98),client_width-pane_x-1,49,L"chevron",Look::section);
    }
    if (wheel) {
        app.gesture[0]=field_at(app,get(app,L"thumb.left"),direction_left,1094,458,355,34);
        app.gesture[1]=field_at(app,get(app,L"thumb.right"),direction_right,1094,507,355,34);
        if (app.show_custom) {
            label(app,L"向左",1032,464,52,25);
            label(app,L"向右",1032,513,52,25);
        }
    } else {
        const auto presets=add(app,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                               1032,442,417,230,gesture_preset);
        SendMessageW(presets,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"手势预设"));
        for (const auto& item:gesture_presets)
            SendMessageW(presets,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item.title));
        SendMessageW(presets,CB_SETCURSEL,0,0);
        if (!app.show_custom) ShowWindow(presets,SW_HIDE);
        for (unsigned i=0;i<app.gesture.size();++i) {
            if (app.show_custom) label(app,gesture_titles[i],1032,494+static_cast<int>(i)*41,55,27);
            app.gesture[i]=field_at(app,get(app,Text(point.key)+gesture_suffix[i]),field+static_cast<int>(i),
                                    1094,489+static_cast<int>(i)*41,355,33);
        }
    }
    if (!app.show_custom)
        for (auto input:app.gesture) if (input) ShowWindow(input,SW_HIDE);
    if (!wheel && !app.show_custom) {
        const Text cur = get(app, Text(point.key));
        if (cur.starts_with(L"Open(") || cur.starts_with(L"DpiToggle(") || cur.starts_with(L"AdvancedClick(")) {
            const int py = bottom + (app.all_actions ? 148 : 99);
            const Text hint = cur.starts_with(L"Open(") ? L"目标路径或网址：" :
                              cur.starts_with(L"DpiToggle(") ? L"两档 DPI（如 1000,5000）：" :
                              L"点击与修饰（模式,掩码）：";
            const Text val = cur.starts_with(L"Open(") ? cur.substr(5, cur.size() - 6) :
                             cur.starts_with(L"DpiToggle(") ? cur.substr(10, cur.size() - 11) :
                             cur.substr(14, cur.size() - 15);
            label(app, hint, 1029, py, 260, 22, app.small);
            app.action_param_box = field_at(app, val, action_param_edit, 1029, py + 24,
                                            cur.starts_with(L"Open(") ? 275 : 345, 30);
            if (cur.starts_with(L"Open("))
                button(app, L"浏览", action_param_browse, 1310, py + 23, 64, 32);
            button(app, L"应用", action_param_apply, 1380, py + 23, 76, 32, true);
        }
    }
    if (app.selected!=L"global") button(app,L"恢复全局",remove_profile,1029,783,125,40);
    if (app.show_custom) icon_button(app,L"返回动作列表",custom_actions,1030,727,174,38,L"back",Look::tab);
}
LRESULT CALLBACK sliding_toggle(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
HWND toggle_at(App& app,const Text& title,bool checked,int id,int x,int y,int width,bool on_bg=false) {
    auto control=add(app,L"BUTTON",title,BS_AUTOCHECKBOX|WS_TABSTOP,x,y,width,36,id,app.bold);
    if(on_bg) SetPropW(control,L"LogiNative.OnBackground",reinterpret_cast<HANDLE>(1));
    lazy_win32::set_window_subclass(control,sliding_toggle,1,0);
    SendMessageW(control,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
    return control;
}
HWND toggle(App& app,const Text& title,bool checked,int id,int y) {
    return toggle_at(app,title,checked,id,1027,y,432,false);
}
void hardware_pane(App& app) {
    const auto index=app.pane_index;
    label(app,pointer_points[index].title,1027,17,380,35,app.subheading);
    if (app.official && index!=2)
        label(app,L"检测到官方 Logi 后台正在使用设备，可在「设置」启动设备服务接管。",1027,540,432,48,app.small);
    if (index==1) {
        const auto tr=[&](const wchar_t* zh,const wchar_t* en){return ui_settings::tr(app.ui.language,zh,en);};
        const auto shown=shown_dpi(app);
        label(app,tr(L"使用 8K DPI 传感器更快速、更精准地办公，可调节指针速度和精度以适应高分辨率显示器。",
                     L"Tune 8K sensor DPI and pointer precision to match your display resolution."),
              1027,85,432,44,app.small);
        app.hardware[0]=toggle(app,tr(L"将传感器范围扩展至 8K DPI",L"Extend Sensor Range to 8K DPI"),extended_dpi(app),dpi_extended,150);
        label(app,tr(L"指针速度",L"Pointer Speed"),1027,206,244,27,app.bold);
        app.dpi_value=label(app,shown?std::to_wstring(*shown)+L" DPI":L"— DPI",1357,206,94,27,app.normal);
        SetWindowLongPtrW(app.dpi_value,GWL_STYLE,(GetWindowLongPtrW(app.dpi_value,GWL_STYLE)&~SS_TYPEMASK)|SS_RIGHT);
        if (!app.device.dpi.empty()) {
            app.slider=add(app,TRACKBAR_CLASSW,tr(L"指针速度",L"Pointer Speed"),TBS_NOTICKS|WS_TABSTOP,1018,236,452,34,dpi_range);
            SendMessageW(app.slider,TBM_SETRANGEMAX,FALSE,100);
            if(shown)SendMessageW(app.slider,TBM_SETPOS,TRUE,native_dpi::to_percent(*shown,extended_dpi(app)));
            else ShowWindow(app.slider,SW_HIDE);
            SendMessageW(app.slider,TBM_SETPAGESIZE,0,10);
        }
        label(app,tr(L"快捷 DPI 预设",L"Quick DPI Presets"),1027,290,260,24,app.small);
        for (int p = 0; p < static_cast<int>(pointer_presets.size()); ++p) {
            const bool active_preset = shown == pointer_presets[p];
            styled_button(app, std::to_wstring(pointer_presets[p]), pointer_dpi_800 + p,
                          1027 + p * 108, 320, 96, 34, {Look::tab, active_preset, {}, {}, {}});
        }
        toggle(app,tr(L"提高指针精确度 (Windows)",L"Enhanced Pointer Precision (Windows)"),windows_pointer_precision(),pointer_precision,374);
    } else if (index==0) {
        const auto tr=[&](const wchar_t* zh,const wchar_t* en){return ui_settings::tr(app.ui.language,zh,en);};
        label(app,tr(L"滚动方向",L"Scroll Direction"),1027,109,285,28,app.bold);
        const bool reversed=shown_setting(app,4).value_or(0)!=0;
        styled_button(app,tr(L"已反转",L"Inverted"),wheel_reverse,1027,146,432,32,{Look::radio,reversed,{},{},{}});
        styled_button(app,tr(L"标准",L"Standard"),wheel_standard,1027,178,432,32,{Look::radio,!reversed,{},{},{}});
        app.hardware[0]=toggle(app,tr(L"平滑滚动",L"Smooth Scrolling"),read_smooth(app),wheel_smooth,230);
        app.hardware[2]=toggle(app,tr(L"全应用平滑滚动（所有桌面软件）",L"Universal Smooth Scroll (All Apps)"),read_smooth_all(app),wheel_smooth_all,270);
        label(app,tr(L"默认支持主流浏览器；开启全应用后在编辑器与桌面软件同样平滑。",L"Smooth in browsers by default; enable All Apps for desktop-wide smoothing."),1027,312,432,38,app.small);
        const bool enabled=smartshift_enabled(app);
        app.hardware[1]=toggle(app,L"SmartShift",enabled,smartshift_toggle,356);
        label(app,tr(L"快速滚动时，自动将滚轮从逐行滚动切换为疾速滚动。",L"Automatically shift from ratchet to free-spin on fast wheel flicks."),1027,402,432,36,app.small);
        if(enabled) {
            const auto sensitivity=shown_sensitivity(app);
            label(app,tr(L"灵敏度值",L"Sensitivity"),1027,442,340,27,app.bold);
            app.sensitivity_value=label(app,std::to_wstring(sensitivity)+L"%",1417,442,42,27,app.small);
            SetWindowLongPtrW(app.sensitivity_value,GWL_STYLE,(GetWindowLongPtrW(app.sensitivity_value,GWL_STYLE)&~SS_TYPEMASK)|SS_RIGHT);
            app.slider=add(app,TRACKBAR_CLASSW,L"SmartShift 灵敏度",TBS_NOTICKS|WS_TABSTOP,1018,468,452,34,dpi_range);
            SendMessageW(app.slider,TBM_SETRANGEMAX,FALSE,100);
            SendMessageW(app.slider,TBM_SETPOS,TRUE,sensitivity);
            SendMessageW(app.slider,TBM_SETPAGESIZE,0,10);
        } else {
            label(app,tr(L"滚动模式",L"Scroll Mode"),1027,442,340,27,app.bold);
            const bool ratchet=shown_setting(app,2).value_or(1)!=0;
            styled_button(app,tr(L"依照您需要的速度滚动",L"Free-Spin"),wheel_free,1027,480,432,32,{Look::radio,!ratchet,{},{},{}});
            styled_button(app,tr(L"分段",L"Ratchet"),wheel_ratchet,1027,512,432,32,{Look::radio,ratchet,{},{},{}});
        }
    } else {
        const auto speed=read_thumb_speed(app);
        label(app,L"拇指滚轮速度",1027,101,205,27,app.bold);
        app.speed_value=label(app,std::to_wstring(speed)+L"%",1410,101,56,27,app.bold);
        app.slider=add(app,TRACKBAR_CLASSW,L"拇指滚轮速度",TBS_NOTICKS|WS_TABSTOP,
                       1027,126,432,34,thumb_speed);
        SendMessageW(app.slider,TBM_SETRANGEMIN,FALSE,min_thumb_speed);
        SendMessageW(app.slider,TBM_SETRANGEMAX,FALSE,max_thumb_speed);
        SendMessageW(app.slider,TBM_SETTICFREQ,10,0);
        SendMessageW(app.slider,TBM_SETPAGESIZE,0,10);
        SendMessageW(app.slider,TBM_SETPOS,TRUE,speed);
        label(app,L"拇指滚轮方向",1027,194,340,27,app.bold);
        app.hardware[0]=toggle(app,L"反转拇指滚动方向",shown_setting(app,5).value_or(0),field,229);
        if (app.official)
            label(app,L"检测到官方 Logi 后台正在使用设备，可在「设置」启动设备服务接管。",1027,279,432,44,app.small);
        else if (app.device.thumb_cached)
            label(app,L"方向来自后台最近确认。",1027,279,432,44,app.small);
        app.hardware[1]=toggle(app,L"平滑滚动",read_smooth(app,true),thumb_smooth,335);
        label(app,L"用于浏览器与支持全应用平滑的桌面软件横向滚动。",1027,383,432,40,app.small);
    }
    const bool enabled=!app.official && !app.device.name.empty();
    if (!enabled) {
        if (app.input) EnableWindow(app.input,FALSE);
        if (app.slider && index!=2) EnableWindow(app.slider,FALSE);
        for (auto control:app.hardware) if (control) EnableWindow(control,FALSE);
        for (int p = 0; p < 4; ++p)
            if (const auto btn = GetDlgItem(app.window, pointer_dpi_800 + p)) EnableWindow(btn, FALSE);
    }
    if (app.slider) InvalidateRect(app.slider,nullptr,TRUE);
}
std::vector<std::pair<Text,Text>> ring_items(const App& app,const Text& section) {
    std::vector<std::pair<Text,Text>> result;
    bool in_section=false;
    for (const auto& line:lines(app.source)) {
        if (const auto name=section_name(line); !name.empty()) {
            if (in_section) break;
            in_section=name==section;
        } else if (in_section) {
            auto item=entry(line);
            if (!item.first.empty()) {
                if (item.first==L"@empty."+std::to_wstring(result.size()+1) && item.second.empty())
                    result.emplace_back();
                else result.push_back(std::move(item));
            }
        }
    }
    return result;
}
std::vector<std::pair<Text,Text>> ring_items(const App& app) { return ring_items(app,app.ring_section); }
void select_ring_action(App& app);
LRESULT CALLBACK ring_scroll(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                             UINT_PTR id,DWORD_PTR data) {
    auto& app=*reinterpret_cast<App*>(data);
    if (message==WM_NCDESTROY) {
        RemovePropW(window,L"RingScroll.Drag");
        lazy_win32::remove_window_subclass(window,ring_scroll,id);
        return lazy_win32::def_subclass_proc(window,message,wparam,lparam);
    }
    if (message!=WM_PAINT && message!=WM_MOUSEWHEEL && message!=WM_LBUTTONDOWN &&
        message!=WM_MOUSEMOVE && message!=WM_LBUTTONUP && message!=WM_CAPTURECHANGED && message!=WM_KEYDOWN)
        return lazy_win32::def_subclass_proc(window,message,wparam,lparam);
    RECT bounds{};
    GetClientRect(window,&bounds);
    const int count=static_cast<int>(SendMessageW(window,LB_GETCOUNT,0,0));
    const int row=std::max(1,static_cast<int>(SendMessageW(window,LB_GETITEMHEIGHT,0,0)));
    const int visible=std::max(1,static_cast<int>(bounds.bottom)/row);
    const int maximum=std::max(0,count-visible);
    const auto scroll_to=[&](int top) {
        SendMessageW(window,LB_SETTOPINDEX,std::clamp(top,0,maximum),0);
        InvalidateRect(window,nullptr,FALSE);
    };
    if (message==WM_MOUSEWHEEL) {
        const int top=static_cast<int>(SendMessageW(window,LB_GETTOPINDEX,0,0));
        scroll_to(top-GET_WHEEL_DELTA_WPARAM(wparam)*3/WHEEL_DELTA);
        return 0;
    }
    const bool track=maximum && GET_X_LPARAM(lparam)>=bounds.right-px(14);
    if (message==WM_LBUTTONDOWN && track) {
        SetPropW(window,L"RingScroll.Drag",reinterpret_cast<HANDLE>(1));
        SetCapture(window);
    }
    if ((message==WM_LBUTTONDOWN && track) || (message==WM_MOUSEMOVE && GetPropW(window,L"RingScroll.Drag"))) {
        scroll_to(MulDiv(GET_Y_LPARAM(lparam),maximum,std::max(1L,bounds.bottom)));
        return 0;
    }
    if (message==WM_LBUTTONUP && GetPropW(window,L"RingScroll.Drag")) {
        RemovePropW(window,L"RingScroll.Drag"); ReleaseCapture(); return 0;
    }
    if (message==WM_CAPTURECHANGED) RemovePropW(window,L"RingScroll.Drag");
    if (message==WM_KEYDOWN && (wparam==VK_RETURN || wparam==VK_SPACE)) {
        const auto selected=SendMessageW(window,LB_GETCURSEL,0,0);
        if (selected>=0 && static_cast<std::size_t>(selected)<app.ring_rows.size()) {
            if (app.ring_rows[selected].action<0) select_ring_action(app);
            else if (app.pane_index>=0)
                PostMessageW(app.window,assign_ring,app.pane_index,app.ring_rows[selected].action);
            return 0;
        }
    }
    if (message==WM_MOUSEMOVE && app.ring_drag_action>=0 && GetCapture()==window) {
        const int dx=GET_X_LPARAM(lparam)-app.ring_drag_start.x,dy=GET_Y_LPARAM(lparam)-app.ring_drag_start.y;
        if (dx*dx+dy*dy>px(5)*px(5)) app.ring_dragging=true;
        if (app.ring_dragging) {SetCursor(LoadCursorW(nullptr,IDC_HAND)); return 0;}
    }
    if (message==WM_LBUTTONUP && app.ring_drag_action>=0) {
        const int action=app.ring_drag_action;
        int slot=app.pane_index;
        if (app.ring_dragging) {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            MapWindowPoints(window,app.window,&point,1);
            const auto target=ChildWindowFromPointEx(app.window,point,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED);
            const int target_id=GetDlgCtrlID(target);
            slot=target_id>=rings_first && target_id<rings_first+8?target_id-rings_first:-1;
        }
        app.ring_drag_action=-1;app.ring_dragging=false;
        const auto result=lazy_win32::def_subclass_proc(window,message,wparam,lparam);
        if (slot>=0) PostMessageW(app.window,assign_ring,slot,action);
        else notice(app,L"将操作拖到环上的槽位。");
        return result;
    }
    const auto result=lazy_win32::def_subclass_proc(window,message,wparam,lparam);
    if (message==WM_LBUTTONDOWN && !track) {
        const auto selected=SendMessageW(window,LB_GETCURSEL,0,0);
        if (selected>=0 && static_cast<std::size_t>(selected)<app.ring_rows.size()) {
            app.ring_drag_action=app.ring_rows[selected].action;
            app.ring_drag_start={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            app.ring_dragging=false;
        }
    }
    if (message==WM_PAINT && maximum) {
        const auto dc=GetDC(window);
        const int height=std::max(px(24),MulDiv(bounds.bottom,visible,count));
        const int top=MulDiv(SendMessageW(window,LB_GETTOPINDEX,0,0),bounds.bottom-height,maximum);
        RECT track_rect{bounds.right-px(10),0,bounds.right,bounds.bottom};
        const auto back=CreateSolidBrush(background),thumb=CreateSolidBrush(RGB(100,100,100));
        FillRect(dc,&track_rect,back);
        RECT thumb_rect{bounds.right-px(6),top,bounds.right-px(2),top+height};
        FillRect(dc,&thumb_rect,thumb);
        DeleteObject(back); DeleteObject(thumb); ReleaseDC(window,dc);
    }
    return result;
}
RingAction ring_library_action(const App& app,std::size_t index) {
    if(index<std::size(ring_actions)) return ring_actions[index];
    index-=std::size(ring_actions);
    if(index<app.ring_smart_actions.size()) {
        const auto& action=app.ring_smart_actions[index];
        return {L"Smart Actions",action.sequence.c_str(),action.name.c_str()};
    }
    return {L"",L"",L""};
}
void fill_ring_library(App& app) {
    if (!app.ring_list) return;
    const auto term = app.ring_searching?Text(trim(control_text(app.ring_filter))):Text{};
    app.ring_query=term;
    SendMessageW(app.ring_list,WM_SETREDRAW,FALSE,0);
    SendMessageW(app.ring_list,LB_RESETCONTENT,0,0);
    app.ring_rows.clear();
    app.ring_smart_actions=read_smart_library(app.mapping());
    int category_row = -1;
    const auto count=std::size(ring_actions)+app.ring_smart_actions.size();
    if(!term.empty()) {
        for(std::size_t i=0;i<count;++i){const auto action=ring_library_action(app,i);
            if(StrStrIW(action.name,term.c_str())||StrStrIW(action.code,term.c_str())||StrStrIW(action.category,term.c_str()))app.ring_rows.push_back({action.name,static_cast<int>(i),false});}
    } else {
        std::vector<Text> categories;
        for(std::size_t i=0;i<count;++i){const Text category=ring_library_action(app,i).category;if(std::ranges::find(categories,category)==categories.end())categories.push_back(category);}
        for(const auto&category:categories){
            const bool expanded=category==app.ring_category;
            if (expanded) category_row=static_cast<int>(app.ring_rows.size());
            app.ring_rows.push_back({category,-1,expanded});
            if(expanded)for(std::size_t i=0;i<count;++i){const auto action=ring_library_action(app,i);if(action.category==category)app.ring_rows.push_back({action.name,static_cast<int>(i),false});}
        }
    }
    for (const auto& row:app.ring_rows)
        SendMessageW(app.ring_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.title.c_str()));
    if (app.ring_rows.empty())
        SendMessageW(app.ring_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"没有匹配的操作"));
    if (category_row>=0) SendMessageW(app.ring_list,LB_SETTOPINDEX,category_row,0);
    SendMessageW(app.ring_list,WM_SETREDRAW,TRUE,0);
    InvalidateRect(app.ring_list,nullptr,TRUE);
}
void select_ring_action(App& app) {
    const auto selected=SendMessageW(app.ring_list,LB_GETCURSEL,0,0);
    if (selected<0 || static_cast<std::size_t>(selected)>=app.ring_rows.size()) return;
    const auto row=app.ring_rows[selected];
    if (row.action<0) {
        app.ring_category=row.expanded?L"":row.title;
        fill_ring_library(app);
        // Keep keyboard navigation on the category after expanding or collapsing it.
        for (std::size_t i=0;i<app.ring_rows.size();++i)
            if (app.ring_rows[i].action<0 && app.ring_rows[i].title==row.title) {
                SendMessageW(app.ring_list,LB_SETCURSEL,i,0); break;
            }
        return;
    }
}
std::wstring_view ring_glyph(std::wstring_view action) {
    if (action.empty()) return L"add";
    if (action.starts_with(L"Folder(") || action==L"@folder" || action==L"Win+E") return L"folder";
    if (action==L"PlayPause") return L"play";
    if (action==L"MediaStop") return L"stop";
    if (action==L"Mute") return L"mute";
    if (action==L"VolumeUp" || action==L"VolumeDown") return L"volume";
    if (action==L"NextTrack") return L"next";
    if (action==L"PreviousTrack") return L"previous";
    if (action==L"Win+Shift+S" || action==L"PrintScreen") return L"capture";
    if (action==L"Win+Tab") return L"flow";
    if (action==L"Win+D") return L"desktop";
    if (action==L"Lock") return L"lock";
    if (action.starts_with(L"Open(")) return L"note";
    if (action.starts_with(L"Mouse") || action.starts_with(L"Scroll")) return L"buttons";
    return L"keyboard";
}
void draw_ring_icon(Gdiplus::Graphics& graphics,std::wstring_view name,int x,int y,int size,COLORREF color) {
    if (name==L"stop") {
        const auto inset=size/4;
        Gdiplus::SolidBrush brush(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
        graphics.FillRectangle(&brush,x+inset,y+inset,size-inset*2,size-inset*2);
    } else draw_native_icon(graphics,name,x,y,size,color);
}
Text ring_label(std::wstring_view name) {
    constexpr std::pair<std::wstring_view,std::wstring_view> translations[]{
        {L"Play / Pause",L"播放/暂停"},{L"Media",L"媒体"},{L"New Note",L"新建笔记"},
        {L"Mute",L"静音"},{L"Task View",L"任务视图"},{L"Show desktop",L"显示桌面"},
        {L"File Explorer",L"文件资源管理器"},{L"Screenshot",L"屏幕截图"},
        {L"Previous track",L"上一曲目"},{L"Next track",L"下一曲目"}
    };
    for (const auto& [source,target]:translations) if (name==source) return Text(target);
    return Text(name);
}
int text_width(App& app,std::wstring_view value,HFONT font) {
    const float width=native_typography::measureText(nullptr,font,value,app.page==ring).Width;
    return static_cast<int>(std::ceil(width*96.0f/layout_dpi));
}
Text ring_text_value(std::wstring_view code) {
    if (code.starts_with(L"Text(") && code.ends_with(L")")) return Text(code.substr(5,code.size()-6));
    Text value;
    if (code.starts_with(L"TextHex(") && code.ends_with(L")")) {
        const auto encoded=code.substr(8,code.size()-9);
        for (std::size_t i=0;i+4<=encoded.size();i+=4) {
            unsigned unit{};
            for (const auto c:encoded.substr(i,4)) {
                if (c>=L'0' && c<=L'9') unit=unit*16+c-L'0';
                else if (c>=L'a' && c<=L'f') unit=unit*16+c-L'a'+10;
                else return {};
            }
            value.push_back(static_cast<wchar_t>(unit));
        }
    }
    return value;
}
Text ring_text_code(std::wstring_view value) {
    constexpr auto hex=L"0123456789abcdef";
    Text code=L"TextHex(";
    for (const auto c:value) for (int shift=12;shift>=0;shift-=4) code+=hex[(c>>shift)&15];
    return code+L")";
}
RingValueKind ring_value_kind(std::wstring_view code) {
    if(code.starts_with(L"AdvancedClick("))return RingValueKind::advanced_click;
    if(code.starts_with(L"DpiToggle("))return RingValueKind::dpi_toggle;
    if (code==L"@shortcut") return RingValueKind::shortcut;
    if (code.starts_with(L"Folder(") && code.ends_with(L")")) return RingValueKind::folder;
    if ((code.starts_with(L"Text(") || code.starts_with(L"TextHex(")) && code.ends_with(L")"))
        return RingValueKind::text;
    if (code.starts_with(L"Open(") && code.ends_with(L")")) return RingValueKind::open;
    if (code.find(L';')!=code.npos) return RingValueKind::sequence;
    if (code.empty() || std::ranges::any_of(ring_actions,[&](const auto& item){return item.code==code;}))
        return RingValueKind::none;
    return RingValueKind::shortcut;
}
LRESULT CALLBACK ring_shortcut(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                               UINT_PTR id,DWORD_PTR) {
    if (message==WM_NCDESTROY) lazy_win32::remove_window_subclass(window,ring_shortcut,id);
    if ((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) &&
        wparam!=VK_TAB && wparam!=VK_ESCAPE) {
        Text key;
        if ((wparam>='A' && wparam<='Z') || (wparam>='0' && wparam<='9'))
            key.push_back(static_cast<wchar_t>(wparam));
        else if (wparam>=VK_F1 && wparam<=VK_F24) key=L"F"+std::to_wstring(wparam-VK_F1+1);
        else {
            constexpr std::pair<UINT,const wchar_t*> keys[]{
                {VK_RETURN,L"Enter"},{VK_SPACE,L"Space"},{VK_BACK,L"Backspace"},{VK_DELETE,L"Delete"},
                {VK_HOME,L"Home"},{VK_END,L"End"},{VK_PRIOR,L"PageUp"},{VK_NEXT,L"PageDown"},
                {VK_LEFT,L"Left"},{VK_RIGHT,L"Right"},{VK_UP,L"Up"},{VK_DOWN,L"Down"},
                {VK_SNAPSHOT,L"PrintScreen"}
            };
            for (const auto& [code,name]:keys) if (wparam==code) {key=name;break;}
        }
        if (!key.empty()) {
            Text chord;
            if (GetKeyState(VK_CONTROL)<0) chord+=L"Ctrl+";
            if (GetKeyState(VK_MENU)<0) chord+=L"Alt+";
            if (GetKeyState(VK_SHIFT)<0) chord+=L"Shift+";
            if (GetKeyState(VK_LWIN)<0 || GetKeyState(VK_RWIN)<0) chord+=L"Win+";
            SetWindowTextW(window,(chord+key).c_str());
        }
        return 0;
    }
    if (message==WM_CHAR || message==WM_SYSCHAR) return 0;
    return lazy_win32::def_subclass_proc(window,message,wparam,lparam);
}
Text ring_editor_code(const App& app) {
    if (!app.ring_value) return app.ring_code;
    const auto value=control_text(app.ring_value);
    if (value.empty()) return {};
    switch (app.ring_value_kind) {
    case RingValueKind::dpi_toggle: {
        const auto second=control_text(app.ring_value2);const auto first_dpi=logi::number(value),second_dpi=logi::number(second);
        return first_dpi&&second_dpi&&*first_dpi>=50&&*second_dpi>=50&&*first_dpi<=25600&&*second_dpi<=25600?L"DpiToggle("+value+L","+second+L")":L"";
    }
    case RingValueKind::open:return L"Open("+value+L")";
    case RingValueKind::text:return ring_text_code(value);
    case RingValueKind::shortcut:return value;
    default:return app.ring_code;
    }
}
Text ring_root(const App& app) {
    const auto chosen=app.ring_ui.active.find(app.ring_application);
    return chosen!=app.ring_ui.active.end()&&app.config.contains(chosen->second)?chosen->second:ring_metadata::root(app.ring_application);
}
Text ring_profile_title(const App&app,const Text&section) {
    for(const auto&profile:app.ring_ui.profiles)if(profile.application==app.ring_application&&profile.section==section)return profile.name;
    return app.ring_application==L"global"?L"Default General Profile":L"Default "+app.ring_application+L" Profile";
}
Text ring_profile_target(const App&app){return app.ring_profile_target.empty()?ring_root(app):app.ring_profile_target;}
RingIcon ring_icon_for(const App& app,const Text& section,std::size_t index,std::wstring_view code) {
    auto icon=ring_metadata::icon(app.ring_ui,section,index);
    if(icon.glyph.empty())icon.glyph=ring_metadata::default_glyph(code);
    return icon;
}
Gdiplus::Image* ring_image(App& app,const RingIcon& icon) {
    if(icon.image.empty())return nullptr;
    auto& image=app.ring_images[icon.image];
    if(!image){auto loaded=std::make_unique<Gdiplus::Image>(icon.image.c_str());if(loaded->GetLastStatus()==Gdiplus::Ok)image=std::move(loaded);}
    return image.get();
}
void render_ring_profiles(App& app) {
    icon_button(app,app.ring_application==L"global"?L"通用配置文件":app.ring_application,ring_application_menu,808,35,43,54,L"grid",Look::profile,true);
    icon_button(app,L"配置文件",ring_profiles_menu,858,35,43,54,L"chevron",Look::glyph,app.ring_profiles_open);
    icon_button(app,L"添加应用程序",ring_application_add,919,35,43,49,L"add",Look::tool);
    icon_button(app,L"设置",ring_settings,919,765,43,40,L"gear",Look::glyph,app.ring_settings_open);
    const auto overlay_begin=app.children.size();
    if(app.ring_settings_open){
        label(app,L"Ring 尺寸",680,618,90,26,app.caption);
        styled_button(app,L"小",ring_size_small,782,616,54,28,{Look::tab,app.ui.ring_size==ui_settings::RingSize::small,{},{},{}});
        styled_button(app,L"中",ring_size_medium,841,616,54,28,{Look::tab,app.ui.ring_size==ui_settings::RingSize::medium,{},{},{}});
        styled_button(app,L"大",ring_size_large,900,616,54,28,{Look::tab,app.ui.ring_size==ui_settings::RingSize::large,{},{},{}});
        label(app,L"适应应用程序",680,668,210,26,app.caption);
        styled_button(app,L"适应应用程序",ring_adaptive,916,669,36,27,{Look::switch_toggle,app.ring_ui.adaptive,{},{},{}});
        label(app,L"根据活动的应用程序，自动调整设置",680,699,270,36,app.small);
    }
    if(app.ring_applications_open){
        app.ring_applications={L"global"};
        for(const auto& [section,items]:app.config)if(section.starts_with(L"ring:"))app.ring_applications.push_back(section.substr(5));
        for(std::size_t i=0;i<app.ring_applications.size();++i)styled_button(app,app.ring_applications[i]==L"global"?L"通用配置文件":app.ring_applications[i],ring_application_first+static_cast<int>(i),660,96+static_cast<int>(i)*40,300,40,{Look::ring_menu_item,app.ring_application==app.ring_applications[i],{},{},{}});
    }
    if(app.ring_profiles_open){
        app.ring_profile_sections={ring_metadata::root(app.ring_application)};
        std::vector<Text> names{ring_profile_title(app,ring_metadata::root(app.ring_application))};
        for(const auto& profile:app.ring_ui.profiles)if(profile.application==app.ring_application&&profile.section!=ring_metadata::root(app.ring_application)){app.ring_profile_sections.push_back(profile.section);names.push_back(profile.name);}
        int y=96;
        constexpr int menu_x=857,menu_width=287;
        for(std::size_t i=0;i<names.size();++i){styled_button(app,names[i],ring_profile_first+static_cast<int>(i),menu_x,y,menu_width-40,40,{Look::ring_menu_item,app.ring_profile_sections[i]==ring_root(app),{},{},{}});styled_button(app,L"···",ring_profile_context_first+static_cast<int>(i),menu_x+menu_width-40,y,40,40,{Look::ring_menu_item,false,{},{},{}});y+=40;}
        styled_button(app,L"+ 添加配置文件",ring_profile_new,menu_x,y,menu_width,40,{Look::ring_menu_item,false,{},{},{}});y+=40;
        styled_button(app,L"导入配置文件",ring_profile_import,menu_x,y,menu_width,40,{Look::ring_menu_item,false,{},{},{}});
        if(!app.ring_profile_target.empty()) {
            const std::pair<int,const wchar_t*> commands[]{{ring_profile_rename,L"重命名"},{ring_profile_copy,L"重复"},{ring_profile_export,L"导出"},{ring_profile_delete,L"移除"}};
            const auto index=std::ranges::find(app.ring_profile_sections,app.ring_profile_target)-app.ring_profile_sections.begin();int menu_y=96+static_cast<int>(index)*40;
            const auto submenu_x=std::min(menu_x+menu_width-11,client_width-144-12);
            for(const auto&[id,name]:commands){auto item=styled_button(app,name,id,submenu_x,menu_y,144,40,{Look::ring_menu_item,false,{},{},{}});if(id==ring_profile_delete)EnableWindow(item,app.ring_profile_target!=ring_metadata::root(app.ring_application));menu_y+=40;}
        }
    }
    if(app.ring_profile_edit){
        RECT modal{px(310),px(265),px(710),px(445)};
        for(const auto child:app.children)if(GetDlgCtrlID(child)!=close_ui&&GetDlgCtrlID(child)!=minimize_ui){
            EnableWindow(child,FALSE);RECT box{},intersection{};GetWindowRect(child,&box);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&box),2);
            if(IntersectRect(&intersection,&box,&modal))ShowWindow(child,SW_HIDE);
        }
        label(app,app.ring_profile_edit==ring_profile_rename?L"重命名配置文件":L"新建配置文件",330,284,355,30,app.bold);
        const auto title=app.ring_profile_edit==ring_profile_rename?ring_profile_title(app,ring_profile_target(app)):app.ring_profile_edit==ring_profile_copy?ring_profile_title(app,ring_profile_target(app))+L" 副本":L"新配置文件";
        app.input=add(app,L"EDIT",title,ES_AUTOHSCROLL|WS_TABSTOP,330,328,355,36,ring_profile_name);SendMessageW(app.input,EM_SETLIMITTEXT,64,0);
        button(app,L"取消",ring_profile_cancel,442,391,114,36);
        button(app,L"完成",ring_profile_accept,570,391,114,36,true);
    }
    for(std::size_t i=overlay_begin;i<app.children.size();++i)SetWindowPos(app.children[i],HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    // Native listboxes/statics are sibling windows. Remove only the covered pixels
    // from their regions, so their repaint and hit-test cannot cross these menus.
    std::vector<RECT> overlays;
    for(std::size_t i=overlay_begin;i<app.children.size();++i)if(IsWindowVisible(app.children[i])){
        RECT box{};GetWindowRect(app.children[i],&box);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&box),2);overlays.push_back(box);
    }
    if(app.ring_settings_open)overlays.push_back({px(650),px(604),px(969),px(752)});
    for(std::size_t i=0;i<overlay_begin;++i){const auto child=app.children[i];
        if(!IsWindowVisible(child)||!(GetWindowLongPtrW(child,GWL_STYLE)&WS_CHILD))continue;
        RECT box{};GetWindowRect(child,&box);MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&box),2);
        const auto region=CreateRectRgn(0,0,box.right-box.left,box.bottom-box.top);bool clipped=false;
        for(const auto&overlay:overlays){RECT intersection{};if(!IntersectRect(&intersection,&box,&overlay))continue;
            const auto cut=CreateRectRgn(intersection.left-box.left,intersection.top-box.top,intersection.right-box.left,intersection.bottom-box.top);CombineRgn(region,region,cut,RGN_DIFF);DeleteObject(cut);clipped=true;
        }
        if(!clipped||!SetWindowRgn(child,region,TRUE))DeleteObject(region);
    }
}
void collect_ring_applications(App& app) {
    app.ring_catalog.clear();
    const auto append=[&](Text path){
        if(path.size()>1&&path.front()==L'"'&&path.back()==L'"')path=path.substr(1,path.size()-2);
        std::error_code error;if(!fs::is_regular_file(path,error))return;
        if(std::ranges::any_of(app.ring_catalog,[&](const auto&item){return _wcsicmp(item.path.c_str(),path.c_str())==0;}))return;
        const auto executable=fs::path(path).filename().wstring();bool supported=app.config.contains(L"ring:"+executable);
        for(const auto&profile:app.profiles)if(_wcsicmp(fs::path(profile).filename().c_str(),executable.c_str())==0)supported=true;
        app.ring_catalog.push_back({fs::path(path).stem().wstring(),std::move(path),supported});
    };
    for(const auto&profile:app.profiles)if(profile!=L"global")append(profile);
    if (app.ring_registry_cache.empty()) {
        for(const auto hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE})for(const auto view:{KEY_WOW64_64KEY,KEY_WOW64_32KEY}) {
            HKEY key{};if(RegOpenKeyExW(hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths",0,KEY_READ|view,&key)!=ERROR_SUCCESS)continue;
            for(DWORD index=0;;++index){wchar_t name[512]{};DWORD length=std::size(name);const auto result=RegEnumKeyExW(key,index,name,&length,nullptr,nullptr,nullptr,nullptr);if(result==ERROR_NO_MORE_ITEMS)break;if(result!=ERROR_SUCCESS)continue;
                wchar_t path[32768]{};DWORD bytes=sizeof(path);
                if(RegGetValueW(key,name,nullptr,RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,path,&bytes)==ERROR_SUCCESS) {
                    Text p(path);
                    if(p.size()>1&&p.front()==L'"'&&p.back()==L'"')p=p.substr(1,p.size()-2);
                    std::error_code error;
                    if(fs::is_regular_file(p,error)) app.ring_registry_cache.push_back(std::move(p));
                }
            }RegCloseKey(key);
        }
    }
    for (const auto& path : app.ring_registry_cache) append(path);
    std::ranges::sort(app.ring_catalog,[](const auto&a,const auto&b){return a.supported!=b.supported?a.supported>b.supported:_wcsicmp(a.name.c_str(),b.name.c_str())<0;});
}
void render_ring_creation(App& app) {
    if(app.ring_creation_stage==1) {
        label(app,L"添加配置文件",100,108,1000,58,app.heading);
        const std::array<std::tuple<const wchar_t*,int,const wchar_t*,const wchar_t*>,3> cards{{
            {L"通用配置文件",ring_create_general,L"grid",L"适用于所有应用程序"},
            {L"应用程序配置文件",ring_create_application,L"buttons",L"为特定应用程序创建配置文件"},
            {L"导入配置文件",ring_create_import,L"folder",L"导入 LogiNative 配置包"}}};
        for(std::size_t i=0;i<cards.size();++i){const auto&[name,id,icon,description]=cards[i];styled_button(app,name,id,339+static_cast<int>(i)*282,287,250,254,{Look::ring_creation_card,false,icon,{},description});}
        return;
    }
    if(app.ring_creation_stage==2) {
        label(app,L"选择应用程序",100,96,1100,58,app.heading);
        app.search=add(app,L"EDIT",app.ring_creation_query,ES_AUTOHSCROLL|WS_TABSTOP,487,190,520,38,ring_create_search);
        SendMessageW(app.search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索应用程序"));
        app.ring_catalog_visible.clear();for(std::size_t i=0;i<app.ring_catalog.size();++i)if(app.ring_creation_query.empty()||StrStrIW(app.ring_catalog[i].name.c_str(),app.ring_creation_query.c_str()))app.ring_catalog_visible.push_back(i);
        constexpr std::size_t page_size=8;const auto pages=std::max<std::size_t>(1,(app.ring_catalog_visible.size()+page_size-1)/page_size);app.ring_creation_page=std::min(app.ring_creation_page,pages-1);
        int y=252,column=0;std::optional<bool> group;
        for(std::size_t at=app.ring_creation_page*page_size;at<app.ring_catalog_visible.size()&&at<(app.ring_creation_page+1)*page_size;++at){
            const auto&entry=app.ring_catalog[app.ring_catalog_visible[at]];
            if(!group||*group!=entry.supported){if(column){column=0;y+=94;}label(app,entry.supported?L"支持的应用程序":L"其他应用程序",487,y,520,26,app.bold);y+=34;group=entry.supported;}
            const auto control=styled_button(app,entry.name,ring_create_app_first+static_cast<int>(at),487+column*130,y,120,86,{Look::ring_application_card,false,{},{},{}});
            app.button_skins[control].application_icon=application_icon(app,entry.path);if(++column==4){column=0;y+=94;}
        }
        if(app.ring_catalog_visible.empty())label(app,L"没有匹配的应用程序",487,300,520,40,app.caption);
        styled_button(app,L"选择其他应用程序",ring_create_browse,487,650,520,40,{Look::plain,false,{},{},{}});
        if(pages>1){auto previous=icon_button(app,L"上一页",ring_create_previous,609,708,38,34,L"back");auto next=icon_button(app,L"下一页",ring_create_next,846,708,38,34,L"forward");EnableWindow(previous,app.ring_creation_page>0);EnableWindow(next,app.ring_creation_page+1<pages);label(app,std::to_wstring(app.ring_creation_page+1)+L" / "+std::to_wstring(pages),681,712,110,28,app.caption);}
        return;
    }
    const auto application=app.ring_creation_application==L"global"?L"通用配置文件":fs::path(app.ring_creation_application).stem().wstring();
    label(app,L"为以下内容选择配置文件 "+application,100,100,1250,64,app.heading);
    label(app,L"此应用程序尚无配置文件",148,287,430,38,app.caption);
    label(app,L"无可用预览",926,360,300,38,app.caption);
    styled_button(app,L"从空白配置文件开始",ring_create_blank,100,745,288,52,{Look::plain,false,{},{},{}});
    const auto next=icon_button(app,L"下一步",0,1370,739,58,58,L"forward",Look::ring_slot);EnableWindow(next,FALSE);
}
void render_ring(App& app) {
    if(app.ring_creation_stage){render_ring_creation(app);return;}
    const auto entries=ring_items(app);
    constexpr int center_x=499,center_y=450;
    constexpr std::array<std::pair<int,int>,8> places{{
        {0,-76},{54,-53},{75,0},{54,53},{0,75},{-54,53},{-76,0},{-54,-53}
    }};
    constexpr std::array<std::pair<int,int>,8> tags{{
        {0,-142},{118,-94},{155,0},{118,91},{0,136},{-118,91},{-149,0},{-151,-94}
    }};
    for (std::size_t i=0;i<places.size();++i) {
        const auto [dx,dy]=places[i];
        const auto [tx,ty]=tags[i];
        const bool configured=i<entries.size() && !entries[i].first.empty(), selected=static_cast<int>(i)==app.pane_index;
        const Text name=configured?ring_label(entries[i].first):L"添加槽位";
        const int id=rings_first+static_cast<int>(i);
        const std::wstring_view action=configured?std::wstring_view(entries[i].second):L"";
        const auto glyph=ring_glyph(action);
        const int diameter=selected?46:42;
        const auto circle=styled_button(app,name,id,center_x+dx-diameter/2,center_y+dy-diameter/2,diameter,diameter,
            {Look::ring_slot,selected,glyph,std::to_wstring(i+1),{}});
        app.button_skins[circle].ring_icon=ring_icon_for(app,app.ring_section,i,action);
        app.button_skins[circle].original_icon=ring_image(app,*app.button_skins[circle].ring_icon);
        EnableWindow(circle,TRUE);
        if (configured) {
            const int width=std::clamp(22+text_width(app,name,app.caption),64,180);
            const int left=ty==0?(tx<0?center_x-103-width:center_x+103):center_x+tx-width/2;
            styled_button(app,name,id,left,center_y+ty-18,width,36,
                          {Look::ring_tag,selected,{},{},{}});
        }
    }
    icon_button(app,L"关闭当前操作",close_pane,center_x-13,center_y-13,26,26,L"close",Look::ring_slot);
    if (!app.ring_navigation.empty())
        icon_button(app,L"返回上一层",ring_folder_back,center_x-60,648,120,40,L"back",Look::tab);
    icon_button(app,L"所有操作",ring_all,1013,35,130,49,L"grid",Look::tool);
    icon_button(app,L"搜索",ring_search,1433,35,49,49,L"search",Look::tool,app.ring_searching);
    icon_button(app,L"预览动作环",ring_preview,1370,35,49,49,L"ring",Look::tool);
    if (!app.ring_searching) {
        label(app,L"系统 操作",1049,126,390,28,app.bold);
        styled_button(app,L"OS",0,1017,129,21,21,{Look::ring_tag,false,{},{},{}});
    }
    app.ring_filter=add(app,L"EDIT",app.ring_query,WS_TABSTOP|ES_AUTOHSCROLL,1027,121,432,29,ring_search);
    ShowWindow(app.ring_filter,app.ring_searching?SW_SHOW:SW_HIDE);
    lazy_win32::set_window_subclass(app.ring_filter,search_cue,1,0);
    app.ring_list=add(app,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|
        LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,1006,171,477,668,ring_library);
    lazy_win32::set_window_theme(app.ring_list,L"",L"");
    lazy_win32::set_window_subclass(app.ring_list,ring_scroll,1,reinterpret_cast<DWORD_PTR>(&app));
    SendMessageW(app.ring_list,LB_SETITEMHEIGHT,0,px(49));
    fill_ring_library(app);
    const auto undo=icon_button(app,L"撤销",ring_undo,43,765,40,40,L"undo");
    const auto redo=icon_button(app,L"重做",ring_redo,103,765,40,40,L"redo");
    EnableWindow(undo,!app.ring_undo_stack.empty()); EnableWindow(redo,!app.ring_redo_stack.empty());
    app.ring_editor={};app.ring_menu_bounds={};
    if (app.pane_index>=0) {
        const auto index=static_cast<std::size_t>(app.pane_index);
        const int x=index==0 || index==4?324:index<4?640:80;
        app.ring_value_kind=ring_value_kind(app.ring_code);
        const int height=app.ring_advanced?(app.ring_value_kind==RingValueKind::text||app.ring_value_kind==RingValueKind::advanced_click?420:325):153;
        const int y=std::min(index==0?125:index==4?512:170,client_height-height-20);
        app.ring_editor={x,y,x+351,y+height};
        Text chosen=app.ring_code.empty()?L"添加操作":action_name(app.ring_code);
        switch (app.ring_value_kind) {
        case RingValueKind::folder:chosen=L"文件夹";break;
        case RingValueKind::open:chosen=L"打开应用程序、文件或网址";break;
        case RingValueKind::text:chosen=L"文本";break;
        case RingValueKind::shortcut:chosen=L"键盘快捷键";break;
        case RingValueKind::sequence:chosen=L"操作序列";break;
        case RingValueKind::advanced_click:chosen=L"高级点击";break;
        case RingValueKind::dpi_toggle:chosen=L"切换指针速度";break;
        default:break;
        }
        styled_button(app,L"OS",0,x+8,y+9,23,23,{Look::ring_tag,false,{},{},{}});
        label(app,chosen,x+40,y+10,225,25,app.caption);
        styled_button(app,L"···",ring_menu,x+273,y+2,36,36,{Look::ring_menu_item,false,{},{},{}});
        icon_button(app,L"关闭操作编辑器",close_pane,x+312,y+2,36,36,L"close");
        const auto icon=icon_button(app,L"编辑图标",ring_icon_edit,x+20,y+61,59,61,ring_glyph(app.ring_code),Look::ring_slot);
        app.button_skins[icon].ring_icon=ring_icon_for(app,app.ring_section,index,app.ring_code);
        app.button_skins[icon].original_icon=ring_image(app,*app.button_skins[icon].ring_icon);
        app.ring_choice=styled_button(app,ring_label(app.ring_name),ring_custom,x+79,y+49,251,28,
                                      {Look::ring_menu_item,false,{},{},{}});
        if (app.ring_code.starts_with(L"Folder(") || app.ring_code.starts_with(L"Open("))
            label(app,app.ring_code.starts_with(L"Folder(")?L"包含一组操作":chosen,x+96,y+82,230,44,app.normal);
        if (app.ring_advanced) {
            label(app,L"显示名称",x+16,y+137,120,20,app.small);
            app.input=add(app,L"EDIT",app.ring_name,WS_TABSTOP|ES_AUTOHSCROLL,x+16,y+161,318,31,field);
            SendMessageW(app.input,EM_SETLIMITTEXT,24,0);
            Text value,title;
            if (app.ring_value_kind==RingValueKind::open) {
                title=L"应用程序、文件夹或网址";
                value=app.ring_code.substr(5,app.ring_code.size()-6);
            } else if (app.ring_value_kind==RingValueKind::text) {
                title=L"文本（最多 1000 个字符）";
                value=ring_text_value(app.ring_code);
            } else if (app.ring_value_kind==RingValueKind::shortcut) {
                title=L"按下所需的快捷键";value=app.ring_code==L"@shortcut"?Text{}:app.ring_code;
            }
            if (!title.empty()) {
                label(app,title,x+16,y+203,318,20,app.small);
                const bool multiline=app.ring_value_kind==RingValueKind::text;
                app.ring_value=add(app,L"EDIT",value,WS_TABSTOP|
                    (multiline?ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN:ES_AUTOHSCROLL),
                    x+16,y+227,318,multiline?120:31,field+1);
                SendMessageW(app.ring_value,EM_SETLIMITTEXT,multiline?1000:2048,0);
                if (app.ring_value_kind==RingValueKind::shortcut) {
                    SendMessageW(app.ring_value,EM_SETREADONLY,TRUE,0);
                    lazy_win32::set_window_subclass(app.ring_value,ring_shortcut,1,0);
                }
            }
            if (app.ring_value_kind==RingValueKind::open)
                button(app,L"浏览…",ring_browse,x+244,y+275,90,32);
            if(app.ring_value_kind==RingValueKind::dpi_toggle) {
                const auto comma=app.ring_code.find(L',');const auto a=app.ring_code.substr(10,comma-10),b=app.ring_code.substr(comma+1,app.ring_code.size()-comma-2);
                label(app,L"速度 1（DPI）",x+16,y+202,145,20,app.small);label(app,L"速度 2（DPI）",x+181,y+202,145,20,app.small);
                app.ring_value=add(app,L"EDIT",a,ES_NUMBER|WS_TABSTOP,x+16,y+227,145,31,field+1);
                app.ring_value2=add(app,L"EDIT",b,ES_NUMBER|WS_TABSTOP,x+181,y+227,145,31,field+2);
            }
            if(app.ring_value_kind==RingValueKind::advanced_click) {
                constexpr const wchar_t*types[]{L"Single",L"Double",L"Triple",L"Right",L"Middle"};constexpr const wchar_t*names[]{L"单击",L"双击",L"三击",L"右键",L"中键"};
                const auto comma=app.ring_code.find(L',');const auto type=app.ring_code.substr(14,comma-14);const auto mask=logi::number(app.ring_code.substr(comma+1,app.ring_code.size()-comma-2)).value_or(0);
                label(app,L"点击类型",x+16,y+203,300,20,app.small);
                for(int i=0;i<5;++i)styled_button(app,names[i],ring_click_type_first+i,x+16+i*64,y+232,60,32,{type==types[i]?Look::filled:Look::ring_menu_item,type==types[i],{},{},{}});
                label(app,L"同时按住修饰键",x+16,y+284,300,20,app.small);constexpr const wchar_t*modifiers[]{L"Shift",L"Ctrl",L"Alt",L"Win"};
                for(int i=0;i<4;++i)styled_button(app,modifiers[i],ring_click_modifier_first+i,x+16+i*80,y+313,76,32,{mask&(1u<<i)?Look::filled:Look::ring_menu_item,(mask&(1u<<i))!=0,{},{},{}});
            }
            button(app,L"更改操作",ring_replace,x+16,y+height-50,app.ring_value_kind==RingValueKind::open?204:318,32);
        }
        if (app.ring_code.starts_with(L"Folder("))
            button(app,L"打开文件夹",ring_folder,x+96,y+115,142,28);
        if (app.ring_menu_open) {
            const int menu_x=std::clamp(static_cast<int>(app.ring_menu_anchor.x),0,pane_x-160);
            const int menu_y=std::clamp(static_cast<int>(app.ring_menu_anchor.y),0,client_height-120);
            app.ring_menu_bounds={menu_x,menu_y,menu_x+160,menu_y+120};
            const bool configured=!app.ring_code.empty() && app.ring_code!=L"@shortcut" &&
                app.ring_code!=L"Open()" && app.ring_code!=L"TextHex()";
            const bool folder=app.ring_code.starts_with(L"Folder(");
            const auto copy=styled_button(app,folder?L"剪切文件夹":L"剪切操作",ring_cut,menu_x,menu_y,160,40,
                                         {Look::ring_menu_item,false,{},{},{}});
            EnableWindow(copy,configured);
            const auto paste=styled_button(app,L"粘贴操作",ring_paste,menu_x,menu_y+40,160,40,
                                          {Look::ring_menu_item,false,{},{},{}});
            EnableWindow(paste,app.ring_clipboard.has_value());
            const auto clear=styled_button(app,folder?L"删除文件夹":L"删除操作",ring_clear,menu_x,menu_y+80,160,40,
                                          {Look::ring_menu_item,false,{},{},{}});
            EnableWindow(clear,configured);
        }
    }
    render_ring_profiles(app);
}
Text write_ring_section(std::wstring_view source,const Text& section,
                        const std::vector<std::pair<Text,Text>>& entries) {
    Text result;
    const auto write_entries=[&] {
        for (std::size_t i=0;i<entries.size();++i) {
            if (entries[i].first.empty() && entries[i].second.empty())
                result+=L"@empty."+std::to_wstring(i+1)+L" =\n";
            else result+=entries[i].first+L" = "+entries[i].second+L"\n";
        }
    };
    bool found=false,in_section=false;
    for (const auto& line:lines(source)) {
        if (const auto name=section_name(line);!name.empty()) {
            in_section=name==section;
            result+=line+L"\n";
            if (in_section) {found=true;write_entries();}
        } else if (!in_section || entry(line).first.empty()) result+=line+L"\n";
    }
    if (!found) {result+=L"\n["+section+L"]\n";write_entries();}
    return result;
}
Text unique_ring_name(const App& app,std::size_t index,std::wstring_view wanted) {
    const auto entries=ring_items(app);
    const Text base(wanted.substr(0,24));
    Text name=base;
    unsigned suffix=2;
    for (;;) {
        bool used=false;
        for (std::size_t i=0;i<entries.size();++i)
            if (i!=index && _wcsicmp(entries[i].first.c_str(),name.c_str())==0) {used=true;break;}
        if (!used) return name;
        const auto ending=L" "+std::to_wstring(suffix++);
        name=base.substr(0,24-ending.size())+ending;
    }
}
bool save_ring_slot(App& app,const RingEdit& edit,
                    const std::map<Text,std::vector<std::pair<Text,Text>>>* folder_copy=nullptr,
                    const ring_metadata::Settings* appearance=nullptr) {
    auto entries=ring_items(app,edit.section);
    const auto index=edit.index;
    const Text name=Text(trim(edit.name)), value=Text(trim(edit.code));
    const bool empty=name.empty() && value.empty();
    if (index>=8 || (!empty && (name.empty() || value.empty())) || name.size()>24 ||
        name.find_first_of(L"=\r\n")!=Text::npos || name.starts_with(L"@empty.") ||
        name.starts_with(L'#') || name.starts_with(L';') || name.starts_with(L'[')) {
        notice(app,L"请输入不超过 24 个字符的名称和有效操作。"); return false;
    }
    for (std::size_t i=0;i<entries.size();++i) {
        if (!empty && i!=index && _wcsicmp(entries[i].first.c_str(),name.c_str())==0) {
            notice(app,L"动作环名称已被使用。"); return false;
        }
    }
    const Text previous=app.source;
    if (!appearance&&index<entries.size() && entries[index].first==name && entries[index].second==value) return true;
    if (index>=entries.size()) entries.resize(index+1);
    entries[index]={name,value};
    Text next=write_ring_section(app.source,edit.section,entries);
    if(value.empty()){auto metadata=app.ring_ui;metadata.icons[edit.section][index]={};next=ring_metadata::write(next,metadata);}
    if(folder_copy) for(const auto& [section,items]:*folder_copy) next=write_ring_section(next,section,items);
    if(appearance)next=ring_metadata::write(next,*appearance);
    if (value.starts_with(L"Folder(") && value.ends_with(L")")) {
        const auto folder=L"ring/"+value.substr(7,value.size()-8);
        if ((!folder_copy || !folder_copy->contains(folder)) && !app.config.contains(folder))
            next=write_ring_section(next,folder,{});
    }
    if (!save(app,next)) return false;
    app.ring_undo_stack.push_back(previous);
    app.ring_redo_stack.clear();
    return true;
}
bool save_ring_slot(App& app) {
    return save_ring_slot(app,{app.ring_section,static_cast<std::size_t>(app.pane_index),app.ring_name,app.ring_code});
}
bool ring_swap_slots(App& app, std::size_t src, std::size_t dst) {
    if (src >= 8 || dst >= 8 || src == dst) return false;
    auto entries = ring_items(app, app.ring_section);
    if (entries.size() < 8) entries.resize(8);
    if (entries[src].first.empty() && entries[src].second.empty() &&
        entries[dst].first.empty() && entries[dst].second.empty()) return false;
    std::swap(entries[src], entries[dst]);
    const Text previous = app.source;
    Text next = write_ring_section(app.source, app.ring_section, entries);
    auto metadata = app.ring_ui;
    if (auto it = metadata.icons.find(app.ring_section); it != metadata.icons.end())
        std::swap(it->second[src], it->second[dst]);
    next = ring_metadata::write(next, metadata);
    if (!save(app, next)) return false;
    app.ring_undo_stack.push_back(previous);
    app.ring_redo_stack.clear();
    return true;
}
void flush_ring_edit(App& app) {
    KillTimer(app.window,7);
    if (!app.pending_ring) return;
    const auto edit=std::move(*app.pending_ring);
    app.pending_ring.reset();
    if (save_ring_slot(app,edit)) {
        if (app.ring_choice && app.page==ring && app.ring_section==edit.section &&
            app.pane_index>=0 && static_cast<std::size_t>(app.pane_index)==edit.index)
            SetWindowTextW(app.ring_choice,ring_label(Text(trim(edit.name))).c_str());
        InvalidateRect(app.window,nullptr,FALSE);
    }
}
void flush_editor_edits(App& app) { flush_binding_edit(app);flush_ring_edit(app);save_sensitivity(app); }
bool ring_save_source(App& app,const Text& next) {
    const auto previous=app.source;
    if(next==previous)return true;
    if(!save(app,next))return false;
    app.ring_undo_stack.push_back(previous);app.ring_redo_stack.clear();return true;
}
Text ring_unique_section(const App& app,std::wstring_view prefix=L"profile-") {
    for(unsigned n=1;;++n){auto section=L"ring/"+Text(prefix)+std::to_wstring(n);if(!app.config.contains(section))return section;}
}
Text ring_clone_tree(const App& destination,const App& source,const Text& root,const Text& target,
                     Text next,ring_metadata::Settings& metadata) {
    std::map<Text,Text> translated{{root,target}};
    unsigned sequence{};
    const auto clone=[&](auto&& self,const Text& from,const Text& to)->void {
        auto entries=ring_items(source,from);
        for(auto&[label,code]:entries)if(code.starts_with(L"Folder(")&&code.ends_with(L")")) {
            const auto child=L"ring/"+code.substr(7,code.size()-8);
            if(!translated.contains(child)) {
                Text generated;do{generated=L"ring/profile-folder-"+std::to_wstring(++sequence);}while(destination.config.contains(generated)||std::ranges::any_of(translated,[&](const auto&item){return item.second==generated;}));
                translated.emplace(child,generated);self(self,child,generated);
            }
            code=L"Folder("+translated.at(child).substr(5)+L")";
        }
        next=write_ring_section(next,to,entries);
        if(const auto found=source.ring_ui.icons.find(from);found!=source.ring_ui.icons.end())metadata.icons[to]=found->second;
    };
    clone(clone,root,target);return next;
}
void ring_transfer_profile(App& app,bool importing) {
    wchar_t path[4096]{};if(!importing)wcscpy_s(path,L"Actions-Ring.lp5");
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=app.window;dialog.lpstrFile=path;dialog.nMaxFile=std::size(path);dialog.lpstrDefExt=L"lp5";
    dialog.lpstrFilter=L"Actions Ring 配置文件 (*.lp5;*.lp4;*.ini)\0*.lp5;*.lp4;*.ini\0";dialog.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(importing?OFN_FILEMUSTEXIST:OFN_OVERWRITEPROMPT);
    if(!(importing?lazy_win32::get_open_file_name(&dialog):lazy_win32::get_save_file_name(&dialog)))return;
    try {
        if(importing) {
            const auto raw=read_binary(path);
            std::vector<std::uint8_t> raw_bytes(raw.size());
            for(std::size_t i=0;i<raw.size();++i)raw_bytes[i]=static_cast<std::uint8_t>(raw[i]);
            const auto package=ring_profile_package::decode_bytes(raw_bytes);
            App imported;imported.source=package.source;imported.config=parse(imported.source);imported.ring_ui=ring_metadata::read(imported.source);
            if(!imported.config.contains(L"ring"))throw std::runtime_error("Ring profile has no root");
            ring_profile_package::materialize(package,imported.ring_ui,app.directory/L"assets"/L"ring-icons");
            const auto section=ring_unique_section(app);auto metadata=app.ring_ui;
            auto next=ring_clone_tree(app,imported,L"ring",section,app.source,metadata);
            metadata.profiles.push_back({app.ring_application,fs::path(path).stem().wstring(),section});metadata.active[app.ring_application]=section;
            if(ring_save_source(app,ring_metadata::write(next,metadata))){app.ring_section=section;app.ring_navigation.clear();app.pane_index=-1;}
        } else {
            App empty;ring_metadata::Settings metadata;
            const auto source=ring_clone_tree(empty,app,ring_profile_target(app),L"ring",L"",metadata);
            const auto ext=fs::path(path).extension().wstring();
            std::ofstream output(fs::path(path),std::ios::binary|std::ios::trunc);
            if (_wcsicmp(ext.c_str(), L".lp5") == 0 || _wcsicmp(ext.c_str(), L".lp4") == 0) {
                const auto bytes = ring_profile_package::encode_lp5(source, metadata, fs::path(path).stem().wstring());
                output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            } else {
                const auto bytes = to_utf8(ring_profile_package::encode(source, metadata));
                output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
            if(!output)throw std::runtime_error("Cannot write Ring profile");
            notice(app,L"Actions Ring 配置文件已导出。");
        }
    }catch(const std::exception&error){notice(app,L"配置文件操作失败："+from_utf8(error.what()));}
}
void ring_edit_icon(App& app) {
    if(app.pane_index<0)return;
    const auto index=static_cast<std::size_t>(app.pane_index);
    auto icon=ring_icon_for(app,app.ring_section,index,app.ring_code);
    if(icon.text.empty())icon.text=app.ring_name;
    std::array<Text,8> names;const auto entries=ring_items(app);for(std::size_t i=0;i<entries.size()&&i<8;++i)names[i]=entries[i].first;
    if(auto edited=ring_editor::edit(app.window,icon,std::move(names),static_cast<unsigned>(index))) {
        if(!edited->icon.image.empty()) {
            try {
                const auto folder=app.directory/L"assets"/L"ring-icons";fs::create_directories(folder);
                const fs::path input(edited->icon.image);
                if(input.parent_path()!=folder) {
                    const auto name=std::to_wstring(GetTickCount64())+L"-"+input.filename().wstring();
                    fs::copy_file(input,folder/name);edited->icon.image=(folder/name).wstring();
                }
            }catch(const std::exception&error){notice(app,L"图像保存失败："+from_utf8(error.what()));return;}
        }
        auto settings=app.ring_ui;
        ring_metadata::apply_icon(settings,app.ring_section,edited->slots,edited->icon);
        ring_save_source(app,ring_metadata::write(app.source,settings));
    }
}
#include "native_cloud_ui.hpp"

void transfer_config(App& app,bool importing) {
    flush_editor_edits(app);
    if(app.busy||!app.pending_values.empty()||app.pending_reload||app.reload_busy) {
        notice(app,L"请等待当前设置保存完成后再导入或导出。");return;
    }
    wchar_t path[4096]{};
    if(!importing)wcscpy_s(path,L"LogiOptionsOpen-backup.ini");
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=app.window;
    dialog.lpstrFilter=L"配置备份 (*.ini)\0*.ini\0所有文件\0*.*\0";
    dialog.lpstrFile=path;dialog.nMaxFile=std::size(path);dialog.lpstrDefExt=L"ini";
    dialog.lpstrTitle=importing?L"导入配置":L"导出配置";
    dialog.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|(importing?OFN_FILEMUSTEXIST:OFN_OVERWRITEPROMPT);
    if(!(importing?lazy_win32::get_open_file_name(&dialog):lazy_win32::get_save_file_name(&dialog)))return;
    try {
        if(!importing){
            if(_wcsicmp(fs::absolute(path).lexically_normal().c_str(),fs::absolute(app.mapping()).lexically_normal().c_str())==0)
                throw std::runtime_error("Choose a backup path different from the active configuration");
            loo_backup::Configuration backup{to_utf8(read(app.mapping()))};
            for(unsigned index=0;index<6;++index)backup.settings[index]=app.device.values[index];
            const auto bytes=loo_backup::encode(backup);
            wchar_t temporary[MAX_PATH]{};
            if(!GetTempFileNameW(fs::path(path).parent_path().c_str(),L"LOB",0,temporary))throw std::runtime_error("Cannot create backup file");
            std::ofstream output(fs::path(temporary),std::ios::binary);output.write(bytes.data(),bytes.size());output.close();
            if(!output||!MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
                DeleteFileW(temporary);throw std::runtime_error("Cannot write backup file");
            }
            notice(app,L"配置已导出，包含全部映射及已读取的 DPI / 滚轮参数。");return;
        }
        std::ifstream input(fs::path(path),std::ios::binary|std::ios::ate);
        if(!input)throw std::runtime_error("Cannot read backup file");
        std::string bytes;
        if (const auto size=input.tellg(); size>0) {
            bytes.resize(static_cast<std::size_t>(size));
            input.seekg(0,std::ios::beg);
            input.read(bytes.data(),size);
        }
        const auto backup=loo_backup::decode(bytes);
        const bool has_settings=std::ranges::any_of(backup.settings,[](const auto& value){return value.has_value();});
        if(has_settings&&(!agent_running()||options_running()||!app.device.values[0])) {
            notice(app,L"备份含设备参数，请先连接鼠标并启动本地后台；当前配置未修改。");return;
        }
        if(backup.settings[0]&&std::ranges::find(app.device.dpi,*backup.settings[0])==app.device.dpi.end()) {
            notice(app,L"备份的 DPI 不受当前鼠标支持；当前配置未修改。");return;
        }
        if(!save(app,from_utf8(backup.mappings)))return;
        for(unsigned index=0;index<6;++index)if(backup.settings[index])queue_parameter(app,index,*backup.settings[index],true);
        flush_parameters(app);
        if(!has_settings)notice(app,L"配置已导入；后台应用结果见重载状态。");
    }catch(const std::exception& error){notice(app,L"配置操作失败："+from_utf8(error.what()));}
}
void render(App& app) {
    if (!app.ui_active) return;
    if (app.page != home) app.front.unload();
    if (app.page != easy) app.bottom.unload();
    if (app.page != buttons && app.page != pointer) app.side.unload();
    if (app.page != buttons) { app.firefox_icon.unload(); app.chrome_icon.unload(); app.wps_icon.unload(); }
    if (app.page != ring) {
        app.ring_images.clear();
        app.ring_fonts.faces.clear();
        app.ring_catalog.clear();
        app.ring_catalog.shrink_to_fit();
        app.ring_catalog_visible.clear();
        app.ring_catalog_visible.shrink_to_fit();
        app.ring_registry_cache.clear();
        app.ring_registry_cache.shrink_to_fit();
        app.ring_rows.clear();
        app.ring_rows.shrink_to_fit();
        app.ring_smart_actions.clear();
        app.ring_smart_actions.shrink_to_fit();
    }
    if (app.page != buttons && app.page != ring) {
        for (const auto& [name, icon] : app.application_icons) if (icon) DestroyIcon(icon);
        app.application_icons.clear();
    }
    if (app.page != pairing) {
        app.bolt_slots.clear();
        app.bolt_slots.shrink_to_fit();
        app.bt_devices.clear();
        app.bt_devices.shrink_to_fit();
    }
    if (app.page != flow) {
        app.discovered_peers.clear();
        app.discovered_peers.shrink_to_fit();
    }
    ring_typography=app.page==ring;
    flush_editor_edits(app);
    app.rendering=true;
    const bool animate=app.has_frame && app.rendered_page==app.page &&
        (app.page==buttons || app.page==pointer) && app.rendered_editing!=(app.pane_index>=0) &&
        !IsIconic(app.window) && GetForegroundWindow()==app.window;
    const auto previous_image=app.visual_image;
    std::map<int,RECT> previous;
    if (animate) for (const auto child:app.children) {
        const int id=GetDlgCtrlID(child);
        if (!id || !IsWindowVisible(child)) continue;
        RECT box{};GetWindowRect(child,&box);
        MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&box),2);
        previous[id]=box;
    }
    KillTimer(app.window,6);app.animating=false;app.moving.clear();
    app.note=app.input=app.slider=app.speed_value=app.dpi_value=app.sensitivity_value=app.action_list=app.search=app.ring_value=app.save_action=nullptr;
    app.ring_list=app.ring_filter=app.ring_choice=app.ring_value2=nullptr;
    app.flow_name_edit=app.flow_host_edit=app.flow_port_edit=app.flow_pin_edit=app.action_param_box=nullptr;
    for (const auto child:app.children) DestroyWindow(child);
    app.children.clear();
    app.button_skins.clear();
    app.tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,app.window,nullptr,GetModuleHandleW(nullptr),nullptr);
    app.children.push_back(app.tips);
    SendMessageW(app.tips,TTM_SETMAXTIPWIDTH,0,360);
    app.gesture={};
    app.hardware={};
    const auto tr=[&](const wchar_t* zh,const wchar_t* en){return Text(ui_settings::tr(app.ui.language,zh,en));};
    const bool device_page=app.page!=home && app.page!=ring && app.page!=pairing;
    const bool editing=app.pane_index>=0;
    icon_button(app,tr(L"最小化",L"Minimize"),minimize_ui,1390,1,48,23,L"minimize");
    icon_button(app,tr(L"关闭",L"Close"),close_ui,1440,1,52,23,L"close");
    if (app.page!=home) icon_button(app,tr(L"返回",L"Back"),back,43,35,40,42,L"back");
    if (!editing && app.page!=ring) {
        Text title;
        if (app.page==home) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            title=now.wHour<11?tr(L"早上好",L"Good morning"):now.wHour<18?tr(L"下午好",L"Good afternoon"):tr(L"晚上好",L"Good evening");
        } else title=app.device.name.empty()?L"MX Master 3S":app.device.name;
        label(app,title,app.page==home?41:86,32,500,54,app.heading);
    }
    if (app.page==home) {
        icon_button(app,L"Smart Actions",smart,1211,35,42,42,L"smart");
        icon_button(app,L"Actions Ring",ring_page,1259,35,42,42,L"ring");
        icon_button(app,tr(L"设置",L"Settings"),nav_first+4,1404,35,42,42,L"settings");
        icon_button(app,tr(L"刷新设备",L"Refresh"),reload,1050,35,126,42,L"search",Look::tab);
    }
    if (app.page==buttons) {
        const auto count=std::min<std::size_t>(app.profiles.size(),6);
        const bool overflow=app.profiles.size()>6;
        app.profile_offset=std::min(app.profile_offset,app.profiles.size()-count);
        const int profile_end=editing?909:1443;
        const int first=profile_end-static_cast<int>(count)*48-36-(overflow?64:0);
        if (overflow) {
            icon_button(app,tr(L"前面的应用",L"Previous Apps"),profiles_previous,first+46,41,28,30,L"back");
            EnableWindow(GetDlgItem(app.window,profiles_previous),app.profile_offset>0);
            icon_button(app,tr(L"后面的应用",L"Next Apps"),profiles_next,profile_end-68,41,28,30,L"forward");
            EnableWindow(GetDlgItem(app.window,profiles_next),app.profile_offset+count<app.profiles.size());
        }
        for (std::size_t i=0;i<count;++i) {
            const auto index=i?i+app.profile_offset:0;
            const auto& name=app.profiles[index];
            auto caption=name==L"global"?tr(L"全局",L"Global"):fs::path(name).stem().wstring();
            auto* original_icon=_wcsicmp(caption.c_str(),L"firefox")==0?app.firefox_icon.ensure():
                _wcsicmp(caption.c_str(),L"chrome")==0?app.chrome_icon.ensure():
                _wcsicmp(caption.c_str(),L"ksolaunch")==0?app.wps_icon.ensure():nullptr;
            const auto real_icon=name==L"global" || original_icon?nullptr:application_icon(app,name);
            const std::wstring_view icon=name==L"global"?L"global":L"application";
            const int x=first+static_cast<int>(i)*48+(overflow && i?32:0);
            auto control=styled_button(app,caption,profiles_first+static_cast<int>(index),x,31,48,48,
                {Look::profile,name==app.selected,icon,caption,{},1,real_icon,original_icon});
            tooltip(app,control);
            if (i && name==app.selected) {
                auto remove=styled_button(app,tr(L"移除此应用配置",L"Remove App Profile"),delete_profile,x+28,31,16,16,
                    {Look::glyph,false,L"close",tr(L"移除此应用配置",L"Remove App Profile"),{}});
                tooltip(app,remove);
            }
        }
        icon_button(app,tr(L"添加应用程序",L"Add App"),new_profile,profile_end-36,31,48,48,L"add");
    } else if (app.page==pointer) {
        // This page controls the device-wide values.
        icon_button(app,tr(L"全局设置",L"Global Settings"),0,editing?685:1219,35,36,43,L"global",Look::profile,true);
    }
    if (device_page && !editing) {
        const wchar_t* nav_zh[]{L"按钮",L"指向和滚动",L"EASY-SWITCH",L"FLOW",L"设置"};
        const wchar_t* nav_en[]{L"Buttons",L"Point & Scroll",L"EASY-SWITCH",L"FLOW",L"Settings"};
        constexpr const wchar_t* glyphs[]{L"buttons",L"pointer",L"easy",L"flow",L"settings"};
        constexpr int widths[]{92,138,154,102,96};
        for (int i=0;i<5;++i)
            icon_button(app,tr(nav_zh[i],nav_en[i]),nav_first+i,48,337+i*57,widths[i],40,glyphs[i],Look::tab,
                        app.page==buttons+i);
    }
    if (app.page==home) {
        styled_button(app,app.device.name.empty()?L"MX Master 3S · 未检测到":app.device.name,
                      nav_first,670,318,154,225,{Look::device,false,{},{},{}});
        if (app.device.name.empty()) label(app,tr(L"设备未连接",L"Device disconnected"),700,552,160,26,app.small);
        styled_button(app,tr(L"+ 添加设备",L"+ Add Device"),open_pairing,864,394,168,46,{Look::plain,false,{},{},{}});
    } else if (app.page==buttons || app.page==pointer) {
        points(app);
        if (app.pane_index>=0) {
            if (app.page==buttons) actions_pane(app);
            else hardware_pane(app);
        }
    } else if (app.page==easy) {
        label(app,L"Easy-Switch",823,240,310,34,app.subheading);
        label(app,tr(L"按下 Easy-Switch 按钮，在已配对电脑之间切换。",L"Press Easy-Switch to switch between paired computers."),823,275,320,44);
        label(app,tr(L"当前主机：",L"This PC: ")+local_computer_name(),823,322,320,24,app.small);
        for (unsigned i=1;i<=3;++i) {
            const auto found=std::ranges::find(app.device.hosts,i,&logi::HostSlot::slot);
            const bool known=found!=app.device.hosts.end();
            const bool current=known&&found->current, paired=known&&found->paired.value_or(false);
            const Text text=current?tr(L"当前电脑",L"This computer"):paired?tr(L"已配对的电脑",L"Paired computer"):known?tr(L"没有已配对的电脑",L"Empty slot"):tr(L"未知",L"Unknown");
            const auto control=styled_button(app,std::to_wstring(i)+L" "+text,hosts_first+i,
                795,367+static_cast<int>(i-1)*80,353,80,{Look::slot,current,{},text,current?L"Logi Bolt":L""});
            EnableWindow(control,paired&&!current&&!app.native&&!app.official&&!app.busy);
        }
        styled_button(app,tr(L"+ 配对新设备",L"+ Pair Device"),open_pairing,795,624,148,36,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"刷新",L"Refresh"),reload,1046,624,94,36,{Look::plain,false,{},{},{}});
    } else if (app.page==pairing) {
        label(app,tr(L"添加或配对设备",L"Add or Pair Device"),120,108,420,36,app.subheading);
        label(app,tr(L"扫描 Logi Bolt 接收器通道或通过 Windows 蓝牙连接罗技设备。",L"Inspect Logi Bolt receiver slots or pair via Windows Bluetooth LE."),
              120,148,760,26,app.small);
        styled_button(app,L"Logi Bolt",pairing_tab_bolt,120,192,140,38,{Look::tab,!app.pairing_tab_bluetooth,{},{},{}});
        styled_button(app,tr(L"蓝牙设备",L"Bluetooth"),pairing_tab_bt,272,192,140,38,{Look::tab,app.pairing_tab_bluetooth,{},{},{}});
        styled_button(app,tr(L"刷新扫描",L"Scan Now"),pairing_scan,432,192,120,38,{Look::plain,false,{},{},{}});
        if (!app.pairing_tab_bluetooth) {
            label(app,app.bolt_pairing_open?tr(L"Logi Bolt 接收器 · 配对锁定已打开（60秒）",L"Bolt Receiver · Pairing lock OPEN (60s)")
                                          :tr(L"Logi Bolt 接收器 · 已配对通道（1~6）",L"Bolt Receiver · Paired Slots (1-6)"),
                  120,256,520,28,app.bold);
            styled_button(app,app.bolt_pairing_open?tr(L"关闭配对锁定",L"Close Pairing Lock"):tr(L"开启配对锁定 (60s)",L"Open Pairing Lock (60s)"),
                          pairing_bolt_lock,120,294,196,40,{Look::plain,app.bolt_pairing_open,{},{},{}});
            if (app.bolt_slots.empty()) {
                label(app,tr(L"暂未发现 Bolt 接收器通道记录（点击「刷新扫描」检测接收器）。",L"No Bolt receiver slots enumerated yet (click Scan Now)."),
                      120,356,700,32,app.small);
            } else {
                int y=354;
                for (std::size_t i=0;i<app.bolt_slots.size()&&i<6;++i) {
                    const auto& slot=app.bolt_slots[i];
                    wchar_t pid_hex[16]{};
                    swprintf_s(pid_hex,L"%04X",slot.pid);
                    const Text row=L"通道 "+std::to_wstring(slot.slot)+L"  ·  "+
                        (slot.name.empty()?tr(L"已配对罗技设备",L"Paired Logitech Device"):slot.name)+
                        L"  (PID "+pid_hex+L")"+(slot.active?tr(L"  [活跃]",L"  [Active]"):L"");
                    label(app,row,120,y,740,30);
                    y+=42;
                }
            }
        } else {
            label(app,tr(L"Windows 蓝牙已配对 / 记忆设备",L"Windows Paired / Remembered Bluetooth Devices"),120,256,520,28,app.bold);
            styled_button(app,tr(L"打开 Windows 蓝牙设置",L"Open Windows Bluetooth"),pairing_open_bt,120,294,226,40,{Look::plain,false,{},{},{}});
            if (app.bt_devices.empty()) {
                label(app,tr(L"未发现蓝牙设备（或系统蓝牙已关闭）。",L"No Bluetooth devices found."),120,356,640,32,app.small);
            } else {
                int y=354;
                for (std::size_t i=0;i<app.bt_devices.size()&&i<8;++i) {
                    const auto& bt=app.bt_devices[i];
                    const Text row=(bt.name.empty()?tr(L"未知设备",L"Unknown Device"):bt.name)+
                        L"  ["+bt.address+L"]  ·  "+(bt.connected?tr(L"已连接",L"Connected"):bt.remembered?tr(L"已配对",L"Paired"):tr(L"已发现",L"Seen"));
                    label(app,row,120,y,880,28);
                    y+=36;
                }
            }
        }
    } else if (app.page==flow) {
        const ui_settings::FlowPeerConfig fallback_peer{L"peer-1",L"Peer-PC",L"",app.ui.flow_port,ui_settings::FlowEdge::right,2,1920,1080};
        if (app.selected_flow_peer >= app.ui.flow_peers.size()) app.selected_flow_peer = 0;
        const auto& peer = app.ui.flow_peers.empty() ? fallback_peer : app.ui.flow_peers[app.selected_flow_peer];
        label(app,tr(L"Flow 跨电脑跟随与剪贴板",L"Flow Cross-Computer & Clipboard"),280,96,560,34,app.subheading);
        label(app,tr(L"基于局域网 TCP + HMAC-SHA256 验证与 HID++ Easy-Switch 通道联动，支持双机/多机跨屏切换与文本共享。",
                     L"LAN TCP + HMAC-SHA256 + HID++ Easy-Switch switching with shared clipboard."),
              280,132,1100,24,app.small);
        // Bottom-left discovered peers / configured nodes under the canvas (canvas paints at y=168..438, x=280..820)
        label(app,tr(L"已配置与局域网发现的节点：",L"Configured & LAN Discovered Peers:"),280,454,320,24,app.bold);
        styled_button(app,tr(L"扫描局域网",L"Scan LAN"),flow_discover,686,448,134,34,{Look::plain,false,{},{},{}});
        int px_cur=280, py_cur=494;
        for (std::size_t i=0;i<app.ui.flow_peers.size()&&i<4;++i) {
            const auto& p=app.ui.flow_peers[i];
            styled_button(app,p.name.empty()?p.host:p.name,flow_peer_first+static_cast<int>(i),
                          px_cur,py_cur,168,36,{Look::tab,i==app.selected_flow_peer,{},{},{}});
            px_cur+=178;
            if (px_cur>660) {px_cur=280;py_cur+=44;}
        }
        int dy=586;
        for (std::size_t i=0;i<app.discovered_peers.size()&&i<3;++i) {
            const auto& d=app.discovered_peers[i];
            styled_button(app,tr(L"填入：",L"Use: ")+d.name+L" ("+d.host+L":"+std::to_wstring(d.port)+L")",
                          flow_peer_first+50+static_cast<int>(i),280,dy,540,34,{Look::plain,false,{},{},{}});
            dy+=40;
        }
        if (app.discovered_peers.empty())
            label(app,tr(L"提示：另一台运行 LogiNative 后台的同网段电脑可被 UDP 59836 自动发现；单机可用「本地回路自检」验证协议闭环。",
                         L"Tip: Peers running LogiNative daemon respond on UDP 59836; use Loopback Self-Test on a single PC."),
                  280,590,540,48,app.small);
        // Right column toggles + fields
        toggle_at(app,tr(L"启用 LogiNative Flow 跨屏跟随",L"Enable LogiNative Flow"),app.ui.flow_enabled,flow_toggle_enabled,860,168,540,true);
        toggle_at(app,tr(L"跨电脑共享剪贴板文本",L"Share clipboard text"),app.ui.flow_share_clipboard,flow_toggle_clip,860,208,540,true);
        toggle_at(app,tr(L"按住 Ctrl 时才触发切屏",L"Hold Ctrl to switch edges"),app.ui.flow_hold_ctrl,flow_toggle_ctrl,860,248,540,true);
        toggle_at(app,tr(L"忽略屏幕角落防止误触",L"Ignore screen corners"),app.ui.flow_edge_corners,flow_toggle_corners,860,288,540,true);
        label(app,tr(L"目标电脑名称",L"Peer Name"),860,342,160,22,app.small);
        app.flow_name_edit=field_at(app,peer.name,flow_edit_name,860,366,258,32);
        label(app,tr(L"目标 IP / 主机名",L"Peer IP / Host"),1138,342,200,22,app.small);
        app.flow_host_edit=field_at(app,peer.host,flow_edit_host,1138,366,262,32);
        label(app,tr(L"TCP 端口",L"TCP Port"),860,412,160,22,app.small);
        app.flow_port_edit=field_at(app,std::to_wstring(peer.port?peer.port:app.ui.flow_port),flow_edit_port,860,436,120,32);
        label(app,tr(L"共享配对 PIN（HMAC-SHA256 密钥）",L"Shared PIN (HMAC-SHA256 Key)"),1000,412,260,22,app.small);
        app.flow_pin_edit=field_at(app,app.ui.flow_pin,flow_edit_pin,1000,436,400,32);
        label(app,tr(L"切换边缘（光标从本机哪一侧离开）",L"Switch Edge"),860,484,320,24,app.small);
        styled_button(app,tr(L"左",L"Left"),flow_edge_left,860,512,92,34,{Look::tab,peer.edge==ui_settings::FlowEdge::left,{},{},{}});
        styled_button(app,tr(L"右",L"Right"),flow_edge_right,960,512,92,34,{Look::tab,peer.edge==ui_settings::FlowEdge::right,{},{},{}});
        styled_button(app,tr(L"上",L"Top"),flow_edge_top,1060,512,92,34,{Look::tab,peer.edge==ui_settings::FlowEdge::top,{},{},{}});
        styled_button(app,tr(L"下",L"Bottom"),flow_edge_bottom,1160,512,92,34,{Look::tab,peer.edge==ui_settings::FlowEdge::bottom,{},{},{}});
        label(app,tr(L"目标对应 Easy-Switch 通道",L"Target Easy-Switch Channel"),860,562,300,24,app.small);
        styled_button(app,L"1",flow_slot_1,860,590,72,34,{Look::tab,peer.host_slot==1,{},{},{}});
        styled_button(app,L"2",flow_slot_2,942,590,72,34,{Look::tab,peer.host_slot==2,{},{},{}});
        styled_button(app,L"3",flow_slot_3,1024,590,72,34,{Look::tab,peer.host_slot==3,{},{},{}});
        button(app,tr(L"保存节点",L"Save Peer"),flow_save_peer,860,652,116,38,true);
        styled_button(app,tr(L"删除节点",L"Remove"),flow_remove_peer,986,652,98,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"测试连通",L"Ping Peer"),flow_ping_peer,1094,652,102,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"本地回路自检",L"Loopback Test"),flow_loopback_test,860,702,148,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"重置 Flow",L"Reset Flow"),flow_reset_all,1018,702,120,38,{Look::plain,false,{},{},{}});
    } else if (app.page==settings && app.cloud_open) {
        render_cloud(app);
    } else if (app.page==settings) {
        label(app,tr(L"关于",L"About"),340,78,500,24,app.bold);
        label(app,L"MX Master 3S",340,108,500,32,app.subheading);
        label(app,app.device.detail.empty()?L"Logi Bolt":app.device.detail,340,144,500,26);
        label(app,tr(L"序列号：",L"Serial: ")+(app.device.serial.empty()?tr(L"未读取",L"Unavailable"):app.device.serial),340,176,500,24,app.small);
        label(app,tr(L"设备固件：",L"Device FW: ")+(app.device.firmware.empty()?tr(L"未读取",L"Unavailable"):app.device.firmware),340,202,500,24,app.small);
        label(app,tr(L"接收器固件：",L"Receiver FW: ")+(app.device.receiver_firmware.empty()?tr(L"无 / 未读取",L"None / Unavailable"):app.device.receiver_firmware),340,228,500,24,app.small);
        styled_button(app,tr(L"检查固件",L"Check Firmware"),settings_check_fw,340,264,132,36,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"恢复默认设置",L"Reset Defaults"),settings_reset_defaults,486,264,142,36,{Look::plain,false,{},{},{}});

        label(app,tr(L"设备后台服务",L"Device Service"),340,334,500,26,app.bold);
        const auto status=app.native?tr(L"本地服务运行中",L"Service running"):app.official?tr(L"检测到其他 Logitech 后台运行中",L"External daemon running"):tr(L"设备服务已停止",L"Service stopped");
        label(app,status,340,368,360,26,app.bold);
        label(app,tr(L"单进程原生驱动鼠标与高阶功能，零遥测、零更新器、零推荐组件。",L"Standalone single-binary mouse service with zero telemetry, updater, or marketing."),340,398,480,38,app.small);
        button(app,app.native?tr(L"停止服务",L"Stop Service"):tr(L"启动服务",L"Start Service"),app.native?end:begin,340,446,144,40,true);

        label(app,tr(L"配置管理",L"Profiles & Config"),340,516,480,26,app.bold);
        styled_button(app,tr(L"刷新设备",L"Refresh"),reload,340,552,116,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"高级配置",L"Advanced INI"),advanced,468,552,116,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"导出配置",L"Export"),export_config,340,602,116,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"导入配置",L"Import"),import_config,468,602,116,38,{Look::plain,false,{},{},{}});
        styled_button(app,tr(L"云同步",L"Cloud sync"),cloud_settings,596,602,124,38,{Look::plain,false,{},{},{}});
        {
            std::error_code ec;
            if (std::filesystem::exists(options_plus_import::default_db_path(), ec))
                styled_button(app,tr(L"导入遗留 settings.db",L"Import settings.db"),settings_import_options_plus,340,652,244,38,{Look::plain,false,{},{},{}});
        }

        // Right column: Theme, Language, Autostart, Tray, Primary Button Swap, Notifications
        label(app,tr(L"后台日志",L"Background logging"),900,648,500,26,app.bold);
        {
            constexpr const wchar_t* zh[]{L"关闭",L"仅错误",L"常规",L"详细"};
            constexpr const wchar_t* en[]{L"Off",L"Errors",L"Normal",L"Detailed"};
            const auto level=static_cast<unsigned>(ui_settings::log_level_now());
            for(unsigned i=0;i<4;++i)
                styled_button(app,tr(zh[i],en[i]),log_level_first+i,900+102*i,681,96,34,{Look::tab,i==level,{},{},{}});
        }
        label(app,tr(L"外观与语言",L"Appearance & Language"),900,78,500,24,app.bold);
        label(app,tr(L"主题模式",L"Theme"),900,114,200,24);
        styled_button(app,tr(L"跟随系统",L"System"),settings_theme_system,900,146,112,34,{Look::tab,app.ui.theme==ui_settings::Theme::system,{},{},{}});
        styled_button(app,tr(L"深色",L"Dark"),settings_theme_dark,1022,146,96,34,{Look::tab,app.ui.theme==ui_settings::Theme::dark,{},{},{}});
        styled_button(app,tr(L"浅色",L"Light"),settings_theme_light,1128,146,96,34,{Look::tab,app.ui.theme==ui_settings::Theme::light,{},{},{}});
        label(app,tr(L"界面语言",L"Language"),900,198,200,24);
        styled_button(app,L"简体中文",settings_lang_zh,900,230,118,34,{Look::tab,app.ui.language==ui_settings::Language::zh_cn,{},{},{}});
        styled_button(app,L"English",settings_lang_en,1028,230,118,34,{Look::tab,app.ui.language==ui_settings::Language::en_us,{},{},{}});

        label(app,tr(L"通用与桌面通知",L"General & Notifications"),900,286,500,24,app.bold);
        const bool swapped=GetSystemMetrics(SM_SWAPBUTTON)!=0;
        const bool autostart=ui_settings::is_windows_autostart_enabled();
        toggle_at(app,tr(L"切换左/右键",L"Swap Left/Right Buttons"),swapped,flip_primary,900,320,500,true);
        toggle_at(app,tr(L"Windows 开机自动启动后台",L"Launch Daemon on Windows Startup"),autostart,settings_autostart,900,362,500,true);
        toggle_at(app,tr(L"关闭窗口时最小化到系统托盘",L"Minimize to System Tray on Close"),app.ui.close_to_tray,settings_close_to_tray,900,404,500,true);
        toggle_at(app,tr(L"低电量桌面提醒 (≤20%)",L"Low Battery Notification (<=20%)"),app.ui.notify_low_battery,settings_notify_battery,900,446,500,true);
        toggle_at(app,tr(L"Caps / Num / Scroll 锁定键提示",L"Caps / Num / Scroll Lock OSD"),app.ui.notify_locks,settings_notify_locks,900,488,500,true);
        toggle_at(app,tr(L"麦克风静音切换提示",L"Microphone Mute OSD"),app.ui.notify_mic,settings_notify_mic,900,530,500,true);
        styled_button(app,tr(L"预览桌面通知 OSD",L"Preview OSD Pill"),settings_notify_test,900,582,184,38,{Look::plain,false,{},{},{}});
    } else if (app.page==ring) render_ring(app);
    if (app.device.battery && app.page!=ring) {
        const auto battery=std::to_wstring(*app.device.battery)+L"%";
        styled_button(app,tr(L"电量 ",L"Battery ")+battery+L" · Logi Bolt",0,app.page==home?695:59,
                      app.page==home?579:781,104,40,{Look::badge,false,{},battery,{}});
    }
    if (app.page==ring && app.pane_index>=0) {
        RECT editor{px(app.ring_editor.left),px(app.ring_editor.top),px(app.ring_editor.right),px(app.ring_editor.bottom)};
        RECT menu{px(app.ring_menu_bounds.left),px(app.ring_menu_bounds.top),
                  px(app.ring_menu_bounds.right),px(app.ring_menu_bounds.bottom)};
        for (const auto child:app.children) {
            const int id=GetDlgCtrlID(child);
            if (id<rings_first || id>=rings_first+8) continue;
            RECT bounds{},intersection{};
            GetWindowRect(child,&bounds);
            MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&bounds),2);
            if (IntersectRect(&intersection,&bounds,&editor) ||
                (app.ring_menu_open && IntersectRect(&intersection,&bounds,&menu))) ShowWindow(child,SW_HIDE);
        }
    }
    app.note=label(app,app.message,419,781,app.page==ring?450:(editing?pane_x:client_width)-455,37,app.small);
    app.animation_to=target_image_area(app);
    app.visual_image=app.animation_to;
    app.pane_to=editing?pane_x:client_width;
    app.visual_pane=app.pane_to;
    if (animate) {
        app.animation_from=previous_image;app.visual_image=previous_image;
        // Original floatText starts at right:-20vw; closing removes the panel.
        app.pane_from=editing?pane_x+client_width/5:client_width;
        app.visual_pane=app.pane_from;
        for (const auto child:app.children) {
            if (!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE) || child==app.tips) continue;
            RECT to{};GetWindowRect(child,&to);
            MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&to),2);
            auto from=to;
            const int id=GetDlgCtrlID(child);
            if (const auto found=previous.find(id);id && found!=previous.end()) from=found->second;
            else if (editing && to.left>=px(pane_x) && id!=minimize_ui && id!=close_ui)
                OffsetRect(&from,px(client_width/5),0);
            if (!EqualRect(&from,&to)) {
                app.moving.push_back({child,from,to});
                SetWindowPos(child,nullptr,from.left,from.top,from.right-from.left,from.bottom-from.top,
                    SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
            }
        }
        app.animating=true;app.animation_start=GetTickCount64();SetTimer(app.window,6,16,nullptr);
    }
    app.has_frame=true;app.rendered_page=app.page;app.rendered_editing=editing;
    app.rendering=false;
    RedrawWindow(app.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
}

void save_binding(App& app,bool inherit,bool redraw=true,bool update_only=false) {
    KillTimer(app.window,5);app.pending_binding.reset();
    const auto key=Text(button_points[app.pane_index].key);
    Section changes;
    if (key==L"thumb") {
        changes[L"thumb.left"]=inherit?L"":control_text(app.gesture[0]);
        changes[L"thumb.right"]=inherit?L"":control_text(app.gesture[1]);
    } else {
        for (unsigned i=0;i<app.gesture.size();++i) {
            const auto item=key+gesture_suffix[i];
            const auto value=control_text(app.gesture[i]);
            if (inherit || value!=get(app,item)) changes[item]=inherit?L"":value;
        }
    }
    normalize_media_edit(app,key,changes);
    if (changes.empty()) return;
    if (save(app,change_section(app.source,app.selected,changes)) && redraw) {
        if (!update_only || app.animating) render(app);
        else {
            // A binding change does not change the page structure. Keep the search,
            // keyboard focus and scroll position instead of rebuilding every child.
            const auto top=SendMessageW(app.action_list,LB_GETTOPINDEX,0,0);
            points(app);
            fill_actions(app);
            if (top>=0) SendMessageW(app.action_list,LB_SETTOPINDEX,top,0);
            InvalidateRect(app.window,nullptr,FALSE);
        }
    }
}
void save_thumb_speed(App& app) {
    if (!app.pending_thumb_speed) return;
    const auto speed=*app.pending_thumb_speed;
    app.pending_thumb_speed.reset();
    if (speed==read_thumb_speed(app)) {notice(app,L"拇指滚轮速度没有变化。");return;}
    const Text value=speed==default_thumb_speed?L"":std::to_wstring(speed);
    if (save(app,change_section(app.source,L"global",{{thumb_speed_key,value}}))) {
        set_agent_thumb_speed(speed);
        if (const auto control=app.page==pointer?GetDlgItem(app.window,pins_first+2):nullptr) {
            if (auto skin=app.button_skins.find(control);skin!=app.button_skins.end()) {
                skin->second.secondary=pointer_summary(app,2);
                SetWindowTextW(control,(skin->second.caption+L" "+skin->second.secondary).c_str());
                InvalidateRect(control,nullptr,FALSE);
            }
        }
    }
}
void add_profile(App& app) {
    wchar_t path[4096]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=app.window;
    dialog.lpstrFilter=L"Windows 应用程序 (*.exe)\0*.exe\0";
    dialog.lpstrFile=path;
    dialog.nMaxFile=std::size(path);
    dialog.lpstrTitle=L"添加应用程序";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!lazy_win32::get_open_file_name(&dialog)) return;
    if (!app.config.contains(path) && !save(app,change_section(app.source,path,{}))) return;
    app.selected=path;
    render(app);
}
void select_action(App& app) {
    const auto index=SendMessageW(app.action_list,LB_GETCURSEL,0,0);
    if (index<0 || static_cast<std::size_t>(index)>=app.visible.size()) return;
    const auto& action=app.visible[index];
    if (app.gesture_panel) {
        const Text key=button_points[app.pane_index].key;
        Section changes;
        if (app.choosing_preset) {
            const auto preset=std::ranges::find_if(gesture_presets,[&](const auto& item) {
                return std::wstring_view(item.title)==action.name;
            });
            if (preset==std::end(gesture_presets)) return;
            if (std::wstring_view(preset->title)!=L"自定义")
                for (unsigned i=0;i<5;++i) changes[key+gesture_suffix[i]]=preset->rules[i];
        } else if (app.gesture_direction>=0) {
            changes[key+gesture_suffix[app.gesture_direction]]=action.code;
            if (app.gesture_direction!=0 && get(app,key)==L"Pan") changes[key]=L"MouseMiddle";
        }
        normalize_media_edit(app,key,changes);
        if (!changes.empty() && !save(app,change_section(app.source,app.selected,changes))) return;
        app.choosing_preset=false; app.gesture_direction=-1;
        render(app);
        return;
    }
    if (std::wstring_view(action.code).starts_with(L"@")) {
        app.gesture_panel=std::wstring_view(action.code)==L"@gesture";
        app.choosing_preset=false; app.gesture_direction=-1;
        app.show_custom=!app.gesture_panel;
        render(app);
        if (app.show_custom) SetFocus(app.gesture[0]);
        return;
    }
    SetWindowTextW(app.gesture[0],action.code);
    if (app.pane_index==2) {
        for (const auto& wheel:wheel_actions) {
            if (std::wstring_view(wheel.name)!=action.name) continue;
            SetWindowTextW(app.gesture[1],wheel.right);
            break;
        }
    } else if (!app.show_custom) {
        for (unsigned i=1;i<app.gesture.size();++i) SetWindowTextW(app.gesture[i],L"None");
    }
    save_binding(app,false,true,true);
}
void navigate(App& app,Page page) {
    app.cloud_open=false;
    app.cloud_secret_draft.clear();
    flush_editor_edits(app);
    app.page=page;
    app.pane_index=-1;
    app.all_actions=app.show_custom=false;
    app.gesture_panel=app.choosing_preset=false; app.gesture_direction=-1;
    update_agents(app);
    read_config(app);
    render(app);
    if (page == easy && app.device.hosts.empty() && !app.device.name.empty() && !app.busy)
        io(app);
    else if(page==pointer)refresh_pointer_dpi(app);
}
void click(App& app,int id) {
    flush_editor_edits(app);
    if(app.ring_creation_stage==2&&id>=ring_create_app_first&&id<ring_create_app_first+static_cast<int>(app.ring_catalog_visible.size())) {
        app.ring_creation_application=app.ring_catalog[app.ring_catalog_visible[id-ring_create_app_first]].path;app.ring_creation_stage=3;render(app);return;
    }
    if(id>=ring_profile_context_first&&id<ring_profile_context_first+static_cast<int>(app.ring_profile_sections.size())) {
        const auto section=app.ring_profile_sections[id-ring_profile_context_first];app.ring_profile_target=app.ring_profile_target==section?L"":section;render(app);return;
    }
    if((id>=ring_click_type_first&&id<ring_click_type_first+5)||(id>=ring_click_modifier_first&&id<ring_click_modifier_first+4)) {
        const auto comma=app.ring_code.find(L',');auto type=app.ring_code.substr(14,comma-14);auto mask=logi::number(app.ring_code.substr(comma+1,app.ring_code.size()-comma-2)).value_or(0);
        constexpr const wchar_t*types[]{L"Single",L"Double",L"Triple",L"Right",L"Middle"};
        if(id<ring_click_modifier_first)type=types[id-ring_click_type_first];else mask^=1u<<(id-ring_click_modifier_first);
        app.ring_code=L"AdvancedClick("+type+L","+std::to_wstring(mask)+L")";save_ring_slot(app);render(app);return;
    }
    if(id>=ring_application_first&&id<ring_application_first+static_cast<int>(app.ring_applications.size())) {
        app.ring_application=app.ring_applications[id-ring_application_first];app.ring_section=ring_root(app);
        app.ring_applications_open=false;app.ring_navigation.clear();app.pane_index=-1;render(app);return;
    }
    if(id>=ring_profile_first&&id<ring_profile_first+static_cast<int>(app.ring_profile_sections.size())) {
        auto settings=app.ring_ui;const auto selected=app.ring_profile_sections[id-ring_profile_first];
        if(selected==ring_metadata::root(app.ring_application))settings.active.erase(app.ring_application);else settings.active[app.ring_application]=selected;
        if(ring_save_source(app,ring_metadata::write(app.source,settings))){app.ring_section=selected;app.ring_profiles_open=false;app.ring_navigation.clear();app.pane_index=-1;render(app);}return;
    }
    if (id>=nav_first&&id<nav_first+5) {
        navigate(app,static_cast<Page>(buttons+id-nav_first));
        return;
    }
    if(id>=log_level_first && id<log_level_first+4) {
        if(!ui_settings::set_log_level(static_cast<ui_settings::LogLevel>(id-log_level_first)))notice(app,L"无法保存日志级别");
        render(app);return;
    }
    if (id>=profiles_first && id<pins_first && static_cast<unsigned>(id-profiles_first)<app.profiles.size()) {
        app.selected=app.profiles[id-profiles_first];
        render(app);
        return;
    }
    if (id>=pins_first && id<pins_first+6) {
        app.pane_index=app.pane_index>=0?-1:id-pins_first;
        app.all_actions=app.show_custom=false;
        app.gesture_panel=app.page==buttons && app.pane_index>=0 && app.pane_index!=2 &&
            point_title(app,button_points[app.pane_index]).starts_with(L"手势 · ");
        app.choosing_preset=false; app.gesture_direction=-1;
        update_agents(app);
        render(app);
        if(app.page==pointer && app.pane_index==1)refresh_pointer_dpi(app);
        return;
    }
    if (id>=gesture_rows_first && id<gesture_rows_first+5) {
        app.gesture_direction=id-gesture_rows_first;
        app.choosing_preset=false; render(app); return;
    }
    if (id>hosts_first && id<=hosts_first+3) {io(app,{},id-hosts_first);return;}
    if (id>=rings_first && id<rings_first+8 && app.page==ring) {
        app.pane_index=id-rings_first;
        const auto items=ring_items(app);
        const auto index=static_cast<std::size_t>(app.pane_index);
        app.ring_name=index<items.size()?items[index].first:L"";
        app.ring_code=index<items.size()?items[index].second:L"";
        app.ring_advanced=false;app.ring_menu_open=false;
        render(app);
        return;
    }
    switch (id) {
    case wheel_standard: case wheel_reverse:
        queue_parameter(app,4,id==wheel_reverse);render(app);break;
    case wheel_free: case wheel_ratchet:
        queue_parameter(app,2,id==wheel_ratchet,true);queue_parameter(app,1,0);render(app);break;
    case profiles_previous: app.profile_offset=app.profile_offset>5?app.profile_offset-5:0;render(app);break;
    case profiles_next: app.profile_offset=std::min(app.profile_offset+5,app.profiles.size()-6);render(app);break;
    case minimize_ui: ShowWindow(app.window,SW_MINIMIZE); break;
    case close_ui: PostMessageW(app.window,WM_CLOSE,0,0); break;
    case more_actions: app.all_actions=!app.all_actions; render(app); break;
    case gesture_presets_open: app.choosing_preset=true; render(app); break;
    case gesture_overview: app.choosing_preset=false; app.gesture_direction=-1; render(app); break;
    case custom_actions:
        if (app.gesture_panel) app.gesture_panel=false;
        else app.show_custom=!app.show_custom;
        render(app); break;
    case back:
        if(app.cloud_open){app.cloud_open=false;app.cloud_secret_draft.clear();render(app);break;}
        if(app.ring_creation_stage){app.ring_creation_stage=app.ring_creation_stage==3?2:app.ring_creation_stage-1;render(app);break;}
        if(app.ring_profile_edit){app.ring_profile_edit=0;app.ring_profile_target.clear();render(app);break;}
        if (app.pane_index>=0) {app.pane_index=-1;render(app);}
        else navigate(app,app.page==home?buttons:home);
        break;
    case close_pane: app.pane_index=-1; render(app); break;
    case smart: show_smart_actions(app.window,app.directory); read_config(app); render(app); break;
    case ring_page: app.ring_section=ring_root(app);app.ring_navigation.clear();navigate(app,ring);break;
    case new_profile: add_profile(app); break;
    case reload: update_agents(app,true); io(app); break;
    case begin: start_agent(app); break;
    case export_config: transfer_config(app,false); break;
    case import_config: transfer_config(app,true); break;
    case cloud_settings: case cloud_dav: case cloud_google: case cloud_onedrive:
    case cloud_automatic: case cloud_save: case cloud_run: case cloud_authorize: case cloud_local: case cloud_remote: case cloud_disconnect:
        click_cloud(app,id);break;
    case end:
        if (stop_agent()==0) {
            app.waiting_agent=false; app.waiting_stop=true; app.timer_checks=0;
            SetTimer(app.window,1,50,nullptr);
            notice(app,L"正在停止设备服务…");
        } else notice(app,L"没有运行中的本地设备服务。");
        break;
    case advanced: {
        const auto argument=L"\"" + app.mapping().wstring() + L"\"";
        ShellExecuteW(app.window,L"open",L"notepad.exe",argument.c_str(),nullptr,SW_SHOWNORMAL);
        break;
    }
    case ring_preview: preview_ring(app.mapping().c_str(),app.ring_section.c_str());break;
    case ring_icon_edit:ring_edit_icon(app);render(app);break;
    case ring_settings:app.ring_settings_open=!app.ring_settings_open;app.ring_profiles_open=app.ring_applications_open=false;render(app);break;
    case ring_adaptive:{auto settings=app.ring_ui;settings.adaptive=!settings.adaptive;ring_save_source(app,ring_metadata::write(app.source,settings));render(app);break;}
    case ring_profiles_menu:app.ring_profiles_open=!app.ring_profiles_open;app.ring_applications_open=app.ring_settings_open=false;app.ring_profile_target.clear();app.pane_index=-1;render(app);break;
    case ring_application_menu:app.ring_applications_open=!app.ring_applications_open;app.ring_profiles_open=app.ring_settings_open=false;app.pane_index=-1;render(app);break;
    case ring_application_add:app.ring_creation_stage=1;app.ring_profiles_open=app.ring_applications_open=false;app.pane_index=-1;render(app);break;
    case ring_create_general:app.ring_application=L"global";app.ring_creation_stage=0;app.ring_profile_edit=ring_profile_new;app.ring_profile_target.clear();render(app);SetFocus(app.input);SendMessageW(app.input,EM_SETSEL,0,-1);break;
    case ring_create_application:collect_ring_applications(app);app.ring_creation_stage=2;app.ring_creation_query.clear();app.ring_creation_page=0;render(app);break;
    case ring_create_import:ring_transfer_profile(app,true);app.ring_creation_stage=0;render(app);break;
    case ring_create_previous:if(app.ring_creation_page)--app.ring_creation_page;render(app);break;
    case ring_create_next:++app.ring_creation_page;render(app);break;
    case ring_create_browse: {
        wchar_t path[4096]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=app.window;dialog.lpstrFile=path;dialog.nMaxFile=std::size(path);dialog.lpstrFilter=L"应用程序 (*.exe)\0*.exe\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
        if(lazy_win32::get_open_file_name(&dialog)){
            app.ring_creation_application=path;app.ring_creation_stage=3;render(app);
        }break;
    }
    case ring_create_blank: {
        const auto application=fs::path(app.ring_creation_application).filename().wstring(),section=ring_metadata::root(application);
        app.ring_application=application;app.ring_section=ring_root(app);app.ring_creation_stage=0;app.ring_profile_target.clear();app.ring_profile_edit=ring_profile_new;render(app);SetFocus(app.input);SendMessageW(app.input,EM_SETSEL,0,-1);break;
    }
    case ring_profile_new:case ring_profile_copy:case ring_profile_rename:
        if(id==ring_profile_new)app.ring_profile_target.clear();
        app.ring_profile_edit=id;app.ring_profiles_open=false;render(app);SetFocus(app.input);SendMessageW(app.input,EM_SETSEL,0,-1);break;
    case ring_profile_cancel:
        if(!app.config.contains(ring_metadata::root(app.ring_application))){app.ring_application=L"global";app.ring_section=ring_root(app);}
        app.ring_profile_edit=0;app.ring_profile_target.clear();render(app);break;
    case ring_profile_accept:{
        const auto name=Text(trim(control_text(app.input)));if(name.empty())break;
        auto metadata=app.ring_ui;auto next=app.source;Text section=ring_profile_target(app);
        if(!app.config.contains(ring_metadata::root(app.ring_application)))next=write_ring_section(next,ring_metadata::root(app.ring_application),{});
        if(app.ring_profile_edit==ring_profile_rename){
            const auto found=std::ranges::find_if(metadata.profiles,[&](const auto&p){return p.application==app.ring_application&&p.section==section;});
            if(found!=metadata.profiles.end())found->name=name;else metadata.profiles.push_back({app.ring_application,name,section});
            section=ring_root(app);
        }
        else {
            section=ring_unique_section(app);
            next=app.ring_profile_edit==ring_profile_copy?ring_clone_tree(app,app,ring_profile_target(app),section,next,metadata):write_ring_section(next,section,{});
            metadata.profiles.push_back({app.ring_application,name,section});metadata.active[app.ring_application]=section;
        }
        if(ring_save_source(app,ring_metadata::write(next,metadata))){app.ring_section=section;app.ring_navigation.clear();app.ring_profile_edit=0;app.ring_profile_target.clear();app.pane_index=-1;render(app);}break;
    }
    case ring_profile_delete:{
        const auto section=ring_profile_target(app);if(section==ring_metadata::root(app.ring_application))break;
        auto metadata=app.ring_ui;std::erase_if(metadata.profiles,[&](const auto&p){return p.section==section&&p.application==app.ring_application;});if(ring_root(app)==section)metadata.active.erase(app.ring_application);
        // A profile section can also be referenced by a folder. Keep its payload until explicitly edited.
        if(ring_save_source(app,ring_metadata::write(app.source,metadata))){app.ring_section=ring_root(app);app.ring_profiles_open=false;app.ring_profile_target.clear();app.pane_index=-1;render(app);}break;
    }
    case ring_profile_export:case ring_profile_import:ring_transfer_profile(app,id==ring_profile_import);app.ring_profiles_open=false;app.ring_profile_target.clear();render(app);break;
    case replace_ring:
        if (save_ring_slot(app)) {app.ring_advanced=false;render(app);} break;
    case ring_all:
        app.ring_searching=false;app.ring_query.clear();app.ring_category.clear();render(app);break;
    case ring_search:
        app.ring_searching=!app.ring_searching;
        render(app);
        if (app.ring_searching) SetFocus(app.ring_filter);
        break;
    case ring_custom:
        app.ring_menu_open=false;
        app.ring_advanced=!app.ring_advanced;
        render(app);
        if (app.ring_advanced) SetFocus(app.input);
        break;
    case ring_menu:
        app.ring_menu_anchor={app.ring_editor.left+273,app.ring_editor.top+38};
        app.ring_menu_open=!app.ring_menu_open;render(app);break;
    case ring_cut: {
        if (app.pane_index<0 || app.ring_code.empty()) break;
        const RingEdit item{app.ring_section,static_cast<std::size_t>(app.pane_index),app.ring_name,app.ring_code};
        std::map<Text,std::vector<std::pair<Text,Text>>> folders;
        std::map<Text,std::array<RingIcon,8>> folder_icons;
        const auto snapshot=[&](auto&& self,const Text& code)->void {
            if(!code.starts_with(L"Folder(") || !code.ends_with(L")"))return;
            const auto section=L"ring/"+code.substr(7,code.size()-8);
            if(folders.contains(section))return;
            const auto items=ring_items(app,section);
            folders.emplace(section,items);
            if(const auto icons=app.ring_ui.icons.find(section);icons!=app.ring_ui.icons.end())folder_icons[section]=icons->second;
            for(const auto& [name,child]:items)self(self,child);
        };
        snapshot(snapshot,app.ring_code);
        const auto appearance=ring_metadata::icon(app.ring_ui,item.section,item.index);
        if (save_ring_slot(app,{item.section,item.index,{}, {}})) {
            app.ring_clipboard_icon=appearance;
            app.ring_clipboard=item;app.ring_copied_folders=std::move(folders);
            app.ring_copied_icons=std::move(folder_icons);
            app.ring_name.clear();app.ring_code.clear();
            app.ring_menu_open=app.ring_advanced=false;render(app);
        }
        break;
    }
    case ring_paste: {
        if (app.pane_index<0 || !app.ring_clipboard) break;
        auto edit=*app.ring_clipboard;
        edit.section=app.ring_section;edit.index=static_cast<std::size_t>(app.pane_index);
        edit.name=unique_ring_name(app,edit.index,edit.name);
        const bool folder=edit.code.starts_with(L"Folder(") && edit.code.ends_with(L")");
        std::map<Text,std::vector<std::pair<Text,Text>>> copies;
        auto metadata=app.ring_ui;metadata.icons[edit.section][edit.index]=app.ring_clipboard_icon;
        if (folder) {
            unsigned suffix=1;
            std::map<Text,Text> names;
            for(const auto& [section,items]:app.ring_copied_folders) {
                Text name;
                do {name=L"folder-copy-"+std::to_wstring(suffix++);} while(app.config.contains(L"ring/"+name));
                names.emplace(section.substr(5),name);
            }
            const auto remap=[&](const Text& code) {
                if(code.starts_with(L"Folder(") && code.ends_with(L")"))
                    if(const auto found=names.find(code.substr(7,code.size()-8));found!=names.end())
                        return L"Folder("+found->second+L")";
                return code;
            };
            edit.code=remap(edit.code);
            for(const auto& [section,items]:app.ring_copied_folders) {
                auto cloned=items;
                for(auto& [name,code]:cloned)code=remap(code);
                copies.emplace(L"ring/"+names.at(section.substr(5)),std::move(cloned));
                if(const auto icons=app.ring_copied_icons.find(section);icons!=app.ring_copied_icons.end())metadata.icons[L"ring/"+names.at(section.substr(5))]=icons->second;
            }
        }
        if (save_ring_slot(app,edit,folder?&copies:nullptr,&metadata)) {
            app.ring_name=edit.name;app.ring_code=edit.code;
            app.ring_menu_open=app.ring_advanced=false;render(app);
        }
        break;
    }
    case ring_clear:
        if (app.pane_index>=0 && save_ring_slot(app,{app.ring_section,static_cast<std::size_t>(app.pane_index),{}, {}})) {
            app.ring_name.clear();app.ring_code.clear();
            app.ring_menu_open=app.ring_advanced=false;render(app);
        }
        break;
    case ring_replace:
        app.ring_menu_open=app.ring_advanced=false;app.ring_searching=true;
        app.ring_query.clear();render(app);SetFocus(app.ring_filter);break;
    case ring_browse: {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=app.window;
        dialog.lpstrFilter=L"应用程序和文件\0*.*\0\0";
        dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if (lazy_win32::get_open_file_name(&dialog) && app.ring_value) SetWindowTextW(app.ring_value,path);
        break;
    }
    case ring_undo: case ring_redo: {
        auto& from=id==ring_undo?app.ring_undo_stack:app.ring_redo_stack;
        auto& to=id==ring_undo?app.ring_redo_stack:app.ring_undo_stack;
        if (from.empty()) break;
        const auto previous=app.source;
        if (save(app,from.back())) {
            from.pop_back();to.push_back(previous);
            if(!app.config.contains(app.ring_section)) {
                app.ring_section=L"ring";app.ring_navigation.clear();
            }
            app.pane_index=-1;app.ring_menu_open=false;render(app);
        }
        break;
    }
    case ring_folder: {
            const auto items=ring_items(app);
            const auto index=static_cast<std::size_t>(app.pane_index);
            if (index<items.size() && items[index].second.starts_with(L"Folder(")) {
                app.ring_navigation.push_back(app.ring_section);
                app.ring_section=L"ring/"+items[index].second.substr(7,items[index].second.size()-8);
            }
        app.pane_index=-1;app.ring_menu_open=false;
        render(app);
        break;
    }
    case ring_folder_back:
        app.ring_section=app.ring_navigation.empty()?ring_root(app):app.ring_navigation.back();
        if(!app.ring_navigation.empty())app.ring_navigation.pop_back();
        app.pane_index=-1;app.ring_menu_open=false;render(app);break;
    case apply:
        if (app.page==buttons) save_binding(app,false);
        else if (app.page==pointer) flush_parameters(app);
        break;
    case apply_thumb_speed: save_thumb_speed(app); break;
    case remove_profile: save_binding(app,true); break;
    case delete_profile:
        if (app.selected!=L"global" && MessageBoxW(app.window,L"移除当前应用的按键方案？",
                L"应用方案",MB_YESNO|MB_ICONQUESTION)==IDYES &&
            save(app,change_section(app.source,app.selected,{},true))) render(app);
        break;
    case flip_primary:
        SwapMouseButton(GetSystemMetrics(SM_SWAPBUTTON)==0);
        notice(app,L"已切换 Windows 主按钮。");
        render(app);
        break;
    case open_pairing:
        app.pairing_tab_bluetooth=false;
        refresh_pairing_inventory(app);
        navigate(app,pairing);
        break;
    case pairing_tab_bolt:
        app.pairing_tab_bluetooth=false;
        render(app);
        break;
    case pairing_tab_bt:
        app.pairing_tab_bluetooth=true;
        app.bt_devices=pairing::enumerate_bluetooth_devices();
        render(app);
        break;
    case pairing_scan:
        refresh_pairing_inventory(app);
        notice(app,L"已重新扫描 Logi Bolt 接收器通道与 Windows 蓝牙设备。");
        render(app);
        break;
    case pairing_bolt_lock: {
        const bool next_open=!app.bolt_pairing_open;
        bool ok=false;
        if (const auto connected=logi::connect()) {
            if (auto* ch=connected->channel.get())
                ok=pairing::set_receiver_pairing_lock(*ch,next_open,60);
        }
        if (ok) {
            app.bolt_pairing_open=next_open;
            refresh_pairing_inventory(app);
            notice(app,next_open?L"Logi Bolt 接收器配对锁定已开启（60 秒），可长按新设备配对键连接。":L"已关闭 Logi Bolt 接收器配对锁定。");
        } else {
            notice(app,L"未检测到可控制的 Logi Bolt 接收器（若被其他后台占用，请先在「设置」启动设备服务）。");
        }
        render(app);
        break;
    }
    case pairing_open_bt:
        pairing::open_windows_bluetooth_settings();
        break;
    case ring_size_small: case ring_size_medium: case ring_size_large:
        app.ui.ring_size=id==ring_size_small?ui_settings::RingSize::small:id==ring_size_large?ui_settings::RingSize::large:ui_settings::RingSize::medium;
        persist_ui_settings(app);
        notice(app,L"Actions Ring 尺寸已更新。");
        render(app);
        break;
    case settings_theme_system: case settings_theme_dark: case settings_theme_light:
        app.ui.theme=id==settings_theme_system?ui_settings::Theme::system:id==settings_theme_light?ui_settings::Theme::light:ui_settings::Theme::dark;
        apply_ui_theme(app);
        persist_ui_settings(app);
        render(app);
        break;
    case settings_lang_zh: case settings_lang_en:
        app.ui.language=id==settings_lang_en?ui_settings::Language::en_us:ui_settings::Language::zh_cn;
        persist_ui_settings(app);
        render(app);
        break;
    case settings_notify_battery:
        app.ui.notify_low_battery=!app.ui.notify_low_battery;
        persist_ui_settings(app);
        render(app);
        break;
    case settings_notify_locks:
        app.ui.notify_locks=!app.ui.notify_locks;
        persist_ui_settings(app);
        render(app);
        break;
    case settings_notify_mic:
        app.ui.notify_mic=!app.ui.notify_mic;
        persist_ui_settings(app);
        render(app);
        break;
    case settings_notify_test:
        notifications_hud::show(L"LogiNative OSD",
            app.device.battery?(L"电量 "+std::to_wstring(*app.device.battery)+L"% · 状态正常"):L"低电量与锁定键通知已启用",
            notifications_hud::Kind::info,2200,ui_settings::effective_light(app.ui.theme));
        break;
    case settings_check_fw: {
        if (const auto connected=logi::connect()) {
            app.device.serial=connected->device.serial;
            app.device.firmware=connected->device.firmware;
            app.device.receiver_firmware=connected->device.receiver_firmware;
            notice(app,L"已读取硬件版本：设备固件 "+(app.device.firmware.empty()?L"未知":app.device.firmware)+
                (app.device.receiver_firmware.empty()?L"":L" · 接收器 "+app.device.receiver_firmware));
        } else {
            notice(app,L"未检测到直接访问的鼠标通道（可先点击「刷新设备」）。");
        }
        render(app);
        break;
    }
    case settings_reset_defaults:
        if (MessageBoxW(app.window,L"将外观、通知与 Flow 偏好及基础默认配置恢复为出厂默认值？",
                        L"恢复默认设置",MB_YESNO|MB_ICONQUESTION)==IDYES) {
            app.ui=ui_settings::Settings{};
            persist_ui_settings(app);
            apply_ui_theme(app);
            save(app,L"[global]\n0052=MiddleClick\n0053=Back\n0056=Forward\n00C3=AltTab\n00C4=WheelMode\nthumb.speed=100\n");
            render(app);
        }
        break;
    case settings_autostart: {
        const bool next = !ui_settings::is_windows_autostart_enabled();
        if (ui_settings::set_windows_autostart(next, app.directory))
            notice(app, next ? L"已启用开机自动启动后台服务。" : L"已关闭开机自动启动。");
        else if (ui_settings::is_windows_autostart_enabled() == next)
            notice(app, next ? L"已启用登录启动，但未能注册后台恢复任务。" : L"已关闭登录启动，但未能移除后台恢复任务。");
        else
            notice(app, L"写入 Windows 启动项失败。");
        render(app);
        break;
    }
    case settings_close_to_tray:
        app.ui.close_to_tray = !app.ui.close_to_tray;
        persist_ui_settings(app);
        notice(app, app.ui.close_to_tray ? L"关闭窗口时将最小化到系统托盘。" : L"关闭窗口时将直接退出界面。");
        render(app);
        break;
    case settings_import_options_plus: {
        const auto snap = options_plus_import::load_from_db();
        if (!snap.found) {
            notice(app, snap.error.empty() ? L"未检测到原版 Logi Options+ 数据库。" : snap.error);
            break;
        }
        Text summary = L"检测到原版 settings.db（" + std::to_wstring(snap.profile_count) + L" 个配置文件";
        std::optional<unsigned> target_dpi;
        if (snap.pointer_value) {
            target_dpi = options_plus_import::dpi_from_options_plus(*snap.pointer_value, snap.high_res_dpi.value_or(true));
            summary += L"，DPI " + std::to_wstring(*target_dpi);
        }
        if (snap.smartshift_sensitivity) summary += L"，SmartShift " + std::to_wstring(*snap.smartshift_sensitivity) + L"%";
        summary += L"）。\n\n是否应用这些主题、滚轮、SmartShift 与 DPI 偏好到当前 LogiNative？";
        if (MessageBoxW(app.window, summary.c_str(), L"导入原版 Options+ 配置", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            if (snap.use_system_theme) app.ui.theme = *snap.use_system_theme ? ui_settings::Theme::system : ui_settings::Theme::dark;
            if (snap.battery_notify) app.ui.notify_low_battery = *snap.battery_notify;
            persist_ui_settings(app);
            apply_ui_theme(app);
            if (snap.high_res_dpi) save(app, native_dpi::write_mode(app.source, *snap.high_res_dpi));
            Section global_updates;
            if (snap.scroll_smooth) global_updates[L"scroll.smooth"] = *snap.scroll_smooth ? L"1" : L"0";
            if (snap.thumb_smooth) global_updates[L"thumb.smooth"] = *snap.thumb_smooth ? L"1" : L"0";
            if (!global_updates.empty()) save(app, change_section(app.source, L"global", global_updates));
            if (!app.official && !app.device.name.empty()) {
                if (target_dpi && !app.device.dpi.empty()) {
                    const unsigned snapped = native_dpi::supported(*target_dpi, app.device.dpi);
                    queue_parameter(app, 0, snapped, true);
                }
                if (snap.scroll_inverted) queue_parameter(app, 4, *snap.scroll_inverted ? 1 : 0, true);
                if (snap.thumb_inverted) queue_parameter(app, 5, *snap.thumb_inverted ? 1 : 0, true);
                if (snap.smartshift_enabled) {
                    queue_parameter(app, 2, snap.smartshift_ratchet.value_or(true) ? 1 : 0, true);
                    queue_parameter(app, 1, *snap.smartshift_enabled ? native_smartshift::threshold(snap.smartshift_sensitivity.value_or(80)) : 0, true);
                }
            }
            notice(app, L"已从原版 Options+ settings.db 同步偏好配置。");
            render(app);
        }
        break;
    }
    case pointer_precision:
        set_windows_pointer_precision(!windows_pointer_precision());
        notice(app, windows_pointer_precision() ? L"已开启 Windows 提高指针精确度。" : L"已关闭 Windows 提高指针精确度。");
        render(app);
        break;
    case pointer_dpi_800: case pointer_dpi_1600: case pointer_dpi_3200: case pointer_dpi_5000: {
        const unsigned target = pointer_presets[id - pointer_dpi_800];
        if (target > 4000 && !extended_dpi(app))
            save(app, native_dpi::write_mode(app.source, true));
        const unsigned snapped = app.device.dpi.empty() ? target : native_dpi::supported(target, app.device.dpi);
        queue_parameter(app, 0, snapped);
        render(app);
        break;
    }
    case tray_open:
        restore_from_tray(app);
        break;
    case tray_refresh:
        click(app, reload);
        break;
    case tray_host_1: case tray_host_2: case tray_host_3:
        io(app, {}, static_cast<unsigned>(id - tray_host_1 + 1));
        break;
    case tray_toggle_flow:
        app.ui.flow_enabled = !app.ui.flow_enabled;
        persist_ui_settings(app);
        if (app.page == flow) render(app);
        break;
    case tray_toggle_trial:
        click(app, app.native ? end : begin);
        break;
    case tray_exit:
        app.force_exit = true;
        PostMessageW(app.window, WM_CLOSE, 0, 0);
        break;
    case flow_toggle_enabled:
        read_flow_edits(app);
        app.ui.flow_enabled=!app.ui.flow_enabled;
        persist_ui_settings(app);
        render(app);
        break;
    case flow_toggle_clip:
        read_flow_edits(app);
        app.ui.flow_share_clipboard=!app.ui.flow_share_clipboard;
        persist_ui_settings(app);
        render(app);
        break;
    case flow_toggle_ctrl:
        read_flow_edits(app);
        app.ui.flow_hold_ctrl=!app.ui.flow_hold_ctrl;
        persist_ui_settings(app);
        render(app);
        break;
    case flow_toggle_corners:
        read_flow_edits(app);
        app.ui.flow_edge_corners=!app.ui.flow_edge_corners;
        persist_ui_settings(app);
        render(app);
        break;
    case flow_edge_left: case flow_edge_right: case flow_edge_top: case flow_edge_bottom: {
        read_flow_edits(app);
        if (app.ui.flow_peers.empty()) app.ui.flow_peers.push_back({L"peer-1",L"Peer-PC",L"",app.ui.flow_port,ui_settings::FlowEdge::right,2,1920,1080});
        auto& peer=app.ui.flow_peers[app.selected_flow_peer];
        peer.edge=id==flow_edge_left?ui_settings::FlowEdge::left:id==flow_edge_top?ui_settings::FlowEdge::top:id==flow_edge_bottom?ui_settings::FlowEdge::bottom:ui_settings::FlowEdge::right;
        persist_ui_settings(app);
        render(app);
        break;
    }
    case flow_slot_1: case flow_slot_2: case flow_slot_3: {
        read_flow_edits(app);
        if (app.ui.flow_peers.empty()) app.ui.flow_peers.push_back({L"peer-1",L"Peer-PC",L"",app.ui.flow_port,ui_settings::FlowEdge::right,2,1920,1080});
        app.ui.flow_peers[app.selected_flow_peer].host_slot=static_cast<unsigned>(id-flow_slot_1+1);
        persist_ui_settings(app);
        render(app);
        break;
    }
    case flow_discover: {
        read_flow_edits(app);
        flow::Controller scanner;
        const auto pin=to_utf8(app.ui.flow_pin.empty()?L"LOGI2026":app.ui.flow_pin);
        if (scanner.start(0,pin,"scan-ui",to_utf8(local_computer_name()),1920,1080,{},true)) {
            scanner.broadcast_presence();
            Sleep(260);
            app.discovered_peers=scanner.discovered_peers();
            scanner.stop();
        }
        notice(app,app.discovered_peers.empty()?L"未发现局域网中正在监听 UDP 59836 的其他 LogiNative 节点；请手动输入对端 IP。":
               L"已扫描到 "+std::to_wstring(app.discovered_peers.size())+L" 个局域网 Flow 节点。");
        render(app);
        break;
    }
    case flow_save_peer:
        read_flow_edits(app);
        persist_ui_settings(app);
        notice(app,L"Flow 节点配置已保存（运行中的后台将自动热加载）。");
        render(app);
        break;
    case flow_remove_peer:
        if (!app.ui.flow_peers.empty() && app.selected_flow_peer<app.ui.flow_peers.size()) {
            app.ui.flow_peers.erase(app.ui.flow_peers.begin()+static_cast<std::ptrdiff_t>(app.selected_flow_peer));
            app.selected_flow_peer=0;
            persist_ui_settings(app);
            notice(app,L"已移除选中的 Flow 节点。");
        }
        render(app);
        break;
    case flow_ping_peer: {
        read_flow_edits(app);
        if (app.ui.flow_peers.empty() || app.ui.flow_peers[app.selected_flow_peer].host.empty()) {
            notice(app,L"请先填写目标电脑 IP 或主机名。");break;
        }
        const auto& peer=app.ui.flow_peers[app.selected_flow_peer];
        flow::Controller probe;
        const bool ok=probe.ping_peer(peer.host,peer.port?peer.port:app.ui.flow_port,
                                      to_utf8(app.ui.flow_pin.empty()?L"LOGI2026":app.ui.flow_pin),900);
        notice(app,ok?(L"节点连通成功："+peer.host+L":"+std::to_wstring(peer.port?peer.port:app.ui.flow_port)):
                      (L"无法连接到 "+peer.host+L"（请确认对端已启动 LogiNative 后台且共享 PIN 一致）。"));
        break;
    }
    case flow_loopback_test:
        read_flow_edits(app);
        notice(app,run_flow_loopback_test(app.ui));
        break;
    case flow_reset_all:
        app.ui.flow_peers.clear();
        app.ui.flow_enabled=false;
        app.discovered_peers.clear();
        app.selected_flow_peer=0;
        persist_ui_settings(app);
        notice(app,L"已重置 Flow 节点和规则配置。");
        render(app);
        break;
    case action_param_browse: {
        wchar_t path[4096]{};
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=app.window;
        dialog.lpstrFilter=L"应用程序与文件\0*.*\0\0";dialog.lpstrFile=path;dialog.nMaxFile=std::size(path);
        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if (lazy_win32::get_open_file_name(&dialog) && app.action_param_box) SetWindowTextW(app.action_param_box,path);
        break;
    }
    case action_param_apply: {
        if (app.page!=buttons || app.pane_index<0 || !app.action_param_box) break;
        const Text key=Text(button_points[app.pane_index].key);
        const Text cur=get(app,key);
        const Text param=Text(trim(control_text(app.action_param_box)));
        Text next_val;
        if (cur.starts_with(L"Open(")) next_val=L"Open("+param+L")";
        else if (cur.starts_with(L"DpiToggle(")) next_val=L"DpiToggle("+(param.empty()?L"1000,5000":param)+L")";
        else if (cur.starts_with(L"AdvancedClick(")) next_val=L"AdvancedClick("+(param.empty()?L"Single,0":param)+L")";
        if (!next_val.empty() && save(app,change_section(app.source,app.selected,{{key,next_val}})))
            render(app);
        break;
    }
    default:
        if (id>=flow_peer_first && id<flow_peer_first+50) {
            read_flow_edits(app);
            const auto idx=static_cast<std::size_t>(id-flow_peer_first);
            if (idx<app.ui.flow_peers.size()) app.selected_flow_peer=idx;
            render(app);
        } else if (id>=flow_peer_first+50 && id<flow_peer_first+100) {
            const auto idx=static_cast<std::size_t>(id-(flow_peer_first+50));
            if (idx<app.discovered_peers.size()) {
                const auto& d=app.discovered_peers[idx];
                if (app.ui.flow_peers.empty()) app.ui.flow_peers.push_back({d.id,d.name,d.host,d.port,ui_settings::FlowEdge::right,2,d.screen_width,d.screen_height});
                else {
                    auto& peer=app.ui.flow_peers[app.selected_flow_peer];
                    peer.id=d.id;peer.name=d.name;peer.host=d.host;peer.port=d.port;
                    peer.screen_width=d.screen_width;peer.screen_height=d.screen_height;
                }
                persist_ui_settings(app);
                render(app);
            }
        }
        break;
    }
}

Gdiplus::Color ink(COLORREF color) { return {255,GetRValue(color),GetGValue(color),GetBValue(color)}; }
void rounded(Gdiplus::Graphics& graphics,int x,int y,int w,int h,int radius,
             COLORREF fill,std::optional<COLORREF> border={}) {
    const int diameter=std::min({px(radius)*2,w,h});
    if (diameter<=0) return;
    Gdiplus::GraphicsPath path;
    path.AddArc(x,y,diameter,diameter,180,90);
    path.AddArc(x+w-diameter,y,diameter,diameter,270,90);
    path.AddArc(x+w-diameter,y+h-diameter,diameter,diameter,0,90);
    path.AddArc(x,y+h-diameter,diameter,diameter,90,90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(ink(fill));
    graphics.FillPath(&brush,&path);
    if (border) {
        Gdiplus::Pen pen(ink(*border));
        graphics.DrawPath(&pen,&path);
    }
}
LRESULT CALLBACK sliding_toggle(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    constexpr auto pressed=L"LogiNative.SwitchPress";
    constexpr auto dragged=L"LogiNative.SwitchDrag";
    if (message==WM_PAINT) {
        PAINTSTRUCT ps{};const auto dc=BeginPaint(window,&ps);
        RECT box{};GetClientRect(window,&box);
        const bool enabled=IsWindowEnabled(window),on=SendMessageW(window,BM_GETCHECK,0,0)==BST_CHECKED;
        {
            Gdiplus::Graphics graphics(dc);
            graphics.Clear(ink(GetPropW(window, L"LogiNative.OnBackground") ? background : panel));
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const int x=box.right-px(28),y=(box.bottom-px(16))/2;
            rounded(graphics,x,y,px(28),px(16),8,enabled?(on?teal:RGB(102,102,102)):RGB(51,51,51));
            Gdiplus::SolidBrush thumb(ink(panel));
            graphics.FillEllipse(&thumb,x+px(on?14:2),y+px(2),px(12),px(12));
        }
        Text title(GetWindowTextLengthW(window)+1,L'\0');
        GetWindowTextW(window,title.data(),static_cast<int>(title.size()));title.pop_back();
        text_at(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),enabled?white:muted,
                title,{0,0,box.right-px(60),box.bottom},DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        if (GetFocus()==window && !(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) {
            RECT focus{box.right-px(32),1,box.right,box.bottom-1};DrawFocusRect(dc,&focus);
        }
        EndPaint(window,&ps);return 0;
    }
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_LBUTTONDOWN && IsWindowEnabled(window)) {
        SendMessageW(GetParent(window),WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
        SetFocus(window);SetCapture(window);
        SetPropW(window,pressed,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(GET_X_LPARAM(lparam)+1)));
        return 0;
    }
    if (message==WM_MOUSEMOVE && GetPropW(window,pressed)) {
        RECT box{};GetClientRect(window,&box);
        const auto start=static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(window,pressed)))-1;
        if (start>=box.right-px(38) && std::abs(GET_X_LPARAM(lparam)-start)>px(3)) {
            SetPropW(window,dragged,reinterpret_cast<HANDLE>(1));
            SendMessageW(window,BM_SETCHECK,GET_X_LPARAM(lparam)>=box.right-px(14)?BST_CHECKED:BST_UNCHECKED,0);
        }
        return 0;
    }
    if (message==WM_LBUTTONUP && GetPropW(window,pressed)) {
        const bool moved=GetPropW(window,dragged)!=nullptr;
        RemovePropW(window,pressed);RemovePropW(window,dragged);ReleaseCapture();
        RECT box{};GetClientRect(window,&box);
        if (POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; moved || PtInRect(&box,point)) {
            if (!moved) SendMessageW(window,BM_SETCHECK,SendMessageW(window,BM_GETCHECK,0,0)==BST_CHECKED?BST_UNCHECKED:BST_CHECKED,0);
            SendMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),BN_CLICKED),reinterpret_cast<LPARAM>(window));
        }
        return 0;
    }
    if (message==WM_CAPTURECHANGED || message==WM_NCDESTROY) {
        RemovePropW(window,pressed);RemovePropW(window,dragged);
        if (message==WM_NCDESTROY) lazy_win32::remove_window_subclass(window,sliding_toggle,id);
    }
    const auto result=lazy_win32::def_subclass_proc(window,message,wparam,lparam);
    if (message==BM_SETCHECK || message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_ENABLE || message==WM_UPDATEUISTATE)
        InvalidateRect(window,nullptr,FALSE);
    return result;
}
void text_at(HDC dc,HFONT font,COLORREF color,std::wstring_view value,RECT rect,UINT flags) {
    native_typography::drawText(dc,font,color,value,rect,flags,ring_typography);
}
bool in_pane(const App& app,HWND child) {
    if (app.pane_index<0 && app.page!=ring) return false;
    RECT rect{};
    GetWindowRect(child,&rect);
    MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&rect),2);
    if (app.page==ring) {
        if(app.ring_creation_stage==2&&rect.left>=px(457)&&rect.right<=px(1037)&&rect.top>=px(162)&&rect.bottom<=px(700))return true;
        if(app.ring_profile_edit&&rect.left>=px(310)&&rect.right<=px(710)&&rect.top>=px(265)&&rect.bottom<=px(445))return true;
        if(app.ring_settings_open&&rect.left>=px(650)&&rect.right<=px(969)&&rect.top>=px(604)&&rect.bottom<=px(752))return true;
        const auto& box=app.ring_editor;
        return (rect.left>=px(pane_x) && rect.top<px(100)) ||
            (app.pane_index>=0 && rect.left>=px(box.left) && rect.right<=px(box.right) &&
             rect.top>=px(box.top) && rect.bottom<=px(box.bottom));
    }
    return rect.left>=px(pane_x) && rect.top>=px(pane_header);
}
void draw_button(App& app, const DRAWITEMSTRUCT& item) {
    const auto found=app.button_skins.find(item.hwndItem);
    if (found==app.button_skins.end()) return;
    const auto& skin=found->second;
    const auto dc=item.hDC;
    const auto bounds=item.rcItem;
    const int w=bounds.right,h=bounds.bottom;
    const bool hover=GetPropW(item.hwndItem,L"LogiNative.Hover") || (item.itemState&ODS_SELECTED);
    const bool disabled=item.itemState&ODS_DISABLED;
    const auto title=skin.caption.empty()?control_text(item.hwndItem):skin.caption;
    const auto fg=disabled?RGB(70,70,70):skin.active?panel:hover?teal:white;
    constexpr UINT centered=DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS;
    constexpr UINT centered_left=DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS;
    FillRect(dc,&bounds,in_pane(app,item.hwndItem)||skin.look==Look::slot?app.panel_brush:app.background_brush);
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto icon=[&](std::wstring_view glyph,int x,int y,int size,COLORREF color) {
            if (glyph==L"stop") {draw_ring_icon(graphics,glyph,x,y,px(size),color);return;}
            const wchar_t symbol=glyph==L"add"?0xE903:glyph==L"global"?0xE953:
                glyph==L"close"?0xE937:glyph==L"back"?0xE907:glyph==L"forward"?0xE908:
                glyph==L"buttons"?0xE926:glyph==L"settings"?0xE979:0;
            if (symbol && app.symbols_loaded && app.symbols) {
                Gdiplus::Font face(L"5thAug2026",static_cast<float>(px(size)),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel,app.symbols.get());
                Gdiplus::SolidBrush brush(ink(color));
                Gdiplus::StringFormat format;
                format.SetAlignment(Gdiplus::StringAlignmentCenter);
                format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                graphics.DrawString(&symbol,1,&face,Gdiplus::RectF(x,y,px(size),px(size)),&format,&brush);
                return;
            }
            draw_native_icon(graphics,glyph,x,y,px(size),color);
        };
        const auto center_icon=[&](int size,COLORREF color) {
            icon(skin.icon,(w-px(size))/2,(h-px(size))/2,size,color);
        };
        const auto divider=[&](int y,COLORREF color) {
            Gdiplus::Pen pen(ink(color));
            graphics.DrawLine(&pen,0,y,w,y);
        };
        switch (skin.look) {
        case Look::ring_creation_card:
            rounded(graphics,1,1,w-2,h-2,8,hover?raised:panel,hover?std::optional<COLORREF>{teal}:std::optional<COLORREF>{line_color});
            icon(skin.icon,(w-px(64))/2,px(39),64,white);break;
        case Look::ring_application_card:
            if(hover)rounded(graphics,1,1,w-2,h-2,4,raised);
            if(skin.application_icon)DrawIconEx(dc,(w-px(40))/2,px(4),skin.application_icon,px(40),px(40),0,nullptr,DI_NORMAL);else icon(L"buttons",(w-px(40))/2,px(4),40,white);break;
        case Look::filled:
        case Look::tab:
            if (skin.active || hover || skin.look==Look::filled)
                rounded(graphics,1,1,w-2,h-2,skin.look==Look::tab?4:8,
                        skin.active?teal:hover?RGB(49,49,49):raised);
            if (skin.look==Look::tab) icon(skin.icon,px(11),(h-px(24))/2,24,fg);
            break;
        case Look::glyph:
            if (hover) rounded(graphics,1,1,w-2,h-2,6,raised);
            center_icon(item.CtlID==minimize_ui || item.CtlID==close_ui?20:item.CtlID==delete_profile?10:item.CtlID==new_profile?30:23,fg);
            break;
        case Look::tool:
            rounded(graphics,1,1,w-2,h-2,3,skin.active?teal:hover?raised:panel,line_color);
            if (w<px(80)) center_icon(w>=px(55)?38:22,fg);
            else icon(skin.icon,px(20),(h-px(22))/2,22,fg);
            break;
        case Look::profile: {
            if (hover) rounded(graphics,0,0,w,h-px(6),6,raised);
            if (skin.original_icon) {
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                graphics.DrawImage(skin.original_icon,(w-px(24))/2,(h-px(24))/2,px(24),px(24));
            } else if (skin.application_icon)
                DrawIconEx(dc,(w-px(24))/2,(h-px(24))/2,skin.application_icon,px(24),px(24),0,nullptr,DI_NORMAL);
            else center_icon(app.page==ring?18:28,skin.active||hover?teal:white);
            if (skin.active) {
                Gdiplus::Pen pen(ink(teal),px(2));
                graphics.DrawLine(&pen,(w-px(32))/2,h-px(1),(w+px(32))/2,h-px(1));
            }
            break;
        }
        case Look::hotspot:
            rounded(graphics,1,1,w-2,h-2,3,skin.active?teal:hover?RGB(39,39,39):RGB(21,21,21),
                    skin.active?teal:RGB(42,42,42));
            break;
        case Look::gesture_row:
            rounded(graphics,0,0,w,h,2,hover?raised:background,line_color);
            break;
        case Look::gesture_header: {
            rounded(graphics,0,0,w,h,6,teal);
            Gdiplus::SolidBrush outer(ink(panel)),inner(ink(teal));
            graphics.FillEllipse(&outer,px(16),(h-px(18))/2,px(18),px(18));
            graphics.FillEllipse(&inner,px(23),(h-px(4))/2,px(4),px(4));
            break;
        }
        case Look::gesture_picker:
            rounded(graphics,0,0,w,h,7,hover?raised:panel,line_color);
            icon(L"chevron",w-px(27),(h-px(14))/2,14,white);
            break;
        case Look::device:
            if (auto* scaled=app.front.scaled(w,h)) {
                graphics.DrawImage(scaled,0,0,w,h);
            }
            break;
        case Look::slot:
            divider(0,line_color);
            rounded(graphics,px(29),px(23),px(34),px(34),17,skin.active?teal:panel,
                    skin.active?teal:line_color);
            break;
        case Look::section:
            if (app.page==ring) rounded(graphics,1,1,w-2,h-2,4,panel,line_color);
            else divider(0,line_color);
            icon(skin.icon,w-px(47),(h-px(18))/2,18,fg);
            break;
        case Look::badge:
            rounded(graphics,1,1,w-2,h-2,3,panel,line_color);
            icon(L"battery",px(53),(h-px(19))/2,19,green);
            icon(L"bolt",px(77),(h-px(17))/2,17,white);
            break;
        case Look::switch_toggle: {
            const int y=(h-px(16))/2;
            rounded(graphics,px(4),y,px(28),px(16),8,skin.active?teal:panel);
            Gdiplus::SolidBrush thumb(ink(skin.active?panel:RGB(105,105,105)));
            graphics.FillEllipse(&thumb,px(skin.active?19:6),y+px(2),px(12),px(12));
            break;
        }
        case Look::ring_slot:
            rounded(graphics,1,1,w-2,h-2,21,w<px(36)?raised:hover?raised:panel,
                    skin.active?std::optional<COLORREF>{teal}:std::nullopt);
            if(skin.ring_icon){const auto size=static_cast<float>(std::min(w,h)-2);draw_ring_visual(graphics,*skin.ring_icon,skin.original_icon,(w-size)/2,(h-size)/2,size,&app.ring_fonts);}
            else if (!skin.icon.empty()) center_icon(w<px(36)?10:22,white);
            break;
        case Look::ring_tag:
            rounded(graphics,1,1,w-2,h-2,2,w<px(32)?white:skin.active?teal:hover?raised:panel,line_color);
            break;
        case Look::ring_menu_item:
            rounded(graphics,0,0,w,h,3,hover&&!disabled?raised:panel,
                    item.itemState&ODS_FOCUS?std::optional<COLORREF>{teal}:std::nullopt);
            break;
        case Look::radio: {
            Gdiplus::SolidBrush outer(ink(skin.active?teal:RGB(51,51,51))),inner(ink(panel));
            const int x=px(1),y=(h-px(18))/2;
            graphics.FillEllipse(&outer,x,y,px(18),px(18));
            if(skin.active)graphics.FillEllipse(&inner,x+px(6),y+px(6),px(6),px(6));
            break;
        }
        case Look::plain: break;
        }
    }
    switch (skin.look) {
    case Look::ring_creation_card:
        text_at(dc,app.subheading,white,title,{px(12),px(136),w-px(12),px(180)},centered);
        text_at(dc,app.caption,muted,skin.secondary,{px(18),px(190),w-px(18),h-px(18)},DT_CENTER|DT_WORDBREAK);break;
    case Look::ring_application_card:text_at(dc,app.small,white,title,{px(4),px(50),w-px(4),h},DT_CENTER|DT_WORDBREAK|DT_END_ELLIPSIS);break;
    case Look::tool:
        if (w>=px(80)) text_at(dc,app.caption,fg,title,{px(54),0,w-px(10),h},centered_left);
        break;
    case Look::filled:
    case Look::plain:
        text_at(dc,app.bold,skin.look==Look::plain?teal:fg,title,bounds,centered);
        break;
    case Look::tab:
        text_at(dc,app.small,fg,title,{px(53),0,w-px(5),h},centered_left);
        break;
    case Look::profile:
        break;
    case Look::hotspot: {
        const int title_y=10;
        const int subtitle_y=title_y+text_line_height(app,app.bold)*skin.title_lines+3;
        text_at(dc,app.bold,skin.active?panel:white,title,
                {px(16),px(title_y),w-px(6),px(subtitle_y)},DT_WORDBREAK);
        text_at(dc,app.small,skin.active?panel:white,skin.secondary,
                {px(16),px(subtitle_y),w-px(5),h-px(4)},DT_WORDBREAK|DT_END_ELLIPSIS);
        break;
    }
    case Look::slot:
        text_at(dc,app.bold,skin.active?panel:muted,std::to_wstring(item.CtlID-hosts_first),
                {px(29),px(23),px(63),px(57)},centered);
        text_at(dc,app.bold,skin.active?white:muted,title,
                {px(92),skin.secondary.empty()?0:px(13),w-px(20),skin.secondary.empty()?h:px(49)},centered_left);
        if (!skin.secondary.empty())
            text_at(dc,app.small,muted,skin.secondary,{px(92),px(47),w-px(20),px(69)},centered_left);
        break;
    case Look::gesture_row: {
        constexpr const wchar_t* arrows[]{L"○",L"←",L"→",L"↑",L"↓"};
        text_at(dc,app.bold,white,arrows[item.CtlID-gesture_rows_first],{px(15),0,px(44),h},centered);
        text_at(dc,app.caption,white,title,{px(53),px(8),w-px(12),px(27)},centered_left);
        text_at(dc,app.bold,white,skin.secondary,{px(53),px(27),w-px(12),h-px(6)},centered_left);
        break;
    }
    case Look::gesture_header:
        text_at(dc,app.bold,panel,title,{px(48),0,w-px(12),h},centered_left);
        break;
    case Look::gesture_picker:
        text_at(dc,app.bold,white,title,{px(19),0,w-px(39),h},centered_left);
        break;
    case Look::section:
        text_at(dc,app.bold,fg,title,{px(37),0,w-px(55),h},centered_left);
        break;
    case Look::badge:
        text_at(dc,app.caption,green,title,{px(11),0,px(54),h},centered_left);
        break;
    case Look::ring_slot:
        if (skin.icon.empty()&&!skin.ring_icon) text_at(dc,app.bold,fg,title,bounds,centered);
        break;
    case Look::ring_tag:
        text_at(dc,app.caption,w<px(32)?panel:fg,title,{w<px(32)?0:px(8),0,w-(w<px(32)?0:px(8)),h},centered);
        break;
    case Look::ring_menu_item:
        if(item.CtlID>=ring_profile_first&&item.CtlID<ring_profile_first+app.ring_profile_sections.size()) {
            if(skin.active)text_at(dc,app.caption,teal,L"✓",{px(12),0,px(40),h},centered);
            text_at(dc,app.caption,white,title,{px(44),0,w-px(8),h},centered_left);break;
        }
        text_at(dc,app.caption,disabled?muted:white,title,{w<px(80)?0:px(16),0,w-px(8),h},
                w<px(80)?centered:centered_left);
        break;
    case Look::radio:
        text_at(dc,app.normal,hover?teal:white,title,{px(34),0,w-px(8),h},centered_left);
        break;
    case Look::device:
    case Look::switch_toggle:
    case Look::glyph: break;
    }
    if (skin.look!=Look::ring_menu_item && (item.itemState&(ODS_FOCUS|ODS_NOFOCUSRECT))==ODS_FOCUS) {
        auto focus=bounds;
        InflateRect(&focus,-px(3),-px(3));
        DrawFocusRect(dc,&focus);
    }
}
void draw_ring_row(App& app,const DRAWITEMSTRUCT& item) {
    const auto b=item.rcItem;
    FillRect(item.hDC,&b,app.background_brush);
    const bool exists=item.itemID<app.ring_rows.size();
    const auto row=exists?app.ring_rows[item.itemID]:RingRow{L"没有匹配的操作",0,false};
    const bool category=row.action<0;
    const bool selected=(item.itemState&ODS_SELECTED) && !category;
    const auto color=category && row.expanded?panel:white;
    {
        Gdiplus::Graphics graphics(item.hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (!category) {
            Gdiplus::SolidBrush group(ink(raised));
            graphics.FillRectangle(&group,static_cast<int>(b.left),static_cast<int>(b.top),
                                   static_cast<int>(b.right-b.left-px(12)),static_cast<int>(b.bottom-b.top));
        }
        const int inset=category?1:px(8);
        rounded(graphics,b.left+inset,b.top+px(2),b.right-b.left-inset-px(13),b.bottom-b.top-px(8),4,
                category&&row.expanded?RGB(200,250,245):panel,selected?teal:line_color);
        if (category) {
            Gdiplus::Pen pen(ink(color),px(1.5));
            const int x=b.left+px(19),y=b.top+px(24);
            if (row.expanded) {
                graphics.DrawLine(&pen,x-px(3),y+px(2),x,y-px(2));
                graphics.DrawLine(&pen,x,y-px(2),x+px(3),y+px(2));
            } else {
                graphics.DrawLine(&pen,x-px(3),y-px(2),x,y+px(2));
                graphics.DrawLine(&pen,x,y+px(2),x+px(3),y-px(2));
            }
        } else if (exists) {
            draw_ring_icon(graphics,ring_glyph(ring_library_action(app,row.action).code),
                             b.left+px(21),b.top+px(15),px(17),white);
        }
    }
    text_at(item.hDC,app.caption,color,row.title,
            {b.left+px(category?43:49),b.top,b.right-px(23),b.bottom-px(3)},DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
}
void draw_action_row(App& app,const DRAWITEMSTRUCT& item) {
    const auto bounds=item.rcItem;
    const auto dc=item.hDC;
    FillRect(dc,&bounds,app.panel_brush);
    if (item.itemID>=app.visible.size()) return;
    const auto selected=(item.itemState&ODS_SELECTED)!=0;
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (selected) rounded(graphics,bounds.left+1,bounds.top+1,bounds.right-bounds.left-2,
                              bounds.bottom-bounds.top-2,4,teal);
        const int x=bounds.left+px(16),y=bounds.top+px(10);
        Gdiplus::SolidBrush radio(ink(selected?panel:raised));
        graphics.FillEllipse(&radio,x,y,px(18),px(18));
        if (selected) {
            Gdiplus::SolidBrush dot(ink(teal));
            graphics.FillEllipse(&dot,x+px(6),y+px(6),px(6),px(6));
        }
    }
    text_at(dc,app.bold,selected?panel:white,app.visible[item.itemID].name,
            {bounds.left+px(47),bounds.top,bounds.right-px(8),bounds.bottom},
            DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
}
LRESULT draw_slider(const App& app,const NMCUSTOMDRAW& draw) {
    if (draw.dwDrawStage!=CDDS_PREPAINT) return CDRF_DODEFAULT;
    RECT bounds{};
    GetClientRect(app.slider,&bounds);
    FillRect(draw.hdc,&bounds,app.panel_brush);
    const int y=bounds.bottom/2;
    const int left=px(10), right=bounds.right-px(10);
    const auto low=SendMessageW(app.slider,TBM_GETRANGEMIN,0,0);
    const auto high=SendMessageW(app.slider,TBM_GETRANGEMAX,0,0);
    const auto value=SendMessageW(app.slider,TBM_GETPOS,0,0);
    const int mid=left+(high>low?MulDiv(right-left,value-low,high-low):0);
    const COLORREF accent=IsWindowEnabled(app.slider)?teal:muted;
    Gdiplus::Graphics graphics(draw.hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen track(ink(line_color),px(4)), filled(ink(accent),px(4));
    track.SetStartCap(Gdiplus::LineCapRound);track.SetEndCap(Gdiplus::LineCapRound);
    filled.SetStartCap(Gdiplus::LineCapRound);filled.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush center(ink(accent)),hole(ink(panel));
    graphics.DrawLine(&track,left,y,right,y);
    graphics.DrawLine(&filled,left,y,mid,y);
    graphics.FillEllipse(&center,mid-px(8),y-px(8),px(16),px(16));
    graphics.FillEllipse(&hole,mid-px(3),y-px(3),px(6),px(6));
    return CDRF_SKIPDEFAULT;
}
void paint_scene(App& app,HDC dc,const RECT& bounds) {
    ring_typography=app.page==ring;
    FillRect(dc,&bounds,app.background_brush);
    if ((app.pane_index>=0 || app.animating) && app.page!=ring) {
        RECT right{px(app.visual_pane),px(pane_header),bounds.right,bounds.bottom};
        FillRect(dc,&right,app.panel_brush);
    }
    if (app.page==ring&&!app.ring_creation_stage) {
        RECT bar{px(pane_x),0,bounds.right,px(100)};
        FillRect(dc,&bar,app.panel_brush);
        Gdiplus::Graphics draw(dc);
        Gdiplus::Pen divider(ink(line_color));
        draw.DrawLine(&divider,px(pane_x),0,px(pane_x),bounds.bottom);
        if(app.ring_profile_edit)rounded(draw,px(310),px(265),px(400),px(180),4,panel);
        if(app.ring_settings_open)rounded(draw,px(650),px(604),px(319),px(148),4,panel,line_color);
        if (app.pane_index>=0) {
            const auto& box=app.ring_editor;
            rounded(draw,px(box.left-1),px(box.top-1),px(box.right-box.left+2),px(box.bottom-box.top+2),4,panel);
            draw.DrawLine(&divider,px(box.left),px(box.top+40),px(box.right),px(box.top+40));
        }
    }
    if(app.page==ring&&app.ring_creation_stage==2){RECT box{px(457),px(162),px(1037),px(700)};FillRect(dc,&box,app.panel_brush);}
    if(app.page==ring&&app.ring_creation_stage==3){Gdiplus::Graphics graphics(dc);rounded(graphics,px(100),px(224),px(520),px(452),8,panel);Gdiplus::Pen outline(ink(line_color));graphics.DrawEllipse(&outline,px(865),px(270),px(280),px(280));}
    if (app.page==buttons && app.pane_index>=0) {
        Gdiplus::Graphics draw(dc);
        draw.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        rounded(draw,px(1011),px(96),px(466),px(48),4,raised);
        draw_native_icon(draw,L"search",px(1442),px(109),px(21),muted);
        Gdiplus::Pen divider(ink(line_color));
        draw.DrawLine(&divider,px(pane_x),px(150),px(client_width),px(150));
    }
    auto* picture=app.page==easy?&app.bottom:
                  (app.page==buttons||app.page==pointer)?&app.side:nullptr;
    if (picture && picture->ensure() && picture->image->GetLastStatus()==Gdiplus::Ok) {
        Gdiplus::Graphics draw(dc);
        draw.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto area=image_area(app);
        if (app.page!=easy) {
            Gdiplus::GraphicsPath shadow;
            shadow.AddEllipse(px(area.left+22),px(area.top+88),px(352),px(484));
            Gdiplus::PathGradientBrush shade(&shadow);
            shade.SetCenterPoint(Gdiplus::PointF(px(area.left+198),px(area.top+334)));
            shade.SetCenterColor(Gdiplus::Color(248,0,0,0));
            Gdiplus::Color transparent(0,0,0,0);
            INT count=1;
            shade.SetSurroundColors(&transparent,&count);
            draw.FillPath(&shade,&shadow);
        }
        if (auto* scaled=picture->scaled(px(area.right-area.left),px(area.bottom-area.top)))
            draw.DrawImage(scaled,px(area.left),px(area.top),px(area.right-area.left),px(area.bottom-area.top));
        if (app.page==buttons || app.page==pointer) {
            const auto items=page_points(app.page);
            Gdiplus::SolidBrush dot(Gdiplus::Color(105,0,0,0));
            for (unsigned i=0;i<items.size();++i) {
                const auto position=pin_position(area,items[i]);
                const int x=px(position.x), y=px(position.y);
                Gdiplus::Pen edge(ink(app.pane_index==static_cast<int>(i)?teal:white),px(2));
                draw.FillEllipse(&dot,x-px(7),y-px(7),px(14),px(14));
                draw.DrawEllipse(&edge,x-px(7),y-px(7),px(14),px(14));
            }
        }
    }
    if (app.page==flow) {
        Gdiplus::Graphics draw(dc);
        draw.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        rounded(draw,px(280),px(168),px(540),px(270),10,panel,line_color);
        const ui_settings::FlowPeerConfig* peer =
            (!app.ui.flow_peers.empty() && app.selected_flow_peer < app.ui.flow_peers.size())
            ? &app.ui.flow_peers[app.selected_flow_peer] : nullptr;
        const auto edge = peer ? peer->edge : ui_settings::FlowEdge::right;
        RECT local_box{px(330),px(244),px(510),px(364)};
        RECT peer_box{px(590),px(244),px(770),px(364)};
        if (edge == ui_settings::FlowEdge::left) std::swap(local_box, peer_box);
        else if (edge == ui_settings::FlowEdge::top) {
            local_box = {px(440),px(314),px(660),px(414)};
            peer_box = {px(440),px(190),px(660),px(286)};
        } else if (edge == ui_settings::FlowEdge::bottom) {
            local_box = {px(440),px(190),px(660),px(286)};
            peer_box = {px(440),px(314),px(660),px(414)};
        }
        rounded(draw,local_box.left,local_box.top,local_box.right-local_box.left,local_box.bottom-local_box.top,8,raised,teal);
        const auto p_border = (peer && !peer->host.empty()) ? (app.ui.flow_enabled ? teal : white) : line_color;
        rounded(draw,peer_box.left,peer_box.top,peer_box.right-peer_box.left,peer_box.bottom-peer_box.top,8,raised,p_border);
        const int lx=(local_box.left+local_box.right)/2, ly=(local_box.top+local_box.bottom)/2;
        const int rx=(peer_box.left+peer_box.right)/2, ry=(peer_box.top+peer_box.bottom)/2;
        Gdiplus::Pen link_pen(ink(app.ui.flow_enabled ? teal : muted), px(2));
        link_pen.SetDashStyle(Gdiplus::DashStyleDash);
        if (edge==ui_settings::FlowEdge::left || edge==ui_settings::FlowEdge::right)
            draw.DrawLine(&link_pen, edge==ui_settings::FlowEdge::right?local_box.right:peer_box.right, ly,
                                     edge==ui_settings::FlowEdge::right?peer_box.left:local_box.left, ry);
        else
            draw.DrawLine(&link_pen, lx, edge==ui_settings::FlowEdge::bottom?local_box.bottom:peer_box.bottom,
                                     rx, edge==ui_settings::FlowEdge::bottom?peer_box.top:local_box.top);
        text_at(dc,app.bold,white,L"本机 · "+local_computer_name(),
                {local_box.left+px(10),local_box.top+px(14),local_box.right-px(10),local_box.top+px(44)},DT_SINGLELINE|DT_CENTER|DT_END_ELLIPSIS);
        text_at(dc,app.small,teal,L"Easy-Switch CH1 · 在线",
                {local_box.left+px(10),local_box.top+px(52),local_box.right-px(10),local_box.bottom-px(10)},DT_SINGLELINE|DT_CENTER|DT_END_ELLIPSIS);
        const Text peer_title = peer && !peer->name.empty() ? peer->name : L"未命名远程节点";
        const Text peer_sub = (peer && !peer->host.empty())
            ? (peer->host + L" · CH" + std::to_wstring(peer->host_slot))
            : L"请在右侧填写目标 IP";
        text_at(dc,app.bold,white,peer_title,
                {peer_box.left+px(10),peer_box.top+px(14),peer_box.right-px(10),peer_box.top+px(44)},DT_SINGLELINE|DT_CENTER|DT_END_ELLIPSIS);
        text_at(dc,app.small,muted,peer_sub,
                {peer_box.left+px(10),peer_box.top+px(52),peer_box.right-px(10),peer_box.bottom-px(10)},DT_SINGLELINE|DT_CENTER|DT_END_ELLIPSIS);
    }
    const auto border=CreateSolidBrush(line_color);
    FrameRect(dc,&bounds,border);
    DeleteObject(border);
}
void paint(App& app) {
    PAINTSTRUCT ps{};
    const auto dc=BeginPaint(app.window,&ps);
    if (!app.ui_active || !IsWindowVisible(app.window)) {
        EndPaint(app.window,&ps);
        return;
    }
    RECT bounds{};
    GetClientRect(app.window,&bounds);
    // Submit one opaque frame; compositing the entire child-window tree can leave
    // stale desktop pixels when render() rebuilds controls during a transition.
    if (!app.paint_dc) app.paint_dc=CreateCompatibleDC(dc);
    if (app.paint_dc && (!app.paint_bitmap || app.paint_w!=bounds.right || app.paint_h!=bounds.bottom)) {
        if (app.paint_bitmap) DeleteObject(app.paint_bitmap);
        app.paint_bitmap=CreateCompatibleBitmap(dc,bounds.right,bounds.bottom);
        app.paint_w=bounds.right;
        app.paint_h=bounds.bottom;
    }
    if (app.paint_dc && app.paint_bitmap) {
        const auto previous=SelectObject(app.paint_dc,app.paint_bitmap);
        paint_scene(app,app.paint_dc,bounds);
        BitBlt(dc,0,0,bounds.right,bounds.bottom,app.paint_dc,0,0,SRCCOPY);
        SelectObject(app.paint_dc,previous);
    } else paint_scene(app,dc,bounds);
    EndPaint(app.window,&ps);
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_CREATE) {
        app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
        update_agents(*app);
        read_config(*app);
        start_agent(*app);
        load_cached_device(*app);
        if (app->ui_active) render(*app);
        if(app->ui_active && !app->waiting_agent) io(*app);
        return 0;
    }
    if (!app) return DefWindowProcW(window,message,wparam,lparam);
    switch (message) {
    case reload_finished:
        complete_reload(*app);
        if (app->closing && !app->busy && !app->reload_busy && !app->pending_reload)
            PostMessageW(window,WM_CLOSE,0,0);
        return 0;
    case WM_DPICHANGED: {
        const auto suggested=*reinterpret_cast<const RECT*>(lparam);
        MONITORINFO monitor{};
        monitor.cbSize=sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromRect(&suggested,MONITOR_DEFAULTTONEAREST),&monitor)) return 0;
        const auto focused=GetFocus();
        const int focus_id=focused && IsChild(window,focused)?GetDlgCtrlID(focused):0;
        wchar_t kind[16]{};
        if (focus_id) GetClassNameW(focused,kind,std::size(kind));
        const bool editing=wcscmp(kind,L"Edit")==0;
        const auto edit_text=editing?control_text(focused):Text{};
        const auto search_text=control_text(app->search);
        DWORD selection_start{},selection_end{};
        if (editing) SendMessageW(focused,EM_GETSEL,reinterpret_cast<WPARAM>(&selection_start),reinterpret_cast<LPARAM>(&selection_end));
        flush_editor_edits(*app);
        const std::array old_fonts{app->normal,app->heading,app->bold,app->subheading,app->small,app->caption};
        layout_dpi=fitted_dpi(monitor.rcWork,HIWORD(wparam));
        if (app->ui_active) create_fonts(*app);
        app->has_frame=false; // Old animation coordinates belong to the previous DPI.
        const int width=px(client_width),height=px(client_height);
        const int left=std::clamp(static_cast<int>(suggested.left),static_cast<int>(monitor.rcWork.left),
                                 std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-width));
        const int top=std::clamp(static_cast<int>(suggested.top),static_cast<int>(monitor.rcWork.top),
                                std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-height));
        SetWindowPos(window,nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        if (!app->ui_active) return 0;
        render(*app);
        for (const auto old:old_fonts) if (old) DeleteObject(old);
        app->rendering=true;
        if (app->search) SetWindowTextW(app->search,search_text.c_str());
        if (focus_id) if (const auto control=GetDlgItem(window,focus_id)) {
            if (editing) {
                SetWindowTextW(control,edit_text.c_str());
                SendMessageW(control,EM_SETSEL,selection_start,selection_end);
            }
            SetFocus(control);
        }
        app->rendering=false;
        if (app->action_list) fill_actions(*app);
        return 0;
    }
    case assign_ring: {
        flush_editor_edits(*app);
        if (app->page!=ring || wparam>=8 || lparam<0 || static_cast<std::size_t>(lparam)>=std::size(ring_actions)+app->ring_smart_actions.size()) return 0;
        const auto action=ring_library_action(*app,lparam);
        app->pane_index=static_cast<int>(wparam);
        app->ring_name=unique_ring_name(*app,wparam,action.name);app->ring_code=action.code;
        app->ring_advanced=app->ring_menu_open=false;
        if (app->ring_code==L"@shortcut" || app->ring_code==L"Open()" || app->ring_code==L"TextHex()") {
            app->ring_advanced=true;render(*app);
            if (app->ring_value) SetFocus(app->ring_value);
            return 0;
        }
        if (app->ring_code==L"@folder") {
            unsigned suffix=1;
            Text name;
            do {name=L"folder-"+std::to_wstring(suffix++);} while (app->config.contains(L"ring/"+name));
            app->ring_code=L"Folder("+name+L")";
        }
        if (save_ring_slot(*app)) {
            const auto kind=ring_value_kind(app->ring_code);app->ring_advanced=kind==RingValueKind::advanced_click||kind==RingValueKind::dpi_toggle;
            render(*app);
        }
        return 0;
    }
    case swap_ring_slots: {
        flush_editor_edits(*app);
        if (app->page!=ring || wparam>=8 || lparam>=8) return 0;
        if (ring_swap_slots(*app, static_cast<std::size_t>(wparam), static_cast<std::size_t>(lparam))) {
            app->pane_index=static_cast<int>(lparam);
            const auto items=ring_items(*app);
            if (static_cast<std::size_t>(lparam)<items.size()) {
                app->ring_name=items[lparam].first;
                app->ring_code=items[lparam].second;
            }
            app->ring_advanced=app->ring_menu_open=false;
            render(*app);
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        const auto control=reinterpret_cast<HWND>(wparam);
        const int id=GetDlgCtrlID(control);
        if (app->page==ring && id>=rings_first && id<rings_first+8) {
            flush_editor_edits(*app);
            app->pane_index=id-rings_first;
            const auto entries=ring_items(*app);
            const auto index=static_cast<std::size_t>(app->pane_index);
            app->ring_name=index<entries.size()?entries[index].first:L"";
            app->ring_code=index<entries.size()?entries[index].second:L"";
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            if (point.x==-1 && point.y==-1) {
                RECT bounds{};GetWindowRect(control,&bounds);
                point={(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2};
            }
            ScreenToClient(window,&point);
            app->ring_menu_anchor={MulDiv(point.x,96,layout_dpi),MulDiv(point.y,96,layout_dpi)};
            app->ring_advanced=false;app->ring_menu_open=true;render(*app);return 0;
        }
        break;
    }
    case finished: {
        if (app->worker.joinable()) app->worker.join();
        const bool visible_settings=finish_settings(*app);
        for (const auto& [id,value]:app->work_unattempted) app->pending_values.try_emplace(id,value);
        app->work_unattempted.clear();
        const bool stale_dpi=app->io_dpi_revision!=app->dpi_revision &&
            (!app->desired_dpi || app->next_device.values[0]!=app->desired_dpi);
        const bool keep_dpi_controls=app->page==pointer && app->pane_index==1 && app->slider &&
            app->device.dpi==app->next_device.dpi;
        if (app->io_writes_dpi && !stale_dpi) app->dpi_unconfirmed=app->work_dpi_failed;
        app->device=std::move(app->next_device);
        app->busy=false;
        update_agents(*app);
        ensure_tray_icon(*app, true);
        if (!stale_dpi && visible_settings) {
            if (app->io_writes_dpi) app->dpi_failure_notice=app->work_dpi_failed?app->work_message:Text{};
            if (app->notice_revision==app->io_notice_revision) {
                notice(*app,parameter_result_notice(*app));
            }
        }
        if (app->ui_active) {
            if (app->partial_io || keep_dpi_controls) {
                if (app->page==pointer) {
                    for (unsigned i=0;i<std::size(pointer_points);++i) {
                        if (i==1 && stale_dpi) continue;
                        const auto control=GetDlgItem(window,pins_first+i);
                        if (auto skin=app->button_skins.find(control);skin!=app->button_skins.end()) {
                            skin->second.secondary=pointer_summary(*app,i);
                            SetWindowTextW(control,(skin->second.caption+L" "+skin->second.secondary).c_str());
                            InvalidateRect(control,nullptr,FALSE);
                        }
                    }
                    if (app->pane_index==0) {
                        if(app->hardware[0])SendMessageW(app->hardware[0],BM_SETCHECK,read_smooth(*app)?BST_CHECKED:BST_UNCHECKED,0);
                        if(app->hardware[2])SendMessageW(app->hardware[2],BM_SETCHECK,read_smooth_all(*app)?BST_CHECKED:BST_UNCHECKED,0);
                        if(app->hardware[1])SendMessageW(app->hardware[1],BM_SETCHECK,smartshift_enabled(*app)?BST_CHECKED:BST_UNCHECKED,0);
                        for(const auto id:{wheel_reverse,wheel_standard,wheel_free,wheel_ratchet}) {
                            const auto control=GetDlgItem(window,id);
                            if(auto skin=app->button_skins.find(control);skin!=app->button_skins.end()) {
                                const bool on=shown_setting(*app,id==wheel_reverse||id==wheel_standard?4:2).value_or(0)!=0;
                                skin->second.active=id==wheel_reverse||id==wheel_ratchet?on:!on;
                                InvalidateRect(control,nullptr,FALSE);
                            }
                        }
                        if(app->sensitivity_value && GetCapture()!=app->slider) {
                            const auto percent=shown_sensitivity(*app);
                            SetWindowTextW(app->sensitivity_value,(std::to_wstring(percent)+L"%").c_str());
                            SendMessageW(app->slider,TBM_SETPOS,TRUE,percent);
                        }
                    }
                    if (app->pane_index==2 && app->hardware[0])
                        if (const auto shown=shown_setting(*app,5))
                            SendMessageW(app->hardware[0],BM_SETCHECK,*shown?BST_CHECKED:BST_UNCHECKED,0);
                    sync_dpi_control(*app);
                }
            } else render(*app);
        }
        flush_parameters(*app);
        if (app->ui_active && app->page == easy && !app->io_included_hosts && app->device.hosts.empty() &&
            !app->device.name.empty() && !app->closing && !app->busy) {
            io(*app);
        }
        if(app->ui_active && app->page==pointer && app->pane_index==0 && (app->sensitivity_value!=nullptr)!=smartshift_enabled(*app))render(*app);
        save_sensitivity(*app);
        if (!app->ui_active && !app->busy) {
            app->connection.reset();
            trim_working_set();
        }
        if (app->closing && !app->busy && !app->reload_busy && !app->pending_reload) PostMessageW(window,WM_CLOSE,0,0);
        return 0;
    }
    case WM_TIMER:
        if (wparam == 35) {
            KillTimer(window, 35);
            if (!app->closing && !app->ending_session)
                ui_settings::ensure_recovery_task_for_install(app->directory);
            return 0;
        }
        if(wparam==34){cloud_retry(*app);return 0;}
        if(wparam==32 || wparam==33){cloud_timer(*app,wparam==32);return 0;}
        if(wparam==9) {save_sensitivity(*app);return 0;}
        if (wparam==8) {
            start_pending_reload(*app);
            if (app->closing && !app->busy && !app->reload_busy && !app->pending_reload)
                PostMessageW(window,WM_CLOSE,0,0);
            return 0;
        }
        if (wparam==5) {flush_binding_edit(*app);return 0;}
        if (wparam==7) {flush_ring_edit(*app);return 0;}
        if (wparam==6) {
            const double t=std::min(1.0,(GetTickCount64()-app->animation_start)/200.0);
            const double ease=css_ease(t);
            const auto lerp=[&](LONG a,LONG b){return a+static_cast<LONG>(std::lround((b-a)*ease));};
            const auto rect=[&](RECT a,RECT b){return RECT{lerp(a.left,b.left),lerp(a.top,b.top),lerp(a.right,b.right),lerp(a.bottom,b.bottom)};};
            app->visual_image=rect(app->animation_from,app->animation_to);
            app->visual_pane=lerp(app->pane_from,app->pane_to);
            auto batch=BeginDeferWindowPos(static_cast<int>(app->moving.size()));
            for (const auto& item:app->moving) {
                const auto box=rect(item.from,item.to);
                batch=DeferWindowPos(batch,item.window,nullptr,box.left,box.top,box.right-box.left,box.bottom-box.top,
                    SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
            }
            if (batch) EndDeferWindowPos(batch);
            if (t>=1) {KillTimer(window,6);app->animating=false;app->moving.clear();}
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
            return 0;
        }
        if (wparam==2) {flush_parameters(*app);return 0;}
        if (wparam==3) {KillTimer(window,3);save_thumb_speed(*app);return 0;}
        if (wparam==1) {
            if (!app->waiting_agent && !app->waiting_stop) return 0;
            update_agents(*app, app->waiting_stop && (app->timer_checks % 4 == 0));
            const bool timeout=++app->timer_checks>270;
            if (app->waiting_agent) {
                if (check_agent_startup(*app,timeout)) {
                    if (app->ui_active) render(*app);
                    else if (!app->busy) trim_working_set();
                }
            } else if ((!app->native && (app->official || app->timer_checks>72)) || timeout) {
                KillTimer(window,1);
                app->waiting_stop=false;
                update_agents(*app,true);
                notice(*app,L"设备服务已停止，已恢复标准硬件输入。");
                if (app->ui_active) render(*app);
                else trim_working_set();
            }
        }
        return 0;
    case WM_COMMAND:
        if (app->rendering) return 0;
        if(app->cloud_open && HIWORD(wparam)==EN_CHANGE && LOWORD(wparam)>=cloud_endpoint && LOWORD(wparam)<=cloud_password){
            update_cloud_draft(*app,LOWORD(wparam),reinterpret_cast<HWND>(lparam));return 0;
        }
        if(app->ring_creation_stage==2&&LOWORD(wparam)==ring_create_search&&HIWORD(wparam)==EN_CHANGE){app->ring_creation_query=control_text(app->search);app->ring_creation_page=0;render(*app);SetFocus(app->search);SendMessageW(app->search,EM_SETSEL,app->ring_creation_query.size(),app->ring_creation_query.size());return 0;}
        if (app->page==buttons && app->show_custom && lparam &&
            std::ranges::find(app->gesture,reinterpret_cast<HWND>(lparam))!=app->gesture.end() &&
            (HIWORD(wparam)==EN_CHANGE || HIWORD(wparam)==EN_KILLFOCUS)) {
            if (HIWORD(wparam)==EN_CHANGE) queue_binding_edit(*app);
            else flush_binding_edit(*app);
            return 0;
        }
        if (app->page==ring && app->ring_advanced && app->pane_index>=0 &&
            (reinterpret_cast<HWND>(lparam)==app->input || reinterpret_cast<HWND>(lparam)==app->ring_value || reinterpret_cast<HWND>(lparam)==app->ring_value2) &&
            (HIWORD(wparam)==EN_CHANGE || HIWORD(wparam)==EN_KILLFOCUS)) {
            if (HIWORD(wparam)==EN_CHANGE) {
                app->ring_name=control_text(app->input);
                const auto code=ring_editor_code(*app);
                if (!app->ring_name.empty() && !code.empty()) {
                    app->ring_code=code;
                    app->pending_ring=RingEdit{app->ring_section,static_cast<std::size_t>(app->pane_index),app->ring_name,app->ring_code};
                    SetTimer(window,7,180,nullptr);
                } else {app->pending_ring.reset();KillTimer(window,7);}
            } else flush_ring_edit(*app);
            return 0;
        }
        if (HIWORD(wparam)==EN_CHANGE && LOWORD(wparam)==ring_search) fill_ring_library(*app);
        else if (HIWORD(wparam)==LBN_SELCHANGE && LOWORD(wparam)==ring_library) select_ring_action(*app);
        else if (HIWORD(wparam)==EN_CHANGE && LOWORD(wparam)==find_action) {
            const auto term=control_text(app->search);
            if (app->gesture_panel && !app->choosing_preset && app->gesture_direction<0 && !term.empty()) {
                app->gesture_panel=false;
                render(*app);
                SetWindowTextW(app->search,term.c_str());
                SetFocus(app->search);
                SendMessageW(app->search,EM_SETSEL,term.size(),term.size());
            } else fill_actions(*app);
        }
        else if (LOWORD(wparam)==profile_choice && HIWORD(wparam)==CBN_SELCHANGE) {
            const auto index=SendMessageW(reinterpret_cast<HWND>(lparam),CB_GETCURSEL,0,0);
            if (index>=0 && static_cast<std::size_t>(index)+3<app->profiles.size()) {
                app->selected=app->profiles[index+3];
                render(*app);
            }
        }
        else if (LOWORD(wparam)==gesture_preset && HIWORD(wparam)==CBN_SELCHANGE) {
            const auto index=SendMessageW(reinterpret_cast<HWND>(lparam),CB_GETCURSEL,0,0);
            if (index>0 && static_cast<std::size_t>(index)<=std::size(gesture_presets)) {
                const auto& preset=gesture_presets[index-1];
                for (std::size_t i=0;i<app->gesture.size();++i)
                    SetWindowTextW(app->gesture[i],preset.rules[i]);
                KillTimer(window,5);
                save_binding(*app,false,false);
            }
        }
        else if (LOWORD(wparam)==link_action && (HIWORD(wparam)==LBN_SELCHANGE || HIWORD(wparam)==LBN_DBLCLK)) select_action(*app);
        else if (HIWORD(wparam)==BN_CLICKED) {
            const auto control=reinterpret_cast<HWND>(lparam);
            const auto found=std::ranges::find(app->hardware,control);
            if(app->page==pointer && app->pane_index==1 && LOWORD(wparam)==dpi_extended) {
                const bool enabled=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
                if(save(*app,native_dpi::write_mode(app->source,enabled))) {
                    const auto shown=shown_dpi(*app);
                    if(!enabled && shown.value_or(0)>4000)
                        queue_parameter(*app,0,native_dpi::supported(4000,app->device.dpi));
                }
                render(*app);
            }
            else if(app->page==pointer && (LOWORD(wparam)==wheel_smooth || LOWORD(wparam)==wheel_smooth_all || LOWORD(wparam)==thumb_smooth)) {
                const bool enabled=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
                const auto key=LOWORD(wparam)==thumb_smooth?L"thumb.smooth":
                    LOWORD(wparam)==wheel_smooth_all?L"scroll.smooth.all":L"scroll.smooth";
                save(*app,change_section(app->source,L"global",{{key,enabled?L"1":L"0"}}));
                render(*app);
            }
            else if(app->page==pointer && app->pane_index==0 && LOWORD(wparam)==smartshift_toggle) {
                const bool enabled=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
                const auto percent=shown_sensitivity(*app);
                app->pending_sensitivity=percent;save_sensitivity(*app);
                queue_parameter(*app,2,1,true);
                queue_parameter(*app,1,enabled?native_smartshift::threshold(percent):0);
                render(*app);
            }
            else if (app->page==pointer && control && found!=app->hardware.end())
                queue_parameter(*app,app->pane_index==0?3:5,
                                SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED);
            else click(*app,LOWORD(wparam));
        }
        return 0;
    case WM_HSCROLL:
        if (app->rendering) return 0;
        if (app->slider && reinterpret_cast<HWND>(lparam)==app->slider) {
            if (LOWORD(wparam)==TB_ENDTRACK) {save_sensitivity(*app);sync_dpi_control(*app);return 0;}
            const auto pos=SendMessageW(app->slider,TBM_GETPOS,0,0);
            InvalidateRect(app->slider,nullptr,TRUE);
            if (app->speed_value) {
                SetWindowTextW(app->speed_value,(std::to_wstring(pos)+L"%").c_str());
                set_agent_thumb_speed(static_cast<unsigned>(pos));
                app->pending_thumb_speed=static_cast<unsigned>(pos);
                // Persist once interaction settles; runtime speed changes immediately.
                SetTimer(window,3,120,nullptr);
            }
            else if (app->page==pointer && app->dpi_value && app->pane_index==1) {
                const bool extended=extended_dpi(*app);
                if(native_dpi::read_mode(app->source)!=extended && !save(*app,native_dpi::write_mode(app->source,extended))) return 0;
                const auto value=native_dpi::supported(native_dpi::from_percent(static_cast<unsigned>(pos),extended),app->device.dpi);
                SetWindowTextW(app->dpi_value,(std::to_wstring(value)+L" DPI").c_str());
                queue_parameter(*app,0,value);
            } else if (app->sensitivity_value && app->pane_index==0) {
                SetWindowTextW(app->sensitivity_value,(std::to_wstring(pos)+L"%").c_str());
                app->pending_sensitivity=static_cast<unsigned>(pos);
                SetTimer(window,9,120,nullptr);
                queue_parameter(*app,1,native_smartshift::threshold(static_cast<unsigned>(pos)));
            }
        }
        return 0;
    case WM_DRAWITEM:
        if (const auto& item=*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam); item.CtlID==ring_library)
            draw_ring_row(*app,item);
        else if (item.CtlType==ODT_LISTBOX)
            draw_action_row(*app,item);
        else draw_button(*app,item);
        return TRUE;
    case WM_NOTIFY:
        if (const auto& source=*reinterpret_cast<const NMHDR*>(lparam);
            source.hwndFrom==app->slider && source.code==NM_CUSTOMDRAW)
            return draw_slider(*app,*reinterpret_cast<const NMCUSTOMDRAW*>(lparam));
        break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        const auto dc=reinterpret_cast<HDC>(wparam);
        const auto control=reinterpret_cast<HWND>(lparam);
        SetTextColor(dc,control==app->dpi_value||control==app->sensitivity_value?teal:white);
        const bool field=message==WM_CTLCOLOREDIT;
        const bool black=in_pane(*app,control)||control==app->action_list;
        SetBkMode(dc,field?OPAQUE:TRANSPARENT);
        SetBkColor(dc,field?raised:black?panel:background);
        return reinterpret_cast<LRESULT>(field?app->field_brush:black?app->panel_brush:app->background_brush);
    }
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        ScreenToClient(window,&point);
        if (point.y>=0 && point.y<px(pane_header) && (app->pane_index<0 || point.x<px(pane_x))) {
            const auto child=ChildWindowFromPointEx(window,point,CWP_SKIPINVISIBLE);
            wchar_t type[16]{};
            if (child) GetClassNameW(child,type,std::size(type));
            if (!child || child==window || wcscmp(type,L"Static")==0) return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_LBUTTONUP:
        if (const int pin=pin_at(*app,{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)})) click(*app,pin);
        else if(app->pane_index>=0 && (app->page==buttons || app->page==pointer) &&
                GET_X_LPARAM(lparam)<px(pane_x)) click(*app,close_pane);
        return 0;
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            if (app->paint_bitmap) { DeleteObject(app->paint_bitmap); app->paint_bitmap = nullptr; }
            if (app->paint_dc) { DeleteDC(app->paint_dc); app->paint_dc = nullptr; }
            app->paint_w = app->paint_h = 0;
        }
        return 0;
    case WM_ERASEBKGND: {
        if (!app->ui_active) return 1;
        RECT bounds{};GetClientRect(window,&bounds);
        FillRect(reinterpret_cast<HDC>(wparam),&bounds,app->background_brush);
        return 1;
    }
    case WM_PAINT: paint(*app); return 0;
    case show_existing_window:
        restore_from_tray(*app);
        return 0;
    case tray_callback:
        if (LOWORD(lparam) == WM_LBUTTONUP || LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            restore_from_tray(*app);
        } else if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            show_tray_menu(*app);
        }
        return 0;
    case WM_QUERYENDSESSION:
        app->ending_session = true;
        return TRUE;
    case WM_ENDSESSION:
        app->ending_session = wparam != FALSE;
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_CLOSE:
        flush_editor_edits(*app);
        save_thumb_speed(*app);
        flush_parameters(*app);
        if (!app->force_exit && !app->closing && app->ui.close_to_tray) {
            ensure_tray_icon(*app);
            ShowWindow(window, SW_HIDE);
            release_ui_resources(*app);
            return 0;
        }
        if (!app->closing && !app->ending_session && ui_settings::current_install_autostarts()
            && (app->agent_thread_running.load() || !tray_recovery::agent_has_owner())) {
            tray_recovery::session_exit_marker(tray_recovery::ExitMarker::set);
        }
        app->closing=true;
        start_pending_reload(*app);
        if (app->busy || app->reload_busy || app->pending_reload) return 0;
        remove_tray_icon(*app);
        DestroyWindow(window);return 0;
    case WM_DESTROY:
        remove_tray_icon(*app);
        if (app->reload_cancel) SetEvent(app->reload_cancel);
        PostQuitMessage(0); return 0;
    }
    if (app->taskbar_created_msg && message == app->taskbar_created_msg) {
        app->tray_added = false;
        ensure_tray_icon(*app);
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void open_default_agent_log() {
    use_gui_agent_log();
    const auto redirected=GetStdHandle(STD_ERROR_HANDLE);
    if (redirected && redirected!=INVALID_HANDLE_VALUE && GetFileType(redirected)!=FILE_TYPE_UNKNOWN) return;
    const auto local=_wgetenv(L"LOCALAPPDATA");
    if (!local || !*local) return;
    const auto directory=fs::path(local)/L"LogiNative";
    std::error_code error;
    fs::create_directories(directory,error);
    if (error) return;
    const auto log=directory/L"agent.log";
    const auto size=fs::file_size(log,error);
    if (!error && size>1024*1024) {
        MoveFileExW(log.c_str(),(directory/L"agent.previous.log").c_str(),MOVEFILE_REPLACE_EXISTING);
    }
    // The secure CRT variant denies other processes access while the agent runs.
    if (auto* stream=_wfreopen(log.c_str(),L"a",stderr)) std::setvbuf(stream,nullptr,_IONBF,0);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,wchar_t*,int show) {
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const auto mode = argv && argc == 2 ? std::wstring_view(argv[1]) : std::wstring_view{};
    const bool recovery = mode == L"--recover-tray";
    const bool start_in_tray = recovery || mode == L"--tray";
    const bool cli_mode = argv && argc > 1 && !start_in_tray;
    if (cli_mode) {
        cli_entry::attach_parent_console_if_needed();
        const int rc = cli_entry::run_cli_main(argc, argv);
        if (argv) LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);
    if (recovery && !ui_settings::current_install_autostarts()) return 0;
    tray_recovery::StartupGate startup_gate(recovery);
    if (!startup_gate.acquired()) return recovery ? 0 : 1;
    if (recovery) {
        if (tray_recovery::session_exit_marker(tray_recovery::ExitMarker::read)
            || tray_recovery::agent_has_owner()) return 0;
    } else {
        tray_recovery::session_exit_marker(tray_recovery::ExitMarker::clear);
    }
    if (const auto existing = FindWindowW(L"LogiNativeDevice", nullptr)) {
        if (!start_in_tray) PostMessageW(existing, show_existing_window, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (start_in_tray) {
        // A background startup must wait for acknowledgement in the GUI or CLI.
        if (loo::notice_required()) return 2;
    } else if (!loo::show_first_run_notice()) {
        return 0;
    }
    open_default_agent_log();
    cli_entry::init_agent_hooks();
    struct DaemonShutdownGuard {
        ~DaemonShutdownGuard() { cli_entry::shutdown_agent_hooks(); }
    } shutdown_guard;
    {
        App app;
        app.instance = instance;
        app.record_recovery_start = recovery;
        app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        layout_dpi=fitted_dpi(work,GetDpiForSystem());
        wchar_t module[4096]{};
        const auto length=GetModuleFileNameW(nullptr,module,std::size(module));
        if (!length || length>=std::size(module)) return 1;
        app.directory=fs::path(module).parent_path();
        app.ui=ui_settings::load(app.mapping());
        apply_ui_theme(app);
        if (!start_in_tray && !ensure_ui_resources(app)) return 1;
        WNDCLASSW wc{};
        wc.lpfnWndProc=window_proc;
        wc.hInstance=instance;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(201));
        app.tray_hicon=wc.hIcon;
        wc.lpszClassName=L"LogiNativeDevice";
        if (!RegisterClassW(&wc)) return 1;
        constexpr DWORD style=WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
        const int width=px(client_width),height=px(client_height);
        const auto window=CreateWindowExW(0,wc.lpszClassName,L"LogiNative · MX Master 3S",style,
            work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,
            width,height,nullptr,nullptr,instance,&app);
        if (!window) return 1;
        app.window=window;
        startup_gate.release();
        if (!recovery) SetTimer(window, 35, 5000, nullptr);
        try {
            app.cloud_options=config_sync::load_options(app.mapping());
            if(app.cloud_options.automatic)SetTimer(window,32,60000,nullptr);
        } catch(const std::exception& e){app.cloud_status=from_utf8(e.what());}
        if (!start_in_tray) {
            apply_ui_theme(app);
            const DWORD corners=2;
            DwmSetWindowAttribute(window,33,&corners,sizeof(corners));
        }
        ensure_tray_icon(app);
        ShowWindow(window,start_in_tray?SW_HIDE:show);
        if (start_in_tray) trim_working_set();
        MSG message{};
        while (GetMessageW(&message,nullptr,0,0)>0) {
            if(message.message==WM_KEYDOWN && (message.hwnd==window || IsChild(window,message.hwnd))) {
                if(message.wParam==VK_ESCAPE&&app.ring_creation_stage){click(app,back);continue;}
                if(app.ring_profile_edit&&(message.wParam==VK_ESCAPE||message.wParam==VK_RETURN)){click(app,message.wParam==VK_ESCAPE?ring_profile_cancel:ring_profile_accept);continue;}
                if(message.wParam==VK_ESCAPE&&(app.ring_profiles_open||app.ring_settings_open||app.ring_applications_open)){
                    app.ring_profiles_open=app.ring_settings_open=app.ring_applications_open=false;app.ring_profile_target.clear();render(app);continue;
                }
            }
            if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE && app.pane_index>=0 &&
               (message.hwnd==window || IsChild(window,message.hwnd))) {
                click(app,close_pane);continue;
            }
            if (!IsDialogMessageW(window,&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    return 0;
}
