#include "mouse.hpp"
#include "agent_log.hpp"
#include "discovery_route.hpp"
#include "owner_profile.hpp"

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

static constexpr wchar_t discovery_key[] = L"Software\\LogiNative\\Discovery";

static std::optional<discovery::Hint> read_route_hint() {
    DWORD bytes{};
    if (RegGetValueW(HKEY_CURRENT_USER, discovery_key, L"Route", RRF_RT_REG_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < 2 * sizeof(wchar_t) || bytes > 32768 * sizeof(wchar_t) || bytes % sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, discovery_key, L"Route", RRF_RT_REG_SZ,
                     nullptr, value.data(), &bytes) != ERROR_SUCCESS || !bytes) return {};
    value.resize(bytes / sizeof(wchar_t));
    if (value.empty() || value.back() != L'\0') return {};
    value.pop_back();
    return discovery::decode(value);
}

static void remember_route_hint(const discovery::Hint& hint) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, discovery_key, 0, nullptr, 0, KEY_SET_VALUE,
                       nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const auto value = discovery::encode(hint);
    RegSetValueExW(key, L"Route", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

static std::optional<OwnerIdentity> read_owner_identity(Channel& channel,const Device& device,
                                                       const std::wstring& path,DWORD timeout) {
    const auto fw_feature=feature(channel,device.slot,0x0003,timeout);
    if(!fw_feature)return {};
    const auto info=channel.request(device.slot,fw_feature,0,{},timeout);
    if(!info || info.packet.size<17)return {};
    const auto data=info.packet.data();
    if(!data[0] || data[0]>8)return {};
    for(Byte index=0;index<data[0];++index) {
        const auto fw=channel.request(device.slot,fw_feature,1,{index},timeout);
        if(!fw || fw.packet.size<12)return {};
        if(fw.packet.data()[0]==0)return owner_profile::verify_identity(device,path,data,fw.packet.data());
    }
    return {};
}

static std::optional<owner_profile::Metadata> read_metadata(const OwnerIdentity& id) {
    DWORD size{};
    if(RegGetValueW(HKEY_CURRENT_USER,discovery_key,L"SensorMetadata",RRF_RT_REG_BINARY,nullptr,nullptr,&size)!=ERROR_SUCCESS ||
       !size || size>owner_profile::max_bytes)return {};
    std::vector<Byte> bytes(size);
    if(RegGetValueW(HKEY_CURRENT_USER,discovery_key,L"SensorMetadata",RRF_RT_REG_BINARY,nullptr,bytes.data(),&size)!=ERROR_SUCCESS ||
       size>bytes.size())return {};
    return owner_profile::decode(id,std::span{bytes}.first(size));
}
static void remember_metadata(const OwnerIdentity& id,const owner_profile::Metadata& data) {
    const auto bytes=owner_profile::encode(id,data);
    if(bytes.empty())return;
    HKEY key{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,discovery_key,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return;
    RegSetValueExW(key,L"SensorMetadata",0,REG_BINARY,bytes.data(),static_cast<DWORD>(bytes.size()));
    RegCloseKey(key);
}

static void read_device_info(Channel& channel, Device& device, bool receiver) {
    if (const auto fw_feat = feature(channel, device.slot, 0x0003)) {
        if (const auto info = channel.request(device.slot, fw_feat, 0); info && info.packet.data().size() >= 5) {
            if (const auto ser = parse_serial_unit_id_0003(info.packet.data()))
                device.serial = *ser;
            const unsigned entities = std::min<unsigned>(info.packet.data()[0], 8);
            for (unsigned e = 0; e < entities; ++e) {
                const auto fw = channel.request(device.slot, fw_feat, 1, {static_cast<Byte>(e)});
                if (fw && !fw.packet.data().empty() && fw.packet.data()[0] == 0) {
                    if (const auto parsed = parse_fw_version_0003(fw.packet.data())) {
                        device.firmware = *parsed;
                        break;
                    }
                }
            }
        }
    }
    if (receiver) {
        if (const auto r_feat = feature(channel, 0xFF, 0x0003)) {
            if (const auto r_info = channel.request(0xFF, r_feat, 0); r_info && !r_info.packet.data().empty()) {
                const unsigned entities = std::min<unsigned>(r_info.packet.data()[0], 8);
                for (unsigned e = 0; e < entities; ++e) {
                    const auto fw = channel.request(0xFF, r_feat, 1, {static_cast<Byte>(e)});
                    if (fw && !fw.packet.data().empty() && fw.packet.data()[0] == 0) {
                        if (const auto parsed = parse_fw_version_0003(fw.packet.data())) {
                            device.receiver_firmware = *parsed;
                            break;
                        }
                    }
                }
            }
        }
    }
}

std::optional<Mouse> connect(HANDLE stop, bool list, bool include_device_info,discovery::OwnerSearch* search) {
    // A saved route only sets the probe order; a fresh identity guards metadata reuse.
    static std::mutex route_mutex;
    static bool route_loaded{};
    static std::optional<discovery::Hint> route;
    static std::optional<owner_profile::Verified> learned;
    std::optional<discovery::Hint> hint;
    if (!list) {
        std::lock_guard lock(route_mutex);
        if (!route_loaded) { route = read_route_hint(); route_loaded = true; }
        hint = route;
    }
    auto interfaces = enumerate();
    discovery::prioritize(interfaces, hint, list);
    auto attempts=discovery::targets(interfaces,hint,list);
    if(search && !list) {
        const bool preferred=hint && !attempts.empty() && interfaces[attempts.front().interface_index].path==hint->path &&
                             attempts.front().slot==hint->slot;
        attempts=search->round(std::move(attempts),preferred,GetTickCount64());
    }
    std::optional<std::size_t> opened;
    std::unique_ptr<Channel> channel;
    for(const auto& attempt:attempts) {
        if(stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0)return std::nullopt;
        const auto& interface=interfaces[attempt.interface_index];
        if(opened!=attempt.interface_index) {
            opened=attempt.interface_index;
            channel=std::make_unique<Channel>(interface,stop);
            if(list && !channel->valid())
                std::printf("046d:%04x: cannot open (Win32 %lu)\n",interface.product,channel->open_error());
        }
        if(!channel->valid())continue;
        const auto slot=attempt.slot;
        const bool owner=search && !list;
        auto device=probe(*channel,static_cast<Byte>(slot),attempt.ping_ms,!owner);
        if (!device) continue;
        if (list) {
            std::printf("046d:%04x slot=%u %s HID++ %u.%u\n", interface.product, slot,
                        device->name.c_str(), device->major, device->minor);
            continue;
        }
        const auto find = [&](unsigned id) { return feature(*channel, device->slot, id); };
        // The mapping owner restores standard reporting for both wheels and
        // buttons before diverting anything. A waking slot can answer a ping
        // before those feature queries succeed. Leave it for the next probe.
        const auto control_feature = [&](unsigned id) {
            const auto timeout=search?std::min<DWORD>(attempt.ping_ms,1200):1200;
            const auto index=feature(*channel,device->slot,id,timeout);
            if(search && !index)
                agent_log::agent_message(agent_log::LogLevel::debug,stderr,
                    "Mouse discovery: slot %u did not return required feature %04X.\n",slot,id);
            return index;
        };
        owner_profile::Features features;
        std::optional<owner_profile::Metadata> metadata;
        const char* metadata_source="live";
        if(owner) {
            device->owner_identity=read_owner_identity(*channel,*device,interface.path,std::min<DWORD>(attempt.ping_ms,1200));
            if(device->owner_identity) {
                std::lock_guard lock(route_mutex);
                if(learned && learned->identity==*device->owner_identity) {
                    metadata=learned->metadata;
                    metadata_source="verified-memory";
                    if(!search->require_live_features)features=learned->features;
                }
            }
        }
        // Confirm that this live feature table is available before using
        // indices learned on a previous connection to the same physical unit.
        bool reused_features=features.complete();
        if(reused_features) {
            const auto buttons=control_feature(0x1B04);
            if(!buttons){search->require_live_features=true;continue;}
            if(buttons!=features.buttons){features={};reused_features=false;}
        }
        if(!reused_features) {
            features.dpi=control_feature(0x2201);
            if(!features.dpi)continue;
            features.buttons=control_feature(0x1B04);
            if(search&&!features.buttons)continue;
            features.wheel=control_feature(0x2121);
            if(search&&!features.wheel)continue;
            features.thumb=control_feature(0x2150);
            if(search&&!features.thumb)continue;
            features.shift=find(0x2111);
            features.enhanced=features.shift!=0;
            if(!features.shift)features.shift=find(0x2110);
            features.battery=find(0x1004);
            features.unified=features.battery!=0;
            if(!features.battery)features.battery=find(0x1000);
        }
        if(device->owner_identity&&!metadata) {
            metadata=read_metadata(*device->owner_identity);
            if(metadata)metadata_source="verified-store";
        }
        if(owner)device->name=metadata?metadata->name:read_device_name(*channel,device->slot);
        if (include_device_info) read_device_info(*channel, *device, interface.receiver);
        Mouse mouse{
            .channel = nullptr, .device = std::move(*device), .dpi = features.dpi,
            .shift = features.shift, .wheel = features.wheel,
            .thumb = features.thumb, .buttons = features.buttons,
            .battery = features.battery, .host = 0,
            .hosts_info = 0,
            .enhanced_shift = features.enhanced, .unified_battery = features.unified,
            .dpi_choices = {}
        };
        mouse.channel = std::move(channel);
        mouse.dpi_choices = metadata?metadata->choices:read_dpi_choices(mouse);
        {
            std::lock_guard lock(route_mutex);
            const discovery::Hint connected{mouse.device.slot, interface.path};
            if (route != connected) {
                remember_route_hint(connected);
                route = connected;
            }
            if(const auto& id=mouse.device.owner_identity) {
                if(!metadata) {
                    metadata=owner_profile::Metadata{mouse.device.name,mouse.dpi_choices};
                    if(metadata->valid())remember_metadata(*id,*metadata);
                }
                if(metadata->valid())learned=owner_profile::Verified{*id,features,*metadata};
                else learned.reset();
            } else if(owner)learned.reset();
        }
        if(owner) {
            search->require_live_features=false;
            agent_log::agent_message(agent_log::LogLevel::debug,stderr,"Owner discovery: identity=%s feature-map=%s sensor-metadata=%s.\n",
                mouse.device.owner_identity?"verified":"unavailable",reused_features?"verified-memory":"live",metadata_source);
        }
        return mouse;
    }
    return std::nullopt;
}

static void ensure_hosts(Mouse& mouse) {
    if (!mouse.channel) return;
    if (!mouse.host) mouse.host = feature(*mouse.channel, mouse.device.slot, 0x1814);
    if (!mouse.hosts_info) mouse.hosts_info = feature(*mouse.channel, mouse.device.slot, 0x1815);
}

std::vector<HostSlot> hosts(Mouse& mouse) {
    ensure_hosts(mouse);
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
        ensure_hosts(mouse);
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

bool restore_saved_dpi(const std::filesystem::path& mapping, Mouse& mouse, std::optional<unsigned>& observed) {
    observed.reset();
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
    observed = get(mouse, L"dpi");
    if (observed == value) return true;
    observed.reset();
    if (!set(mouse, L"dpi", value)) return false;
    observed = get(mouse, L"dpi");
    if (observed != value) return false;
    agent_log::agent_message(agent_log::LogLevel::info,stdout,"Restored saved pointer speed: %u DPI.\n",value);
    return true;
}

bool restore_standard_input(Mouse& mouse, StandardReporting* observed, std::optional<ControlCatalog>* previous) {
    // A stopped Options+ process can leave both wheels routed to HID++.
    // Establish normal reporting before enabling the mappings we actually handle.
    auto wheel = mouse.request(mouse.wheel, 1);
    auto thumb = mouse.request(mouse.thumb, 1);
    if (!wheel || wheel.packet.size < 5 || !thumb || thumb.packet.size < 6) return false;
    const auto mode = static_cast<Byte>(wheel.packet.data()[0] & ~1);
    if (wheel.packet.data()[0] != mode) {
        if (!mouse.request(mouse.wheel, 2, {mode})) return false;
        wheel = mouse.request(mouse.wheel, 1);
        if (!wheel || wheel.packet.size < 5 || wheel.packet.data()[0] != mode) return false;
    }
    if (thumb.packet.data()[0]) {
        const auto flags = thumb.packet.data()[1];
        if (!mouse.request(mouse.thumb, 2, {0, flags})) return false;
        thumb = mouse.request(mouse.thumb, 1);
        if (!thumb || thumb.packet.size < 6 || thumb.packet.data()[0] || thumb.packet.data()[1] != flags) return false;
    }
    const auto count = mouse.request(mouse.buttons, 0);
    if (!count || count.packet.size < 5) return false;
    const auto total=count.packet.data()[0];
    StandardReporting state{wheel.packet, thumb.packet, {}, {}};
    const auto normalize=[&](unsigned id) {
        auto current = mouse.request(mouse.buttons, 2, {high(id), low(id)});
        if (!current || current.packet.size < 7 || word(current.packet.data()) != id) return false;
        if (current.packet.data()[2] & 0x11) {
            if (!mouse.request(mouse.buttons, 3, {high(id), low(id), 0x22})) return false;
            current = mouse.request(mouse.buttons, 2, {high(id), low(id)});
            if (!current || current.packet.size < 7 || word(current.packet.data()) != id ||
                (current.packet.data()[2] & 0x11)) return false;
        }
        if (observed) state.controls.push_back(current.packet);
        return true;
    };
    const bool identified=previous && observed && mouse.device.name=="MX Master 3S" && mouse.device.owner_identity && total;
    if (identified && *previous && (*previous)->owner==*mouse.device.owner_identity && (*previous)->count==total) {
        // Only the read-only catalog survives a reconnect. Re-query the live
        // routing for every divertable CID, including controls without bindings.
        if (std::ranges::all_of((*previous)->divertable,normalize)) {
            agent_log::agent_message(agent_log::LogLevel::debug,stdout,
                "Reused control catalog: verified %zu live reporting states, skipped %u metadata requests.\n",
                state.controls.size(),static_cast<unsigned>(total));
            *observed=std::move(state);
            return true;
        }
        state.controls.clear();
    }
    if (previous) previous->reset();
    if (identified) state.learned.emplace(*mouse.device.owner_identity,total,std::vector<unsigned>{});
    std::vector<unsigned> seen;
    for (unsigned i=0; i<total; ++i) {
        const auto info=mouse.request(mouse.buttons,1,{low(i)});
        if (!info || info.packet.size<13) return false;
        const auto data=info.packet.data();
        const auto id=word(data);
        if (state.learned) {
            if (!id || std::ranges::contains(seen,id)) state.learned.reset();
            else seen.push_back(id);
        }
        if (!(data[4]&0x20)) continue;
        if (!normalize(id)) return false;
        if (state.learned) state.learned->divertable.push_back(id);
    }
    if (state.learned && state.learned->divertable.empty()) state.learned.reset();
    if (observed) *observed = std::move(state);
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
