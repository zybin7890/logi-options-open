#include "mouse.hpp"
#include "remap.hpp"
#include "../first_run.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace {

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
    std::printf(",\"receiver_slot\":%u,\"hidpp\":\"%u.%u\",\"battery\":",
                mouse.device.slot, mouse.device.major, mouse.device.minor);
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
    std::puts("]}");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    const std::wstring_view command = argc > 1 ? argv[1] : L"help";
    if (command==L"disclaimer") { std::puts(loo::notice_text); return 0; }
    if (command==L"check-disclaimer") {
        if (!loo::notice_required()) return 0;
        std::fprintf(stderr,"%s\nRun logi-cli accept-disclaimer after reading.\n",loo::notice_text);
        return 2;
    }
    if (command==L"accept-disclaimer") {
        std::puts(loo::notice_text);
        return loo::accept_notice()?0:2;
    }
    const bool changes_input=command==L"watch" || command==L"run" || command==L"run-preview" ||
        command==L"set" || command==L"switch-host" || command==L"play-actions";
    if (changes_input && loo::notice_required()) {
        std::fprintf(stderr,"%s\n\nRead the notice and run logi-cli accept-disclaimer to acknowledge it.\n",loo::notice_text);
        return 2;
    }
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
              "  list                        List awake HID++ devices\n"
              "  status                      Read first compatible mouse\n"
              "  status-json                 Read mouse settings as JSON\n"
              "  agent-status                Read native and official process state as JSON\n"
              "  set <setting> <value>       Set DPI or wheel behavior\n"
              "  hosts                       List Easy-Switch pairing states\n"
              "  hosts-json                  List Easy-Switch pairing states as JSON\n"
              "  switch-host <slot>          Request a paired Easy-Switch host\n"
              "  run <mappings.ini>          Run remaps and gestures\n"
              "  run-preview <mappings.ini>  Temporarily take over and restore Options+ on stop\n"
              "  preview-ring <mappings.ini> Preview action selection without executing it\n"
              "  check-config <mappings.ini> Validate mappings before starting\n"
              "  stop                        Stop and restore button reporting\n"
              "  reload <mappings.ini>       Confirm this configuration version in the running agent\n"
              "  recover-input               Restore standard wheel/buttons with agents stopped\n"
              "  watch                       Measure the passive event loop\n"
              "Settings: dpi, smartshift (0-50), ratchet, hires, invert, thumb-invert");
    return command == L"help" || command == L"--help" ? 0 : 2;
}
