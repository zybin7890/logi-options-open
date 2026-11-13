#include "mouse.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace logi {

Reply Mouse::request(Byte feature, Byte function, std::initializer_list<Byte> data) {
    if (!feature) return {Result::device_error};
    return channel->request(device.slot, feature, function, data);
}

static std::vector<unsigned> read_dpi_choices(Mouse& mouse) {
    const auto response = mouse.request(mouse.dpi, 1, {0});
    if (!response || response.packet.data().size() < 5) return {};
    const auto data = response.packet.data().subspan(1);
    std::vector<unsigned> choices;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        const auto entry = word(data.subspan(i));
        if (!entry) break;
        if ((entry & 0xE000) != 0xE000) {
            choices.push_back(entry);
            continue;
        }
        if (choices.empty() || i + 3 >= data.size() || !(entry & 0x1FFF)) return {};
        const auto step = entry & 0x1FFF;
        const auto end = word(data.subspan(i + 2));
        for (auto value = choices.back() + step; value <= end; value += step)
            choices.push_back(value);
        i += 2;
    }
    return choices;
}

std::optional<Mouse> connect(HANDLE stop, bool list) {
    // Discovery is a hint, never a cached device/feature result. Re-probe the
    // last successful route before waiting on empty receiver slots again.
    static std::mutex route_mutex;
    static std::wstring route_path;
    static Byte route_slot{};
    for (const auto& interface : enumerate()) {
        auto channel = std::make_unique<Channel>(interface, stop);
        if (!channel->valid()) {
            if (list) std::printf("046d:%04x: cannot open (Win32 %lu)\n",
                                  interface.product, channel->open_error());
            continue;
        }
        const unsigned first = interface.receiver ? 1 : 255;
        const unsigned last = interface.receiver ? 6 : 255;
        unsigned preferred{};
        if (!list) {
            std::lock_guard lock(route_mutex);
            if (route_path == interface.path && route_slot >= first && route_slot <= last)
                preferred = route_slot;
        }
        std::vector<unsigned> slots;
        if (preferred) slots.push_back(preferred);
        for (unsigned slot = first; slot <= last; ++slot)
            if (slot != preferred) slots.push_back(slot);
        for (const auto slot : slots) {
            if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return std::nullopt;
            auto device = probe(*channel, static_cast<Byte>(slot));
            if (!device) continue;
            if (list) {
                std::printf("046d:%04x slot=%u %s HID++ %u.%u\n", interface.product, slot,
                            device->name.c_str(), device->major, device->minor);
                continue;
            }
            const auto find = [&](unsigned id) { return feature(*channel, device->slot, id); };
            const auto dpi = find(0x2201);
            const auto buttons = find(0x1B04);
            if (!dpi) continue;
            const auto enhanced = find(0x2111);
            const auto unified = find(0x1004);
            Mouse mouse{
                .channel = nullptr, .device = *device, .dpi = dpi,
                .shift = enhanced ? enhanced : find(0x2110), .wheel = find(0x2121),
                .thumb = find(0x2150), .buttons = buttons,
                .battery = unified ? unified : find(0x1000), .host = find(0x1814),
                .hosts_info = find(0x1815),
                .enhanced_shift = enhanced != 0, .unified_battery = unified != 0,
                .dpi_choices = {}
            };
            mouse.channel = std::move(channel);
            mouse.dpi_choices = read_dpi_choices(mouse);
            {
                std::lock_guard lock(route_mutex);
                route_path = interface.path;
                route_slot = mouse.device.slot;
            }
            return mouse;
        }
    }
    return std::nullopt;
}

std::vector<HostSlot> hosts(Mouse& mouse) {
    const auto state = mouse.request(mouse.host, 0);
    if (!state) return {};
    const auto count = state.packet.data()[0];
    const auto selected = state.packet.data()[1];
    const auto info = mouse.request(mouse.hosts_info, 0);
    const bool has_status = info && info.packet.data().size() >= 4 &&
                            (info.packet.data()[0] & 1) && info.packet.data()[2] == count;
    std::vector<HostSlot> slots;
    for (Byte index = 0; index < count; ++index) {
        HostSlot slot{static_cast<unsigned>(index) + 1, index == selected, std::nullopt};
        if (has_status) {
            const auto status = mouse.request(mouse.hosts_info, 1, {index});
            if (status && status.packet.data()[0] == index) slot.paired = status.packet.data()[1] != 0;
        }
        slots.push_back(slot);
    }
    return slots;
}

bool switch_host(Mouse& mouse, unsigned target) {
    const auto slots = hosts(mouse);
    const auto found = std::ranges::find(slots, target, &HostSlot::slot);
    if (found == slots.end()) return false;
    if (found->current) return true;
    if (!found->paired.value_or(false)) return false;
    // ChangeHost normally disconnects before a reply can arrive.
    return mouse.channel->send(mouse.device.slot, mouse.host, 1, {low(target - 1)}) == Result::ok;
}

std::optional<unsigned> get(Mouse& mouse, std::wstring_view setting) {
    if (setting == L"dpi") {
        const auto reply = mouse.request(mouse.dpi, 2, {0});
        if (!reply || reply.packet.data().size() < 5) return std::nullopt;
        const auto data = reply.packet.data();
        const auto current = word(data.subspan(1));
        return current ? current : word(data.subspan(3));
    }
    if (setting == L"smartshift" || setting == L"ratchet") {
        const auto reply = mouse.request(mouse.shift, mouse.enhanced_shift ? 1 : 0);
        if (!reply) return std::nullopt;
        const auto data = reply.packet.data();
        if (setting == L"ratchet") return data[0] == 2;
        return data[1] == 255 ? 0 : data[1];
    }
    if (setting == L"hires" || setting == L"invert") {
        const auto reply = mouse.request(mouse.wheel, 1);
        if (!reply) return std::nullopt;
        return (reply.packet.data()[0] & (setting == L"hires" ? 2 : 4)) != 0;
    }
    if (setting == L"thumb-invert") {
        const auto reply = mouse.request(mouse.thumb, 1);
        if (!reply) return std::nullopt;
        return (reply.packet.data()[1] & 1) != 0;
    }
    if (setting == L"host") {
        const auto reply = mouse.request(mouse.host, 0);
        if (!reply) return std::nullopt;
        return reply.packet.data()[1] + 1;
    }
    return std::nullopt;
}

bool set(Mouse& mouse, std::wstring_view setting, unsigned value) {
    if (setting == L"dpi") {
        if (!std::ranges::contains(mouse.dpi_choices, value)) return false;
        return static_cast<bool>(mouse.request(mouse.dpi, 3, {0, high(value), low(value)}));
    }
    if (setting == L"smartshift" || setting == L"ratchet") {
        if (value > (setting == L"smartshift" ? 50u : 1u)) return false;
        const Byte mode = setting == L"ratchet" ? (value ? 2 : 1) : 0;
        const Byte threshold = setting == L"smartshift" ? (value ? low(value) : 255) : 0;
        return static_cast<bool>(mouse.request(mouse.shift, mouse.enhanced_shift ? 2 : 1,
                                               {mode, threshold, 0}));
    }
    if (value > 1) return false;
    if (setting == L"hires" || setting == L"invert") {
        const auto reply = mouse.request(mouse.wheel, 1);
        if (!reply) return false;
        const Byte flags = reply.packet.data()[0];
        const Byte mask = setting == L"hires" ? 2 : 4;
        return static_cast<bool>(mouse.request(mouse.wheel, 2,
                                               {static_cast<Byte>(value ? flags | mask : flags & ~mask)}));
    }
    if (setting == L"thumb-invert") {
        const auto reply = mouse.request(mouse.thumb, 1);
        if (!reply) return false;
        const auto data = reply.packet.data();
        return static_cast<bool>(mouse.request(mouse.thumb, 2,
            {data[0],static_cast<Byte>((data[1]&~1u)|value)}));
    }
    return false;
}

static std::filesystem::path dpi_file(std::filesystem::path mapping) {
    return mapping.replace_extension(L".dpi");
}

bool read_saved_settings(const std::filesystem::path& mapping,const Mouse& mouse,SavedSettings& settings) {
    auto path=mapping;path.replace_extension(L".settings");
    errno=0;std::ifstream input(path);
    if(!input.is_open())return errno==ENOENT;
    std::string version,device;
    if(!std::getline(input,version)||version!="LogiNative settings 1"||!std::getline(input,device)||device!=mouse.device.name)return false;
    constexpr std::array names{"smartshift","ratchet","hires","invert","thumb-invert"};
    SavedSettings parsed{};std::string name;unsigned value{};
    while(input>>name) {
        if(!(input>>value))return false;
        const auto found=std::ranges::find(names,name);
        if(found==names.end())return false;
        const auto index=static_cast<std::size_t>(found-names.begin());
        if(parsed[index]||value>(index==0?254u:1u))return false;
        parsed[index]=value;
    }
    if(!input.eof()||input.bad())return false;
    settings=parsed;return true;
}

bool remember_settings(const std::filesystem::path& mapping,const Mouse& mouse,const SavedSettings& settings) {
    auto path=mapping;path.replace_extension(L".settings");
    auto temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    std::ofstream output(temporary);
    output<<"LogiNative settings 1\n"<<mouse.device.name<<'\n';
    constexpr std::array names{"smartshift","ratchet","hires","invert","thumb-invert"};
    for(std::size_t i=0;i<settings.size();++i)if(settings[i])output<<names[i]<<' '<<*settings[i]<<'\n';
    output.close();
    if(output&&MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    DeleteFileW(temporary.c_str());return false;
}

bool remember_dpi(const std::filesystem::path& mapping, const Mouse& mouse, unsigned value) {
    const auto file = dpi_file(mapping);
    auto temporary = file;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream output(temporary);
    output << mouse.device.name << '\n' << value << '\n';
    output.close();
    if (output && MoveFileExW(temporary.c_str(), file.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary.c_str());
    return false;
}

bool restore_saved_dpi(const std::filesystem::path& mapping, Mouse& mouse) {
    errno = 0;
    std::ifstream input(dpi_file(mapping));
    // Only an absent sidecar means there is nothing to restore. Access or I/O
    // failure must reach the agent's degraded state, not masquerade as success.
    if (!input.is_open()) return errno == ENOENT;
    std::string device;
    unsigned value{};
    if (!std::getline(input, device) || !(input >> value)) return false;
    if (device != mouse.device.name) return false;
    if (!std::ranges::contains(mouse.dpi_choices, value)) return false;
    if (get(mouse, L"dpi") == value) return true;
    if (!set(mouse, L"dpi", value) || get(mouse, L"dpi") != value) return false;
    std::printf("Restored saved pointer speed: %u DPI.\n", value);
    return true;
}

bool restore_standard_input(Mouse& mouse) {
    // A stopped Options+ process can leave both wheels routed to HID++.
    // Establish normal reporting before enabling the mappings we actually handle.
    const auto wheel = mouse.request(mouse.wheel, 1);
    const auto thumb = mouse.request(mouse.thumb, 1);
    if (!wheel || !thumb) return false;
    const auto mode = static_cast<Byte>(wheel.packet.data()[0] & ~1);
    if (wheel.packet.data()[0] != mode && !mouse.request(mouse.wheel, 2, {mode})) return false;
    if (thumb.packet.data()[0] && !mouse.request(mouse.thumb, 2, {0, thumb.packet.data()[1]})) return false;
    const auto main_check = mouse.request(mouse.wheel, 1);
    const auto thumb_check = mouse.request(mouse.thumb, 1);
    if (!main_check || main_check.packet.data()[0] != mode || !thumb_check ||
        thumb_check.packet.data()[0] || thumb_check.packet.data()[1] != thumb.packet.data()[1]) return false;
    const auto count = mouse.request(mouse.buttons, 0);
    if (!count) return false;
    for (unsigned i = 0; i < count.packet.data()[0]; ++i) {
        const auto info = mouse.request(mouse.buttons, 1, {low(i)});
        if (!info || info.packet.data().size() < 9) return false;
        const auto data = info.packet.data();
        if (!(data[4] & 0x20)) continue;
        const auto current = mouse.request(mouse.buttons, 2, {data[0], data[1]});
        if (!current || word(current.packet.data()) != word(data)) return false;
        if (!(current.packet.data()[2] & 0x11)) continue;
        if (!mouse.request(mouse.buttons, 3, {data[0], data[1], 0x22})) return false;
        const auto check = mouse.request(mouse.buttons, 2, {data[0], data[1]});
        if (!check || word(check.packet.data()) != word(data) || (check.packet.data()[2] & 0x11)) return false;
    }
    return true;
}

void describe(Mouse& mouse) {
    std::printf("%s (slot %u, HID++ %u.%u)\n", mouse.device.name.c_str(), mouse.device.slot,
                mouse.device.major, mouse.device.minor);
    const auto battery = mouse.request(mouse.battery, mouse.unified_battery ? 1 : 0);
    if (battery) {
        const auto data = battery.packet.data();
        if (data[0] > 0 && data[0] <= 100) std::printf("battery=%u%% ", data[0]);
        std::printf("battery-status=0x%02x\n", data[2]);
    }
    for (const auto name : {L"dpi", L"smartshift", L"ratchet", L"hires", L"invert", L"thumb-invert", L"host"})
        if (const auto value = get(mouse, name)) std::printf("%ls=%u\n", name, *value);
    if (!mouse.dpi_choices.empty()) {
        std::printf("dpi-options:");
        for (const auto value : mouse.dpi_choices) std::printf(" %u", value);
        std::puts("");
    }
    const auto count = mouse.request(mouse.buttons, 0);
    if (!count) return;
    std::puts("HID++ controls (D = divertable, G = raw XY gesture):");
    for (unsigned i = 0; i < count.packet.data()[0]; ++i) {
        const auto info = mouse.request(mouse.buttons, 1, {low(i)});
        if (!info || info.packet.data().size() < 9) continue;
        const auto data = info.packet.data();
        std::printf("  %04X  %c%c\n", word(data), data[4] & 0x20 ? 'D' : '-',
                    data[8] & 1 ? 'G' : '-');
    }
}

std::optional<unsigned> number(std::wstring_view input, int base) {
    std::array<char, 16> ascii{};
    if (input.empty() || input.size() >= ascii.size()) return std::nullopt;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] > 127) return std::nullopt;
        ascii[i] = static_cast<char>(input[i]);
    }
    unsigned value = 0;
    const auto end = ascii.data() + input.size();
    const auto result = std::from_chars(ascii.data(), end, value, base);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

} // namespace logi
