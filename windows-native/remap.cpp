#include "mouse.hpp"
#include "remap.hpp"
#include "ring.hpp"
#include "dpi_ipc.hpp"
#include "reload_ipc.hpp"
#include "settings_ipc.hpp"
#include "gesture_lifecycle.hpp"
#include "continuous_media.hpp"

#include <tlhelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

namespace {

constexpr auto stop_name = L"Local\\LogiNative.Stop";
constexpr auto mutex_name = L"Local\\LogiNative.Agent";
constexpr auto speed_name = L"Local\\LogiNative.ThumbSpeed.v1";
constexpr auto health_name = L"Local\\LogiNative.Health.v2";
struct alignas(8) AgentHealth {
    LONG pid{},ready{},connections{},repairs{},failures{},input_failures{};
    LONG64 checked{},events{},last_event{};
    LONG mappings_ready{},settings_ready{};
};
AgentHealth* health{};
bool dpi_restored{true};
bool parameters_restored{true};
bool settings_loaded{},settings_saved{true};
bool settings_conflict{};
unsigned settings_unverified_mask{};
logi::SavedSettings desired_settings;
settings_ipc::Host* setting_commands{};
const wchar_t* agent_mapping{};
bool apply_wheel_mode(logi::Mouse& mouse,unsigned value);
dpi_ipc::Host* dpi_commands{};
reload_ipc::Host* reload_commands{};
std::optional<unsigned> desired_dpi;
bool desired_dpi_saved{};
void mapping_readiness(bool ready) {
    if (!health) return;
    InterlockedExchange(&health->mappings_ready,ready);
    InterlockedExchange(&health->ready,ready && dpi_restored && parameters_restored);
}
void dpi_readiness(bool ready) {
    dpi_restored=ready;
    if (!health) return;
    InterlockedExchange(&health->settings_ready,ready && parameters_restored);
    InterlockedExchange(&health->ready,ready && parameters_restored && InterlockedCompareExchange(&health->mappings_ready,0,0));
}
void parameter_readiness(bool ready) {
    parameters_restored=ready;
    if(!health)return;
    InterlockedExchange(&health->settings_ready,ready&&dpi_restored);
    InterlockedExchange(&health->ready,ready&&dpi_restored&&InterlockedCompareExchange(&health->mappings_ready,0,0));
}
struct LiveHealth {
    HANDLE mapping{};
    LiveHealth() {
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(AgentHealth),health_name);
        if(mapping)health=static_cast<AgentHealth*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(AgentHealth)));
        if(health){*health={};health->pid=static_cast<LONG>(GetCurrentProcessId());}
    }
    ~LiveHealth(){if(health){InterlockedExchange(&health->ready,0);UnmapViewOfFile(health);health=nullptr;}if(mapping)CloseHandle(mapping);}
};
UINT send_input(UINT count,INPUT* events,int size) {
    const auto sent=SendInput(count,events,size);
    if(sent!=count && health)InterlockedIncrement(&health->input_failures);
    return sent;
}
// A short accepted prefix may leave owned downs. Attempt their release once in
// this action, never retry a stale key-up during later physical user input.
bool same_release(const INPUT& a,const INPUT& b) {
    if(a.type!=b.type)return false;
    if(a.type==INPUT_KEYBOARD)return a.ki.wVk==b.ki.wVk&&a.ki.wScan==b.ki.wScan&&a.ki.dwFlags==b.ki.dwFlags;
    return a.type==INPUT_MOUSE&&a.mi.dwFlags==b.mi.dwFlags&&a.mi.mouseData==b.mi.mouseData;
}
bool balanced_input(std::span<INPUT> events) {
    const auto sent=send_input(static_cast<UINT>(events.size()),events.data(),sizeof(INPUT));
    if(sent==events.size())return true;
    std::vector<INPUT> held;
    for(const auto& input:events.first(std::min<std::size_t>(sent,events.size()))) {
        auto release=input;bool down=false,up=false;
        if(input.type==INPUT_KEYBOARD) {
            up=(input.ki.dwFlags&KEYEVENTF_KEYUP)!=0;down=!up;release.ki.dwFlags|=KEYEVENTF_KEYUP;
        } else if(input.type==INPUT_MOUSE) {
            if(input.mi.dwFlags&MOUSEEVENTF_MIDDLEDOWN){release.mi.dwFlags=MOUSEEVENTF_MIDDLEUP;down=true;}
            else if(input.mi.dwFlags&MOUSEEVENTF_XDOWN){release.mi.dwFlags=MOUSEEVENTF_XUP;down=true;}
            else up=(input.mi.dwFlags&(MOUSEEVENTF_MIDDLEUP|MOUSEEVENTF_XUP))!=0;
        }
        if(down)held.push_back(release);
        else if(up) {
            const auto found=std::find_if(held.rbegin(),held.rend(),[&](const auto& value){return same_release(value,release);});
            if(found!=held.rend())held.erase(std::next(found).base());
        }
    }
    std::reverse(held.begin(),held.end());
    if(!held.empty() && send_input(static_cast<UINT>(held.size()),held.data(),sizeof(INPUT))!=held.size())
        std::fputs("Windows rejected owned input cleanup.\n",stderr);
    return false;
}
LONG* live_thumb_speed{};
struct LiveThumbSpeed {
    HANDLE mapping{};
    explicit LiveThumbSpeed(unsigned speed) {
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(LONG),speed_name);
        if (mapping) live_thumb_speed=static_cast<LONG*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(LONG)));
        if (live_thumb_speed) InterlockedExchange(live_thumb_speed,static_cast<LONG>(speed));
    }
    ~LiveThumbSpeed() {
        if (live_thumb_speed) UnmapViewOfFile(live_thumb_speed);
        live_thumb_speed=nullptr;
        if (mapping) CloseHandle(mapping);
    }
};
HANDLE stop_event;
HANDLE reload_event;
std::wstring reload_name(const wchar_t* path) {
    std::error_code error;
    auto canonical=std::filesystem::weakly_canonical(path,error).wstring();
    if (error) return {};
    CharLowerBuffW(canonical.data(),static_cast<DWORD>(canonical.size()));
    std::uint64_t hash=14695981039346656037ULL;
    for (auto ch:canonical) {hash^=static_cast<unsigned>(ch);hash*=1099511628211ULL;}
    return L"Local\\LogiNative.Reload."+std::to_wstring(hash);
}

BOOL WINAPI console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}

enum class Gesture { tap, left, right, up, down };
enum class Operation { keys, lock, minimize, maximize, wheel_mode, scroll_left, scroll_right, zoom_in, zoom_out,
                       pan, wait, text, open, ring, folder, continuous_media };
constexpr unsigned thumb_wheel = 0x10000;

struct Action {
    Operation operation{Operation::keys};
    std::array<WORD, 5> keys{};
    unsigned delay{};
    std::wstring text;
};

struct Binding {
    std::wstring application;
    unsigned control;
    Gesture gesture;
    std::vector<Action> actions;
};

struct RingItem {
    std::wstring label;
    Binding binding;
};

struct RingFolder {
    std::wstring name;
    std::vector<RingItem> items;
};

struct Hotkey {
    UINT modifiers{};
    WORD key{};
    Binding binding{{}, 0, Gesture::tap, {}};
};

struct Mappings {
    std::vector<Binding> bindings;
    std::vector<RingItem> ring;
    std::vector<RingFolder> folders;
    std::vector<Hotkey> hotkeys;
    std::optional<unsigned> thumb_speed;
};

bool valid_media_bindings(const std::vector<Binding>& bindings);

int show_ring(std::span<const RingItem> ring, HANDLE stop, bool submenu = false) {
    std::array<std::wstring, 8> labels;
    for (std::size_t i = 0; i < ring.size(); ++i) labels[i] = ring[i].label;
    return choose_ring_action(std::span{labels}.first(ring.size()), stop, submenu);
}

const RingItem* choose_item(const Mappings& mappings, HANDLE stop) {
    for (;;) {
        const int index = show_ring(mappings.ring, stop);
        if (index < 0) return nullptr;
        const auto& selected = mappings.ring[index];
        const auto& action = selected.binding.actions.front();
        if (action.operation != Operation::folder) return &selected;
        const auto folder = std::ranges::find(mappings.folders, action.text, &RingFolder::name);
        if (folder == mappings.folders.end()) return nullptr;
        const int child = show_ring(folder->items, stop, true);
        if (child == ring_back) continue;
        return child < 0 ? nullptr : &folder->items[child];
    }
}

struct InputContext {
    HWND window{};
    DWORD pid{};
    std::wstring application;
    bool current() const {
        if(!window||!pid)return false;
        const auto foreground=GetForegroundWindow();DWORD owner{};
        if(foreground)GetWindowThreadProcessId(foreground,&owner);
        return foreground==window&&owner==pid;
    }
};

struct Control {
    unsigned id;
    bool xy;
    logi::Byte restore;
    bool down{};
    bool first_motion{};
    bool panning{};
    bool pan_scrolled{};
    bool continuous_media{};
    pointer_gesture::ContinuousMedia media;
    int x{}, y{};
    bool wait_release{};
    int threshold{83},counts_per_pan_step{62};
    pointer_gesture::Lifecycle session;
    InputContext owner;
    std::array<const Binding*,5> captured{};
};

void cancel_gesture(Control& control,bool fence=true) {
    control.session.cancel();
    control.wait_release=fence?(control.wait_release||control.down):false;
    control.down=false;control.first_motion=false;control.panning=false;control.pan_scrolled=false;
    control.x=control.y=0;control.captured={};control.owner={};
    control.continuous_media=false;control.media.reset();
}

struct Wheel {
    std::array<logi::Byte, 2> original;
    int native_resolution;
    int diverted_resolution;
    int direction;
    std::int64_t remainder{};
    const Binding* previous{};
    bool logical_invert{};

    int steps(std::span<const logi::Byte> data, const Binding* binding, unsigned speed) {
        const auto rotation = static_cast<std::int16_t>(logi::word(data));
        if (data[4] == 1 || binding != previous) remainder = 0;
        previous = binding;
        remainder += std::int64_t{direction} * rotation * native_resolution * speed;
        const auto divisor = std::int64_t{diverted_resolution} * 100;
        const auto whole = remainder / divisor;
        remainder %= divisor;
        return static_cast<int>(std::clamp<std::int64_t>(whole, -32, 32));
    }
};

WORD key(std::wstring_view name) {
    if (name.size() == 1 && ((name[0] >= L'A' && name[0] <= L'Z') ||
                            (name[0] >= L'0' && name[0] <= L'9'))) return name[0];
    if (name.starts_with(L"F")) {
        const auto index = logi::number(name.substr(1));
        if (index && *index >= 1 && *index <= 24) return VK_F1 + *index - 1;
    }
    struct Key { std::wstring_view name; WORD code; };
    constexpr Key keys[] = {
        {L"Ctrl", VK_LCONTROL}, {L"Alt", VK_LMENU}, {L"Shift", VK_LSHIFT}, {L"Win", VK_LWIN},
        {L"Left", VK_LEFT}, {L"Right", VK_RIGHT}, {L"Up", VK_UP}, {L"Down", VK_DOWN},
        {L"Enter", VK_RETURN}, {L"Esc", VK_ESCAPE}, {L"Space", VK_SPACE}, {L"Tab", VK_TAB},
        {L"Backspace", VK_BACK}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END},
        {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"PrintScreen", VK_SNAPSHOT},
        {L"VolumeUp", VK_VOLUME_UP}, {L"VolumeDown", VK_VOLUME_DOWN}, {L"Mute", VK_VOLUME_MUTE},
        {L"PlayPause", VK_MEDIA_PLAY_PAUSE}, {L"NextTrack", VK_MEDIA_NEXT_TRACK},
        {L"PreviousTrack", VK_MEDIA_PREV_TRACK},
        {L"MouseBack", VK_XBUTTON1}, {L"MouseForward", VK_XBUTTON2},
        {L"MouseMiddle", VK_MBUTTON},
    };
    for (const auto& item : keys) if (item.name == name) return item.code;
    return 0;
}

std::wstring_view trim(std::wstring_view view) {
    const auto start = view.find_first_not_of(L" \t\r");
    if (start == view.npos) return {};
    return view.substr(start, view.find_last_not_of(L" \t\r") - start + 1);
}

bool parse_action(std::wstring_view chord, Action& action) {
    if (chord == L"Lock") action.operation = Operation::lock;
    else if (chord == L"Minimize") action.operation = Operation::minimize;
    else if (chord == L"Maximize") action.operation = Operation::maximize;
    else if (chord == L"WheelMode") action.operation = Operation::wheel_mode;
    else if (chord == L"ScrollLeft") action.operation = Operation::scroll_left;
    else if (chord == L"ScrollRight") action.operation = Operation::scroll_right;
    else if (chord == L"ZoomIn") action.operation = Operation::zoom_in;
    else if (chord == L"ZoomOut") action.operation = Operation::zoom_out;
    else if (chord == L"Pan") action.operation = Operation::pan;
    else if (chord == L"GestureMedia") {
        action.operation=Operation::continuous_media;
        action.keys[0]=VK_MEDIA_PLAY_PAUSE;
    }
    else if (chord == L"Ring") action.operation = Operation::ring;
    else if (chord.starts_with(L"Folder(") && chord.ends_with(L")")) {
        action.operation = Operation::folder;
        action.text = trim(chord.substr(7, chord.size() - 8));
        if (action.text.empty()) return false;
    }
    else if (chord.starts_with(L"Wait(") && chord.ends_with(L")")) {
        const auto delay = logi::number(chord.substr(5, chord.size() - 6));
        if (!delay || *delay > 30000) return false;
        action.operation = Operation::wait;
        action.delay = *delay;
    } else if (chord.starts_with(L"TextHex(") && chord.ends_with(L")")) {
        const auto encoded = chord.substr(8, chord.size() - 9);
        if (encoded.empty() || encoded.size() > 4000 || encoded.size() % 4) return false;
        action.operation = Operation::text;
        for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
            unsigned unit = 0;
            for (auto digit : encoded.substr(offset, 4)) {
                if (digit >= L'0' && digit <= L'9') unit = unit * 16 + digit - L'0';
                else if (digit >= L'a' && digit <= L'f') unit = unit * 16 + digit - L'a' + 10;
                else return false;
            }
            if (!unit) return false;
            action.text.push_back(static_cast<wchar_t>(unit));
        }
    } else if ((chord.starts_with(L"Text(") || chord.starts_with(L"Open(")) && chord.ends_with(L")")) {
        action.operation = chord.starts_with(L"Text(") ? Operation::text : Operation::open;
        action.text = chord.substr(5, chord.size() - 6);
        if (action.text.empty() || action.text.size() > 2048) return false;
    } else if (chord != L"None") {
        for (auto& code : action.keys) {
            const auto separator = chord.find(L'+');
            code = key(trim(chord.substr(0, separator)));
            if (!code) return false;
            if (separator == chord.npos) {
                chord = {};
                break;
            }
            chord.remove_prefix(separator + 1);
            if (chord.empty()) return false;
        }
        if (!chord.empty()) return false;
    }
    const auto first = action.keys[0];
    if ((first == VK_XBUTTON1 || first == VK_XBUTTON2 || first == VK_MBUTTON) &&
        action.keys[1]) return false;
    return std::ranges::none_of(std::span{action.keys}.subspan(1), [](WORD code) {
        return code == VK_XBUTTON1 || code == VK_XBUTTON2 || code == VK_MBUTTON;
    });
}

bool parse_actions(std::wstring_view remaining, std::vector<Action>& actions) {
    for (;;) {
        auto delimiter = remaining.npos;
        unsigned parentheses = 0;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            if (remaining[i] == L';' && !parentheses) { delimiter = i; break; }
            if (remaining[i] == L'(') ++parentheses;
            if (remaining[i] == L')' && parentheses) --parentheses;
        }
        Action action;
        if (actions.size() == 32 ||
            !parse_action(trim(remaining.substr(0, delimiter)), action)) return false;
        actions.push_back(std::move(action));
        if (delimiter == remaining.npos) break;
        remaining.remove_prefix(delimiter + 1);
    }
    return actions.size() == 1 || std::ranges::none_of(actions, [](const Action& action) {
        return action.operation == Operation::ring || action.operation == Operation::folder ||
               action.operation == Operation::pan || action.operation == Operation::continuous_media;
    });
}

bool add_binding(std::wstring_view line, const std::wstring& app, std::vector<Binding>& bindings) {
    const auto equals = line.find(L'=');
    if (equals == line.npos) return false;
    const auto left = trim(line.substr(0, equals));
    const auto dot = left.find(L'.');
    const auto control_name = left.substr(0, dot);
    const auto id = control_name == L"thumb" ? std::optional{thumb_wheel} : logi::number(control_name, 16);
    // Preserve the primary and secondary click paths during testing.
    if (!id || *id <= 0x51 || (*id > 0xFFFF && control_name != L"thumb")) return false;
    auto gesture = Gesture::tap;
    if (dot != left.npos) {
        const auto direction = left.substr(dot + 1);
        if (direction == L"left") gesture = Gesture::left;
        else if (direction == L"right") gesture = Gesture::right;
        else if (direction == L"up") gesture = Gesture::up;
        else if (direction == L"down") gesture = Gesture::down;
        else return false;
    }
    if (*id == thumb_wheel && gesture != Gesture::left && gesture != Gesture::right) return false;
    for (const auto& binding : bindings) {
        if (binding.control == *id && binding.gesture == gesture &&
            _wcsicmp(app.c_str(), binding.application.c_str()) == 0) return false;
    }
    Binding binding{app, *id, gesture, {}};
    if (!parse_actions(line.substr(equals + 1), binding.actions)) return false;
    if (binding.actions[0].operation == Operation::folder ||
        ((binding.actions[0].operation == Operation::pan ||
          binding.actions[0].operation == Operation::continuous_media) && gesture != Gesture::tap) ||
        (*id == thumb_wheel && binding.actions[0].operation == Operation::ring)) return false;
    bindings.push_back(std::move(binding));
    return true;
}

bool add_ring_item(std::wstring_view line, std::vector<RingItem>& items, bool root = true) {
    const auto equals = line.find(L'=');
    if (equals == line.npos || items.size() == 8) return false;
    const auto label = trim(line.substr(0, equals));
    if (label.empty() || label.size() > 24) return false;
    RingItem item{std::wstring{label}, Binding{{}, 0, Gesture::tap, {}}};
    if (!parse_actions(line.substr(equals + 1), item.binding.actions) ||
        item.binding.actions[0].operation == Operation::ring ||
        item.binding.actions[0].operation == Operation::pan ||
        item.binding.actions[0].operation == Operation::continuous_media ||
        (!root && item.binding.actions[0].operation == Operation::folder)) return false;
    items.push_back(std::move(item));
    return true;
}

bool add_hotkey(std::wstring_view line, std::vector<Hotkey>& hotkeys) {
    const auto equals = line.find(L'=');
    if (equals == line.npos) return false;
    auto chord = trim(line.substr(0, equals));
    Hotkey hotkey;
    for (auto plus = chord.find(L'+'); plus != chord.npos; plus = chord.find(L'+')) {
        const auto modifier = trim(chord.substr(0, plus));
        const UINT flag = modifier == L"Ctrl" ? MOD_CONTROL : modifier == L"Alt" ? MOD_ALT :
                          modifier == L"Shift" ? MOD_SHIFT : 0;
        if (!flag || (hotkey.modifiers & flag)) return false;
        hotkey.modifiers |= flag;
        chord.remove_prefix(plus + 1);
    }
    hotkey.key = key(trim(chord));
    if (!hotkey.modifiers || !hotkey.key || hotkey.key == VK_LWIN ||
        hotkey.key == VK_LCONTROL || hotkey.key == VK_LMENU || hotkey.key == VK_LSHIFT ||
        hotkey.key == VK_MBUTTON || hotkey.key == VK_XBUTTON1 || hotkey.key == VK_XBUTTON2)
        return false;
    if (std::ranges::any_of(hotkeys, [&](const Hotkey& other) {
            return hotkey.key == other.key && hotkey.modifiers == other.modifiers;
        })) return false;
    if (!parse_actions(line.substr(equals + 1), hotkey.binding.actions) ||
        hotkey.binding.actions[0].operation == Operation::folder ||
        hotkey.binding.actions[0].operation == Operation::pan ||
        hotkey.binding.actions[0].operation == Operation::continuous_media) return false;
    hotkeys.push_back(std::move(hotkey));
    return true;
}

bool add_rule(std::wstring_view line, const std::wstring& section, Mappings& mappings) {
    if (_wcsicmp(section.c_str(), L"ring") == 0) return add_ring_item(line, mappings.ring);
    if (_wcsicmp(section.c_str(), L"hotkeys") == 0) return add_hotkey(line, mappings.hotkeys);
    if (section.starts_with(L"ring/") && section.size() > 5) {
        const auto name = section.substr(5);
        auto found = std::ranges::find(mappings.folders, name, &RingFolder::name);
        if (found != mappings.folders.end()) return add_ring_item(line, found->items, false);
        mappings.folders.push_back({name, {}});
        return add_ring_item(line, mappings.folders.back().items, false);
    }
    const auto equals = line.find(L'=');
    if (equals != line.npos && trim(line.substr(0, equals)) == L"thumb.speed") {
        if (!section.empty() || mappings.thumb_speed) return false;
        const auto value = logi::number(trim(line.substr(equals + 1)));
        if (!value || *value < 10 || *value > 200) return false;
        mappings.thumb_speed = *value;
        return true;
    }
    return add_binding(line, section, mappings.bindings);
}

bool parse_mappings(std::istream& file, Mappings& mappings) {
    const auto& bindings = mappings.bindings;
    std::wstring app;
    std::string raw;
    unsigned line_number = 0;
    while (std::getline(file, raw)) {
        ++line_number;
        if (line_number == 1 && raw.starts_with("\xEF\xBB\xBF")) raw.erase(0, 3);
        if (raw.empty()) continue;
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                                                static_cast<int>(raw.size()), nullptr, 0);
        if (!length) return false;
        std::wstring converted(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                            static_cast<int>(raw.size()), converted.data(), length);
        const auto line = trim(converted);
        if (line.empty() || line.starts_with(L'#') || line.starts_with(L';')) continue;
        if (line.starts_with(L'[') && line.ends_with(L']')) {
            app = trim(line.substr(1, line.size() - 2));
            if (_wcsicmp(app.c_str(), L"global") == 0) app.clear();
        } else if (!add_rule(line, app, mappings)) {
            std::fprintf(stderr, "Invalid or repeated mapping at line %u.\n", line_number);
            return false;
        }
    }
    for (const auto& item : mappings.ring) {
        const auto& action = item.binding.actions.front();
        if (action.operation == Operation::folder &&
            std::ranges::find(mappings.folders, action.text, &RingFolder::name) == mappings.folders.end()) {
            std::fputs("Folder action requires a matching [ring/Name] section.\n", stderr);
            return false;
        }
    }
    const auto missing_ring = [&](const Binding& binding) {
        return binding.actions[0].operation == Operation::ring && mappings.ring.empty();
    };
    if (std::ranges::any_of(bindings, missing_ring) ||
        std::ranges::any_of(mappings.hotkeys, [&](const Hotkey& hotkey) { return missing_ring(hotkey.binding); })) {
        std::fputs("Ring action requires a [ring] section.\n", stderr);
        return false;
    }
    for (const auto& binding : bindings) {
        if (std::ranges::none_of(bindings, [&](const Binding& rule) {
                const auto required = binding.control == thumb_wheel ? binding.gesture : Gesture::tap;
                return rule.control == binding.control && rule.application.empty() && rule.gesture == required;
            })) {
            std::fprintf(stderr, "Control %04X requires a matching [global] action.\n", binding.control);
            return false;
        }
    }
    if (std::ranges::any_of(bindings, [](const Binding& binding) { return binding.control == thumb_wheel; })) {
        for (const auto direction : {Gesture::left, Gesture::right}) {
            if (std::ranges::none_of(bindings, [=](const Binding& binding) {
                    return binding.control == thumb_wheel && binding.application.empty() &&
                           binding.gesture == direction;
                })) {
                std::fputs("Both [global] thumb.left and thumb.right are required.\n", stderr);
                return false;
            }
        }
    }
    return !file.bad() && valid_media_bindings(bindings);
}

bool read_mapping_snapshot(const wchar_t* path,std::string& bytes) {
    std::ifstream file{std::filesystem::path(path),std::ios::binary};
    if(!file)return false;
    bytes.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
    return !file.bad();
}
bool load(const wchar_t* path,Mappings& mappings) {
    std::string bytes;
    if(!read_mapping_snapshot(path,bytes)){std::fputs("Cannot read mapping file.\n",stderr);return false;}
    std::istringstream snapshot(bytes);
    return parse_mappings(snapshot,mappings);
}
ReloadStatus prepare_reload_snapshot(const wchar_t* path,const std::optional<reload_ipc::Request>& request,Mappings& next) {
    std::string bytes;
    if(!read_mapping_snapshot(path,bytes))return ReloadStatus::read_failed;
    if(request) {
        ReloadFingerprint fingerprint{};
        if(!mapping_fingerprint(bytes,fingerprint))return ReloadStatus::ipc_error;
        if(fingerprint!=request->fingerprint)return ReloadStatus::superseded;
    }
    // Parse the exact immutable bytes just fingerprinted, never reopen the file.
    std::istringstream snapshot(bytes);
    if(!parse_mappings(snapshot,next) || (next.bindings.empty() && next.hotkeys.empty()))return ReloadStatus::invalid_mapping;
    return ReloadStatus::applied; // Preparation only; caller must still activate and commit.
}

struct Foreground {
    HWND window{};
    DWORD pid{};
    std::wstring path;

    const std::wstring& get() {
        const auto current = GetForegroundWindow();
        DWORD owner{};
        if (current) GetWindowThreadProcessId(current, &owner);
        if (current == window && owner == pid) return path;
        window = current;
        pid = owner;
        path.clear();
        if (!pid) return path;
        const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!handle) return path;
        wchar_t buffer[1024];
        DWORD size = std::size(buffer);
        if (QueryFullProcessImageNameW(handle, 0, buffer, &size)) path.assign(buffer, size);
        CloseHandle(handle);
        return path;
    }
    InputContext capture(){const auto application=get();return {window,pid,application};}
};

bool extended(WORD code) {
    return (code >= VK_PRIOR && code <= VK_DOWN) || code == VK_DELETE ||
           code == VK_LWIN || code == VK_SNAPSHOT || (code >= VK_VOLUME_MUTE && code <= VK_MEDIA_PLAY_PAUSE);
}

INPUT wheel_input(DWORD axis, int steps) {
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = axis;
    event.mi.mouseData = static_cast<DWORD>(steps * WHEEL_DELTA);
    return event;
}

void scroll(int horizontal, int vertical) {
    INPUT inputs[2];
    UINT count = 0;
    if (horizontal) inputs[count++] = wheel_input(MOUSEEVENTF_HWHEEL, horizontal);
    if (vertical) inputs[count++] = wheel_input(MOUSEEVENTF_WHEEL, vertical);
    if (count && send_input(count, inputs, sizeof(INPUT)) != count)
        std::fputs("Windows rejected scroll input.\n", stderr);
}

void zoom(int direction) {
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
        scroll(0, direction);
        return;
    }
    INPUT inputs[3]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LCONTROL;
    inputs[1] = wheel_input(MOUSEEVENTF_WHEEL, direction);
    inputs[2] = inputs[0];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    if(!balanced_input(inputs))std::fputs("Windows rejected zoom input.\n", stderr);
}

int pan_steps(int& remainder, int delta, int counts_per_step) {
    remainder += delta;
    const int steps = remainder / counts_per_step;
    remainder %= counts_per_step;
    return std::clamp(steps, -32, 32);
}

void execute(const Action& action, logi::Mouse* device) {
    if (action.operation == Operation::minimize || action.operation == Operation::maximize) {
        const auto window = GetForegroundWindow();
        if (window && !ShowWindowAsync(window, action.operation == Operation::minimize ? SW_MINIMIZE : SW_MAXIMIZE))
            std::fputs("Windows rejected the window action.\n", stderr);
        return;
    }
    if (action.operation == Operation::lock) {
        if (!LockWorkStation()) std::fputs("Could not lock the workstation.\n", stderr);
        return;
    }
    if (action.operation == Operation::wheel_mode) {
        const auto mode = device ? logi::get(*device, L"ratchet") : std::nullopt;
        if (!mode || !apply_wheel_mode(*device, !*mode))
            std::fputs("Could not switch the scroll wheel mode.\n", stderr);
        return;
    }
    if (action.operation == Operation::open) {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", action.text.c_str(), nullptr,
                                                    nullptr, SW_SHOWNORMAL)) <= 32)
            std::fputs("Could not open action target.\n", stderr);
        return;
    }
    if (action.operation == Operation::text) {
        std::vector<INPUT> inputs(action.text.size() * 2);
        for (std::size_t i = 0; i < action.text.size(); ++i) {
            inputs[i * 2].type = INPUT_KEYBOARD;
            inputs[i * 2].ki.wScan = action.text[i];
            inputs[i * 2].ki.dwFlags = KEYEVENTF_UNICODE;
            inputs[i * 2 + 1] = inputs[i * 2];
            inputs[i * 2 + 1].ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        if (!balanced_input(inputs))
            std::fputs("Windows rejected some Unicode input.\n", stderr);
        return;
    }
    if (action.operation == Operation::scroll_left || action.operation == Operation::scroll_right) {
        scroll(action.operation == Operation::scroll_left ? -1 : 1, 0);
        return;
    }
    if (action.operation == Operation::zoom_in || action.operation == Operation::zoom_out) {
        zoom(action.operation == Operation::zoom_in ? 1 : -1);
        return;
    }
    INPUT events[10]{};
    const auto mouse = action.keys[0];
    if (mouse == VK_XBUTTON1 || mouse == VK_XBUTTON2 || mouse == VK_MBUTTON) {
        if(GetAsyncKeyState(mouse)&0x8000)return;
        events[0].type = events[1].type = INPUT_MOUSE;
        events[0].mi.dwFlags = mouse == VK_MBUTTON ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_XDOWN;
        events[1].mi.dwFlags = mouse == VK_MBUTTON ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_XUP;
        if (mouse != VK_MBUTTON)
            events[0].mi.mouseData = events[1].mi.mouseData = mouse == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
        if (!balanced_input(std::span{events}.first(2)))
            std::fputs("Windows rejected mouse input.\n", stderr);
        return;
    }
    unsigned count = 0;
    std::array<WORD, 5> pressed{};
    for (const auto code : action.keys) {
        if (!code) break;
        if (GetAsyncKeyState(code) & 0x8000) continue;
        pressed[count] = code;
        events[count].type = INPUT_KEYBOARD;
        events[count].ki.wVk = code;
        events[count].ki.dwFlags = extended(code) ? KEYEVENTF_EXTENDEDKEY : 0;
        ++count;
    }
    if (!count) return;
    for (unsigned i = 0; i < count; ++i) {
        auto& event = events[count + i];
        event.type = INPUT_KEYBOARD;
        event.ki.wVk = pressed[count - i - 1];
        event.ki.dwFlags = KEYEVENTF_KEYUP | (extended(event.ki.wVk) ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (!balanced_input(std::span{events}.first(count*2)))
        std::fputs("Windows rejected some shortcut input.\n", stderr);
}

void move_pan(Control& control, int x, int y, int counts_per_step) {
    const auto horizontal = pan_steps(control.x, x, counts_per_step);
    const auto vertical = pan_steps(control.y, y, counts_per_step);
    control.pan_scrolled |= horizontal != 0 || vertical != 0;
    scroll(horizontal, -vertical);
}

void release_pan(const Control& control) {
    // The shipped CUSTOM_PAN preset assigns a middle click to a stationary tap.
    // Once scrolling happened, moving back to the origin must not produce a click.
    if (!control.pan_scrolled) {
        Action middle;
        middle.keys[0] = VK_MBUTTON;
        execute(middle, nullptr);
    }
}

struct Sequences {
    struct Run {
        const Binding* binding;
        std::size_t next{};
        ULONGLONG due{};
        std::optional<InputContext> owner;
    };
    std::vector<Run> runs;
    const Mappings& mappings;
    ~Sequences(){cancel();}
    void cancel(){runs.clear();}
    static bool needs_mouse(const Run& run) {
        if(run.owner&&!run.owner->current())return false;
        if(run.due>GetTickCount64())return false;
        for(auto i=run.next;i<run.binding->actions.size();++i) {
            const auto& action=run.binding->actions[i];
            if(action.operation==Operation::wait && action.delay)return false;
            if(action.operation==Operation::wheel_mode)return true;
        }
        return false;
    }
    bool needs_mouse() const {
        return std::ranges::any_of(runs,[](const auto& run){return needs_mouse(run);});
    }

    static bool advance(Run& run, logi::Mouse& mouse) {
        const auto& actions = run.binding->actions;
        while (run.next < actions.size()) {
            if(run.owner&&!run.owner->current())return false;
            const auto& action = actions[run.next++];
            if (action.operation != Operation::wait) {
                // These existing macro actions explicitly request another context.
                // Preserve their continuation, without following/activating windows.
                if(action.operation==Operation::open||action.operation==Operation::minimize||
                   action.operation==Operation::maximize||action.operation==Operation::lock)run.owner.reset();
                execute(action, &mouse);
            }
            else if (action.delay && run.next < actions.size()) {
                run.due = GetTickCount64() + action.delay;
                return true;
            }
        }
        return false;
    }

    void start(const Binding& binding, logi::Mouse& mouse,std::optional<InputContext> owner={}) {
        if(owner&&!owner->current())return;
        if (binding.actions[0].operation == Operation::ring) {
            if (const auto* chosen = choose_item(mappings, stop_event)) {
                if(!owner||owner->current())start(chosen->binding, mouse,std::move(owner));
            }
            return;
        }
        Run run{&binding,0,0,std::move(owner)};
        if(mouse.channel && mouse.channel->control_pending() && needs_mouse(run)) {
            if(runs.size()<32)runs.push_back(run);
            return;
        }
        if (runs.size() < 32 && advance(run, mouse)) runs.push_back(run);
    }

    DWORD service(logi::Mouse& mouse) {
        std::erase_if(runs,[](const auto& run){return run.owner&&!run.owner->current();});
        DWORD next = INFINITE;
        for (auto run = runs.begin(); run != runs.end();) {
            if(mouse.channel && mouse.channel->control_pending() && needs_mouse(*run)) {
                ++run;continue;
            }
            const auto now = GetTickCount64();
            if (run->due <= now && !advance(*run, mouse)) {
                run = runs.erase(run);
                continue;
            }
            next = std::min(next, static_cast<DWORD>(run->due - now));
            ++run;
        }
        return next;
    }
};

bool hotkey_is_held(const Hotkey& hotkey) {
    const auto held = [](int key_code) { return (GetAsyncKeyState(key_code) & 0x8000) != 0; };
    return held(hotkey.key) || ((hotkey.modifiers & MOD_CONTROL) && held(VK_CONTROL)) ||
           ((hotkey.modifiers & MOD_ALT) && held(VK_MENU)) ||
           ((hotkey.modifiers & MOD_SHIFT) && held(VK_SHIFT));
}

void dispatch_hotkeys(const std::vector<Hotkey>& hotkeys, std::vector<std::size_t>& pending,
                      Sequences& sequences, logi::Mouse& mouse) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
        if (message.wParam >= 1 && message.wParam <= hotkeys.size() &&
            !std::ranges::contains(pending, message.wParam - 1)) pending.push_back(message.wParam - 1);
    }
    for (auto item = pending.begin(); item != pending.end();) {
        const auto& hotkey = hotkeys[*item];
        if (hotkey_is_held(hotkey)) {
            ++item;
        } else {
            sequences.start(hotkey.binding, mouse);
            item = pending.erase(item);
        }
    }
}

const Binding* lookup(const std::vector<Binding>& bindings, unsigned id, Gesture gesture,
                      const std::wstring& app) {
    const Binding* fallback = nullptr;
    const Binding* basename = nullptr;
    const auto file = app.c_str() + (app.find_last_of(L"\\/") + 1);
    for (const auto& binding : bindings) {
        if (binding.control != id || binding.gesture != gesture) continue;
        if (binding.application.empty()) fallback = &binding;
        else if (_wcsicmp(app.c_str(), binding.application.c_str()) == 0) return &binding;
        else if (_wcsicmp(file, binding.application.c_str()) == 0) basename = &binding;
    }
    return basename ? basename : fallback;
}

void fire(logi::Mouse& mouse,const Control& control,Gesture gesture,Sequences& sequences) {
    if(const auto* binding=control.captured[static_cast<std::size_t>(gesture)])sequences.start(*binding,mouse,control.owner);
}

bool valid_media_bindings(const std::vector<Binding>& bindings) {
    constexpr WORD expected[]{VK_MEDIA_PREV_TRACK,VK_MEDIA_NEXT_TRACK,VK_VOLUME_UP,VK_VOLUME_DOWN};
    for(const auto& marker:bindings) {
        if(marker.actions.front().operation!=Operation::continuous_media)continue;
        // Validate effective app rules, including inherited markers and basename
        // profiles, so changing a direction cannot silently change media semantics.
        for(const auto& context:bindings) {
            const auto* tap=lookup(bindings,marker.control,Gesture::tap,context.application);
            if(!tap || tap->actions.front().operation!=Operation::continuous_media)continue;
            for(unsigned i=0;i<4;++i) {
                const auto* direction=lookup(bindings,marker.control,static_cast<Gesture>(i+1),context.application);
                const std::array<WORD,5> keys{expected[i]};
                if(!direction || direction->actions.size()!=1 ||
                   direction->actions[0].operation!=Operation::keys || direction->actions[0].keys!=keys) {
                    std::fputs("GestureMedia requires PreviousTrack/NextTrack/VolumeUp/VolumeDown directions.\n",stderr);
                    return false;
                }
            }
        }
    }
    return true;
}

bool divert_wheel(logi::Mouse& mouse, std::optional<Wheel>& wheel, unsigned speed) {
    const auto info = mouse.request(mouse.thumb, 0);
    const auto state = mouse.request(mouse.thumb, 1);
    if (!info || info.packet.data().size() < 5 || !state) return false;
    const auto data = info.packet.data();
    const auto status = state.packet.data();
    const auto native = logi::word(data);
    const auto diverted = logi::word(data.subspan(2));
    if (!native || !diverted || status[0] > 1) return false;
    const auto inverted = static_cast<logi::Byte>(status[1] & 1);
    const int direction = ((data[4] & 1) != inverted) ? 1 : -1;
    wheel = Wheel{{status[0], status[1]}, static_cast<int>(native), static_cast<int>(diverted), direction};
    wheel->logical_invert=inverted!=0;
    if (!mouse.request(mouse.thumb, 2, {1, static_cast<logi::Byte>(status[1]&~1u)})) return false;
    const auto current = mouse.request(mouse.thumb, 1);
    if (!current || current.packet.data()[0] != 1 || current.packet.data()[1] != (status[1]&~1u)) return false;
    std::printf("Thumb wheel: native=%u diverted=%u direction=%d speed=%u%%.\n",
                native, diverted, direction, speed);
    return true;
}

bool needs_raw_motion(const Binding& binding) {
    if (binding.control==thumb_wheel) return false;
    if (binding.actions.front().operation==Operation::pan ||
        binding.actions.front().operation==Operation::continuous_media) return true;
    if (binding.gesture==Gesture::tap) return false;
    // Explicit None directions suppress inherited actions without requiring XY reports.
    return std::ranges::any_of(binding.actions,[](const Action& action) {
        return action.operation!=Operation::keys || action.keys[0]!=0;
    });
}

bool enable(logi::Mouse& mouse, const std::vector<Binding>& bindings,
            std::vector<Control>& controls) {
    for (const auto& binding : bindings) {
        const auto id = binding.control;
        if (id == thumb_wheel || std::ranges::find(controls, id, &Control::id) != controls.end()) continue;
        const bool xy = std::ranges::any_of(bindings, [=](const Binding& rule) {
            return rule.control == id && needs_raw_motion(rule);
        });
        const auto high = logi::high(id);
        const auto low = logi::low(id);
        const auto original = mouse.request(mouse.buttons, 2, {high, low});
        if (!original || logi::word(original.packet.data()) != id) return false;
        const logi::Byte restore = 0x22 | (original.packet.data()[2] & 0x11);
        controls.push_back({id, xy, restore});
        const auto flags = static_cast<logi::Byte>(xy ? 0x33 : 0x03);
        if (!mouse.request(mouse.buttons, 3, {high, low, flags})) return false;
        const auto current = mouse.request(mouse.buttons, 2, {high, low});
        if (!current || logi::word(current.packet.data()) != id ||
            (current.packet.data()[2] & (xy ? 0x11 : 0x01)) != (xy ? 0x11 : 0x01)) return false;
    }
    return true;
}

bool restore(logi::Mouse& mouse, const std::vector<Control>& controls, const std::optional<Wheel>& wheel,
             bool shutdown=true) {
    if (shutdown) mouse.channel->finish_shutdown();
    bool restored=true;
    for (const auto& control : controls) {
        if (!mouse.request(mouse.buttons, 3, {logi::high(control.id), logi::low(control.id), control.restore})) {
            std::fprintf(stderr, "Could not restore control %04X. Reconnect the device.\n", control.id);
            restored=false;
        }
    }
    if (wheel && !mouse.request(mouse.thumb, 2, {wheel->original[0], wheel->original[1]})) {
        std::fputs("Could not restore thumb wheel reporting. Reconnect the device.\n", stderr);
        restored=false;
    }
    return restored;
}

// A Bolt receiver remains attached when its wireless mouse sleeps/reconnects.
// Therefore an outstanding ReadFile is not proof that diversion is still set.
// Read back the actual device state; only write a control that lost its flags.
bool check_reporting(logi::Mouse& mouse,std::vector<Control>& controls,std::optional<Wheel>& wheel) {
    bool repaired=false;
    const auto failed=[](const char* operation,const logi::Reply& reply,unsigned control=0){
        std::fprintf(stderr,"Reporting check detail: %s control=%04X result=%u error=%u report=%u data=%02X%02X%02X.\n",
            operation,control,static_cast<unsigned>(reply.result),reply.error,reply.packet.size,
            reply.packet.bytes[4],reply.packet.bytes[5],reply.packet.bytes[6]);
        return false;
    };
    if(mouse.wheel) {
        auto current=mouse.channel->request(mouse.device.slot,mouse.wheel,1,{},1200);
        if(!current)return failed("main-wheel",current);
        if(current.packet.data()[0]&1) {
            const auto mode=static_cast<logi::Byte>(current.packet.data()[0]&~1);
            if(!mouse.channel->request(mouse.device.slot,mouse.wheel,2,{mode},300))return false;
            current=mouse.channel->request(mouse.device.slot,mouse.wheel,1,{},300);
            if(!current || (current.packet.data()[0]&1))return false;
            repaired=true;std::fputs("Recovered standard main wheel reporting.\n",stderr);
        }
    }
    for(auto& control:controls) {
        const auto high=logi::high(control.id),low=logi::low(control.id);
        const auto wanted=static_cast<logi::Byte>(control.xy?0x11:0x01);
        auto current=mouse.channel->request(mouse.device.slot,mouse.buttons,2,{high,low},300);
        if(!current || current.packet.size<7 || logi::word(current.packet.data())!=control.id)return failed("button",current,control.id);
        if((current.packet.data()[2]&0x11)==wanted)continue;
        mapping_readiness(false);
        const auto flags=static_cast<logi::Byte>(control.xy?0x33:0x23);
        if(!mouse.channel->request(mouse.device.slot,mouse.buttons,3,{high,low,flags},300))return false;
        current=mouse.channel->request(mouse.device.slot,mouse.buttons,2,{high,low},300);
        if(!current || logi::word(current.packet.data())!=control.id || (current.packet.data()[2]&0x11)!=wanted)return false;
        cancel_gesture(control);repaired=true;
        std::fprintf(stderr,"Recovered reporting for control %04X.\n",control.id);
    }
    if(wheel) {
        auto current=mouse.channel->request(mouse.device.slot,mouse.thumb,1,{},300);
        if(!current)return failed("thumb-wheel",current);
        const auto flags=static_cast<logi::Byte>(wheel->original[1]&~1u);
        if(current.packet.data()[0]!=1 || current.packet.data()[1]!=flags) {
            mapping_readiness(false);
            if(!mouse.channel->request(mouse.device.slot,mouse.thumb,2,{1,flags},300))return false;
            current=mouse.channel->request(mouse.device.slot,mouse.thumb,1,{},300);
            if(!current || current.packet.data()[0]!=1 || current.packet.data()[1]!=flags)return false;
            wheel->previous=nullptr;wheel->remainder=0;repaired=true;
            std::fputs("Recovered thumb wheel reporting.\n",stderr);
        }
    }
    if(health){InterlockedExchange64(&health->checked,static_cast<LONG64>(GetTickCount64()));
        if(repaired)InterlockedIncrement(&health->repairs);
        mapping_readiness(true);}
    return true;
}

constexpr std::array setting_names{L"smartshift",L"ratchet",L"hires",L"invert",L"thumb-invert"};
unsigned setting_group_mask(SettingKey key) {return key<=SettingKey::ratchet?3u:key<=SettingKey::invert?12u:16u;}
bool load_setting_targets(const wchar_t* path,const logi::Mouse& mouse) {
    if(settings_loaded)return true;
    if(path&&!logi::read_saved_settings(path,mouse,desired_settings))return false;
    settings_loaded=true;settings_saved=true;
    for(unsigned i=0;i<5;++i)if(desired_settings[i])settings_unverified_mask|=1u<<i;
    return true;
}
void publish_setting_observation(const SettingResult& result,unsigned invalid=0) {
    parameter_readiness(settings_loaded&&settings_saved&&settings_unverified_mask==0&&!settings_conflict);
    if(setting_commands)setting_commands->observe(result,parameters_restored,settings_saved,invalid);
}
SettingResult observe_setting_group(logi::Mouse& mouse,const std::optional<Wheel>& wheel,SettingKey key) {
    SettingResult result;result.key=key;
    if(key<=SettingKey::ratchet&&mouse.shift) {
        const auto reply=mouse.request(mouse.shift,mouse.enhanced_shift?1:0);
        if(reply&&reply.packet.data().size()>=2&&reply.packet.data()[1]&&(reply.packet.data()[0]==1||reply.packet.data()[0]==2)) {
            result.observed[0]=reply.packet.data()[1]==255?0:reply.packet.data()[1];
            result.observed[1]=reply.packet.data()[0]==2;result.observed_mask=3;
        }
    } else if(key>=SettingKey::hires&&key<=SettingKey::invert&&mouse.wheel) {
        const auto reply=mouse.request(mouse.wheel,1);
        if(reply&&!reply.packet.data().empty()) {
            result.observed[2]=(reply.packet.data()[0]&2)!=0;result.observed[3]=(reply.packet.data()[0]&4)!=0;
            result.observed_mask=12;
        }
    } else if(key==SettingKey::thumb_invert&&mouse.thumb) {
        const auto reply=mouse.request(mouse.thumb,1);
        if(reply&&reply.packet.data().size()>=2) {
            if(!wheel || (reply.packet.data()[0]==1&&!(reply.packet.data()[1]&1))) {
                result.observed[4]=wheel?wheel->logical_invert:(reply.packet.data()[1]&1);
                result.observed_mask=16;result.software=wheel.has_value();
            }
        }
    }
    return result;
}
bool restore_owner_settings(const wchar_t* path,logi::Mouse& mouse) {
    settings_conflict=options_running();
    if(settings_conflict){parameter_readiness(false);return false;}
    if(!load_setting_targets(path,mouse)){parameter_readiness(false);return false;}
    const std::optional<Wheel> native;
    for(const auto key:{SettingKey::smartshift,SettingKey::hires,SettingKey::thumb_invert}) {
        auto observed=observe_setting_group(mouse,native,key);
        const auto mask=setting_group_mask(key);
        bool changed=false;
        for(unsigned i=0;i<5;++i)if((mask&(1u<<i))&&desired_settings[i]&&
            (!(observed.observed_mask&(1u<<i))||observed.observed[i]!=*desired_settings[i]))changed=true;
        if(changed) {
            if(key==SettingKey::smartshift) {
                // Restore the coupled snapshot together, including a pre-existing
                // protocol-valid threshold above the UI's 0..50 editing range.
                const auto mode=desired_settings[1]?(*desired_settings[1]?2u:1u):0u;
                const auto threshold=desired_settings[0]?(*desired_settings[0]?*desired_settings[0]:255u):0u;
                mouse.request(mouse.shift,mouse.enhanced_shift?2:1,{logi::low(mode),logi::low(threshold),0});
            } else for(unsigned i=2;i<5;++i)if((mask&(1u<<i))&&desired_settings[i])logi::set(mouse,setting_names[i],*desired_settings[i]);
            observed=observe_setting_group(mouse,native,key);
        }
        for(unsigned i=0;i<5;++i)if((mask&(1u<<i))&&desired_settings[i]) {
            if((observed.observed_mask&(1u<<i))&&observed.observed[i]==*desired_settings[i])settings_unverified_mask&=~(1u<<i);
            else settings_unverified_mask|=1u<<i;
        }
        publish_setting_observation(observed,mask);
    }
    if(!settings_unverified_mask&&!settings_saved)settings_saved=path&&logi::remember_settings(path,mouse,desired_settings);
    publish_setting_observation({});return parameters_restored;
}

SettingResult handle_setting_command(logi::Mouse* mouse,std::optional<Wheel>& wheel,const wchar_t* path,
    settings_ipc::Request request,auto&& cancel) {
    SettingResult result;result.request_id=request.id;result.revision=request.revision;result.key=request.key;result.requested=request.value;
    const auto index=static_cast<unsigned>(request.key);
    if(index>=5||request.value>(index==0?50u:1u)){result.status=SettingStatus::invalid_value;return result;}
    if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){result.status=SettingStatus::stopped;return result;}
    if(!mouse||!mouse->channel){result.status=SettingStatus::device_unavailable;return result;}
    settings_conflict=options_running();
    if(settings_conflict){parameter_readiness(false);if(setting_commands)setting_commands->observe({},false,settings_saved,31);result.status=SettingStatus::official_conflict;return result;}
    if((index<2&&!mouse->shift)||(index>=2&&index<4&&!mouse->wheel)||(index==4&&!mouse->thumb)){result.status=SettingStatus::unsupported;return result;}
    if(!load_setting_targets(path,*mouse)){parameter_readiness(false);result.status=SettingStatus::save_failed;return result;}
    const auto mask=setting_group_mask(request.key);
    cancel();desired_settings[index]=request.value;settings_saved=false;settings_unverified_mask|=1u<<index;
    publish_setting_observation(result,mask);
    bool written=false;
    if(index==4&&wheel) {
        // Diverted reports retain a fixed raw orientation. Apply inversion once,
        // in the software direction, and retain the requested native exit state.
        const auto reply=mouse->request(mouse->thumb,1);
        if(reply&&reply.packet.data().size()>=2) {
            const auto flags=static_cast<logi::Byte>(reply.packet.data()[1]&~1u);
            written=static_cast<bool>(mouse->request(mouse->thumb,2,{1,flags}));
            if(wheel->logical_invert!=(request.value!=0))wheel->direction=-wheel->direction;
            wheel->logical_invert=request.value!=0;wheel->original[1]=static_cast<logi::Byte>(flags|request.value);
            wheel->remainder=0;wheel->previous=nullptr;
        }
    } else written=logi::set(*mouse,setting_names[index],request.value);
    if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){result.status=SettingStatus::stopped;return result;}
    const auto observed=observe_setting_group(*mouse,wheel,request.key);
    result.observed=observed.observed;result.observed_mask=observed.observed_mask;result.software=observed.software;
    result.applied=(observed.observed_mask&(1u<<index))&&observed.observed[index]==request.value;
    if(result.applied) {
        // Save the actually observed coupled mode/threshold, not a guessed sibling.
        if(index<2)for(unsigned i=0;i<2;++i)if(observed.observed_mask&(1u<<i))desired_settings[i]=observed.observed[i];
        for(unsigned i=0;i<5;++i)if((mask&(1u<<i))&&desired_settings[i]&&(observed.observed_mask&(1u<<i))&&observed.observed[i]==*desired_settings[i])settings_unverified_mask&=~(1u<<i);
        result.saved=!settings_unverified_mask&&path&&logi::remember_settings(path,*mouse,desired_settings);settings_saved=result.saved;
        result.status=result.saved?SettingStatus::applied:SettingStatus::save_failed;
    } else {
        // A known mismatch is the actual device state, not a deferred promise to
        // apply a rejected target later. Unknown readback retains only an unsaved
        // in-memory recovery intention; no other successful key can persist it.
        for(unsigned i=0;i<5;++i)if((mask&(1u<<i))&&(observed.observed_mask&(1u<<i))) {
            desired_settings[i]=observed.observed[i];settings_unverified_mask&=~(1u<<i);
        }
        result.status=written?SettingStatus::readback_failed:SettingStatus::write_failed;
    }
    publish_setting_observation(result,mask);return result;
}
bool apply_wheel_mode(logi::Mouse& mouse,unsigned value) {
    // Explicit actions use the same in-process owner and persistence path; periodic
    // checks therefore cannot undo a successful top-button WheelMode change.
    std::optional<Wheel> none;
    if(setting_commands)setting_commands->invalidate(SettingKey::ratchet);
    const auto result=handle_setting_command(&mouse,none,agent_mapping,{0,SettingKey::ratchet,value,0},[]{});
    return result.applied;
}

// One outstanding control transaction; read() remains the only notification reader.
struct PeriodicHealth {
    enum class Stage { idle,main_read,main_write,main_verify,button_read,button_write,button_verify,
                       thumb_read,thumb_write,thumb_verify,shift_read,shift_write,shift_verify,dpi_read,dpi_write,dpi_verify,complete };
    enum class Progress { pending,complete,failed };
    Stage stage{Stage::idle};
    std::size_t index{};
    bool inflight{},repaired{},settings_valid{true},save_target{};
    logi::Byte main_mode{},thumb_mode{},thumb_flags{},shift_mode{},shift_threshold{};
    bool parameters_allowed{true};
    std::optional<unsigned> target;
    bool active() const{return stage!=Stage::idle;}
    void reset(){*this={};}
    void start(logi::Mouse& mouse,const wchar_t* path) {
        reset();stage=mouse.wheel?Stage::main_read:Stage::button_read;
        settings_conflict=options_running();parameters_allowed=!settings_conflict;
        if(!settings_loaded)load_setting_targets(path,mouse);
        if(!parameters_allowed)parameter_readiness(false);
        if(desired_dpi){target=desired_dpi;save_target=!desired_dpi_saved;}
        else if(!dpi_restored && path) {
            auto file=std::filesystem::path(path);file.replace_extension(L".dpi");
            errno=0;std::ifstream saved(file);std::string device;unsigned value{};
            if(!saved.is_open())settings_valid=errno==ENOENT;
            else if(std::getline(saved,device) && (saved>>value) && device==mouse.device.name)target=value;
            else settings_valid=false;
        }
        if(target && !std::ranges::contains(mouse.dpi_choices,*target)){target.reset();settings_valid=false;}
    }
    Progress pump(logi::Mouse& mouse,std::vector<Control>& controls,std::optional<Wheel>& wheel,
                  const wchar_t* path,auto&& cancel_repair,auto&& observe_dpi) {
        const auto observe_parameters=[&](SettingResult observed) {
            for(unsigned i=0;i<5;++i)if((observed.observed_mask&(1u<<i))&&desired_settings[i]) {
                if(observed.observed[i]==*desired_settings[i])settings_unverified_mask&=~(1u<<i);
                else settings_unverified_mask|=1u<<i;
            }
            publish_setting_observation(observed);
        };
        const auto complete_dpi=[&](unsigned observed) {
            observe_dpi(std::optional<unsigned>{observed});
            bool saved=settings_valid;
            if(target && observed!=*target)saved=false;
            else if(save_target && target) {
                saved=path && logi::remember_dpi(path,mouse,*target);
                desired_dpi_saved=saved;
            }
            dpi_readiness(saved);stage=Stage::complete;
        };
        const auto failed=[&](const logi::Reply& reply) {
            std::fprintf(stderr,"Asynchronous reporting check failed: stage=%u result=%u error=%u.\n",
                static_cast<unsigned>(stage),static_cast<unsigned>(reply.result),reply.error);
            reset();return Progress::failed;
        };
        if(inflight) {
            const auto completed=mouse.channel->poll_control();
            if(!completed)return Progress::pending;
            inflight=false;const auto& reply=*completed;
            const bool dpi_stage=stage==Stage::dpi_read || stage==Stage::dpi_write || stage==Stage::dpi_verify;
            const bool shift_stage=stage==Stage::shift_read || stage==Stage::shift_write || stage==Stage::shift_verify;
            if(!reply) {
                if(shift_stage) {
                    if(stage==Stage::shift_write)stage=Stage::shift_verify;
                    else {
                        for(unsigned i=0;i<2;++i)if(desired_settings[i])settings_unverified_mask|=1u<<i;
                        publish_setting_observation({},3);stage=Stage::dpi_read;
                    }
                } else if(!dpi_stage)return failed(reply);
                else if(stage==Stage::dpi_write){stage=Stage::dpi_verify;}
                else {
                // Unknown DPI pauses XY only; ordinary reporting was already verified.
                dpi_readiness(false);observe_dpi(std::optional<unsigned>{});stage=Stage::complete;
                }
            } else {
                const auto data=reply.packet.size>=4?reply.packet.data():std::span<const logi::Byte>{};
                switch(stage) {
                case Stage::main_read:
                    if(data.empty())return failed(reply);
                    main_mode=static_cast<logi::Byte>(data[0]&~1);
                    if(parameters_allowed)for(unsigned i=2;i<4;++i)if(desired_settings[i]) {
                        const unsigned bit=i==2?2:4;main_mode=static_cast<logi::Byte>(*desired_settings[i]?main_mode|bit:main_mode&~bit);
                    }
                    if(data[0]!=main_mode){cancel_repair();mapping_readiness(false);stage=Stage::main_write;}
                    else {SettingResult observed;observed.observed_mask=12;observed.observed[2]=(data[0]&2)!=0;observed.observed[3]=(data[0]&4)!=0;observe_parameters(observed);stage=Stage::button_read;}
                    break;
                case Stage::main_write:stage=Stage::main_verify;break;
                case Stage::main_verify:
                    if(data.empty() || data[0]!=main_mode)return failed(reply);
                    {SettingResult observed;observed.observed_mask=12;observed.observed[2]=(data[0]&2)!=0;observed.observed[3]=(data[0]&4)!=0;observe_parameters(observed);}
                    repaired=true;stage=Stage::button_read;break;
                case Stage::button_read:
                case Stage::button_verify: {
                    const auto& control=controls[index];const auto wanted=control.xy?0x11:0x01;
                    if(data.size()<3 || logi::word(data)!=control.id)return failed(reply);
                    if((data[2]&0x11)!=wanted) {
                        if(stage==Stage::button_verify)return failed(reply);
                        cancel_repair();mapping_readiness(false);stage=Stage::button_write;
                    } else {if(stage==Stage::button_verify)repaired=true;++index;stage=Stage::button_read;}
                    break;
                }
                case Stage::button_write:stage=Stage::button_verify;break;
                case Stage::thumb_read:
                case Stage::thumb_verify:
                    if(data.size()<2)return failed(reply);
                    if(stage==Stage::thumb_read) {
                        thumb_mode=wheel?1:0;thumb_flags=data[1];
                        if(wheel)thumb_flags=static_cast<logi::Byte>(thumb_flags&~1u);
                        else if(parameters_allowed&&desired_settings[4])thumb_flags=static_cast<logi::Byte>((thumb_flags&~1u)|*desired_settings[4]);
                    }
                    if(data[0]!=thumb_mode || data[1]!=thumb_flags) {
                        if(stage==Stage::thumb_verify)return failed(reply);
                        cancel_repair();mapping_readiness(false);stage=Stage::thumb_write;
                    } else {
                        if(stage==Stage::thumb_verify)repaired=true;
                        if(wheel&&parameters_allowed&&desired_settings[4]) {
                            const bool inverted=*desired_settings[4]!=0;
                            if(wheel->logical_invert!=inverted){cancel_repair();wheel->direction=-wheel->direction;wheel->remainder=0;wheel->previous=nullptr;}
                            wheel->logical_invert=inverted;wheel->original[1]=static_cast<logi::Byte>((data[1]&~1u)|*desired_settings[4]);
                        }
                        SettingResult observed;observed.observed_mask=16;observed.software=wheel.has_value();observed.observed[4]=wheel?wheel->logical_invert:(data[1]&1);observe_parameters(observed);
                        stage=Stage::shift_read;
                    }
                    break;
                case Stage::thumb_write:stage=Stage::thumb_verify;break;
                case Stage::shift_read:
                case Stage::shift_verify: {
                    if(data.size()<2||!data[1]||(data[0]!=1&&data[0]!=2)) {
                        for(unsigned i=0;i<2;++i)if(desired_settings[i])settings_unverified_mask|=1u<<i;
                        publish_setting_observation({},3);stage=Stage::dpi_read;break;
                    }
                    SettingResult observed;observed.observed_mask=3;observed.observed[0]=data[1]==255?0:data[1];observed.observed[1]=data[0]==2;
                    observe_parameters(observed);
                    if(stage==Stage::shift_read&&parameters_allowed&&(settings_unverified_mask&3)) {
                        shift_mode=desired_settings[1]?logi::low(*desired_settings[1]?2:1):0;
                        shift_threshold=desired_settings[0]?logi::low(*desired_settings[0]?*desired_settings[0]:255):0;
                        cancel_repair();stage=Stage::shift_write;
                    } else stage=Stage::dpi_read;
                    break;
                }
                case Stage::shift_write:stage=Stage::shift_verify;break;
                case Stage::dpi_read:
                case Stage::dpi_verify: {
                    if(data.size()<5){dpi_readiness(false);observe_dpi(std::optional<unsigned>{});stage=Stage::complete;break;}
                    const auto current=logi::word(data.subspan(1));const auto observed=current?current:logi::word(data.subspan(3));
                    if(!observed){dpi_readiness(false);observe_dpi(std::optional<unsigned>{});stage=Stage::complete;break;}
                    if(stage==Stage::dpi_read && target && observed!=*target) {
                        observe_dpi(std::optional<unsigned>{observed});cancel_repair();dpi_readiness(false);stage=Stage::dpi_write;
                    } else complete_dpi(observed);
                    break;
                }
                case Stage::dpi_write:stage=Stage::dpi_verify;break;
                default:break;
                }
            }
        }
        if(stage==Stage::button_read && index>=controls.size())stage=(wheel||(mouse.thumb&&desired_settings[4]))?Stage::thumb_read:Stage::shift_read;
        if(stage==Stage::shift_read&&(!mouse.shift||(!desired_settings[0]&&!desired_settings[1])))stage=Stage::dpi_read;
        if(stage==Stage::dpi_read && !mouse.dpi)stage=Stage::complete;
        if(stage==Stage::complete) {
            if(parameters_allowed&&settings_loaded&&!settings_unverified_mask&&!settings_saved)
                settings_saved=path&&logi::remember_settings(path,mouse,desired_settings);
            publish_setting_observation({});
            if(health){InterlockedExchange64(&health->checked,static_cast<LONG64>(GetTickCount64()));if(repaired)InterlockedIncrement(&health->repairs);}
            mapping_readiness(true);reset();return Progress::complete;
        }
        logi::Result started=logi::Result::io_error;
        if(parameters_allowed&&(stage==Stage::main_write||stage==Stage::thumb_write||stage==Stage::shift_write)&&options_running()) {
            settings_conflict=true;parameters_allowed=false;parameter_readiness(false);
            if(setting_commands)setting_commands->observe({},false,settings_saved,31);
            stage=stage==Stage::main_write?Stage::main_read:stage==Stage::thumb_write?Stage::thumb_read:Stage::dpi_read;
            return Progress::pending;
        }
        const auto begin=[&](logi::Byte feature,logi::Byte function,std::initializer_list<logi::Byte> parameters,DWORD timeout=300) {
            return mouse.channel->begin_control(mouse.device.slot,feature,function,parameters,timeout);
        };
        switch(stage) {
        case Stage::main_read:started=begin(mouse.wheel,1,{},1200);break;
        case Stage::main_write:started=begin(mouse.wheel,2,{main_mode});break;
        case Stage::main_verify:started=begin(mouse.wheel,1,{});break;
        case Stage::button_read:
        case Stage::button_verify:started=begin(mouse.buttons,2,{logi::high(controls[index].id),logi::low(controls[index].id)});break;
        case Stage::button_write:started=begin(mouse.buttons,3,{logi::high(controls[index].id),logi::low(controls[index].id),static_cast<logi::Byte>(controls[index].xy?0x33:0x23)});break;
        case Stage::thumb_read:
        case Stage::thumb_verify:started=begin(mouse.thumb,1,{});break;
        case Stage::thumb_write:started=begin(mouse.thumb,2,{thumb_mode,thumb_flags});break;
        case Stage::shift_read:
        case Stage::shift_verify:started=begin(mouse.shift,mouse.enhanced_shift?1:0,{});break;
        case Stage::shift_write:started=begin(mouse.shift,mouse.enhanced_shift?2:1,{shift_mode,shift_threshold,0});break;
        case Stage::dpi_read:
        case Stage::dpi_verify:started=begin(mouse.dpi,2,{0},1200);break;
        case Stage::dpi_write:started=begin(mouse.dpi,3,{0,logi::high(*target),logi::low(*target)},1200);break;
        default:return Progress::pending;
        }
        if(started==logi::Result::busy)return Progress::pending;
        if(started!=logi::Result::ok)return failed(logi::Reply{started});
        inflight=true;return Progress::pending;
    }
};

std::map<unsigned,bool> control_layout(const Mappings& mappings) {
    std::map<unsigned,bool> layout;
    for (const auto& binding:mappings.bindings)
        layout[binding.control]|=needs_raw_motion(binding);
    return layout;
}
void unregister_keys(std::size_t& count) {
    for (std::size_t i=1;i<=count;++i) UnregisterHotKey(nullptr,static_cast<int>(i));
    count=0;
    MSG discarded{};
    while (PeekMessageW(&discarded,nullptr,WM_HOTKEY,WM_HOTKEY,PM_REMOVE)) {}
}
bool register_keys(const std::vector<Hotkey>& hotkeys,std::size_t& count) {
    for (const auto& hotkey:hotkeys) {
        if (count==0xBFFF || !RegisterHotKey(nullptr,static_cast<int>(count+1),hotkey.modifiers|MOD_NOREPEAT,hotkey.key)) {
            std::fprintf(stderr,"Cannot register global shortcut %zu (Win32 %lu).\n",count+1,GetLastError());
            unregister_keys(count);return false;
        }
        ++count;
    }
    return true;
}

bool restore_owner_dpi(const wchar_t* path,logi::Mouse& mouse) {
    if(!desired_dpi)return !path || logi::restore_saved_dpi(path,mouse);
    // A failed save must not allow reconnect/watchdog to replay the old sidecar.
    const auto value=*desired_dpi;
    if(logi::get(mouse,L"dpi")!=value) {
        logi::set(mouse,L"dpi",value);
        if(logi::get(mouse,L"dpi")!=value)return false;
    }
    if(!desired_dpi_saved)desired_dpi_saved=path && logi::remember_dpi(path,mouse,value);
    return desired_dpi_saved;
}

DpiCommandResult handle_dpi_command(logi::Mouse* mouse,const wchar_t* path,dpi_ipc::Request request,auto&& cancel) {
    DpiCommandResult result;result.request_id=request.id;result.requested=request.value;
    if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){result.status=DpiCommandStatus::stopped;return result;}
    if(!mouse){result.status=DpiCommandStatus::device_unavailable;return result;}
    if(!std::ranges::contains(mouse->dpi_choices,request.value)){result.status=DpiCommandStatus::invalid_value;return result;}
    cancel();
    desired_dpi=request.value;desired_dpi_saved=false;dpi_readiness(false);
    const bool written=logi::set(*mouse,L"dpi",request.value);
    if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){result.status=DpiCommandStatus::stopped;return result;}
    const auto observed=logi::get(*mouse,L"dpi");
    result.observed_valid=observed.has_value();result.observed=observed.value_or(0);
    result.applied=observed==request.value;
    if(!result.applied){result.status=written?DpiCommandStatus::readback_failed:DpiCommandStatus::write_failed;return result;}
    result.saved=path && logi::remember_dpi(path,*mouse,request.value);
    desired_dpi_saved=result.saved;dpi_readiness(result.saved);
    result.status=result.saved?DpiCommandStatus::applied:DpiCommandStatus::save_failed;
    return result;
}

void events(logi::Mouse& mouse, std::vector<Control>& controls, std::optional<Wheel>& wheel,
            Mappings& mappings, const wchar_t* path,std::size_t& registered) {
    const auto& bindings = mappings.bindings;
    const auto& hotkeys = mappings.hotkeys;
    const auto initial_dpi=mouse.channel?logi::get(mouse,L"dpi"):std::optional<unsigned>{};
    auto dpi=initial_dpi.value_or(0);
    bool dpi_known=initial_dpi.has_value();
    if(mouse.channel && !dpi_known) {
        dpi_readiness(false);
        std::fputs("Pointer speed unreadable; mappings remain active, XY gestures paused.\n",stderr);
    }
    auto dpi_path=path?std::filesystem::path(path):std::filesystem::path{};
    if(path)dpi_path.replace_extension(L".dpi");
    const auto dpi_stamp=[&] {
        std::error_code error;
        return path?std::filesystem::last_write_time(dpi_path,error):std::filesystem::file_time_type::min();
    };
    auto last_dpi_stamp=dpi_stamp();
    const auto cancel_dpi_gestures=[&] {
        for(auto& control:controls)if(control.xy)cancel_gesture(control);
    };
    Sequences sequences{{}, mappings};
    struct SessionCleanup {
        std::vector<Control>& controls;Sequences& sequences;
        ~SessionCleanup(){for(auto& control:controls)cancel_gesture(control);sequences.cancel();}
    } cleanup{controls,sequences};
    Foreground foreground;
    std::vector<std::size_t> pending;
    bool await_neutral{};
    unsigned dpi_window_packets{};
    unsigned reload_window_packets{};
    PeriodicHealth periodic;
    bool retry_health{},repair_quarantine{},neutral_report{},sidecar_refresh_pending{};
    const auto cancel_health=[&] {
        if(mouse.channel && !mouse.channel->cancel_control())return false;
        periodic.reset();repair_quarantine=false;return true;
    };
    const auto cancel_notification_loss=[&] {
        if(!mouse.channel || !mouse.channel->pending_notification_loss())return false;
        const auto lost=mouse.channel->take_notification_loss();
        cancel_health();retry_health=true;
        dpi_window_packets=0;
        reload_window_packets=0;
        sequences.cancel();pending.clear();
        for(auto& control:controls) {
            cancel_gesture(control,false);
        }
        if(wheel){wheel->previous=nullptr;wheel->remainder=0;}
        await_neutral=true;mapping_readiness(false);
        std::fprintf(stderr,"Input notification loss: %llu; discarded=%llu. Actions cancelled; waiting for neutral buttons.\n",
            static_cast<unsigned long long>(lost),static_cast<unsigned long long>(mouse.channel->notification_discards()));
        return true;
    };
    if (mouse.channel) {
        mouse.channel->keep_notifications();
        mouse.channel->wake_on_command(dpi_commands?dpi_commands->wake():nullptr);
    }
    auto next_check=GetTickCount64()+3000;
    ULONGLONG health_deferred_since{};
    LASTINPUTINFO last_input{sizeof(LASTINPUTINFO),0};
    GetLastInputInfo(&last_input);
    auto activity=last_input.dwTime;
    LONG64 last_checked_event{};
    for (;;) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {cancel_health();return;}
        for(auto& control:controls)if(control.session.active()&&!control.owner.current())cancel_gesture(control);
        const bool dpi_waiting=dpi_commands && dpi_commands->waiting();
        const bool setting_waiting=setting_commands && setting_commands->waiting();
        const bool command_waiting=dpi_waiting || setting_waiting;
        const bool reload_waiting=path && ((reload_commands && reload_commands->waiting()) || WaitForSingleObject(reload_event,0)==WAIT_OBJECT_0);
        const bool mouse_action_waiting=sequences.needs_mouse();
        bool sync_ready=true;
        if(command_waiting || reload_waiting || mouse_action_waiting || await_neutral || sidecar_refresh_pending) {
            const bool interrupted=periodic.active() || (mouse.channel && mouse.channel->control_pending());
            sync_ready=cancel_health();
            if(interrupted){retry_health=true;next_check=GetTickCount64();}
        }
        if(sync_ready && sidecar_refresh_pending) {
            sidecar_refresh_pending=false;retry_health=true;next_check=GetTickCount64();
        }
        if(sync_ready && neutral_report) {
            neutral_report=false;await_neutral=false;
            if(!check_reporting(mouse,controls,wheel)) {
                if(cancel_notification_loss())continue;
                mapping_readiness(false);return;
            }
            repair_quarantine=false;
        }
        // Drain already queued input first, then serialize the requested hardware change
        // before routine health traffic. No GUI/CLI writer participates in this section.
        bool owner_serviced=false;
        if(sync_ready && dpi_waiting && (!mouse.channel || !mouse.channel->pending_notifications())) {
            if(const auto request=dpi_commands->take()) {
                owner_serviced=true;
                const auto result=handle_dpi_command(mouse.channel?&mouse:nullptr,path,*request,[&] {
                    cancel_dpi_gestures();sequences.cancel();pending.clear();
                });
                if(result.observed_valid){dpi=result.observed;dpi_known=true;}else if(result.status!=DpiCommandStatus::invalid_value){dpi_known=false;}
                // HID commands can collect notifications while waiting for their replies.
                // None of those XY packets may start/end a gesture using the new scale.
                dpi_window_packets=mouse.channel?mouse.channel->pending_notification_count():0;
                if(dpi_window_packets)for(auto& control:controls)if(control.xy)control.wait_release=true;
                last_dpi_stamp=dpi_stamp();
                dpi_commands->finish(result);
                cancel_notification_loss();
            }
        }
        if(sync_ready && setting_waiting && (!mouse.channel || !mouse.channel->pending_notifications())) {
            if(dpi_commands&&dpi_commands->wake())ResetEvent(dpi_commands->wake());
            if(const auto request=setting_commands->take()) {
                owner_serviced=true;
                const auto result=handle_setting_command(mouse.channel?&mouse:nullptr,wheel,path,*request,[&] {
                    cancel_dpi_gestures();sequences.cancel();pending.clear();
                    if(wheel){wheel->previous=nullptr;wheel->remainder=0;}
                });
                reload_window_packets=mouse.channel?mouse.channel->pending_notification_count():0;
                cancel_notification_loss();
                setting_commands->finish(result);
            }
        }
        // A different owner slot may have been published while this handler reset
        // the shared command event. Recheck both slots before entering a wait.
        if(owner_serviced)continue;
        if(sync_ready && mouse.channel && !await_neutral && !periodic.active() && !command_waiting && !reload_waiting && !mouse_action_waiting && GetTickCount64()>=next_check) {
            GetLastInputInfo(&last_input);
            const auto device_events=health?std::atomic_ref<LONG64>(health->events).load(std::memory_order_relaxed):0;
            const bool occupied=dpi_window_packets || std::ranges::any_of(controls,[](const auto& c){return c.down;}) || sequences.needs_mouse();
            if(occupied && !health_deferred_since)health_deferred_since=GetTickCount64();
            if(!occupied)health_deferred_since=0;
            const bool deferred=occupied && GetTickCount64()-health_deferred_since<5000;
            const bool active=!deferred && (retry_health || last_input.dwTime!=activity || device_events!=last_checked_event);
            if(!deferred){activity=last_input.dwTime;last_checked_event=device_events;health_deferred_since=0;}
            // Do not wake a sleeping wireless mouse just to poll it. Check after
            // resumed normal input or a HID++ event, including standard movement
            // when custom button reporting was reset and no HID++ event arrives.
            if(active){periodic.start(mouse,path);retry_health=false;}
            if(await_neutral)mapping_readiness(false);
            next_check=GetTickCount64()+(deferred?500:3000);
        }
        if(sync_ready && periodic.active() && !command_waiting && !reload_waiting && !mouse_action_waiting && !await_neutral) {
            const auto progress=periodic.pump(mouse,controls,wheel,path,[&] {
                sequences.cancel();pending.clear();repair_quarantine=true;
                for(auto& control:controls) {
                    cancel_gesture(control);
                }
                if(wheel){wheel->previous=nullptr;wheel->remainder=0;}
            },[&](std::optional<unsigned> observed) {
                if(!observed){cancel_dpi_gestures();dpi_known=false;return;}
                if(!dpi_known || dpi!=*observed)cancel_dpi_gestures();
                dpi=*observed;dpi_known=true;
            });
            if(progress==PeriodicHealth::Progress::failed) {
                if(cancel_notification_loss())continue;
                mapping_readiness(false);if(health)InterlockedIncrement(&health->failures);
                return;
            }
            if(progress==PeriodicHealth::Progress::complete) {
                repair_quarantine=false;last_dpi_stamp=dpi_stamp();next_check=GetTickCount64()+3000;
            }
        }
        if (sync_ready && reload_waiting) {
            ResetEvent(reload_event);
            const auto request=reload_commands?reload_commands->take():std::optional<reload_ipc::Request>{};
            const auto finish=[&](ReloadStatus status){if(request)reload_commands->finish(*request,status);};
            Mappings next;
            const auto prepared=prepare_reload_snapshot(path,request,next);
            if(prepared!=ReloadStatus::applied || (!mouse.channel && !next.bindings.empty())) {
                finish(prepared==ReloadStatus::applied?ReloadStatus::device_unavailable:prepared);
                std::fputs("Reload rejected; previous mappings remain active.\n",stderr);std::fflush(stderr);
                continue;
            }
            // A prior loss/partially cancelled reporting repair is still unresolved.
            // Do not turn a table-only swap into an ACK that claims device readiness.
            if(request && mouse.channel && (await_neutral || repair_quarantine ||
                (health && !InterlockedCompareExchange(&health->mappings_ready,0,0)))) {
                finish(ReloadStatus::device_unavailable);
                continue;
            }
            if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){finish(ReloadStatus::stopped);return;}
            unregister_keys(registered);
            if (!register_keys(next.hotkeys,registered)) {
                if (!register_keys(hotkeys,registered)) {mapping_readiness(false);finish(ReloadStatus::hotkey_failed);SetEvent(stop_event);return;}
                finish(ReloadStatus::hotkey_failed);
                std::fputs("Reload rejected; previous shortcuts restored.\n",stderr);std::fflush(stderr);
                continue;
            }
            const bool hardware_changed=control_layout(mappings)!=control_layout(next);
            std::vector<unsigned> held_controls;
            // A replacement cancels an old gesture; it must never synthesize its END action.
            for(auto& control:controls) {
                cancel_gesture(control);
                if(control.wait_release)held_controls.push_back(control.id);
            }
            sequences.cancel();pending.clear();
            const auto fail_hardware=[&](ReloadStatus status) {
                mapping_readiness(false);
                unregister_keys(registered);
                if(!register_keys(hotkeys,registered))SetEvent(stop_event);
                finish(status);
            };
            if (hardware_changed && mouse.channel) {
                mapping_readiness(false);
                if (!restore(mouse,controls,wheel,false)) {
                    fail_hardware(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0?ReloadStatus::stopped:ReloadStatus::hardware_failed);
                    std::fputs("Reload reporting restore failed; reconnecting previous mappings.\n",stderr);
                    return;
                }
                // Removed controls also need readback: an ACK alone is not proof the
                // device returned to the prior reporting state.
                bool verified=true;
                for(const auto& control:controls) {
                    const auto current=mouse.request(mouse.buttons,2,{logi::high(control.id),logi::low(control.id)});
                    verified=verified && current && logi::word(current.packet.data())==control.id &&
                        (current.packet.data()[2]&0x11)==(control.restore&0x11);
                }
                if(wheel) {
                    const auto current=mouse.request(mouse.thumb,1);
                    verified=verified && current && current.packet.data()[0]==wheel->original[0] &&
                        (current.packet.data()[1]&1)==(wheel->original[1]&1);
                }
                if(!verified){fail_hardware(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0?ReloadStatus::stopped:ReloadStatus::hardware_failed);return;}
                controls.clear();wheel.reset();
                const bool wants=std::ranges::any_of(next.bindings,[](const auto& binding){return binding.control==thumb_wheel;});
                if (!enable(mouse,next.bindings,controls) || (wants && !divert_wheel(mouse,wheel,next.thumb_speed.value_or(100)))) {
                    fail_hardware(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0?ReloadStatus::stopped:ReloadStatus::hardware_failed);
                    std::fputs("Reload hardware activation failed; reconnecting previous mappings.\n",stderr);
                    return;
                }
                dpi_window_packets=0;
                for(auto& control:controls)control.wait_release=std::ranges::contains(held_controls,control.id);
            }
            if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){fail_hardware(ReloadStatus::stopped);return;}
            MSG stale{};
            while(PeekMessageW(&stale,nullptr,WM_HOTKEY,WM_HOTKEY,PM_REMOVE)){}
            mappings=std::move(next);
            // Preserve reconfiguration-window snapshots to learn physical releases;
            // suppress their actions instead of discarding them and swallowing a fresh click.
            reload_window_packets=mouse.channel?mouse.channel->pending_notification_count():0;
            for (auto& control:controls) {control.down=false;control.panning=false;control.x=control.y=0;}
            if (wheel) {wheel->previous=nullptr;wheel->remainder=0;}
            if (live_thumb_speed) InterlockedExchange(live_thumb_speed,static_cast<LONG>(mappings.thumb_speed.value_or(100)));
            if(hardware_changed)mapping_readiness(true);
            if(request)finish(ReloadStatus::applied);
            else if(reload_commands)reload_commands->legacy_commit();
            std::printf("Reloaded: %zu bindings, %zu shortcuts; PID %lu unchanged.\n",bindings.size(),registered,GetCurrentProcessId());
            std::fflush(stdout);
        }
        if (!hotkeys.empty()) dispatch_hotkeys(hotkeys, pending, sequences, mouse);
        auto timeout = sequences.service(mouse);
        // A retired software-ID window can make begin_control busy without any I/O
        // outstanding; retain a timer wake even when the mouse sends no notifications.
        if(periodic.active() && !periodic.inflight)timeout=std::min<DWORD>(timeout,50);
        if(mouse.channel && !periodic.active() && sync_ready){const auto now=GetTickCount64();timeout=std::min<DWORD>(timeout,static_cast<DWORD>(next_check>now?next_check-now:0));}
        if (!pending.empty()) timeout = std::min<DWORD>(timeout, 16);
        if (!mouse.channel) {
            const HANDLE signals[]{stop_event,reload_event,dpi_commands?dpi_commands->wake():nullptr};
            const auto wait = MsgWaitForMultipleObjectsEx(signals[2]?3:2,signals,timeout,QS_HOTKEY,MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) return;
            continue;
        }
        const auto reply = mouse.channel->read(timeout, !hotkeys.empty(),reload_event);
        if(reply.result==logi::Result::notification_loss) {
            cancel_notification_loss();continue;
        }
        if (reply.result == logi::Result::timeout || reply.result == logi::Result::hotkey || reply.result==logi::Result::reload || reply.result==logi::Result::command || reply.result==logi::Result::control) continue;
        if (!reply) {
            mapping_readiness(false);
            if(reply.result!=logi::Result::stopped){
                if(health)InterlockedIncrement(&health->failures);
                std::fprintf(stderr,"HID read ended (%u); reconnecting.\n",static_cast<unsigned>(reply.result));
            }
            return;
        }
        const bool dpi_window=dpi_window_packets!=0;
        if(dpi_window)--dpi_window_packets;
        const bool reload_window=reload_window_packets!=0;
        if(reload_window)--reload_window_packets;
        const auto& packet = reply.packet;
        if (packet.size != 20 || packet.bytes[1] != mouse.device.slot) continue;
        // Observe sidecar changes made by older/external clients. Current GUI/CLI use
        // the owner command above, which cancels before writing the hardware.
        const auto current_stamp=dpi_stamp();
        if(current_stamp!=last_dpi_stamp) {
            last_dpi_stamp=current_stamp;sidecar_refresh_pending=true;
            cancel_dpi_gestures();dpi_known=false;dpi_readiness(false);
        }
        if(cancel_notification_loss())continue;
        const auto data = packet.data();
        const auto event = packet.bytes[3];
        if(await_neutral) {
            if(packet.bytes[2]==mouse.buttons && event==0 &&
               std::ranges::all_of(data.first(8),[](auto b){return b==0;})) {
                neutral_report=true;
            }
            continue;
        }
        if(health && !(event&15) && (packet.bytes[2]==mouse.thumb || packet.bytes[2]==mouse.buttons)) {
            InterlockedIncrement64(&health->events);InterlockedExchange64(&health->last_event,static_cast<LONG64>(GetTickCount64()));
        }
        if (wheel && packet.bytes[2] == mouse.thumb && event == 0) {
            if(repair_quarantine || reload_window || (settings_unverified_mask&16))continue;
            const auto rotation = static_cast<std::int16_t>(logi::word(data));
            if (!rotation) continue;
            const auto gesture = rotation * wheel->direction > 0 ? Gesture::right : Gesture::left;
            const auto owner=foreground.capture();
            const auto* binding = lookup(bindings, thumb_wheel, gesture, owner.application);
            const auto speed=live_thumb_speed?static_cast<unsigned>(InterlockedCompareExchange(live_thumb_speed,0,0)):
                                             mappings.thumb_speed.value_or(100);
            const auto steps = std::abs(wheel->steps(data, binding, speed));
            for (int i = 0; i < steps && binding; ++i) sequences.start(*binding, mouse,owner);
            continue;
        }
        if (packet.bytes[2] != mouse.buttons || !mouse.buttons) continue;
        if (event == 0) {
            for (auto& control : controls) {
                bool down = false;
                for (unsigned i = 0; i < 8; i += 2) down |= logi::word(data.subspan(i)) == control.id;
                if(control.session.active()&&!control.owner.current())cancel_gesture(control);
                if(repair_quarantine || reload_window || (dpi_window && control.xy)){cancel_gesture(control,false);control.wait_release=down;continue;}
                if(control.wait_release) {if(!down)control.wait_release=false;continue;}
                if (down && !control.down) {
                    if(control.xy && !dpi_known){control.wait_release=true;continue;}
                    control.x = 0;
                    control.y = 0;
                    control.pan_scrolled = false;
                    control.first_motion = mouse.device.name == "MX Master 3S";
                    control.threshold=std::max(20,static_cast<int>(dpi/12));
                    control.counts_per_pan_step=std::max(20,static_cast<int>(dpi/16));
                    control.owner=foreground.capture();
                    if(!control.owner.current()){cancel_gesture(control,false);control.wait_release=true;continue;}
                    control.session.start();
                    for(unsigned i=0;i<control.captured.size();++i)
                        control.captured[i]=lookup(bindings,control.id,static_cast<Gesture>(i),control.owner.application);
                    const auto* tap = control.captured[static_cast<std::size_t>(Gesture::tap)];
                    control.panning = tap && tap->actions.front().operation == Operation::pan;
                    control.continuous_media=tap && tap->actions.front().operation==Operation::continuous_media;
                    control.media.reset();
                    if (!control.xy && tap) sequences.start(*tap, mouse,control.owner);
                }
                if (!down && control.down && control.session.end()) {
                    if(control.panning)release_pan(control);
                    else if(control.continuous_media) {
                        if(!control.media.engaged())fire(mouse,control,Gesture::tap,sequences);
                        control.media.reset();control.continuous_media=false;
                    }
                    else if(control.xy) {
                        auto gesture = Gesture::tap;
                        if (std::max(std::abs(control.x), std::abs(control.y)) >= control.threshold) {
                            if (std::abs(control.x) >= std::abs(control.y))
                                gesture = control.x < 0 ? Gesture::left : Gesture::right;
                            else gesture = control.y < 0 ? Gesture::up : Gesture::down;
                        }
                        fire(mouse,control,gesture,sequences);
                    }
                    control.captured={};control.owner={};
                }
                control.down = down;
            }
        } else if (event == 0x10) {
            if(dpi_window || reload_window || repair_quarantine)continue;
            for (auto& control : controls) {
                if (!control.xy || !control.down) continue;
                if(!control.owner.current()){cancel_gesture(control);continue;}
                if(!control.session.progress())continue;
                if (control.first_motion) {
                    control.first_motion = false;
                    continue;
                }
                const auto x = static_cast<std::int16_t>(logi::word(data));
                const auto y = static_cast<std::int16_t>(logi::word(data.subspan(2)));
                if (control.panning) {
                    move_pan(control, x, y, control.counts_per_pan_step);
                } else if(control.continuous_media) {
                    if(const auto direction=control.media.move(x,y,GetTickCount64(),control.threshold,control.threshold))
                        fire(mouse,control,static_cast<Gesture>(*direction),sequences);
                } else {
                    control.x = std::clamp(control.x + x, -100000, 100000);
                    control.y = std::clamp(control.y + y, -100000, 100000);
                }
            }
        }
    }
}

} // namespace

bool options_running() {
    const auto processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (auto ok = Process32FirstW(processes, &entry); ok; ok = Process32NextW(processes, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"logioptionsplus_agent.exe") == 0) {
            found = true;
            break;
        }
    }
    CloseHandle(processes);
    return found;
}

namespace {

class OptionsSession {
public:
    bool pause() {
        wchar_t program_files[1024]{};
        const auto size = GetEnvironmentVariableW(L"ProgramFiles", program_files, std::size(program_files));
        if (!size || size >= std::size(program_files)) return false;
        const auto directory = std::filesystem::path(program_files) / L"LogiOptionsPlus";
        // Close the original UI first so it cannot request another Agent while we take over.
        for (const auto* name : {L"logioptionsplus.exe", L"logioptionsplus_agent.exe"}) {
            const auto expected = directory / name;
            const auto processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (processes == INVALID_HANDLE_VALUE) return false;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool ready = true;
            for (auto ok = Process32FirstW(processes, &entry); ok; ok = Process32NextW(processes, &entry)) {
                if (_wcsicmp(entry.szExeFile, expected.filename().c_str()) != 0) continue;
                const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                                                SYNCHRONIZE, FALSE, entry.th32ProcessID);
                if (!handle) {
                    // Closing the Electron main process also exits its children in this snapshot.
                    const auto error=GetLastError();
                    if (error == ERROR_INVALID_PARAMETER) continue;
                    std::fprintf(stderr,"Cannot open Options+ process %lu (Win32 %lu).\n",entry.th32ProcessID,error);
                    ready = false;
                    break;
                }
                if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
                    CloseHandle(handle);
                    continue;
                }
                wchar_t actual[1024]{};
                DWORD count = std::size(actual);
                const bool verified = QueryFullProcessImageNameW(handle, 0, actual, &count) &&
                                      _wcsicmp(actual, expected.c_str()) == 0;
                if (verified && TerminateProcess(handle, 0)) {
                    if (_wcsicmp(name,L"logioptionsplus_agent.exe")==0) restart_ = expected;
                    ready = WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0;
                } else {
                    // A sibling may be terminating: path queries can fail before the handle signals.
                    ready = WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0;
                    if (!ready) std::fprintf(stderr,"Cannot stop Options+ process %lu (verified=%d, Win32 %lu).\n",
                                             entry.th32ProcessID,verified,GetLastError());
                }
                CloseHandle(handle);
                if (!ready) break;
            }
            CloseHandle(processes);
            if (!ready) return false;
        }
        return true;
    }

    void restore() {
        if (restart_.empty() || options_running()) return;
        if(!std::filesystem::exists(restart_)) {
            std::puts("Previously paused Options+ was uninstalled; standalone input restored.");
            restart_.clear();return;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(restart_.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                           restart_.parent_path().c_str(), &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            std::puts("Options+ agent restarted.");
        } else {
            std::fprintf(stderr, "Could not restart Options+ (Win32 %lu).\n", GetLastError());
        }
        restart_.clear();
    }

    ~OptionsSession() { restore(); }

private:
    std::filesystem::path restart_;
};

} // namespace

int stop_agent() {
    const auto stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_name);
    if (!stop) return 1;
    const bool signaled = SetEvent(stop);
    CloseHandle(stop);
    return signaled ? 0 : 1;
}
bool reload_agent(const wchar_t* path) {
    const auto name=reload_name(path);
    if (name.empty()) return false;
    const auto event=OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str());
    if (!event) return false;
    const bool sent=SetEvent(event)!=FALSE;CloseHandle(event);return sent;
}

bool agent_running() {
    const auto event = OpenEventW(SYNCHRONIZE, FALSE, stop_name);
    if (!event) return false;
    CloseHandle(event);
    return true;
}
AgentStatusSnapshot read_agent_status() {
    AgentStatusSnapshot result;
    const auto mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,health_name);
    const auto view=mapping?static_cast<AgentHealth*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(AgentHealth))):nullptr;
    if(view) {
        const auto load=[]<class T>(T& field){return std::atomic_ref<T>(field).load(std::memory_order_relaxed);};
        result.available=true;result.pid=static_cast<unsigned long>(load(view->pid));
        result.mappings_ready=load(view->mappings_ready)!=0;
        result.settings_ready=load(view->settings_ready)!=0;result.ready=load(view->ready)!=0;
        UnmapViewOfFile(view);
    }
    if(mapping)CloseHandle(mapping);
    return result;
}

void print_agent_status() {
    const bool native=agent_running(),official=options_running();
    const auto mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,health_name);
    const auto view=mapping?static_cast<AgentHealth*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(AgentHealth))):nullptr;
    if(!view){std::printf("{\"native\":%s,\"options_plus\":%s,\"ready\":false,\"health_available\":false}\n",native?"true":"false",official?"true":"false");}
    else {
        const auto load=[]<class T>(T& field){return std::atomic_ref<T>(field).load(std::memory_order_relaxed);};
        const auto checked=load(view->checked),last=load(view->last_event);const auto now=GetTickCount64();
        const bool mappings=native&&load(view->mappings_ready),settings=load(view->settings_ready);
        std::printf("{\"native\":%s,\"options_plus\":%s,\"ready\":%s,\"mappings_ready\":%s,\"settings_ready\":%s,\"degraded\":%s,\"health_available\":true,\"pid\":%ld,\"connections\":%ld,\"reporting_repairs\":%ld,\"device_failures\":%ld,\"input_failures\":%ld,\"device_events\":%lld,\"checked_ago_ms\":%lld,\"event_ago_ms\":%lld}\n",
            native?"true":"false",official?"true":"false",native&&load(view->ready)?"true":"false",mappings?"true":"false",settings?"true":"false",mappings&&!settings?"true":"false",load(view->pid),load(view->connections),load(view->repairs),load(view->failures),load(view->input_failures),load(view->events),checked?static_cast<long long>(now-checked):-1ll,last?static_cast<long long>(now-last):-1ll);
        UnmapViewOfFile(view);
    }
    if(mapping)CloseHandle(mapping);
}
bool set_agent_thumb_speed(unsigned speed) {
    if (speed<10 || speed>200) return false;
    const auto mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,speed_name);
    if (!mapping) return false;
    const auto value=static_cast<LONG*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(LONG)));
    if (value) {InterlockedExchange(value,static_cast<LONG>(speed));UnmapViewOfFile(value);}
    CloseHandle(mapping);
    return value!=nullptr;
}

int preview_ring(const wchar_t* path) {
    Mappings mappings;
    if (!load(path, mappings) || mappings.ring.empty()) return 2;
    if (const auto* choice = choose_item(mappings, nullptr))
        std::printf("Selected: %ls\n", choice->label.c_str());
    return 0;
}

bool check_mappings(const wchar_t* path) {
    Mappings mappings;
    return load(path, mappings) && (!mappings.bindings.empty() || !mappings.hotkeys.empty());
}

int play_actions(const wchar_t* path, bool validate_only) {
    Mappings mappings;
    if (!load(path, mappings) || mappings.hotkeys.size() != 1) return 2;
    const auto& actions = mappings.hotkeys.front().binding.actions;
    if (std::ranges::any_of(actions, [](const Action& action) {
            return action.operation == Operation::wheel_mode || action.operation == Operation::ring ||
                   action.operation == Operation::folder;
        })) {
        std::fputs("This action requires the mouse agent and cannot run in preview.\n", stderr);
        return 2;
    }
    if (validate_only) return 0;
    Sleep(3000);
    for (const auto& action : actions) {
        if (action.operation == Operation::wait) Sleep(action.delay);
        else execute(action, nullptr);
    }
    return 0;
}

int run_agent(const wchar_t* path, bool take_over) {
    Mappings mappings;
    const auto& bindings = mappings.bindings;
    const auto& hotkeys = mappings.hotkeys;
    if (path && (!load(path, mappings) || (bindings.empty() && hotkeys.empty()))) return 2;
    if (!bindings.empty() && !take_over && options_running()) {
        std::fputs("Options+ agent is active. Exit it before starting button remapping.\n", stderr);
        return 2;
    }
    const auto mutex = CreateMutexW(nullptr, TRUE, mutex_name);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        std::fputs("Cannot start a second agent.\n", stderr);
        return 2;
    }
    OptionsSession official;
    if (take_over && !bindings.empty() && !official.pause()) {
        std::fputs("Could not pause the installed Options+ agent.\n", stderr);
        official.restore();
        CloseHandle(mutex);
        return 2;
    }
    stop_event = CreateEventW(nullptr, TRUE, FALSE, stop_name);
    if (!stop_event) {
        official.restore();
        CloseHandle(mutex);
        return 2;
    }
    const auto reload_id=path?reload_name(path):std::wstring{};
    reload_event=CreateEventW(nullptr,TRUE,FALSE,reload_id.empty()?nullptr:reload_id.c_str());
    if (!reload_event) {official.restore();CloseHandle(stop_event);CloseHandle(mutex);return 2;}
    SetConsoleCtrlHandler(console_event, TRUE);
    LiveThumbSpeed live_speed(mappings.thumb_speed.value_or(100));
    LiveHealth live_health;
    dpi_ipc::Host command_host(path);
    reload_ipc::Host reload_host(path);
    settings_ipc::Host settings_host(path);
    dpi_commands=&command_host;desired_dpi.reset();desired_dpi_saved=false;
    reload_commands=&reload_host;
    setting_commands=&settings_host;agent_mapping=path;desired_settings={};settings_loaded=false;settings_saved=true;
    settings_unverified_mask=0;parameters_restored=true;
    int result = 0;
    std::size_t registered = 0;
    if (!register_keys(hotkeys,registered)) result=2;
    if (!result && path && bindings.empty()) {
        logi::Mouse desktop{};
        std::vector<Control> controls;
        std::optional<Wheel> wheel;
        std::printf("Ready: %zu global shortcuts.\n", registered);
        std::fflush(stdout);
        events(desktop, controls, wheel, mappings,path,registered);
    }
    while (!result && (!path || !bindings.empty()) && WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
        if (!bindings.empty() && options_running()) {
            std::fputs("Options+ agent is active.\n", stderr);
            result = 2;
            break;
        }
        auto mouse = logi::connect(stop_event);
        if (mouse) {
            restore_owner_settings(path,*mouse);
            const bool wants_wheel=std::ranges::any_of(bindings,[](const auto& binding){return binding.control==thumb_wheel;});
            std::vector<Control> controls;
            std::optional<Wheel> wheel;
            if ((!bindings.empty() && !logi::restore_standard_input(*mouse)) ||
                !enable(*mouse, bindings, controls) ||
                (wants_wheel && !divert_wheel(*mouse, wheel, mappings.thumb_speed.value_or(100)))) {
                std::fputs("Could not activate the requested mappings.\n", stderr);
                restore(*mouse, controls, wheel);
                mapping_readiness(false);
                if(health)InterlockedIncrement(&health->failures);
                if(WaitForSingleObject(stop_event,2000)==WAIT_OBJECT_0)break;
                continue;
            }
            dpi_readiness(restore_owner_dpi(path,*mouse));
            if(wheel)publish_setting_observation(observe_setting_group(*mouse,wheel,SettingKey::thumb_invert),16);
            if (!dpi_restored)
                std::fputs("Could not restore saved pointer speed.\n", stderr);
            if(health){InterlockedIncrement(&health->connections);InterlockedExchange64(&health->checked,static_cast<LONG64>(GetTickCount64()));mapping_readiness(true);}
            std::printf("%s: %s, %zu remapped controls.\n", dpi_restored&&parameters_restored?"Ready":"Degraded (saved mouse settings pending)",mouse->device.name.c_str(), controls.size());
            std::fflush(stdout);
            events(*mouse, controls, wheel, mappings,path,registered);
            mapping_readiness(false);
            if(setting_commands)setting_commands->observe({},false,settings_saved,31);
            restore(*mouse, controls, wheel);
        }
        if(dpi_commands && dpi_commands->waiting())if(const auto request=dpi_commands->take())
            dpi_commands->finish(handle_dpi_command(nullptr,path,*request,[]{}));
        if(reload_commands && reload_commands->waiting())if(const auto request=reload_commands->take())
            reload_commands->finish(*request,WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0?ReloadStatus::stopped:ReloadStatus::device_unavailable);
        if(setting_commands && setting_commands->waiting())if(const auto request=setting_commands->take()) {
            std::optional<Wheel> none;setting_commands->finish(handle_setting_command(nullptr,none,path,*request,[]{}));
        }
        if (WaitForSingleObject(stop_event, 2000) == WAIT_OBJECT_0) break;
    }
    unregister_keys(registered);
    SetConsoleCtrlHandler(console_event, FALSE);
    official.restore();
    command_host.shutdown();dpi_commands=nullptr;
    reload_host.shutdown();reload_commands=nullptr;
    settings_host.shutdown();setting_commands=nullptr;agent_mapping=nullptr;
    CloseHandle(stop_event);
    CloseHandle(reload_event);reload_event=nullptr;
    CloseHandle(mutex);
    return result;
}
