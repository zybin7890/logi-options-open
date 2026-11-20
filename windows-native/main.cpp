#include "cli_entry.hpp"
#include "mouse.hpp"
#include "remap.hpp"
#include "notifications_hud.hpp"
#include "ui_settings.hpp"
#include "flow.hpp"
#include "../first_run.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>

namespace {

struct FlowRuntime {
    std::mutex mutex;
    flow::Controller controller;
    bool active{};
    unsigned active_port{};
    std::string active_pin;
    ULONGLONG last_edge_ms{};
    ULONGLONG last_beacon_ms{};

    void stop() {
        std::lock_guard lock(mutex);
        if (active) {
            controller.stop();
            active = false;
        }
    }

    unsigned poll(const ui_settings::Settings& settings, std::wstring& peer_name) {
        std::lock_guard lock(mutex);
        if (!settings.flow_enabled) {
            if (active) {
                controller.stop();
                active = false;
            }
            return 0;
        }
        const std::string pin = ui_settings::wide_to_utf8(settings.flow_pin);
        if (!active || active_port != settings.flow_port || active_pin != pin) {
            controller.stop();
            wchar_t host_buf[128]{};
            DWORD host_len = std::size(host_buf);
            if (!GetComputerNameW(host_buf, &host_len)) wcscpy_s(host_buf, L"LogiNative-PC");
            const std::string host_utf8 = ui_settings::wide_to_utf8(host_buf);
            const unsigned sw = static_cast<unsigned>(std::max(640, GetSystemMetrics(SM_CXSCREEN)));
            const unsigned sh = static_cast<unsigned>(std::max(480, GetSystemMetrics(SM_CYSCREEN)));
            flow::Controller::Callbacks cb;
            cb.on_edge_enter = [share_clip = settings.flow_share_clipboard, light = ui_settings::effective_light(settings.theme)](const flow::EdgeEvent& ev) {
                const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                                   GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                   GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
                const POINT arrival = flow::map_arrival_cursor(desktop, ev.edge, ev.normalized_axis);
                SetCursorPos(arrival.x, arrival.y);
                if (share_clip && !ev.clipboard_text.empty()) {
                    flow::write_clipboard_text(ev.clipboard_text);
                }
                notifications_hud::dispatch_show(L"Logi Flow", L"已切入本机 · 来自 " + ev.peer_name,
                                                 notifications_hud::Kind::flow, 1600, light);
            };
            cb.on_clipboard = [share_clip = settings.flow_share_clipboard](const std::wstring& text) {
                if (share_clip && !text.empty()) flow::write_clipboard_text(text);
            };
            active = controller.start(settings.flow_port, pin, host_utf8, host_utf8, sw, sh, std::move(cb), true);
            active_port = settings.flow_port;
            active_pin = pin;
        }
        const ULONGLONG now = GetTickCount64();
        if (active && now - last_beacon_ms >= 5000) {
            last_beacon_ms = now;
            controller.broadcast_presence();
        }
        if (!active || settings.flow_peers.empty() || now - last_edge_ms < 800) return 0;
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return 0;
        const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                           GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                           GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        const bool ctrl_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto hit = flow::evaluate_edge_hit(cursor, desktop, settings.flow_peers,
                                                 settings.flow_hold_ctrl, ctrl_down, settings.flow_edge_corners);
        if (!hit || hit->peer_index >= settings.flow_peers.size()) return 0;
        const auto& peer = settings.flow_peers[hit->peer_index];
        const std::wstring clip = settings.flow_share_clipboard ? flow::read_clipboard_text().value_or(L"") : std::wstring{};
        if (controller.send_edge_switch(peer, hit->normalized_axis, clip, pin, 900)) {
            last_edge_ms = now;
            peer_name = peer.name;
            return peer.host_slot;
        }
        return 0;
    }
};

FlowRuntime& flow_runtime() {
    static FlowRuntime rt;
    return rt;
}

unsigned flow_poll_bridge(const ui_settings::Settings& s, std::wstring& peer_name) {
    return flow_runtime().poll(s, peer_name);
}

void flow_stop_bridge() {
    flow_runtime().stop();
}

void json_string(std::string_view value) {
    std::putchar('"');
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') std::putchar('\\');
        if (ch < 32) std::printf("\\u%04x", ch);
        else std::putchar(ch);
    }
    std::putchar('"');
}

const char* json_boolean(bool value) { return value ? "true" : "false"; }

std::optional<unsigned> read_status_setting(logi::Mouse& mouse,std::wstring_view setting,
                                          const AgentSettingsSnapshot& cached,bool owned) {
    if (owned && setting==L"thumb-invert")
        return cached.available && (cached.observed_mask&16u)?std::optional{cached.observed[4]}:std::nullopt;
    return logi::get(mouse,setting);
}

void print_status_text(logi::Mouse& mouse) {
    const auto cached=read_agent_settings();
    const bool owned=cached.available || agent_running();
    std::printf("%s (slot %u, HID++ %u.%u)\n",mouse.device.name.c_str(),mouse.device.slot,mouse.device.major,mouse.device.minor);
    if (!mouse.device.serial.empty()) std::printf("serial=%ls\n", mouse.device.serial.c_str());
    if (!mouse.device.firmware.empty()) std::printf("firmware=%ls\n", mouse.device.firmware.c_str());
    if (!mouse.device.receiver_firmware.empty()) std::printf("receiver-firmware=%ls\n", mouse.device.receiver_firmware.c_str());
    if (const auto battery=mouse.request(mouse.battery,mouse.unified_battery?1:0);battery) {
        const auto data=battery.packet.data();
        if (data[0]>0 && data[0]<=100) std::printf("battery=%u%% ",data[0]);
        std::printf("battery-status=0x%02x\n",data[2]);
    }
    for (const auto name:{L"dpi",L"smartshift",L"ratchet",L"hires",L"invert",L"thumb-invert",L"host"}) {
        if (const auto value=read_status_setting(mouse,name,cached,owned)) std::printf("%ls=%u\n",name,*value);
        else if (owned && std::wstring_view(name)==L"thumb-invert") std::puts("thumb-invert=unknown");
    }
    if (owned) std::printf("thumb-direction-source=%s software=%s sampled_at_ms=%llu\n",
        cached.available && (cached.observed_mask&16u)?"daemon-last-confirmed":"unknown",
        json_boolean(cached.available && (cached.software_mask&16u)),cached.sampled_at_ms[4]);
    if (!mouse.dpi_choices.empty()) {
        std::printf("dpi-options:");
        for (const auto value:mouse.dpi_choices) std::printf(" %u",value);
        std::puts("");
    }
    const auto count=mouse.request(mouse.buttons,0);
    if (!count) return;
    std::puts("HID++ controls (D = divertable, G = raw XY gesture):");
    for (unsigned i=0;i<count.packet.data()[0];++i) {
        const auto info=mouse.request(mouse.buttons,1,{logi::low(i)});
        if (!info || info.packet.data().size()<9) continue;
        const auto data=info.packet.data();
        std::printf("  %04X  %c%c\n",logi::word(data),data[4]&0x20?'D':'-',data[8]&1?'G':'-');
    }
}

void print_hosts(const std::vector<logi::HostSlot>& slots) {
    std::putchar('[');
    bool first = true;
    for (const auto& slot : slots) {
        if (!first) std::putchar(',');
        first = false;
        std::printf("{\"slot\":%u,\"current\":%s,\"paired\":%s}", slot.slot, json_boolean(slot.current),
                    slot.paired ? json_boolean(*slot.paired) : "null");
    }
    std::puts("]");
}

void print_status(logi::Mouse& mouse) {
    const auto cached=read_agent_settings();
    const bool owned=cached.available || agent_running();
    std::printf("{\"device\":");
    json_string(mouse.device.name);
    std::printf(",\"receiver_slot\":%u,\"hidpp\":\"%u.%u\"",
                mouse.device.slot, mouse.device.major, mouse.device.minor);
    if (!mouse.device.serial.empty()) {
        std::fputs(",\"serial\":", stdout);
        json_string(ui_settings::wide_to_utf8(mouse.device.serial));
    }
    if (!mouse.device.firmware.empty()) {
        std::fputs(",\"firmware\":", stdout);
        json_string(ui_settings::wide_to_utf8(mouse.device.firmware));
    }
    if (!mouse.device.receiver_firmware.empty()) {
        std::fputs(",\"receiver_firmware\":", stdout);
        json_string(ui_settings::wide_to_utf8(mouse.device.receiver_firmware));
    }
    std::fputs(",\"battery\":", stdout);
    const auto battery = mouse.request(mouse.battery, mouse.unified_battery ? 1 : 0);
    if (battery && battery.packet.data()[0] <= 100) std::printf("%u", battery.packet.data()[0]);
    else std::fputs("null", stdout);
    std::fputs(",\"settings\":{", stdout);
    bool first = true;
    for (const auto setting : {L"dpi", L"smartshift", L"ratchet", L"hires", L"invert", L"thumb-invert", L"host"}) {
        if (!first) std::putchar(',');
        first = false;
        std::printf("\"%ls\":", setting);
        const auto value=read_status_setting(mouse,setting,cached,owned);
        if (value) std::printf("%u", *value);
        else std::fputs("null", stdout);
    }
    std::printf("},\"thumb_direction_source\":\"%s\",\"thumb_direction_software\":%s,"
                "\"thumb_direction_sampled_at_ms\":%llu,\"dpi_choices\":[",
                owned?(cached.available && (cached.observed_mask&16u)?"daemon_last_confirmed":"unknown"):"hardware_readback",
                json_boolean(cached.available && (cached.software_mask&16u)),cached.sampled_at_ms[4]);
    for (std::size_t i = 0; i < mouse.dpi_choices.size(); ++i) {
        if (i) std::putchar(',');
        std::printf("%u", mouse.dpi_choices[i]);
    }
    std::fputs("]",stdout);
    std::optional<unsigned> wheel_mode;
    auto sampled_at=cached.wheel_mode_sampled_at_ms;
    const char* source="unknown";
    if(cached.available&&cached.wheel_mode_valid){wheel_mode=cached.wheel_mode;source="daemon_last_confirmed";}
    else if(!owned) {
        const auto mode=mouse.request(mouse.wheel,1);
        if(mode&&mode.packet.size>=5){wheel_mode=mode.packet.data()[0];sampled_at=GetTickCount64();source="hardware_readback";}
    }
    if(wheel_mode)
        std::printf(",\"wheel_mode\":%u,\"wheel_target\":%u,\"wheel_mode_source\":\"%s\",\"wheel_mode_sampled_at_ms\":%llu",
            *wheel_mode,*wheel_mode&1,source,sampled_at);
    else std::fputs(",\"wheel_mode\":null,\"wheel_target\":null,\"wheel_mode_source\":\"unknown\",\"wheel_mode_sampled_at_ms\":null",stdout);
    std::puts("}");
}

} // namespace

namespace cli_entry {

void init_agent_hooks() {
    notifications_hud::init_hooks();
    ui_settings::flow_poll_hook() = &flow_poll_bridge;
    ui_settings::flow_stop_hook() = &flow_stop_bridge;
}

void shutdown_agent_hooks() {
    flow_stop_bridge();
    notifications_hud::shutdown();
}

void attach_parent_console_if_needed() {
    const auto needs_bind = [](DWORD std_id) {
        const HANDLE h = GetStdHandle(std_id);
        return !h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN;
    };
    const bool need_out = needs_bind(STD_OUTPUT_HANDLE);
    const bool need_err = needs_bind(STD_ERROR_HANDLE);
    if (!need_out && !need_err) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* dummy{};
    if (need_out) freopen_s(&dummy, "CONOUT$", "w", stdout);
    if (need_err) freopen_s(&dummy, "CONOUT$", "w", stderr);
}

int run_cli_main(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    const std::wstring_view command = argc > 1 ? argv[1] : L"help";
    if (command == L"disclaimer" && argc == 2) {
        std::puts(loo::notice_text);
        return 0;
    }
    if (command == L"accept-disclaimer" && argc == 2) {
        std::puts(loo::notice_text);
        return loo::accept_notice() ? 0 : 2;
    }
    const bool check_notice = command == L"check-disclaimer" && argc == 2;
    const bool changes_input = command == L"watch" || command == L"run" || command == L"run-preview" ||
        command == L"set" || command == L"switch-host" || command == L"play-actions" || command == L"reload";
    if ((check_notice || changes_input) && loo::notice_required()) {
        std::fprintf(stderr, "%s\n\nRead the notice and run logi-native.exe accept-disclaimer to continue.\n", loo::notice_text);
        return 2;
    }
    if (check_notice) return 0;
    init_agent_hooks();
    struct DaemonShutdownGuard {
        ~DaemonShutdownGuard() { shutdown_agent_hooks(); }
    } shutdown_guard;
    if (command == L"list" && argc == 2) {
        logi::connect(nullptr, true);
        return 0;
    }
    if (command == L"watch" && argc == 2) return run_agent(nullptr);
    if (command == L"run" && argc == 3) return run_agent(argv[2]);
    if (command == L"run-preview" && argc == 3) return run_agent(argv[2], true);
    if (command == L"preview-ring" && argc == 3) return preview_ring(argv[2]);
    if ((command == L"play-actions" || command == L"check-actions") && argc == 3)
        return play_actions(argv[2], command == L"check-actions");
    if (command == L"check-config" && argc == 3) {
        if (!check_mappings(argv[2])) return 2;
        std::puts("Configuration is valid.");
        return 0;
    }
    if (command == L"stop" && argc == 2) return stop_agent();
    if (command == L"reload" && argc==3) {
        std::ifstream file(std::filesystem::path(argv[2]),std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        ReloadFingerprint fingerprint{};
        if (!file.is_open() || file.bad() || !mapping_fingerprint(bytes,fingerprint)) {
            std::puts("{\"confirmed\":false,\"status\":\"read_failed\"}");
            return 2;
        }
        constexpr unsigned long long revision=1;
        auto result=reload_agent_confirmed(argv[2],revision,fingerprint);
        if (result.status==ReloadStatus::applied &&
            (result.revision!=revision || result.fingerprint!=fingerprint || !is_current_reload_result(result)))
            result.status=ReloadStatus::superseded;
        const bool confirmed=result.status==ReloadStatus::applied;
        std::printf("{\"confirmed\":%s,\"status\":\"%s\",\"request_id\":%llu,\"instance_id\":%llu,"
                    "\"pid\":%lu,\"revision\":%llu,\"fingerprint\":\"",json_boolean(confirmed),
                    reload_status_name(result.status),result.request_id,result.instance_id,result.pid,result.revision);
        for (const auto byte:result.fingerprint) std::printf("%02x",static_cast<unsigned>(byte));
        std::puts("\"}");
        return confirmed?0:2;
    }
    if (command == L"recover-input" && argc == 2) {
        if (agent_running() || options_running()) {
            std::fputs("Exit both mouse agents before recovering standard input.\n", stderr);
            return 2;
        }
        auto mouse = logi::connect();
        if (!mouse || !logi::restore_standard_input(*mouse)) {
            std::fputs("Could not restore all standard input reports.\n", stderr);
            return 2;
        }
        std::puts("Standard wheel and button reporting restored and verified.");
        return 0;
    }
    if (command == L"agent-status" && argc == 2) {
        print_agent_status();
        return 0;
    }

    const bool switch_host = command == L"switch-host" && argc == 3;
    const bool settings_read = argc == 2 && (command == L"status" || command == L"status-json");
    const bool hosts_read = argc == 2 && (command == L"hosts" || command == L"hosts-json");
    if (settings_read || hosts_read || (command == L"set" && argc == 4) || switch_host) {
        if ((command == L"set" || switch_host) && options_running()) {
            std::fputs("Options+ agent is active. Exit it before changing mouse settings.\n", stderr);
            return 2;
        }
        if (agent_running() && switch_host) {
            std::fputs("Stop remapping before switching hosts.\n", stderr);
            return 2;
        }
        constexpr std::wstring_view wheel_keys[]{L"smartshift",L"ratchet",L"hires",L"invert",L"thumb-invert"};
        if (command==L"set") for (unsigned index=0;index<std::size(wheel_keys);++index) {
            if (std::wstring_view(argv[2])!=wheel_keys[index]) continue;
            const auto value=logi::number(argv[3]);
            if (!value || *value>(index==0?50u:1u)) {
                std::fputs("Invalid wheel setting value.\n",stderr);
                return 2;
            }
            wchar_t executable[4096]{};
            const auto length=GetModuleFileNameW(nullptr,executable,std::size(executable));
            if (!length || length>=std::size(executable)) return 2;
            const auto mapping=std::filesystem::path(executable).parent_path()/L"mappings.ini";
            constexpr unsigned long long revision=1;
            const auto key=static_cast<SettingKey>(index);
            const auto result=set_agent_setting(key,*value,revision,4000,mapping.c_str());
            const bool confirmed=result.key==key && result.requested==*value && result.revision==revision &&
                result.applied && (result.observed_mask&(1u<<index)) && result.observed[index]==*value &&
                is_current_setting_result(result);
            std::printf("{\"status\":\"%s\",\"confirmed\":%s,\"saved\":%s,\"software\":%s,"
                        "\"request_id\":%llu,\"instance_id\":%llu,\"pid\":%lu,\"revision\":%llu,\"observed\":{",
                        setting_status_name(result.status),json_boolean(confirmed),json_boolean(confirmed && result.saved),
                        json_boolean(result.software),result.request_id,result.instance_id,result.pid,result.revision);
            bool first=true;
            for (unsigned bit=0;bit<std::size(wheel_keys);++bit) if (result.observed_mask&(1u<<bit)) {
                if (!first) std::putchar(',');
                first=false;
                std::printf("\"%ls\":%u",wheel_keys[bit].data(),result.observed[bit]);
            }
            std::puts("}}");
            if (result.status==SettingStatus::not_running)
                std::fputs("Start the native background agent before changing wheel settings.\n",stderr);
            return confirmed && result.saved?0:2;
        }
        if (command==L"set" && std::wstring_view(argv[2])==L"dpi") {
            const auto value=logi::number(argv[3]);
            if (!value) {
                std::fputs("Invalid DPI value.\n",stderr);
                return 2;
            }
            wchar_t executable[4096]{};
            const auto length=GetModuleFileNameW(nullptr,executable,std::size(executable));
            if (!length || length>=std::size(executable)) {
                std::fputs("Cannot identify the settings path.\n",stderr);
                return 2;
            }
            const auto mapping=std::filesystem::path(executable).parent_path()/L"mappings.ini";
            const bool owned=agent_running();
            const auto result=set_agent_dpi(*value,4000,mapping.c_str());
            if (owned || result.status!=DpiCommandStatus::not_running) {
                std::printf("dpi_status=%s request_id=%llu requested=%u applied=%s saved=%s\n",
                            dpi_command_status_name(result.status),result.request_id,result.requested,
                            json_boolean(result.applied),json_boolean(result.saved));
                if (result.observed_valid) std::printf("dpi=%u\n",result.observed);
                if (!result.applied || !result.saved) {
                    std::fputs(result.applied?"Pointer speed applied, but could not save it for the next start.\n":
                               "DPI was not confirmed by the daemon; direct HID fallback is disabled.\n",stderr);
                    return 2;
                }
                return 0;
            }
        }
        auto mouse = logi::connect();
        if (!mouse) {
            std::fputs("No responsive HID++ mouse with adjustable DPI.\n", stderr);
            return 2;
        }
        if (command == L"status") {
            print_status_text(*mouse);
            return 0;
        }
        if (command == L"status-json") {
            print_status(*mouse);
            return 0;
        }
        if (hosts_read) {
            const auto slots = logi::hosts(*mouse);
            if (command == L"hosts-json") {
                print_hosts(slots);
                return slots.empty() ? 2 : 0;
            }
            for (const auto& slot : slots) {
                const auto status = slot.paired ? (*slot.paired ? "paired" : "unpaired") : "unknown";
                std::printf("slot=%u %s%s\n", slot.slot, status, slot.current ? " current" : "");
            }
            return slots.empty() ? 2 : 0;
        }
        if (switch_host) {
            const auto target = logi::number(argv[2]);
            if (!target || !logi::switch_host(*mouse, *target)) {
                std::fputs("Target host is unavailable, unpaired, or could not be requested.\n", stderr);
                return 2;
            }
            std::puts("Host request accepted locally.");
            return 0;
        }
        const auto value = logi::number(argv[3]);
        if (!value || !logi::set(*mouse, argv[2], *value)) {
            std::fputs("Setting unsupported, value invalid, or device write failed.\n", stderr);
            return 2;
        }
        const auto actual = logi::get(*mouse, argv[2]);
        if (actual) std::printf("%ls=%u\n", argv[2], *actual);
        if (actual != value) {
            std::fputs("Device readback differs from the requested value.\n", stderr);
            return 2;
        }
        if (std::wstring_view(argv[2]) == L"dpi") {
            wchar_t executable[4096]{};
            const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
            if (!length || length >= std::size(executable) ||
                !logi::remember_dpi(std::filesystem::path(executable).parent_path() / L"mappings.ini",
                                    *mouse, *value)) {
                std::fputs("Pointer speed applied, but could not save it for the next start.\n", stderr);
                return 2;
            }
        }
        return 0;
    }
    std::puts("LogiNative | Windows x64\n"
              "  disclaimer                  Show the first-use notice\n"
              "  check-disclaimer            Check whether the notice has been acknowledged\n"
              "  accept-disclaimer           Acknowledge the notice before changing input\n"
              "  list                        List awake HID++ devices\n"
              "  status                      Read first compatible mouse\n"
              "  status-json                 Read mouse settings as JSON\n"
              "  agent-status                Read native and official process state as JSON\n"
              "  set <setting> <value>       Set DPI or wheel behavior\n"
              "  hosts                       List Easy-Switch pairing states\n"
              "  hosts-json                  List Easy-Switch pairing states as JSON\n"
              "  switch-host <slot>          Request a paired Easy-Switch host\n"
              "  run <mappings.ini>          Run remaps and gestures\n"
              "  run-preview <mappings.ini>  Run remaps after closing conflicting daemons\n"
              "  preview-ring <mappings.ini> Preview action selection without executing it\n"
              "  check-config <mappings.ini> Validate mappings before starting\n"
              "  stop                        Stop and restore button reporting\n"
              "  reload <mappings.ini>       Confirm this configuration version in the running agent\n"
              "  recover-input               Restore standard wheel/buttons with agents stopped\n"
              "  watch                       Measure the passive event loop\n"
              "Settings: dpi, smartshift (0-50), ratchet, hires, invert, thumb-invert");
    return command == L"help" || command == L"--help" ? 0 : 2;
}

} // namespace cli_entry
