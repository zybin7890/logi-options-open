#include "mouse.hpp"
#include "remap.hpp"
#include "first_run.hpp"

#include <cstdio>

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
        if (const auto value = logi::get(mouse, setting)) std::printf("%u", *value);
        else std::fputs("null", stdout);
    }
    std::fputs("},\"dpi_choices\":[", stdout);
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
        std::printf("{\"reload_requested\":%s}\n",reload_agent(argv[2])?"true":"false");
        return 0;
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
        std::printf("{\"native\":%s,\"options_plus\":%s}\n",
                    json_boolean(agent_running()), json_boolean(options_running()));
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
        if (agent_running() && (switch_host || (command == L"set" && std::wstring_view(argv[2]) == L"thumb-invert"))) {
            std::fputs("Stop remapping before changing thumb direction or switching hosts.\n", stderr);
            return 2;
        }
        auto mouse = logi::connect();
        if (!mouse) {
            std::fputs("No responsive HID++ mouse with adjustable DPI.\n", stderr);
            return 2;
        }
        if (command == L"status") {
            logi::describe(*mouse);
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
        return 0;
    }
    std::puts("Logi Options Open | Windows x64 | experimental third-party software\n"
              "  disclaimer                  Show the first-use notice\n"
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
              "  run-preview <mappings.ini>  Temporarily take over and restore Options+ on stop\n"
              "  preview-ring <mappings.ini> Preview action selection without executing it\n"
              "  check-config <mappings.ini> Validate mappings before starting\n"
              "  stop                        Stop and restore button reporting\n"
              "  reload <mappings.ini>       Notify the matching running agent without restarting\n"
              "  recover-input               Restore standard wheel/buttons with agents stopped\n"
              "  watch                       Measure the passive event loop\n"
              "Settings: dpi, smartshift (0-50), ratchet, hires, invert, thumb-invert");
    return command == L"help" || command == L"--help" ? 0 : 2;
}
