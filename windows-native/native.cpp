#include "mouse.hpp"
#include "remap.hpp"
#include "smart_native.hpp"
#include "../first_run.hpp"

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

#include <algorithm>
#include <array>
#include <cwchar>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Text = std::wstring;
using Section = std::map<Text, Text>;
using Config = std::map<Text, Section>;

constexpr COLORREF background = RGB(26,26,26), panel = RGB(0,0,0);
constexpr COLORREF raised = RGB(36,36,36), white = RGB(245,245,245);
constexpr COLORREF muted = RGB(184,184,184), teal = RGB(0,216,198);
constexpr COLORREF line_color = RGB(53,53,53), green = RGB(133,222,83);
constexpr int client_width = 1494, client_height = 848, pane_x = 995, pane_header = 69;
int layout_dpi = 96;
constexpr unsigned min_thumb_speed = 10, default_thumb_speed = 100, max_thumb_speed = 200;
constexpr auto thumb_speed_key = L"thumb.speed";
constexpr UINT finished = WM_APP + 1;
constexpr UINT assign_ring = WM_APP + 2;
constexpr UINT reload_finished = WM_APP + 3;
enum Page { home, buttons, pointer, easy, flow, settings, ring };
enum Id {
    back = 10, smart, ring_page, new_profile, reload, begin, end, advanced, ring_preview,
    ring_add, close_pane, apply, find_action, dpi_range, field, direction_left, direction_right,
    remove_profile, link_action, thumb_speed, replace_ring, flip_primary, profile_choice,
    delete_profile, ring_folder, gesture_preset, apply_thumb_speed,
    minimize_ui, close_ui, more_actions, custom_actions, ring_library, ring_search, ring_custom,
    ring_all, ring_undo, ring_redo, ring_commit, gesture_presets_open, gesture_overview, profiles_previous, profiles_next,
    nav_first = 100, profiles_first = 200, pins_first = 400, hosts_first = 500, rings_first = 600,
    gesture_rows_first = 700,
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
    {L"Win+D",L"显示或隐藏桌面"},{L"Alt+Tab",L"切换应用"},
    {L"Maximize",L"最大化窗口"},{L"Minimize",L"最小化窗口"},
    {L"Ctrl+C",L"复制"},{L"Ctrl+V",L"粘贴"},{L"Ctrl+X",L"剪切"},
    {L"Ctrl+Z",L"撤销"},{L"Ctrl+Y",L"重做"},{L"Ctrl+F",L"查找"},
    {L"Ctrl+Tab",L"下一个标签页"},{L"Ctrl+Shift+Tab",L"上一个标签页"},
    {L"Ctrl+W",L"关闭标签页"},{L"Ctrl+T",L"打开新标签页"},{L"Ctrl+Shift+T",L"重新打开标签页"},
    {L"VolumeUp",L"音量升高"},{L"VolumeDown",L"音量降低"},{L"Mute",L"静音"},
    {L"PlayPause",L"播放 / 暂停"},{L"NextTrack",L"下一曲"},{L"PreviousTrack",L"上一曲"},
    {L"Win+Ctrl+Left",L"向左切换虚拟桌面"},{L"Win+Ctrl+Right",L"向右切换虚拟桌面"},
    {L"Win",L"开始菜单"},{L"Win+E",L"文件资源管理器"},{L"Win+Shift+S",L"截图"},
    {L"PrintScreen",L"全屏截图"},{L"ScrollLeft",L"向左滚动"},{L"ScrollRight",L"向右滚动"},
    {L"ZoomIn",L"放大"},{L"ZoomOut",L"缩小"},
    {L"Ring",L"Actions Ring"},{L"None",L"无动作"},
};
struct RingAction { const wchar_t* category; const wchar_t* code; const wchar_t* name; };
constexpr RingAction ring_actions[] = {
    {L"媒体和音量",L"PlayPause",L"播放 / 暂停"},{L"媒体和音量",L"Mute",L"静音"},
    {L"媒体和音量",L"VolumeUp",L"音量升高"},{L"媒体和音量",L"VolumeDown",L"音量降低"},
    {L"媒体和音量",L"NextTrack",L"下一曲"},{L"媒体和音量",L"PreviousTrack",L"上一曲"},
    {L"打开",L"Win+E",L"文件资源管理器"},{L"打开",L"Open(notepad.exe)",L"记事本"},
    {L"打开",L"Open(calc.exe)",L"计算器"},
    {L"导航",L"Alt+Tab",L"切换应用"},{L"导航",L"Ctrl+Tab",L"下一个标签页"},
    {L"导航",L"Ctrl+Shift+Tab",L"上一个标签页"},{L"导航",L"Ctrl+T",L"打开新标签页"},
    {L"导航",L"Ctrl+W",L"关闭标签页"},{L"导航",L"Ctrl+Shift+T",L"重新打开标签页"},
    {L"导航",L"Win+Ctrl+Left",L"向左切换虚拟桌面"},{L"导航",L"Win+Ctrl+Right",L"向右切换虚拟桌面"},
    {L"实用程序",L"Win+Shift+S",L"截图"},{L"实用程序",L"PrintScreen",L"全屏截图"},
    {L"实用程序",L"ZoomIn",L"放大"},{L"实用程序",L"ZoomOut",L"缩小"},
    {L"系统",L"Win+Tab",L"任务视图"},{L"系统",L"Win+D",L"显示或隐藏桌面"},
    {L"系统",L"Maximize",L"最大化窗口"},{L"系统",L"Minimize",L"最小化窗口"},
    {L"系统",L"Lock",L"锁定"},{L"系统",L"Win",L"开始菜单"},
    {L"鼠标",L"MouseMiddle",L"中键"},{L"鼠标",L"MouseBack",L"后退"},
    {L"鼠标",L"MouseForward",L"前进"},{L"鼠标",L"ScrollLeft",L"向左滚动"},
    {L"鼠标",L"ScrollRight",L"向右滚动"},
    {L"键盘",L"Ctrl+C",L"复制"},{L"键盘",L"Ctrl+V",L"粘贴"},
    {L"键盘",L"Ctrl+X",L"剪切"},{L"键盘",L"Ctrl+Z",L"撤销"},
    {L"键盘",L"Ctrl+Y",L"重做"},{L"键盘",L"Ctrl+F",L"查找"},
    {L"高级",L"None",L"无动作"},
};
struct RingRow { Text title; int action{-1}; bool expanded{}; };
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
constexpr const wchar_t* gesture_suffix[]{L"",L".left",L".right",L".up",L".down"};
constexpr const wchar_t* gesture_titles[]{L"点击",L"向左",L"向右",L"向上",L"向下"};
struct GesturePreset { const wchar_t* title; std::array<const wchar_t*,5> rules; };
constexpr GesturePreset gesture_presets[] = {
    {L"虚拟桌面",{L"Win+Tab",L"Win+Ctrl+Left",L"Win+Ctrl+Right",L"Win",L"Win+D"}},
    {L"媒体控制",{L"PlayPause",L"PreviousTrack",L"NextTrack",L"VolumeUp",L"VolumeDown"}},
    {L"窗口管理",{L"Alt+Tab",L"Win+Left",L"Win+Right",L"Maximize",L"Win+D"}},
    {L"应用程序导航",{L"Alt+Tab",L"Alt+Shift+Tab",L"Alt+Tab",L"Win",L"Win+D"}},
    {L"缩放（无旋转）",{L"None",L"None",L"None",L"ZoomIn",L"ZoomOut"}},
    {L"平移",{L"Pan",L"None",L"None",L"None",L"None"}},
    {L"排列窗口",{L"Alt+Tab",L"Win+Left",L"Win+Right",L"Maximize",L"Minimize"}},
    {L"自定义",{L"None",L"None",L"None",L"None",L"None"}},
};

int px(int number) { return MulDiv(number, layout_dpi, 96); }
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
    std::ifstream file(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return from_utf8(bytes);
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
    bool thumb_cached{};
    std::optional<unsigned> battery;
    std::array<std::optional<unsigned>,std::size(hardware_keys)> values{};
    std::vector<unsigned> dpi;
    std::vector<logi::HostSlot> hosts;
};
struct Picture {
    IStream* stream{};
    std::unique_ptr<Gdiplus::Image> image;
    ~Picture() { image.reset(); if (stream) stream->Release(); }
    void load(HINSTANCE instance, int resource) {
        const auto item = FindResourceW(instance, MAKEINTRESOURCEW(resource), RT_RCDATA);
        if (!item) return;
        const DWORD length = SizeofResource(instance,item);
        const auto bytes = static_cast<const BYTE*>(LockResource(LoadResource(instance,item)));
        if (!bytes || !length) return;
        stream = SHCreateMemStream(bytes,length);
        if (stream) image.reset(Gdiplus::Image::FromStream(stream));
    }
};
enum class Look {
    filled, plain, glyph, tab, profile, hotspot, device, slot, section, badge, switch_toggle, ring_slot, ring_tag, tool,
    gesture_row, gesture_header, gesture_picker
};
struct ButtonSkin {
    Look look = Look::filled;
    bool active = false;
    std::wstring_view icon;
    Text caption, secondary;
    int title_lines = 1;
    HICON application_icon{};
    Gdiplus::Image* original_icon{};
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
struct App {
    HWND window{}, note{}, input{}, slider{}, speed_value{}, action_list{}, search{}, ring_value{}, save_action{};
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
    Text ring_category;
    Text ring_name,ring_code,ring_query;
    RECT ring_editor{};
    std::vector<Text> ring_undo_stack,ring_redo_stack;
    std::vector<RingRow> ring_rows;
    HWND ring_list{}, ring_filter{}, ring_choice{};
    bool ring_advanced{},ring_searching{};
    int ring_drag_action{-1};
    POINT ring_drag_start{};
    bool ring_dragging{};
    Config config;
    std::vector<Text> profiles;
    std::vector<Action> visible;
    Page page{home};
    int pane_index{-1};
    bool busy{}, native{}, official{}, waiting_agent{}, waiting_stop{}, all_actions{}, show_custom{};
    bool startup_takeover{true};
    bool gesture_panel{}, choosing_preset{};
    int gesture_direction{-1};
    HANDLE started{};
    unsigned timer_checks{};
    Device device, next_device;
    std::optional<logi::Mouse> connection;
    std::map<unsigned,unsigned> pending_values;
    unsigned long long dpi_revision{}, io_dpi_revision{};
    std::optional<unsigned> desired_dpi;
    bool dpi_unconfirmed{}, io_writes_dpi{}, work_dpi_failed{};
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
    std::optional<BindingEdit> pending_binding;
    std::optional<RingEdit> pending_ring;
    bool partial_io{},closing{};
    std::thread worker;
    Text work_message;
    Picture side, front, bottom, firefox_icon, chrome_icon, wps_icon;
    Gdiplus::PrivateFontCollection symbols;
    bool symbols_loaded{};
    ~App() {
        if (reload_cancel) SetEvent(reload_cancel);
        if (reload_worker.joinable()) reload_worker.join();
        if (reload_cancel) CloseHandle(reload_cancel);
        if (worker.joinable()) worker.join();
        if (started) CloseHandle(started);
        for (const auto& [name,icon]:application_icons) if (icon) DestroyIcon(icon);
        for (auto font : {normal,heading,bold,subheading,small,caption}) if (font) DeleteObject(font);
        for (auto brush : {background_brush,panel_brush,field_brush}) DeleteObject(brush);
    }
    fs::path mapping() const { return directory / L"mappings.ini"; }
};

HFONT font(int size, int weight = FW_NORMAL) {
    return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,L"Microsoft YaHei UI");
}
void create_fonts(App& app) {
    app.normal=font(15);
    app.heading=font(36,FW_BOLD);
    app.bold=font(15,FW_SEMIBOLD);
    app.subheading=font(21,FW_SEMIBOLD);
    app.small=font(13);
    app.caption=font(13,FW_BOLD);
}
HWND add(App& app, const wchar_t* kind, const Text& title, DWORD style, int x,int y,int w,int h,
         int id = 0, HFONT use_font = nullptr, DWORD extension = 0) {
    auto control = CreateWindowExW(extension,kind,title.c_str(),WS_CHILD|WS_VISIBLE|style,
                                  px(x),px(y),px(w),px(h),app.window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr),nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(use_font ? use_font : app.normal),FALSE);
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
        text_at(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),white,value,box,DT_WORDBREAK);
        EndPaint(window,&paint);return 0;
    }
    if (message==WM_NCDESTROY) RemoveWindowSubclass(window,smooth_label,id);
    return DefSubclassProc(window,message,wparam,lparam);
}
HWND label(App& app,const Text& value,int x,int y,int width,int height,HFONT use_font = nullptr) {
    const auto control=add(app,L"STATIC",value,SS_LEFT,x,y,width,height,0,use_font);
    SetWindowSubclass(control,smooth_label,1,0);
    return control;
}
LRESULT CALLBACK hover_button(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                              UINT_PTR id,DWORD_PTR) {
    if (message==WM_MOUSEMOVE && !GetPropW(window,L"LogiNative.Hover")) {
        TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0};
        if (TrackMouseEvent(&event)) {
            SetPropW(window,L"LogiNative.Hover",reinterpret_cast<HANDLE>(1));
            InvalidateRect(window,nullptr,FALSE);
        }
    } else if (message==WM_MOUSELEAVE || message==WM_NCDESTROY) {
        RemovePropW(window,L"LogiNative.Hover");
        if (message==WM_NCDESTROY) RemoveWindowSubclass(window,hover_button,id);
        else InvalidateRect(window,nullptr,FALSE);
    }
    return DefSubclassProc(window,message,wparam,lparam);
}
HWND styled_button(App& app,const Text& title,int id,int x,int y,int width,int height,ButtonSkin skin) {
    auto control = add(app,L"BUTTON",title,BS_OWNERDRAW|WS_TABSTOP,x,y,width,height,id,app.bold);
    app.button_skins.emplace(control,std::move(skin));
    SetWindowSubclass(control,hover_button,1,0);
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
    app.source = read(app.mapping());
    app.config = parse(app.source);
    app.profiles = {L"global"};
    for (const auto& [name,values] : app.config) {
        if (name.size() > 4 && _wcsicmp(name.c_str()+name.size()-4,L".exe")==0)
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
        std::ifstream file(edit.path,std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
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
    app.pending_binding=std::move(edit);
    SetTimer(app.window,5,180,nullptr);
}
Text action_name(const Text& code) {
    if (code==L"Pan") return L"中键（按住移动以平移）";
    if (code==L"Win+Left") return L"窗口靠左";
    if (code==L"Win+Right") return L"窗口靠右";
    if (code==L"Alt+Shift+Tab") return L"上一个应用程序";
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
void update_agents(App& app) { app.native=agent_running(); app.official=options_running(); }

void load_owned_thumb(Device& device) {
    const auto status=read_agent_settings();
    device.thumb_cached=false;
    if (!status.available) {
        if (agent_running()) device.values[5].reset();
        return;
    }
    device.values[5]=(status.observed_mask&16u)?std::optional{status.observed[4]}:std::nullopt;
    device.thumb_cached=(status.observed_mask&16u)!=0;
}

Device query_device(logi::Mouse& connection) {
    Device device;
    auto* mouse=&connection;
    device.name = from_utf8(mouse->device.name);
    device.detail = L"HID++ " + std::to_wstring(mouse->device.major) + L"." + std::to_wstring(mouse->device.minor);
    if (const auto battery=mouse->request(mouse->battery,mouse->unified_battery?1:0);
        battery && battery.packet.data()[0]<=100) {
        device.battery=battery.packet.data()[0];
        device.detail = std::to_wstring(battery.packet.data()[0]) + L"%  ·  Logi Bolt";
    }
    for (unsigned i=0; i<device.values.size(); ++i) device.values[i]=logi::get(*mouse,hardware_keys[i]);
    load_owned_thumb(device);
    device.dpi=mouse->dpi_choices;
    device.hosts=logi::hosts(*mouse);
    return device;
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
    return id<=2?std::max(revisions[1],revisions[2]):revisions[id];
}
std::optional<unsigned> shown_setting(const App& app,unsigned id) {
    return app.setting_unconfirmed[id] && app.desired_setting[id]?app.desired_setting[id]:app.device.values[id];
}
Text setting_result_notice(const SettingResult& result,bool confirmed) {
    constexpr const wchar_t* names[]{L"SmartShift",L"滚轮模式",L"高分辨率滚动",L"滚动方向",L"拇指滚轮方向"};
    Text message=names[static_cast<unsigned>(result.key)];
    if (confirmed) return message+(result.saved?L"已生效。":L"已生效，但未能保存供下次启动使用。");
    switch (result.status) {
    case SettingStatus::not_running: return message+L"尚未应用：后台未运行，请先在「设置」开始试用。";
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
    for (const unsigned group:{1u,3u,4u,5u}) {
        const auto last=app.work_setting_last[group];
        if (!last) continue;
        any=true;
        const auto& result=*app.work_settings[last];
        const unsigned id=static_cast<unsigned>(result.key)+1;
        const bool valid=id==last && result.revision==app.io_setting_revision[id] &&
            app.io_setting_values[id]==result.requested;
        const bool current=valid && result.applied && (result.observed_mask&(1u<<(id-1))) &&
            result.observed[id-1]==result.requested && is_current_setting_result(result);
        const unsigned affected=group==1?3u:1u<<(group-1);
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
                field_message=(field==1?L"SmartShift":L"滚轮模式")+Text(L"期望值尚未确认，请核对当前设置。");
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
void io(App& app, std::vector<std::pair<unsigned,unsigned>> changes = {}, unsigned host = 0) {
    if (app.busy) {
        for (const auto& [id,value]:changes) app.pending_values[id]=value;
        return;
    }
    if ((!changes.empty() || host) && options_running()) {
        notice(app,L"请先在「设置」开始试用。");
        return;
    }
    if (agent_running() && host) {
        notice(app,L"切换电脑前，请先退出两个鼠标后台。");
        return;
    }
    app.busy=true;
    app.partial_io=!changes.empty() && !host;
    app.io_dpi_revision=app.dpi_revision;
    app.io_setting_revision=app.setting_revision;
    app.io_setting_values={};
    for (const auto& [id,value]:changes) if (id && id<6) app.io_setting_values[id]=value;
    app.work_settings={};
    app.work_setting_last={};
    app.io_writes_dpi=std::ranges::any_of(changes,[](auto item){return item.first==0;});
    notice(app,app.partial_io?L"正在应用设置…":L"正在读取设备…");
    app.io_notice_revision=app.notice_revision;
    app.worker=std::thread([&app, base=app.device, changes=std::move(changes),host]() mutable {
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
            app.work_setting_last[id<=2?1:id]=id;
            for (unsigned bit=0;bit<5;++bit)
                if (result.observed_mask&(1u<<bit)) base.values[bit+1]=result.observed[bit];
            item=changes.erase(item);
        }
        if (changes.empty() && app.partial_io) {
            app.next_device=std::move(base);
            PostMessageW(app.window,finished,0,0);
            return;
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
                    base.values[1]=logi::get(*mouse,L"smartshift");
                    base.values[2]=logi::get(*mouse,L"ratchet");
                }
                app.work_message=success?(host?L"鼠标已接受切换请求。":L"已生效")
                                        :L"设备未确认此次写入，请核对当前值。";
                if (success && !dpi_saved) app.work_message=L"指针速度已生效，但未能保存供下次启动使用。";
                if (!success) app.connection.reset();
            }
            if (!changes.empty() && !host) app.next_device=std::move(base);
            else if (!host) app.next_device=query_device(*app.connection);
            else {app.connection.reset();app.next_device=std::move(base);}
        } else {
            if (app.io_writes_dpi) base.values[0].reset();
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
        app.dpi_unconfirmed=app.dpi_unconfirmed || app.device.values[0]!=value;
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
void sync_dpi_control(App& app) {
    if (app.page!=pointer || app.pane_index!=1 || app.busy || app.dpi_unconfirmed || !app.pending_values.empty() ||
        !app.input || !app.slider || GetFocus()==app.input || GetCapture()==app.slider || !app.device.values[0]) return;
    const auto value=*app.device.values[0];
    const auto position=std::ranges::find(app.device.dpi,value);
    // Programmatic readback must not enqueue another hardware write.
    const bool was_rendering=app.rendering;
    app.rendering=true;
    SetWindowTextW(app.input,std::to_wstring(value).c_str());
    if (position!=app.device.dpi.end()) SendMessageW(app.slider,TBM_SETPOS,TRUE,position-app.device.dpi.begin());
    InvalidateRect(app.slider,nullptr,FALSE);
    app.rendering=was_rendering;
}
void start_agent(App& app) {
    update_agents(app);
    if (app.native || app.waiting_agent) return;
    if (!check_mappings(app.mapping().c_str())) { notice(app,L"配置无效，请先检查按键设置。"); return; }
    const auto exe=app.directory/L"logi-cli.exe";
    auto command=L"\"" + exe.wstring() + L"\" run-preview \"" + app.mapping().wstring() + L"\"";
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    const auto output=CreateFileW((app.directory/L"agent.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                                  &security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    const auto input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,
                                 OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=input;
    startup.hStdOutput=startup.hStdError=output;
    PROCESS_INFORMATION process{};
    const bool started=output!=INVALID_HANDLE_VALUE && input!=INVALID_HANDLE_VALUE &&
        CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,
                       app.directory.c_str(),&startup,&process);
    if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output!=INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!started) { notice(app,L"无法启动后台。请检查 logi-cli.exe 是否在同目录。"); return; }
    CloseHandle(process.hThread);
    if (app.started) CloseHandle(app.started);
    app.started=process.hProcess;
    app.waiting_agent=true;
    app.waiting_stop=false;
    app.timer_checks=0;
    SetTimer(app.window,1,300,nullptr);
    notice(app,L"正在接管鼠标…");
    app.startup_notice_revision=app.notice_revision;
}

bool check_agent_startup(App& app,bool timeout) {
    if (!app.waiting_agent) return false;
    const bool exited=app.started && WaitForSingleObject(app.started,0)==WAIT_OBJECT_0;
    const DWORD started_pid=app.started?GetProcessId(app.started):0;
    const auto status=read_agent_status();
    const bool same_process=status.available && started_pid && status.pid==started_pid;
    const bool mappings_ready=same_process && !exited && app.native && status.mappings_ready;
    if (!mappings_ready && !exited && !timeout) return false;
    KillTimer(app.window,1);
    app.waiting_agent=false;
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
        for (unsigned id=1;id<6;++id)
            if (app.setting_unconfirmed[id] && app.desired_setting[id])
                app.pending_values.try_emplace(id,*app.desired_setting[id]);
        flush_parameters(app);
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
    if (index==1) return L"速度：" + (values[0]?std::to_wstring(*values[0])+L" DPI":Text(L"—"));
    if (index==2) return L"速度：" + std::to_wstring(read_thumb_speed(app)) +
                         L"%\n滚动方向：" + (!values[5]?L"尚未确认":*values[5]?L"已反转":L"默认");
    return Text(L"滚动方向：")+(values[4].value_or(0)?L"反转":L"标准")+
           L"\n高分辨率：" + (values[3].value_or(0)?L"开启":L"关闭")+
           L"\nSmartShift：" + (values[1].value_or(0)?L"开启":L"关闭");
}
int text_width(App& app,std::wstring_view value,HFONT font);
int text_line_height(App& app,HFONT font) {
    const auto dc=GetDC(app.window);
    int height{};
    {
        Gdiplus::Graphics graphics(dc);
        Gdiplus::Font face(dc,font);
        height=static_cast<int>(std::ceil(face.GetHeight(&graphics)*96.0f/layout_dpi));
    }
    ReleaseDC(app.window,dc);
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
}
LRESULT CALLBACK search_cue(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                            UINT_PTR id,DWORD_PTR) {
    const auto result=DefSubclassProc(window,message,wparam,lparam);
    if (message==WM_PAINT && !GetWindowTextLengthW(window) && GetFocus()!=window) {
        const auto dc=GetDC(window);
        RECT rect{};
        GetClientRect(window,&rect);
        const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)));
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,muted);
        DrawTextW(dc,L"搜索",2,&rect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        SelectObject(dc,old);
        ReleaseDC(window,dc);
    }
    if (message==WM_SETFOCUS || message==WM_KILLFOCUS) InvalidateRect(window,nullptr,TRUE);
    if (message==WM_NCDESTROY) RemoveWindowSubclass(window,search_cue,id);
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
    SetWindowSubclass(app.search,search_cue,1,0);
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
            SetWindowTheme(app.action_list,L"DarkMode_Explorer",nullptr);
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
    SetWindowTheme(app.action_list,L"DarkMode_Explorer",nullptr);
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
    if (app.selected!=L"global") button(app,L"恢复全局",remove_profile,1029,783,125,40);
    if (app.show_custom) icon_button(app,L"返回动作列表",custom_actions,1030,727,174,38,L"back",Look::tab);
}
LRESULT CALLBACK sliding_toggle(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
LRESULT CALLBACK numeric_edit(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if (message==WM_KEYDOWN && wparam=='A' && (GetKeyState(VK_CONTROL)&0x8000)) {
        SendMessageW(window,EM_SETSEL,0,-1);return 0;
    }
    if (message==WM_CHAR && wparam==1) return 0;
    if (message==WM_NCDESTROY) RemoveWindowSubclass(window,numeric_edit,id);
    return DefSubclassProc(window,message,wparam,lparam);
}
HWND toggle(App& app,const Text& title,bool checked,int id,int y) {
    auto control=add(app,L"BUTTON",title,BS_AUTOCHECKBOX|WS_TABSTOP,1027,y,432,36,id);
    SetWindowSubclass(control,sliding_toggle,1,0);
    SendMessageW(control,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
    return control;
}
void hardware_pane(App& app) {
    const auto index=app.pane_index;
    const auto& values=app.device.values;
    label(app,pointer_points[index].title,1027,17,380,35,app.subheading);
    if (app.official && index!=2)
        label(app,L"原版后台正在使用设备。可在「设置」开始试用。",1027,454,432,48,app.small);
    if (index==1) {
        const auto shown=app.dpi_unconfirmed && app.desired_dpi?app.desired_dpi:values[0];
        label(app,L"指针速度",1027,105,244,27,app.bold);
        app.input=add(app,L"EDIT",shown?std::to_wstring(*shown):L"",
                      WS_TABSTOP|ES_NUMBER|ES_CENTER|ES_AUTOHSCROLL,1341,105,65,23,field);
        SetWindowSubclass(app.input,numeric_edit,1,0);
        SendMessageW(app.input,EM_SETLIMITTEXT,5,0);
        if (!app.device.dpi.empty()) {
            app.slider=add(app,TRACKBAR_CLASSW,L"指针速度",TBS_NOTICKS|WS_TABSTOP,1027,149,432,34,dpi_range);
            SendMessageW(app.slider,TBM_SETRANGEMAX,FALSE,app.device.dpi.size()-1);
            const auto it=std::ranges::find(app.device.dpi,shown.value_or(0));
            SendMessageW(app.slider,TBM_SETPOS,TRUE,it==app.device.dpi.end()?0:it-app.device.dpi.begin());
            SendMessageW(app.slider,TBM_SETTICFREQ,20,0);
        }
    } else if (index==0) {
        label(app,L"滚轮模式",1027,102,285,28,app.bold);
        app.hardware[0]=toggle(app,L"逐行模式",shown_setting(app,2).value_or(0),field,138);
        app.hardware[1]=toggle(app,L"HID++ 高分辨率滚动",shown_setting(app,3).value_or(0),field+1,178);
        app.hardware[2]=toggle(app,L"反转滚动方向",shown_setting(app,4).value_or(0),field+2,218);
        label(app,L"SmartShift 灵敏度（0 = 关闭）",1027,284,340,28,app.bold);
        const auto smartshift=shown_setting(app,1);
        app.input=add(app,L"EDIT",smartshift?std::to_wstring(*smartshift):L"",
                      WS_TABSTOP|ES_NUMBER|ES_CENTER|ES_AUTOHSCROLL,1398,283,53,23,field+3);
        SetWindowSubclass(app.input,numeric_edit,1,0);
        SendMessageW(app.input,EM_SETLIMITTEXT,2,0);
        app.slider=add(app,TRACKBAR_CLASSW,L"SmartShift 灵敏度",TBS_NOTICKS|WS_TABSTOP,1027,318,432,34,dpi_range);
        SendMessageW(app.slider,TBM_SETRANGEMAX,FALSE,50);
        SendMessageW(app.slider,TBM_SETPOS,TRUE,smartshift.value_or(0));
        SendMessageW(app.slider,TBM_SETTICFREQ,10,0);
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
            label(app,L"原版后台正在使用设备。可在「设置」开始试用。",1027,279,432,44,app.small);
        else if (app.device.thumb_cached)
            label(app,L"方向来自后台最近确认。",1027,279,432,44,app.small);
    }
    const bool enabled=!app.official && !app.device.name.empty();
    if (!enabled) {
        if (app.input) EnableWindow(app.input,FALSE);
        if (app.slider && index!=2) EnableWindow(app.slider,FALSE);
        for (auto control:app.hardware) if (control) EnableWindow(control,FALSE);
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
            if (!item.first.empty()) result.push_back(std::move(item));
        }
    }
    return result;
}
std::vector<std::pair<Text,Text>> ring_items(const App& app) { return ring_items(app,app.ring_section); }
LRESULT CALLBACK ring_scroll(HWND window,UINT message,WPARAM wparam,LPARAM lparam,
                             UINT_PTR id,DWORD_PTR data) {
    auto& app=*reinterpret_cast<App*>(data);
    if (message==WM_NCDESTROY) {
        RemovePropW(window,L"RingScroll.Drag");
        RemoveWindowSubclass(window,ring_scroll,id);
        return DefSubclassProc(window,message,wparam,lparam);
    }
    if (message!=WM_PAINT && message!=WM_MOUSEWHEEL && message!=WM_LBUTTONDOWN &&
        message!=WM_MOUSEMOVE && message!=WM_LBUTTONUP && message!=WM_CAPTURECHANGED && message!=WM_KEYDOWN)
        return DefSubclassProc(window,message,wparam,lparam);
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
        if (selected>=0 && static_cast<std::size_t>(selected)<app.ring_rows.size() &&
            app.ring_rows[selected].action>=0 && app.pane_index>=0) {
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
        const auto result=DefSubclassProc(window,message,wparam,lparam);
        if (slot>=0) PostMessageW(app.window,assign_ring,slot,action);
        else notice(app,L"将操作拖到环上的槽位。");
        return result;
    }
    const auto result=DefSubclassProc(window,message,wparam,lparam);
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
void fill_ring_library(App& app) {
    if (!app.ring_list) return;
    const auto term = app.ring_searching?control_text(app.ring_filter):Text{};
    app.ring_query=term;
    SendMessageW(app.ring_list,WM_SETREDRAW,FALSE,0);
    SendMessageW(app.ring_list,LB_RESETCONTENT,0,0);
    app.ring_rows.clear();
    Text category;
    int category_row = -1;
    for (std::size_t i=0;i<std::size(ring_actions);++i) {
        const auto& action=ring_actions[i];
        if (!term.empty()) {
            if (!StrStrIW(action.name,term.c_str()) && !StrStrIW(action.code,term.c_str()) &&
                !StrStrIW(action.category,term.c_str())) continue;
        } else if (category!=action.category) {
            category=action.category;
            const bool expanded=category==app.ring_category;
            if (expanded) category_row=static_cast<int>(app.ring_rows.size());
            app.ring_rows.push_back({category,-1,expanded});
        }
        if (!term.empty() || category==app.ring_category)
            app.ring_rows.push_back({action.name,static_cast<int>(i),false});
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
    if (action.starts_with(L"Folder(") || action==L"Win+E") return L"folder";
    if (action==L"PlayPause") return L"play";
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
    const auto dc=GetDC(app.window);
    const auto old=SelectObject(dc,font);
    SIZE size{};
    GetTextExtentPoint32W(dc,value.data(),static_cast<int>(value.size()),&size);
    SelectObject(dc,old); ReleaseDC(app.window,dc);
    return MulDiv(size.cx,96,layout_dpi);
}
void render_ring(App& app) {
    const auto entries=ring_items(app);
    constexpr int center_x=497,center_y=450;
    constexpr std::array<std::pair<int,int>,8> places{{
        {0,-76},{54,-54},{76,0},{54,54},{0,76},{-54,54},{-76,0},{-54,-54}
    }};
    constexpr std::array<std::pair<int,int>,8> tags{{
        {0,-142},{118,-94},{155,0},{118,91},{0,136},{-118,91},{-149,0},{-151,-94}
    }};
    for (std::size_t i=0;i<places.size();++i) {
        const auto [dx,dy]=places[i];
        const auto [tx,ty]=tags[i];
        const bool configured=i<entries.size(), selected=static_cast<int>(i)==app.pane_index;
        const Text name=configured?ring_label(entries[i].first):L"添加槽位";
        const int id=rings_first+static_cast<int>(i);
        const std::wstring_view action=configured?std::wstring_view(entries[i].second):L"";
        const auto glyph=ring_glyph(action);
        const auto circle=styled_button(app,name,id,center_x+dx-21,center_y+dy-21,42,42,
            {Look::ring_slot,selected,glyph,std::to_wstring(i+1),{}});
        EnableWindow(circle,i<=entries.size());
        if (configured) {
            const int width=std::clamp(22+text_width(app,name,app.caption),64,180);
            const int left=ty==0?(tx<0?center_x-103-width:center_x+103):center_x+tx-width/2;
            styled_button(app,name,id,left,center_y+ty-18,width,36,
                          {Look::ring_tag,selected,{},{},{}});
        }
    }
    icon_button(app,L"关闭当前操作",close_pane,center_x-13,center_y-13,26,26,L"close",Look::ring_slot);
    if (app.ring_section!=L"ring")
        icon_button(app,L"返回外环",ring_folder,center_x-60,648,120,40,L"back",Look::tab);
    icon_button(app,L"通用方案",ring_all,860,35,43,54,L"grid",Look::profile,true);
    icon_button(app,L"所有操作",ring_all,1013,35,130,49,L"grid",Look::tool);
    icon_button(app,L"搜索",ring_search,1433,35,49,49,L"search",Look::tool,app.ring_searching);
    icon_button(app,L"预览动作环",ring_preview,1370,35,49,49,L"ring",Look::tool);
    if (!app.ring_searching) {
        label(app,L"系统 操作",1049,126,390,28,app.bold);
        styled_button(app,L"OS",0,1017,129,21,21,{Look::ring_tag,false,{},{},{}});
    }
    app.ring_filter=add(app,L"EDIT",app.ring_query,WS_TABSTOP|ES_AUTOHSCROLL,1027,121,432,29,ring_search);
    ShowWindow(app.ring_filter,app.ring_searching?SW_SHOW:SW_HIDE);
    SetWindowSubclass(app.ring_filter,search_cue,1,0);
    app.ring_list=add(app,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|
        LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,1006,171,477,668,ring_library);
    SetWindowTheme(app.ring_list,L"",L"");
    SetWindowSubclass(app.ring_list,ring_scroll,1,reinterpret_cast<DWORD_PTR>(&app));
    SendMessageW(app.ring_list,LB_SETITEMHEIGHT,0,px(49));
    fill_ring_library(app);
    const auto undo=icon_button(app,L"撤销",ring_undo,43,765,40,40,L"undo");
    const auto redo=icon_button(app,L"重做",ring_redo,103,765,40,40,L"redo");
    EnableWindow(undo,!app.ring_undo_stack.empty()); EnableWindow(redo,!app.ring_redo_stack.empty());
    app.ring_editor={};
    if (app.pane_index>=0) {
        const auto index=static_cast<std::size_t>(app.pane_index);
        const int x=index==0 || index==4?325:index<4?640:80;
        const int y=index==0?125:index==4?512:170;
        const int height=app.ring_advanced?300:154;
        app.ring_editor={x,y,x+350,y+height};
        Text chosen=app.ring_code.empty()?L"添加操作":action_name(app.ring_code);
        if (app.ring_code.starts_with(L"Folder(")) chosen=L"文件夹";
        else if (app.ring_code.starts_with(L"Open(")) chosen=L"打开应用程序";
        styled_button(app,L"OS",0,x+8,y+9,23,23,{Look::ring_tag,false,{},{},{}});
        label(app,chosen,x+40,y+10,265,25,app.caption);
        icon_button(app,L"关闭操作编辑器",close_pane,x+312,y+2,36,36,L"close");
        icon_button(app,L"编辑操作",ring_custom,x+16,y+59,64,64,ring_glyph(app.ring_code),Look::tool);
        icon_button(app,L"编辑名称和动作",ring_custom,x+62,y+51,24,24,L"edit");
        app.ring_choice=label(app,ring_label(app.ring_name),x+96,y+49,230,26,app.caption);
        if (app.ring_code.starts_with(L"Folder(") || app.ring_code.starts_with(L"Open("))
            label(app,app.ring_code.starts_with(L"Folder(")?L"包含一组操作":chosen,x+96,y+82,230,44,app.normal);
        if (app.ring_advanced) {
            label(app,L"显示名称",x+16,y+137,120,20,app.small);
            app.input=add(app,L"EDIT",app.ring_name,WS_TABSTOP|ES_AUTOHSCROLL,x+16,y+161,318,31,field);
            label(app,L"动作",x+16,y+203,120,20,app.small);
            app.ring_value=add(app,L"EDIT",app.ring_code,WS_TABSTOP|ES_AUTOHSCROLL,x+16,y+227,318,31,field+1);
            label(app,L"自动保存",x+16,y+267,318,22,app.small);
        }
        if (app.ring_code.starts_with(L"Folder("))
            button(app,L"打开文件夹",ring_folder,x+96,y+115,142,28);
    }
}
bool save_ring_slot(App& app,const RingEdit& edit) {
    const auto entries=ring_items(app,edit.section);
    const auto index=edit.index;
    const Text name=Text(trim(edit.name)), value=Text(trim(edit.code));
    if (name.empty() || value.empty() || name.find(L'=')!=Text::npos ||
        name.starts_with(L'#') || name.starts_with(L';') || name.starts_with(L'[')) {
        notice(app,L"请输入有效的名称和动作。"); return false;
    }
    for (std::size_t i=0;i<entries.size();++i) {
        if (i!=index && _wcsicmp(entries[i].first.c_str(),name.c_str())==0) {
            notice(app,L"动作环名称已被使用。"); return false;
        }
    }
    Text next;
    bool in_section=false;
    std::size_t slot=0;
    for (const auto& line:lines(app.source)) {
        if (const auto section=section_name(line);!section.empty()) {
            if (in_section && slot==index && index==entries.size()) {
                next+=name+L" = "+value+L"\n";
                ++slot;
            }
            in_section=section==edit.section;
        }
        if (in_section && !entry(line).first.empty() && slot++==index)
            next+=name+L" = "+value+L"\n";
        else
            next+=line+L"\n";
    }
    if (in_section && slot==index && index==entries.size()) next+=name+L" = "+value+L"\n";
    if (!app.config.contains(edit.section))
        next=change_section(app.source,edit.section,{{name,value}});
    const Text previous=app.source;
    if (index<entries.size() && entries[index].first==name && entries[index].second==value) return true;
    if (!save(app,next)) return false;
    app.ring_undo_stack.push_back(previous);
    app.ring_redo_stack.clear();
    return true;
}
bool save_ring_slot(App& app) {
    return save_ring_slot(app,{app.ring_section,static_cast<std::size_t>(app.pane_index),app.ring_name,app.ring_code});
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
void flush_editor_edits(App& app) { flush_binding_edit(app);flush_ring_edit(app); }
void render(App& app) {
    flush_editor_edits(app);
    app.rendering=true;
    const bool animate=app.has_frame && app.rendered_page==app.page &&
        (app.page==buttons || app.page==pointer) && app.rendered_editing!=(app.pane_index>=0) &&
        !IsIconic(app.window) && GetForegroundWindow()==app.window;
    const auto previous_image=app.visual_image;
    const int previous_pane=app.visual_pane;
    std::map<int,RECT> previous;
    if (animate) for (const auto child:app.children) {
        const int id=GetDlgCtrlID(child);
        if (!id || !IsWindowVisible(child)) continue;
        RECT box{};GetWindowRect(child,&box);
        MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&box),2);
        previous[id]=box;
    }
    KillTimer(app.window,6);app.animating=false;app.moving.clear();
    app.note=app.input=app.slider=app.speed_value=app.action_list=app.search=app.ring_value=app.save_action=nullptr;
    app.ring_list=app.ring_filter=app.ring_choice=nullptr;
    for (const auto child:app.children) DestroyWindow(child);
    app.children.clear();
    app.button_skins.clear();
    app.tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,app.window,nullptr,GetModuleHandleW(nullptr),nullptr);
    app.children.push_back(app.tips);
    SendMessageW(app.tips,TTM_SETMAXTIPWIDTH,0,360);
    app.gesture={};
    app.hardware={};
    const bool device_page=app.page!=home && app.page!=ring;
    const bool editing=app.pane_index>=0;
    icon_button(app,L"最小化",minimize_ui,1390,1,48,23,L"minimize");
    icon_button(app,L"关闭",close_ui,1440,1,52,23,L"close");
    if (app.page!=home) icon_button(app,L"返回",back,43,35,40,42,L"back");
    if (editing && app.page!=ring) icon_button(app,L"关闭侧栏",close_pane,1427,25,39,32,L"close");
    if (!editing && app.page!=ring) {
        Text title;
        if (app.page==home) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            title=now.wHour<11?L"早上好":now.wHour<18?L"下午好":L"晚上好";
        } else title=app.device.name.empty()?L"MX Master 3S":app.device.name;
        label(app,title,app.page==home?41:86,32,500,54,app.heading);
    }
    if (app.page==home) {
        icon_button(app,L"Smart Actions",smart,1211,35,42,42,L"smart");
        icon_button(app,L"Actions Ring",ring_page,1259,35,42,42,L"ring");
        icon_button(app,L"设置",nav_first+4,1404,35,42,42,L"settings");
        icon_button(app,L"刷新设备",reload,1050,35,126,42,L"search",Look::tab);
    }
    if (app.page==buttons) {
        const auto count=std::min<std::size_t>(app.profiles.size(),6);
        const bool overflow=app.profiles.size()>6;
        app.profile_offset=std::min(app.profile_offset,app.profiles.size()-count);
        const int profile_end=editing?909:1443;
        const int first=profile_end-static_cast<int>(count)*48-36-(overflow?64:0);
        if (overflow) {
            icon_button(app,L"前面的应用",profiles_previous,first+46,41,28,30,L"back");
            EnableWindow(GetDlgItem(app.window,profiles_previous),app.profile_offset>0);
            icon_button(app,L"后面的应用",profiles_next,profile_end-68,41,28,30,L"forward");
            EnableWindow(GetDlgItem(app.window,profiles_next),app.profile_offset+count<app.profiles.size());
        }
        for (std::size_t i=0;i<count;++i) {
            const auto index=i?i+app.profile_offset:0;
            const auto& name=app.profiles[index];
            auto caption=name==L"global"?L"全局":fs::path(name).stem().wstring();
            auto* original_icon=_wcsicmp(caption.c_str(),L"firefox")==0?app.firefox_icon.image.get():
                _wcsicmp(caption.c_str(),L"chrome")==0?app.chrome_icon.image.get():
                _wcsicmp(caption.c_str(),L"ksolaunch")==0?app.wps_icon.image.get():nullptr;
            const auto real_icon=name==L"global" || original_icon?nullptr:application_icon(app,name);
            const std::wstring_view icon=name==L"global"?L"global":L"application";
            const int x=first+static_cast<int>(i)*48+(overflow && i?32:0);
            auto control=styled_button(app,caption,profiles_first+static_cast<int>(index),x,31,48,48,
                {Look::profile,name==app.selected,icon,caption,{},1,real_icon,original_icon});
            tooltip(app,control);
            if (i && name==app.selected) {
                auto remove=styled_button(app,L"移除此应用配置",delete_profile,x+28,31,16,16,
                    {Look::glyph,false,L"close",L"移除此应用配置",{}});
                tooltip(app,remove);
            }
        }
        icon_button(app,L"添加应用程序",new_profile,profile_end-36,31,48,48,L"add");
    } else if (app.page==pointer) {
        // This page controls the device-wide values.
        icon_button(app,L"全局设置",0,editing?685:1219,35,36,43,L"global",Look::profile,true);
    }
    if (device_page && !editing) {
        constexpr const wchar_t* nav[]{L"按钮",L"指向和滚动",L"EASY-SWITCH",L"FLOW",L"设置"};
        constexpr const wchar_t* glyphs[]{L"buttons",L"pointer",L"easy",L"flow",L"settings"};
        constexpr int widths[]{92,132,154,102,92};
        for (int i=0;i<5;++i)
            icon_button(app,nav[i],nav_first+i,48,337+i*57,widths[i],40,glyphs[i],Look::tab,
                        app.page==buttons+i);
    }
    if (app.page==home) {
        styled_button(app,app.device.name.empty()?L"MX Master 3S · 未检测到":app.device.name,
                      nav_first,670,318,154,225,{Look::device,false,{},{},{}});
        if (app.device.name.empty()) label(app,L"设备未连接",700,552,160,26,app.small);
    } else if (app.page==buttons || app.page==pointer) {
        points(app);
        if (app.pane_index>=0) {
            if (app.page==buttons) actions_pane(app);
            else hardware_pane(app);
        }
    } else if (app.page==easy) {
        label(app,L"Easy-Switch",823,269,310,34,app.subheading);
        label(app,L"按下 Easy-Switch 按钮，在已配对电脑之间切换。",823,304,298,54);
        for (unsigned i=1;i<=3;++i) {
            const auto found=std::ranges::find(app.device.hosts,i,&logi::HostSlot::slot);
            const bool known=found!=app.device.hosts.end();
            const bool current=known&&found->current, paired=known&&found->paired.value_or(false);
            const Text text=current?L"当前电脑":paired?L"已配对的电脑":known?L"没有已配对的电脑":L"未知";
            const auto control=styled_button(app,std::to_wstring(i)+L" "+text,hosts_first+i,
                795,367+static_cast<int>(i-1)*80,353,80,{Look::slot,current,{},text,current?L"Bolt 接收器":L""});
            EnableWindow(control,paired&&!current&&!app.native&&!app.official&&!app.busy);
        }
        styled_button(app,L"刷新",reload,1046,624,94,36,{Look::plain,false,{},{},{}});
    } else if (app.page==flow) {
        label(app,L"Flow",663,320,400,41,app.subheading);
        label(app,L"当前版本尚未实现跨电脑跟随和剪贴板。",663,372,480,52);
    } else if (app.page==settings) {
        label(app,L"关于",663,80,510,24,app.bold);
        label(app,L"MX Master 3S",663,117,510,31,app.subheading);
        label(app,app.device.detail,663,154,510,30);
        label(app,L"通用",663,237,510,25,app.bold);
        label(app,L"切换左/右键",663,281,230,34,app.bold);
        const bool swapped=GetSystemMetrics(SM_SWAPBUTTON)!=0;
        styled_button(app,swapped?L"切换左/右键 · 已开启":L"切换左/右键 · 已关闭",flip_primary,
                      1141,282,36,27,{Look::switch_toggle,swapped,{},{},{}});
        label(app,L"试用模式",663,372,500,29,app.bold);
        const auto status=app.native?L"本地后台运行中":app.official?L"原版 Options+ 后台运行中":L"鼠标后台未运行";
        label(app,status,663,420,390,27,app.bold);
        label(app,L"试用时接管鼠标。停止试用后恢复原版后台。",663,459,511,45);
        button(app,app.native?L"停止试用":L"开始试用",app.native?end:begin,1038,410,136,42,true);
        styled_button(app,L"刷新设备",reload,663,536,106,40,{Look::plain,false,{},{},{}});
        styled_button(app,L"高级配置",advanced,787,536,106,40,{Look::plain,false,{},{},{}});
    } else if (app.page==ring) render_ring(app);
    if (app.device.battery && app.page!=ring) {
        const auto battery=std::to_wstring(*app.device.battery)+L"%";
        styled_button(app,L"电量 "+battery+L" · Logi Bolt",0,app.page==home?695:59,
                      app.page==home?579:781,104,40,{Look::badge,false,{},battery,{}});
    }
    if (app.page==ring && app.pane_index>=0) {
        RECT editor{px(app.ring_editor.left),px(app.ring_editor.top),px(app.ring_editor.right),px(app.ring_editor.bottom)};
        for (const auto child:app.children) {
            const int id=GetDlgCtrlID(child);
            if (id<rings_first || id>=rings_first+8) continue;
            RECT bounds{},intersection{};
            GetWindowRect(child,&bounds);
            MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&bounds),2);
            if (IntersectRect(&intersection,&bounds,&editor)) ShowWindow(child,SW_HIDE);
        }
    }
    app.note=label(app,app.message,419,781,((editing||app.page==ring)?pane_x:client_width)-455,37,app.small);
    app.animation_to=target_image_area(app);
    app.visual_image=app.animation_to;
    app.pane_to=editing?pane_x:client_width;
    app.visual_pane=app.pane_to;
    if (animate) {
        app.animation_from=previous_image;app.visual_image=previous_image;
        app.pane_from=previous_pane;app.visual_pane=previous_pane;
        for (const auto child:app.children) {
            if (!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE) || child==app.tips) continue;
            RECT to{};GetWindowRect(child,&to);
            MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&to),2);
            auto from=to;
            const int id=GetDlgCtrlID(child);
            if (const auto found=previous.find(id);id && found!=previous.end()) from=found->second;
            else if (editing && to.left>=px(pane_x) && id!=minimize_ui && id!=close_ui)
                OffsetRect(&from,px(client_width-pane_x),0);
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
void hardware_apply(App& app) {
    std::vector<std::pair<unsigned,unsigned>> changes;
    const auto assign=[&](unsigned id,unsigned value) {
        if (app.device.values[id] && app.device.values[id]!=value) changes.emplace_back(id,value);
    };
    if (app.pane_index==1) {
        const auto value=logi::number(control_text(app.input));
        if (!value || std::ranges::find(app.device.dpi,*value)==app.device.dpi.end()) {
            notice(app,L"请用滑杆选取鼠标支持的 DPI。");
            return;
        }
        assign(0,*value);
    } else if (app.pane_index==0) {
        const auto value=logi::number(control_text(app.input));
        if (!value || *value>50) {notice(app,L"SmartShift 范围为 0 到 50。");return;}
        for (unsigned i=0;i<3;++i) assign(i+2,SendMessageW(app.hardware[i],BM_GETCHECK,0,0)==BST_CHECKED);
        assign(1,*value);
    } else assign(5,SendMessageW(app.hardware[0],BM_GETCHECK,0,0)==BST_CHECKED);
    if (changes.empty()) {notice(app,L"没有需要写入的变更。");return;}
    io(app,std::move(changes));
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
    if (!GetOpenFileNameW(&dialog)) return;
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
    flush_editor_edits(app);
    app.page=page;
    app.pane_index=-1;
    app.all_actions=app.show_custom=false;
    app.gesture_panel=app.choosing_preset=false; app.gesture_direction=-1;
    update_agents(app);
    read_config(app);
    render(app);
}
void click(App& app,int id) {
    flush_editor_edits(app);
    if (id>=nav_first&&id<nav_first+5) {
        navigate(app,static_cast<Page>(buttons+id-nav_first));
        return;
    }
    if (id>=profiles_first && id<pins_first && static_cast<unsigned>(id-profiles_first)<app.profiles.size()) {
        app.selected=app.profiles[id-profiles_first];
        render(app);
        return;
    }
    if (id>=pins_first && id<pins_first+6) {
        app.pane_index=id-pins_first;
        app.all_actions=app.show_custom=false;
        app.gesture_panel=app.page==buttons && app.pane_index!=2 &&
            point_title(app,button_points[app.pane_index]).starts_with(L"手势 · ");
        app.choosing_preset=false; app.gesture_direction=-1;
        update_agents(app);
        render(app);
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
        app.ring_advanced=false;
        render(app);
        return;
    }
    switch (id) {
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
        if (app.pane_index>=0) {app.pane_index=-1;render(app);}
        else navigate(app,app.page==home?buttons:home);
        break;
    case close_pane: app.pane_index=-1; render(app); break;
    case smart: show_smart_actions(app.window,app.directory); read_config(app); render(app); break;
    case ring_page: app.ring_section=L"ring"; navigate(app,ring); break;
    case new_profile: add_profile(app); break;
    case reload: update_agents(app); io(app); break;
    case begin: start_agent(app); break;
    case end:
        if (stop_agent()==0) {
            app.waiting_agent=false; app.waiting_stop=true; app.timer_checks=0;
            SetTimer(app.window,1,300,nullptr);
            notice(app,L"正在停止试用…");
        } else notice(app,L"没有运行中的本地后台。");
        break;
    case advanced: {
        const auto argument=L"\"" + app.mapping().wstring() + L"\"";
        ShellExecuteW(app.window,L"open",L"notepad.exe",argument.c_str(),nullptr,SW_SHOWNORMAL);
        break;
    }
    case ring_preview: preview_ring(app.mapping().c_str()); break;
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
        app.ring_advanced=!app.ring_advanced;
        render(app);
        if (app.ring_advanced) SetFocus(app.input);
        break;
    case ring_undo: case ring_redo: {
        auto& from=id==ring_undo?app.ring_undo_stack:app.ring_redo_stack;
        auto& to=id==ring_undo?app.ring_redo_stack:app.ring_undo_stack;
        if (from.empty()) break;
        const auto previous=app.source;
        if (save(app,from.back())) {
            from.pop_back();to.push_back(previous);
            app.pane_index=-1;render(app);
        }
        break;
    }
    case ring_folder:
        if (app.ring_section!=L"ring") app.ring_section=L"ring";
        else {
            const auto items=ring_items(app);
            const auto index=static_cast<std::size_t>(app.pane_index);
            if (index<items.size() && items[index].second.starts_with(L"Folder("))
                app.ring_section=L"ring/"+items[index].second.substr(7,items[index].second.size()-8);
        }
        app.pane_index=-1;
        render(app);
        break;
    case apply:
        if (app.page==buttons) save_binding(app,false);
        else if (app.page==pointer) hardware_apply(app);
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
            graphics.Clear(ink(panel));
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const int x=box.right-px(44),y=(box.bottom-px(24))/2;
            rounded(graphics,x,y,px(44),px(24),12,enabled?(on?teal:RGB(65,65,65)):RGB(43,43,43));
            Gdiplus::SolidBrush thumb(ink(enabled?white:RGB(110,110,110)));
            graphics.FillEllipse(&thumb,x+px(on?23:3),y+px(3),px(18),px(18));
        }
        Text title(GetWindowTextLengthW(window)+1,L'\0');
        GetWindowTextW(window,title.data(),static_cast<int>(title.size()));title.pop_back();
        text_at(dc,reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0)),enabled?white:muted,
                title,{0,0,box.right-px(60),box.bottom},DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        if (GetFocus()==window && !(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) {
            RECT focus{box.right-px(48),1,box.right,box.bottom-1};DrawFocusRect(dc,&focus);
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
        if (start>=box.right-px(54) && std::abs(GET_X_LPARAM(lparam)-start)>px(3)) {
            SetPropW(window,dragged,reinterpret_cast<HANDLE>(1));
            SendMessageW(window,BM_SETCHECK,GET_X_LPARAM(lparam)>=box.right-px(22)?BST_CHECKED:BST_UNCHECKED,0);
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
        if (message==WM_NCDESTROY) RemoveWindowSubclass(window,sliding_toggle,id);
    }
    const auto result=DefSubclassProc(window,message,wparam,lparam);
    if (message==BM_SETCHECK || message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_ENABLE || message==WM_UPDATEUISTATE)
        InvalidateRect(window,nullptr,FALSE);
    return result;
}
void text_at(HDC dc,HFONT font,COLORREF color,std::wstring_view value,RECT rect,UINT flags) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::Font face(dc,font);
    Gdiplus::SolidBrush brush(ink(color));
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetAlignment(flags&DT_CENTER?Gdiplus::StringAlignmentCenter:
        flags&DT_RIGHT?Gdiplus::StringAlignmentFar:Gdiplus::StringAlignmentNear);
    format.SetLineAlignment(flags&DT_VCENTER?Gdiplus::StringAlignmentCenter:Gdiplus::StringAlignmentNear);
    if (flags&DT_SINGLELINE) format.SetFormatFlags(format.GetFormatFlags()|Gdiplus::StringFormatFlagsNoWrap);
    if (flags&DT_END_ELLIPSIS) format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    graphics.DrawString(value.data(),static_cast<int>(value.size()),&face,
        Gdiplus::RectF(static_cast<float>(rect.left),static_cast<float>(rect.top),
                      static_cast<float>(rect.right-rect.left),static_cast<float>(rect.bottom-rect.top)),&format,&brush);
}
bool in_pane(const App& app,HWND child) {
    if (app.pane_index<0 && app.page!=ring) return false;
    RECT rect{};
    GetWindowRect(child,&rect);
    MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&rect),2);
    if (app.page==ring) {
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
            const wchar_t symbol=glyph==L"add"?0xE903:glyph==L"global"?0xE953:
                glyph==L"close"?0xE937:glyph==L"back"?0xE907:glyph==L"forward"?0xE908:
                glyph==L"buttons"?0xE926:glyph==L"settings"?0xE979:0;
            if (symbol && app.symbols_loaded) {
                Gdiplus::Font face(L"5thAug2026",static_cast<float>(px(size)),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel,&app.symbols);
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
            if (w<px(80)) center_icon(w==px(64)?38:22,fg);
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
            if (app.front.image) {
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.DrawImage(app.front.image.get(),0,0,w,h);
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
            if (!skin.icon.empty()) center_icon(w<px(36)?10:22,white);
            break;
        case Look::ring_tag:
            rounded(graphics,1,1,w-2,h-2,2,w<px(32)?white:skin.active?teal:hover?raised:panel,line_color);
            break;
        case Look::plain: break;
        }
    }
    switch (skin.look) {
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
        if (skin.icon.empty()) text_at(dc,app.bold,fg,title,bounds,centered);
        break;
    case Look::ring_tag:
        text_at(dc,app.caption,w<px(32)?panel:fg,title,{w<px(32)?0:px(8),0,w-(w<px(32)?0:px(8)),h},centered);
        break;
    case Look::device:
    case Look::switch_toggle:
    case Look::glyph: break;
    }
    if ((item.itemState&(ODS_FOCUS|ODS_NOFOCUSRECT))==ODS_FOCUS) {
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
            draw_native_icon(graphics,ring_glyph(ring_actions[row.action].code),
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
    RECT bounds{}, knob{};
    GetClientRect(app.slider,&bounds);
    SendMessageW(app.slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&knob));
    FillRect(draw.hdc,&bounds,app.panel_brush);
    const int y=std::max(px(9),static_cast<int>((knob.top+knob.bottom)/2));
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
    Gdiplus::SolidBrush center(ink(accent));
    graphics.DrawLine(&track,left,y,right,y);
    graphics.DrawLine(&filled,left,y,mid,y);
    graphics.FillEllipse(&center,mid-px(7),y-px(7),px(14),px(14));
    return CDRF_SKIPDEFAULT;
}
void paint_scene(App& app,HDC dc,const RECT& bounds) {
    FillRect(dc,&bounds,app.background_brush);
    if ((app.pane_index>=0 || app.animating) && app.page!=ring) {
        RECT right{px(app.visual_pane),px(pane_header),bounds.right,bounds.bottom};
        FillRect(dc,&right,app.panel_brush);
    }
    if (app.page==pointer && app.pane_index==0) {
        Gdiplus::Graphics draw(dc);draw.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        rounded(draw,px(1390),px(276),px(69),px(36),8,raised);
    }
    if (app.page==pointer && app.pane_index==1) {
        {
            Gdiplus::Graphics draw(dc);draw.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            rounded(draw,px(1330),px(94),px(129),px(42),8,raised);
        }
        text_at(dc,app.small,muted,L"DPI",{px(1417),px(94),px(1450),px(136)},DT_SINGLELINE|DT_VCENTER);
    }
    if (app.page==ring) {
        RECT bar{px(pane_x),0,bounds.right,px(100)};
        FillRect(dc,&bar,app.panel_brush);
        Gdiplus::Graphics draw(dc);
        Gdiplus::Pen divider(ink(line_color));
        draw.DrawLine(&divider,px(pane_x),0,px(pane_x),bounds.bottom);
        if (app.pane_index>=0) {
            const auto& box=app.ring_editor;
            rounded(draw,px(box.left-1),px(box.top-1),px(box.right-box.left+2),px(box.bottom-box.top+2),4,panel);
            draw.DrawLine(&divider,px(box.left),px(box.top+40),px(box.right),px(box.top+40));
        }
    }
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
    if (picture && picture->image && picture->image->GetLastStatus()==Gdiplus::Ok) {
        Gdiplus::Graphics draw(dc);
        draw.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
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
        draw.DrawImage(picture->image.get(),px(area.left),px(area.top),px(area.right-area.left),px(area.bottom-area.top));
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
    const auto border=CreateSolidBrush(line_color);
    FrameRect(dc,&bounds,border);
    DeleteObject(border);
}
void paint(App& app) {
    PAINTSTRUCT ps{};
    const auto dc=BeginPaint(app.window,&ps);
    RECT bounds{};
    GetClientRect(app.window,&bounds);
    // Submit one opaque frame; compositing the entire child-window tree can leave
    // stale desktop pixels when render() rebuilds controls during a transition.
    const auto buffer=CreateCompatibleDC(dc);
    const auto bitmap=CreateCompatibleBitmap(dc,bounds.right,bounds.bottom);
    if (buffer && bitmap) {
        const auto previous=SelectObject(buffer,bitmap);
        paint_scene(app,buffer,bounds);
        BitBlt(dc,0,0,bounds.right,bounds.bottom,buffer,0,0,SRCCOPY);
        SelectObject(buffer,previous);
    } else paint_scene(app,dc,bounds);
    if (bitmap) DeleteObject(bitmap);
    if (buffer) DeleteDC(buffer);
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
        render(*app);
        io(*app);
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
        create_fonts(*app);
        app->has_frame=false; // Old animation coordinates belong to the previous DPI.
        const int width=px(client_width),height=px(client_height);
        const int left=std::clamp(static_cast<int>(suggested.left),static_cast<int>(monitor.rcWork.left),
                                 std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-width));
        const int top=std::clamp(static_cast<int>(suggested.top),static_cast<int>(monitor.rcWork.top),
                                std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-height));
        SetWindowPos(window,nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
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
        if (app->page!=ring || wparam>=8 || lparam<0 || static_cast<std::size_t>(lparam)>=std::size(ring_actions)) return 0;
        const auto entries=ring_items(*app);
        if (wparam>entries.size()) return 0;
        const auto& action=ring_actions[lparam];
        app->pane_index=static_cast<int>(wparam);
        app->ring_name=action.name;app->ring_code=action.code;app->ring_advanced=false;
        unsigned suffix=2;
        const auto used=[&] {
            for (std::size_t i=0;i<entries.size();++i)
                if (i!=wparam && _wcsicmp(entries[i].first.c_str(),app->ring_name.c_str())==0) return true;
            return false;
        };
        while (used()) app->ring_name=Text(action.name)+L" "+std::to_wstring(suffix++);
        if (save_ring_slot(*app)) render(*app);
        return 0;
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
        if (!stale_dpi && visible_settings) {
            if (app->io_writes_dpi) app->dpi_failure_notice=app->work_dpi_failed?app->work_message:Text{};
            if (app->notice_revision==app->io_notice_revision) {
                notice(*app,parameter_result_notice(*app));
            }
        }
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
                    for (unsigned i=0;i<3;++i) if (const auto shown=shown_setting(*app,i+2);app->hardware[i] && shown)
                        SendMessageW(app->hardware[i],BM_SETCHECK,*shown?BST_CHECKED:BST_UNCHECKED,0);
                    const auto shown=shown_setting(*app,1);
                    if (app->input && GetFocus()!=app->input && shown && GetCapture()!=app->slider) {
                        const bool was_rendering=app->rendering;app->rendering=true;
                        SetWindowTextW(app->input,std::to_wstring(*shown).c_str());
                        SendMessageW(app->slider,TBM_SETPOS,TRUE,*shown);
                        app->rendering=was_rendering;
                    }
                }
                if (app->pane_index==2 && app->hardware[0])
                    if (const auto shown=shown_setting(*app,5))
                        SendMessageW(app->hardware[0],BM_SETCHECK,*shown?BST_CHECKED:BST_UNCHECKED,0);
                sync_dpi_control(*app);
            }
        } else render(*app);
        flush_parameters(*app);
        if (app->startup_takeover && !app->busy && !app->closing) {
            app->startup_takeover=false;
            start_agent(*app);
        }
        if (app->closing && !app->busy && !app->reload_busy && !app->pending_reload) PostMessageW(window,WM_CLOSE,0,0);
        return 0;
    }
    case WM_TIMER:
        if (wparam==8) {
            start_pending_reload(*app);
            if (app->closing && !app->busy && !app->reload_busy && !app->pending_reload)
                PostMessageW(window,WM_CLOSE,0,0);
            return 0;
        }
        if (wparam==5) {flush_binding_edit(*app);return 0;}
        if (wparam==7) {flush_ring_edit(*app);return 0;}
        if (wparam==6) {
            const double t=std::min(1.0,(GetTickCount64()-app->animation_start)/180.0);
            const double ease=1-std::pow(1-t,3);
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
            update_agents(*app);
            const bool timeout=++app->timer_checks>45;
            if (app->waiting_agent) {
                if (check_agent_startup(*app,timeout)) render(*app);
            } else if ((!app->native && (app->official || app->timer_checks>12)) || timeout) {
                KillTimer(window,1);
                app->waiting_stop=false;
                notice(*app,app->official?L"试用已停止，原版后台已恢复。":L"本地后台已停止。");
                render(*app);
            }
        }
        return 0;
    case WM_COMMAND:
        if (app->rendering) return 0;
        if (app->page==buttons && app->show_custom && lparam &&
            std::ranges::find(app->gesture,reinterpret_cast<HWND>(lparam))!=app->gesture.end() &&
            (HIWORD(wparam)==EN_CHANGE || HIWORD(wparam)==EN_KILLFOCUS)) {
            if (HIWORD(wparam)==EN_CHANGE) queue_binding_edit(*app);
            else flush_binding_edit(*app);
            return 0;
        }
        if (app->page==ring && app->ring_advanced && app->pane_index>=0 &&
            (reinterpret_cast<HWND>(lparam)==app->input || reinterpret_cast<HWND>(lparam)==app->ring_value) &&
            (HIWORD(wparam)==EN_CHANGE || HIWORD(wparam)==EN_KILLFOCUS)) {
            if (HIWORD(wparam)==EN_CHANGE) {
                app->ring_name=control_text(app->input);
                app->ring_code=control_text(app->ring_value);
                app->pending_ring=RingEdit{app->ring_section,static_cast<std::size_t>(app->pane_index),app->ring_name,app->ring_code};
                SetTimer(window,7,180,nullptr);
            } else flush_ring_edit(*app);
            return 0;
        }
        if (app->page==pointer && app->pane_index==1 && HIWORD(wparam)==EN_KILLFOCUS &&
            reinterpret_cast<HWND>(lparam)==app->input) {
            flush_parameters(*app);
            sync_dpi_control(*app);
            return 0;
        }
        if (app->page==pointer && app->pane_index==1 && HIWORD(wparam)==EN_CHANGE &&
            reinterpret_cast<HWND>(lparam)==app->input && app->slider) {
            const auto value=control_text(app->input);
            const auto it=std::ranges::find(app->device.dpi,static_cast<unsigned>(wcstoul(value.c_str(),nullptr,10)));
            if (it!=app->device.dpi.end()) {
                SendMessageW(app->slider,TBM_SETPOS,TRUE,it-app->device.dpi.begin());
                InvalidateRect(app->slider,nullptr,TRUE);
                queue_parameter(*app,0,*it,true);
            }
        }
        else if (app->page==pointer && app->pane_index==0 && HIWORD(wparam)==EN_CHANGE &&
                 reinterpret_cast<HWND>(lparam)==app->input && app->slider) {
            const auto value=logi::number(control_text(app->input));
            if (value && *value<=50) {
                SendMessageW(app->slider,TBM_SETPOS,TRUE,*value);
                InvalidateRect(app->slider,nullptr,TRUE);
                queue_parameter(*app,1,*value,true);
            }
        }
        else if (HIWORD(wparam)==EN_CHANGE && LOWORD(wparam)==ring_search) fill_ring_library(*app);
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
            if (app->page==pointer && control && found!=app->hardware.end())
                queue_parameter(*app,app->pane_index==0?static_cast<unsigned>(found-app->hardware.begin())+2:5,
                                SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED);
            else click(*app,LOWORD(wparam));
        }
        return 0;
    case WM_HSCROLL:
        if (app->slider && reinterpret_cast<HWND>(lparam)==app->slider) {
            if (LOWORD(wparam)==TB_ENDTRACK) {sync_dpi_control(*app);return 0;}
            const auto pos=SendMessageW(app->slider,TBM_GETPOS,0,0);
            InvalidateRect(app->slider,nullptr,TRUE);
            if (app->speed_value) {
                SetWindowTextW(app->speed_value,(std::to_wstring(pos)+L"%").c_str());
                set_agent_thumb_speed(static_cast<unsigned>(pos));
                app->pending_thumb_speed=static_cast<unsigned>(pos);
                // Persist once interaction settles; runtime speed changes immediately.
                SetTimer(window,3,120,nullptr);
            }
            else if (app->input && app->pane_index==1 && static_cast<std::size_t>(pos)<app->device.dpi.size()) {
                SetWindowTextW(app->input,std::to_wstring(app->device.dpi[pos]).c_str());
                queue_parameter(*app,0,app->device.dpi[pos]);
            } else if (app->input && app->pane_index==0) {
                SetWindowTextW(app->input,std::to_wstring(pos).c_str());
                queue_parameter(*app,1,static_cast<unsigned>(pos));
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
        SetTextColor(dc,white);
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
        return 0;
    case WM_ERASEBKGND: {
        RECT bounds{};GetClientRect(window,&bounds);
        FillRect(reinterpret_cast<HDC>(wparam),&bounds,app->background_brush);
        return 1;
    }
    case WM_PAINT: paint(*app); return 0;
    case WM_CLOSE:
        flush_editor_edits(*app);
        save_thumb_speed(*app);
        flush_parameters(*app);
        app->closing=true;
        start_pending_reload(*app);
        if (app->busy || app->reload_busy || app->pending_reload) return 0;
        DestroyWindow(window);return 0;
    case WM_DESTROY:
        if (app->reload_cancel) SetEvent(app->reload_cancel);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,wchar_t*,int show) {
    if (!loo::show_first_run_notice()) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    ULONG_PTR gdiplus{};
    Gdiplus::GdiplusStartupInput graphics;
    if (Gdiplus::GdiplusStartup(&gdiplus,&graphics,nullptr)!=Gdiplus::Ok) return 1;
    {
        App app;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        layout_dpi=fitted_dpi(work,GetDpiForSystem());
        wchar_t module[4096]{};
        const auto length=GetModuleFileNameW(nullptr,module,std::size(module));
        if (!length || length>=std::size(module)) return 1;
        app.directory=fs::path(module).parent_path();
        create_fonts(app);
        app.side.load(instance,101);
        app.front.load(instance,102);
        app.bottom.load(instance,103);
        app.firefox_icon.load(instance,104);
        app.chrome_icon.load(instance,105);
        app.wps_icon.load(instance,106);
        if (const auto resource=FindResourceW(instance,MAKEINTRESOURCEW(107),RT_RCDATA)) {
            const auto data=LockResource(LoadResource(instance,resource));
            app.symbols_loaded=app.symbols.AddMemoryFont(data,SizeofResource(instance,resource))==Gdiplus::Ok;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc=window_proc;
        wc.hInstance=instance;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(201));
        wc.lpszClassName=L"LogiNativeDevice";
        if (!RegisterClassW(&wc)) return 1;
        constexpr DWORD style=WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
        const int width=px(client_width),height=px(client_height);
        const auto window=CreateWindowExW(0,wc.lpszClassName,L"LogiNative · MX Master 3S",style,
            work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,
            width,height,nullptr,nullptr,instance,&app);
        if (!window) return 1;
        const BOOL dark=TRUE;
        DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
        const DWORD corners=2;
        DwmSetWindowAttribute(window,33,&corners,sizeof(corners));
        ShowWindow(window,show);
        MSG message{};
        while (GetMessageW(&message,nullptr,0,0)>0) {
            if (!IsDialogMessageW(window,&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    return 0;
}
