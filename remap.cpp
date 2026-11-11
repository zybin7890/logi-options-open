#include "mouse.hpp"
#include "remap.hpp"
#include "ring.hpp"

#include <tlhelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>

namespace {

constexpr auto stop_name = L"Local\\LogiNative.Stop";
constexpr auto mutex_name = L"Local\\LogiNative.Agent";
constexpr auto speed_name = L"Local\\LogiNative.ThumbSpeed.v1";
constexpr auto health_name = L"Local\\LogiNative.Health.v1";
struct alignas(8) AgentHealth {
    LONG pid{},ready{},connections{},repairs{},failures{},input_failures{};
    LONG64 checked{},events{},last_event{};
};
AgentHealth* health{};
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
                       pan, wait, text, open, ring, folder };
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

struct Control {
    unsigned id;
    bool xy;
    logi::Byte restore;
    bool down{};
    bool first_motion{};
    bool panning{};
    bool pan_scrolled{};
    int x{}, y{};
};

struct Wheel {
    std::array<logi::Byte, 2> original;
    int native_resolution;
    int diverted_resolution;
    int direction;
    std::int64_t remainder{};
    const Binding* previous{};

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
               action.operation == Operation::pan;
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
        (binding.actions[0].operation == Operation::pan && gesture != Gesture::tap) ||
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
        hotkey.binding.actions[0].operation == Operation::pan) return false;
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

bool load(const wchar_t* path, Mappings& mappings) {
    const auto& bindings = mappings.bindings;
    std::ifstream file{std::filesystem::path(path), std::ios::binary};
    if (!file) {
        std::fputs("Cannot read mapping file.\n", stderr);
        return false;
    }
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
    return !file.bad();
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
    const auto sent = send_input(3, inputs, sizeof(INPUT));
    if (sent == 3) return;
    // SendInput may accept a prefix of the batch. Release our own Ctrl press.
    if (sent) send_input(1, &inputs[2], sizeof(INPUT));
    std::fputs("Windows rejected zoom input.\n", stderr);
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
        if (!mode || !logi::set(*device, L"ratchet", !*mode))
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
        if (send_input(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size())
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
        events[0].type = events[1].type = INPUT_MOUSE;
        events[0].mi.dwFlags = mouse == VK_MBUTTON ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_XDOWN;
        events[1].mi.dwFlags = mouse == VK_MBUTTON ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_XUP;
        if (mouse != VK_MBUTTON)
            events[0].mi.mouseData = events[1].mi.mouseData = mouse == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
        if (send_input(2, events, sizeof(INPUT)) != 2)
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
    if (send_input(count * 2, events, sizeof(INPUT)) != count * 2)
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
    };
    std::vector<Run> runs;
    const Mappings& mappings;

    static bool advance(Run& run, logi::Mouse& mouse) {
        const auto& actions = run.binding->actions;
        while (run.next < actions.size()) {
            const auto& action = actions[run.next++];
            if (action.operation != Operation::wait) execute(action, &mouse);
            else if (action.delay && run.next < actions.size()) {
                run.due = GetTickCount64() + action.delay;
                return true;
            }
        }
        return false;
    }

    void start(const Binding& binding, logi::Mouse& mouse) {
        if (binding.actions[0].operation == Operation::ring) {
            if (const auto* chosen = choose_item(mappings, stop_event)) start(chosen->binding, mouse);
            return;
        }
        Run run{&binding};
        if (runs.size() < 32 && advance(run, mouse)) runs.push_back(run);
    }

    DWORD service(logi::Mouse& mouse) {
        DWORD next = INFINITE;
        for (auto run = runs.begin(); run != runs.end();) {
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

void fire(logi::Mouse& mouse, const std::vector<Binding>& bindings, unsigned id, Gesture gesture,
          Sequences& sequences, Foreground& foreground) {
    if (const auto* binding = lookup(bindings, id, gesture, foreground.get())) sequences.start(*binding, mouse);
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
    wheel = Wheel{{status[0], inverted}, static_cast<int>(native), static_cast<int>(diverted), direction};
    if (!mouse.request(mouse.thumb, 2, {1, 0})) return false;
    const auto current = mouse.request(mouse.thumb, 1);
    if (!current || current.packet.data()[0] != 1 || (current.packet.data()[1] & 1)) return false;
    std::printf("Thumb wheel: native=%u diverted=%u direction=%d speed=%u%%.\n",
                native, diverted, direction, speed);
    return true;
}

bool needs_raw_motion(const Binding& binding) {
    if (binding.control==thumb_wheel) return false;
    if (binding.actions.front().operation==Operation::pan) return true;
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
        if(health)InterlockedExchange(&health->ready,0);
        const auto flags=static_cast<logi::Byte>(control.xy?0x33:0x23);
        if(!mouse.channel->request(mouse.device.slot,mouse.buttons,3,{high,low,flags},300))return false;
        current=mouse.channel->request(mouse.device.slot,mouse.buttons,2,{high,low},300);
        if(!current || logi::word(current.packet.data())!=control.id || (current.packet.data()[2]&0x11)!=wanted)return false;
        control.down=false;control.panning=false;control.x=control.y=0;repaired=true;
        std::fprintf(stderr,"Recovered reporting for control %04X.\n",control.id);
    }
    if(wheel) {
        auto current=mouse.channel->request(mouse.device.slot,mouse.thumb,1,{},300);
        if(!current)return failed("thumb-wheel",current);
        if(current.packet.data()[0]!=1 || (current.packet.data()[1]&1)) {
            if(health)InterlockedExchange(&health->ready,0);
            if(!mouse.channel->request(mouse.device.slot,mouse.thumb,2,{1,0},300))return false;
            current=mouse.channel->request(mouse.device.slot,mouse.thumb,1,{},300);
            if(!current || current.packet.data()[0]!=1 || (current.packet.data()[1]&1))return false;
            wheel->previous=nullptr;wheel->remainder=0;repaired=true;
            std::fputs("Recovered thumb wheel reporting.\n",stderr);
        }
    }
    if(health){InterlockedExchange64(&health->checked,static_cast<LONG64>(GetTickCount64()));
        if(repaired)InterlockedIncrement(&health->repairs);
        InterlockedExchange(&health->ready,1);}
    return true;
}

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

void events(logi::Mouse& mouse, std::vector<Control>& controls, std::optional<Wheel>& wheel,
            Mappings& mappings, const wchar_t* path,std::size_t& registered) {
    const auto& bindings = mappings.bindings;
    const auto& hotkeys = mappings.hotkeys;
    const auto dpi = logi::get(mouse, L"dpi").value_or(1000);
    const int threshold = std::max(20, static_cast<int>(dpi / 12));
    const int counts_per_pan_step = std::max(20, static_cast<int>(dpi / 16));
    Sequences sequences{{}, mappings};
    Foreground foreground;
    std::vector<std::size_t> pending;
    if (mouse.channel) mouse.channel->keep_notifications();
    auto next_check=GetTickCount64()+3000;
    LASTINPUTINFO last_input{sizeof(LASTINPUTINFO),0};
    GetLastInputInfo(&last_input);
    auto activity=last_input.dwTime;
    LONG64 last_checked_event{};
    for (;;) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) return;
        if(mouse.channel && GetTickCount64()>=next_check) {
            GetLastInputInfo(&last_input);
            const auto device_events=health?std::atomic_ref<LONG64>(health->events).load(std::memory_order_relaxed):0;
            const bool active=last_input.dwTime!=activity || device_events!=last_checked_event;
            activity=last_input.dwTime;last_checked_event=device_events;
            // Do not wake a sleeping wireless mouse just to poll it. Check after
            // resumed normal input or a HID++ event, including standard movement
            // when custom button reporting was reset and no HID++ event arrives.
            if(active && !check_reporting(mouse,controls,wheel)) {
                if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0)return;
                if(health){InterlockedExchange(&health->ready,0);InterlockedIncrement(&health->failures);}
                std::fputs("Device reporting check failed; reconnecting without exiting the agent.\n",stderr);
                return;
            }
            next_check=GetTickCount64()+3000;
        }
        if (path && WaitForSingleObject(reload_event,0)==WAIT_OBJECT_0) {
            ResetEvent(reload_event);
            Mappings next;
            if (!load(path,next) || (next.bindings.empty() && next.hotkeys.empty()) ||
                (!mouse.channel && !next.bindings.empty())) {
                std::fputs("Reload rejected; previous mappings remain active.\n",stderr);std::fflush(stderr);
                continue;
            }
            unregister_keys(registered);
            if (!register_keys(next.hotkeys,registered)) {
                if (!register_keys(hotkeys,registered)) {SetEvent(stop_event);return;}
                std::fputs("Reload rejected; previous shortcuts restored.\n",stderr);std::fflush(stderr);
                continue;
            }
            const bool hardware_changed=control_layout(mappings)!=control_layout(next);
            if (hardware_changed && mouse.channel) {
                if (!restore(mouse,controls,wheel,false)) {
                    unregister_keys(registered);
                    if (!register_keys(hotkeys,registered)) SetEvent(stop_event);
                    std::fputs("Reload reporting restore failed; reconnecting previous mappings.\n",stderr);
                    return;
                }
                controls.clear();wheel.reset();
                const bool wants=std::ranges::any_of(next.bindings,[](const auto& binding){return binding.control==thumb_wheel;});
                if (!enable(mouse,next.bindings,controls) || (wants && !divert_wheel(mouse,wheel,next.thumb_speed.value_or(100)))) {
                    unregister_keys(registered);
                    if (!register_keys(hotkeys,registered)) SetEvent(stop_event);
                    std::fputs("Reload hardware activation failed; reconnecting previous mappings.\n",stderr);
                    return;
                }
                mouse.channel->discard_notifications();
            }
            sequences.runs.clear();pending.clear();
            mappings=std::move(next);
            for (auto& control:controls) {control.down=false;control.panning=false;control.x=control.y=0;}
            if (wheel) {wheel->previous=nullptr;wheel->remainder=0;}
            if (live_thumb_speed) InterlockedExchange(live_thumb_speed,static_cast<LONG>(mappings.thumb_speed.value_or(100)));
            std::printf("Reloaded: %zu bindings, %zu shortcuts; PID %lu unchanged.\n",bindings.size(),registered,GetCurrentProcessId());
            std::fflush(stdout);
        }
        if (!hotkeys.empty()) dispatch_hotkeys(hotkeys, pending, sequences, mouse);
        auto timeout = sequences.service(mouse);
        if(mouse.channel){const auto now=GetTickCount64();timeout=std::min<DWORD>(timeout,static_cast<DWORD>(next_check>now?next_check-now:0));}
        if (!pending.empty()) timeout = std::min<DWORD>(timeout, 16);
        if (!mouse.channel) {
            const HANDLE signals[]{stop_event,reload_event};
            const auto wait = MsgWaitForMultipleObjectsEx(2,signals,timeout,QS_HOTKEY,MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) return;
            continue;
        }
        const auto reply = mouse.channel->read(timeout, !hotkeys.empty(),reload_event);
        if (reply.result == logi::Result::timeout || reply.result == logi::Result::hotkey || reply.result==logi::Result::reload) continue;
        if (!reply) {
            if(health)InterlockedExchange(&health->ready,0);
            if(reply.result!=logi::Result::stopped){
                if(health)InterlockedIncrement(&health->failures);
                std::fprintf(stderr,"HID read ended (%u); reconnecting.\n",static_cast<unsigned>(reply.result));
            }
            return;
        }
        const auto& packet = reply.packet;
        if (packet.size != 20 || packet.bytes[1] != mouse.device.slot) continue;
        const auto data = packet.data();
        const auto event = packet.bytes[3];
        if(health && !(event&15) && (packet.bytes[2]==mouse.thumb || packet.bytes[2]==mouse.buttons)) {
            InterlockedIncrement64(&health->events);InterlockedExchange64(&health->last_event,static_cast<LONG64>(GetTickCount64()));
        }
        if (wheel && packet.bytes[2] == mouse.thumb && event == 0) {
            const auto rotation = static_cast<std::int16_t>(logi::word(data));
            if (!rotation) continue;
            const auto gesture = rotation * wheel->direction > 0 ? Gesture::right : Gesture::left;
            const auto* binding = lookup(bindings, thumb_wheel, gesture, foreground.get());
            const auto speed=live_thumb_speed?static_cast<unsigned>(InterlockedCompareExchange(live_thumb_speed,0,0)):
                                             mappings.thumb_speed.value_or(100);
            const auto steps = std::abs(wheel->steps(data, binding, speed));
            for (int i = 0; i < steps && binding; ++i) sequences.start(*binding, mouse);
            continue;
        }
        if (packet.bytes[2] != mouse.buttons || !mouse.buttons) continue;
        if (event == 0) {
            for (auto& control : controls) {
                bool down = false;
                for (unsigned i = 0; i < 8; i += 2) down |= logi::word(data.subspan(i)) == control.id;
                if (down && !control.down) {
                    control.x = 0;
                    control.y = 0;
                    control.pan_scrolled = false;
                    control.first_motion = mouse.device.name == "MX Master 3S";
                    const auto* tap = lookup(bindings, control.id, Gesture::tap, foreground.get());
                    control.panning = tap && tap->actions.front().operation == Operation::pan;
                    if (!control.xy && tap) sequences.start(*tap, mouse);
                }
                if (!down && control.down && control.panning) release_pan(control);
                else if (!down && control.down && control.xy) {
                    auto gesture = Gesture::tap;
                    if (std::max(std::abs(control.x), std::abs(control.y)) >= threshold) {
                        if (std::abs(control.x) >= std::abs(control.y))
                            gesture = control.x < 0 ? Gesture::left : Gesture::right;
                        else gesture = control.y < 0 ? Gesture::up : Gesture::down;
                    }
                    fire(mouse, bindings, control.id, gesture, sequences, foreground);
                }
                control.down = down;
            }
        } else if (event == 0x10) {
            for (auto& control : controls) {
                if (!control.xy || !control.down) continue;
                if (control.first_motion) {
                    control.first_motion = false;
                    continue;
                }
                const auto x = static_cast<std::int16_t>(logi::word(data));
                const auto y = static_cast<std::int16_t>(logi::word(data.subspan(2)));
                if (control.panning) {
                    move_pan(control, x, y, counts_per_pan_step);
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
void print_agent_status() {
    const bool native=agent_running(),official=options_running();
    const auto mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,health_name);
    const auto view=mapping?static_cast<AgentHealth*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(AgentHealth))):nullptr;
    if(!view){std::printf("{\"native\":%s,\"options_plus\":%s,\"ready\":false,\"health_available\":false}\n",native?"true":"false",official?"true":"false");}
    else {
        const auto load=[]<class T>(T& field){return std::atomic_ref<T>(field).load(std::memory_order_relaxed);};
        const auto checked=load(view->checked),last=load(view->last_event);const auto now=GetTickCount64();
        std::printf("{\"native\":%s,\"options_plus\":%s,\"ready\":%s,\"health_available\":true,\"pid\":%ld,\"connections\":%ld,\"reporting_repairs\":%ld,\"device_failures\":%ld,\"input_failures\":%ld,\"device_events\":%lld,\"checked_ago_ms\":%lld,\"event_ago_ms\":%lld}\n",
            native?"true":"false",official?"true":"false",native&&load(view->ready)?"true":"false",load(view->pid),load(view->connections),load(view->repairs),load(view->failures),load(view->input_failures),load(view->events),checked?static_cast<long long>(now-checked):-1ll,last?static_cast<long long>(now-last):-1ll);
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
            const bool wants_wheel=std::ranges::any_of(bindings,[](const auto& binding){return binding.control==thumb_wheel;});
            std::vector<Control> controls;
            std::optional<Wheel> wheel;
            if ((!bindings.empty() && !logi::restore_standard_input(*mouse)) ||
                !enable(*mouse, bindings, controls) ||
                (wants_wheel && !divert_wheel(*mouse, wheel, mappings.thumb_speed.value_or(100)))) {
                std::fputs("Could not activate the requested mappings.\n", stderr);
                restore(*mouse, controls, wheel);
                if(health){InterlockedExchange(&health->ready,0);InterlockedIncrement(&health->failures);}
                if(WaitForSingleObject(stop_event,2000)==WAIT_OBJECT_0)break;
                continue;
            }
            if(health){InterlockedIncrement(&health->connections);InterlockedExchange64(&health->checked,static_cast<LONG64>(GetTickCount64()));InterlockedExchange(&health->ready,1);}
            std::printf("Ready: %s, %zu remapped controls.\n", mouse->device.name.c_str(), controls.size());
            std::fflush(stdout);
            events(*mouse, controls, wheel, mappings,path,registered);
            if(health)InterlockedExchange(&health->ready,0);
            restore(*mouse, controls, wheel);
        }
        if (WaitForSingleObject(stop_event, 2000) == WAIT_OBJECT_0) break;
    }
    unregister_keys(registered);
    SetConsoleCtrlHandler(console_event, FALSE);
    official.restore();
    CloseHandle(stop_event);
    CloseHandle(reload_event);reload_event=nullptr;
    CloseHandle(mutex);
    return result;
}
