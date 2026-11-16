#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "reject_effects.hpp"
#include "../ui_settings.hpp"
#include "../options_plus_import.hpp"
#include "../zip_codec.hpp"
#include "../ring_profile_package.hpp"
#include "../pairing.hpp"
#include "../flow.hpp"
#include "../continuous_media.hpp"
#include "../remap.cpp"

int choose_ring_action(std::span<const std::wstring>, HANDLE, bool, std::span<const RingIcon>, float) {
    return -1;
}

namespace {
bool parse_text(const char* text, Mappings& result) {
    std::istringstream input(text);
    return parse_mappings(input, result);
}
} // namespace

int main() {
    namespace fs = std::filesystem;
    const auto temp_root = fs::temp_directory_path() / L"loginative_parity_test";
    std::error_code ec;
    fs::remove_all(temp_root, ec);
    fs::create_directories(temp_root, ec);

    // 1. ui_settings round-trip persistence & tr/scale helpers
    {
        const auto mapping_ini = temp_root / L"mappings.ini";
        ui_settings::Settings s;
        s.theme = ui_settings::Theme::light;
        s.language = ui_settings::Language::en_us;
        s.ring_size = ui_settings::RingSize::large;
        s.notify_low_battery = false;
        s.notify_locks = true;
        s.notify_mic = false;
        s.close_to_tray = false;
        s.flow_enabled = true;
        s.flow_share_clipboard = true;
        s.flow_hold_ctrl = true;
        s.flow_edge_corners = false;
        s.flow_pin = L"PIN-9981";
        s.flow_port = 59901;
        s.flow_peers.push_back({L"peer-2", L"Workstation-B", L"192.168.1.42", 59902,
                                ui_settings::FlowEdge::left, 3, 2560, 1440});
        assert(ui_settings::save(mapping_ini, s));

        const auto loaded = ui_settings::load(mapping_ini);
        assert(loaded.theme == ui_settings::Theme::light);
        assert(loaded.language == ui_settings::Language::en_us);
        assert(loaded.ring_size == ui_settings::RingSize::large);
        assert(!loaded.notify_low_battery && loaded.notify_locks && !loaded.notify_mic);
        assert(!loaded.close_to_tray);
        assert(loaded.flow_enabled && loaded.flow_share_clipboard && loaded.flow_hold_ctrl && !loaded.flow_edge_corners);
        assert(loaded.flow_pin == L"PIN-9981");
        assert(loaded.flow_port == 59901);
        assert(loaded.flow_peers.size() == 1);
        assert(loaded.flow_peers[0].name == L"Workstation-B");
        assert(loaded.flow_peers[0].edge == ui_settings::FlowEdge::left);
        assert(loaded.flow_peers[0].host_slot == 3);
        assert(loaded.flow_peers[0].screen_width == 2560 && loaded.flow_peers[0].screen_height == 1440);
        assert(ui_settings::ring_scale(ui_settings::RingSize::small) < 0.9f);
        assert(ui_settings::ring_scale(ui_settings::RingSize::large) > 1.1f);
        assert(std::wstring(ui_settings::tr(ui_settings::Language::en_us, L"中文", L"EN")) == L"EN");
    }

    // 2. zip_codec + .lp5 ZIP container round-trip (Stored + manifest + icon payload)
    {
        ring_metadata::Settings meta;
        meta.adaptive = true;
        meta.active[L"global"] = L"ring";
        meta.icons[L"ring"][0].text = L"Lock";
        meta.icons[L"ring"][0].glyph = L"lock";
        const auto lp5 = ring_profile_package::encode_lp5(
            L"[ring]\nLock=LockWorkStation\nVolume=VolumeUp\n", meta, L"WorkRing");
        assert(zip_codec::is_zip(lp5));
        const auto entries = zip_codec::unpack(lp5);
        assert(entries.contains("manifest.json"));
        assert(entries.contains("ring_profile.ini"));
        const auto pkg = ring_profile_package::decode_bytes(lp5);
        assert(pkg.source.find(L"LockWorkStation") != std::wstring::npos);
        const auto parsed_meta = ring_metadata::read(pkg.source);
        assert(parsed_meta.adaptive);
        assert(parsed_meta.icons.at(L"ring")[0].glyph == L"lock");
    }

    // 3. pairing HID++ 0x0003 parsers + BT enumeration smoke check
    {
        const logi::Byte serial_pkt[5]{0x02, 0x12, 0xAB, 0xCD, 0xEF};
        const auto serial = pairing::parse_serial_unit_id(serial_pkt);
        assert(serial && *serial == L"12ABCDEF");

        // FW v01.02 build B0045 (BCD: prefix RQM, ver1=0x01, ver2=0x02, build_hi=0x00, build_lo=0x45)
        const logi::Byte fw_pkt[10]{0x00, 'R', 'Q', 'M', 0x01, 0x02, 0x00, 0x45, 0x00, 0x00};
        const auto fw = pairing::parse_fw_version(fw_pkt);
        assert(fw && *fw == L"RQM 01.02.B0045");

        const auto bt = pairing::enumerate_bluetooth_devices();
        (void)bt;
    }

    // 4. flow pure logic + BCrypt HMAC + live 127.0.0.1 two-instance TCP loopback + clipboard
    {
        ui_settings::FlowPeerConfig peer{L"p1", L"PeerOne", L"127.0.0.1", 0,
                                          ui_settings::FlowEdge::right, 2, 1920, 1080};
        const std::vector<ui_settings::FlowPeerConfig> peers{peer};
        const RECT desk{0, 0, 1920, 1080};

        // Corner deadzone blocks y=5 when corner_deadzone=true
        assert(!flow::evaluate_edge_hit({1919, 5}, desk, peers, false, false, true).has_value());
        // Hold Ctrl requirement
        assert(!flow::evaluate_edge_hit({1919, 540}, desk, peers, true, false, true).has_value());
        // Valid right-edge hit
        const auto hit = flow::evaluate_edge_hit({1919, 540}, desk, peers, true, true, true);
        assert(hit && hit->peer_index == 0 && hit->edge == ui_settings::FlowEdge::right);
        assert(hit->normalized_axis > 0.48 && hit->normalized_axis < 0.52);

        // Arrival cursor mapping (entering left edge of target screen)
        const auto arrived = flow::map_arrival_cursor(desk, ui_settings::FlowEdge::left, 0.5);
        assert(arrived.x == 6 && arrived.y >= 530 && arrived.y <= 550);

        // Deterministic HMAC-SHA256 via BCrypt
        const auto mac1 = flow::hmac_sha256_hex("LOGI2026", "hello-flow");
        const auto mac2 = flow::hmac_sha256_hex("LOGI2026", "hello-flow");
        const auto mac3 = flow::hmac_sha256_hex("OTHERPIN", "hello-flow");
        assert(mac1.size() == 64 && mac1 == mac2 && mac1 != mac3);

        // Live 127.0.0.1 two-instance TCP edge transfer + wrong-PIN rejection
        flow::Controller receiver;
        bool received = false;
        std::wstring received_clip;
        flow::Controller::Callbacks cb;
        cb.on_edge_enter = [&](const flow::EdgeEvent& ev) {
            received = true;
            received_clip = ev.clipboard_text;
        };
        assert(receiver.start(0, "PIN-TEST-2026", "node-b", "NodeB", 1920, 1080, std::move(cb), false));
        peer.port = receiver.bound_port();
        assert(peer.port != 0);

        flow::Controller sender;
        assert(sender.ping_peer(L"127.0.0.1", peer.port, "PIN-TEST-2026", 1000));
        assert(!sender.ping_peer(L"127.0.0.1", peer.port, "WRONG-PIN", 800));
        assert(sender.send_edge_switch(peer, 0.5, L"ClipPayload-中文", "PIN-TEST-2026", 1000));
        for (int i = 0; i < 40 && !received; ++i) Sleep(10);
        receiver.stop();
        assert(received && received_clip == L"ClipPayload-中文");
    }

    // 5. ContinuousPreset ZoomRotate direction lock + cooldown + click reset
    {
        pointer_gesture::ContinuousPreset preset;
        preset.start(pointer_gesture::PresetKind::zoom_rotate);
        // Move past threshold horizontally -> locks horizontal (right=2 => ZoomIn), never emits up/down
        const auto first = preset.move(90, 10, 1000, 70, 55);
        assert(first && *first == 2);
        assert(preset.engaged());
        // Within 300ms cooldown no trigger
        assert(!preset.move(80, 0, 1100, 70, 55).has_value());
        // Vertical jitter while locked horizontal (after cooldown) should NOT emit rotate (up=3 / down=4)
        const auto jitter = preset.move(-80, 120, 1350, 70, 55);
        assert(!jitter.has_value());
        // After 300ms cooldown, another horizontal step fires
        const auto second = preset.move(-75, 0, 1400, 70, 55);
        assert(second && *second == 1);
    }

    // 6. remap.cpp parser for new official parity actions
    {
        Mappings m;
        assert(parse_text(
            "[global]\n"
            "0052=DateTime(datetime)\n"
            "0053=Widget(spotlight)\n"
            "0056=HostSwitch(2)\n"
            "00C4=ZoomReset\n"
            "00C3=GestureZoomRotate\n"
            "thumb.left=RotateLeft\n"
            "thumb.right=RotateRight\n",
            m));
        assert(m.bindings.size() == 7);
        assert(m.bindings[0].actions[0].operation == Operation::date_time);
        assert(m.bindings[0].actions[0].text == L"datetime");
        assert(m.bindings[1].actions[0].operation == Operation::widget);
        assert(m.bindings[1].actions[0].text == L"spotlight");
        assert(m.bindings[2].actions[0].operation == Operation::host_switch);
        assert(m.bindings[2].actions[0].value == 2);
        assert(m.bindings[3].actions[0].operation == Operation::zoom_reset);
        assert(m.bindings[4].actions[0].operation == Operation::continuous_preset);
        assert(m.bindings[5].actions[0].operation == Operation::rotate_left);
        assert(m.bindings[6].actions[0].operation == Operation::rotate_right);
    }

    // 7. Official Logi Options+ settings.db JSON parser & DPI scaling
    {
        constexpr const char* sample_json =
            "{\"use_system_theme\":true,\"battery_notify\":{\"level\":\"CRITICAL\"},"
            "\"profile-default\":{\"id\":\"default\"},\"profile-chrome\":{\"id\":\"chrome\"},"
            "\"pointerSpeed\":{\"active\":{\"dpiLevel\":1,\"highResolutionSensorActive\":true,\"value\":0.66}},"
            "\"mouseScrollWheelSettings\":{\"dir\":\"INVERTED\",\"isSmooth\":true,"
            "\"smartshift\":{\"isEnabled\":true,\"mode\":\"RATCHET\",\"sensitivity\":83}},"
            "\"mouseThumbWheelSettings\":{\"dir\":\"STANDARD\",\"isSmooth\":false}}";
        const auto snap = options_plus_import::parse_settings_json(sample_json);
        assert(snap.found);
        assert(snap.profile_count == 2);
        assert(snap.use_system_theme && *snap.use_system_theme);
        assert(snap.battery_notify && *snap.battery_notify);
        assert(snap.high_res_dpi && *snap.high_res_dpi);
        assert(snap.pointer_value && *snap.pointer_value > 0.65);
        assert(options_plus_import::dpi_from_options_plus(*snap.pointer_value, true) == 5350);
        assert(snap.scroll_inverted && *snap.scroll_inverted);
        assert(snap.scroll_smooth && *snap.scroll_smooth);
        assert(snap.smartshift_enabled && *snap.smartshift_enabled);
        assert(snap.smartshift_ratchet && *snap.smartshift_ratchet);
        assert(snap.smartshift_sensitivity && *snap.smartshift_sensitivity == 83);
        assert(snap.thumb_inverted && !*snap.thumb_inverted);
        assert(snap.thumb_smooth && !*snap.thumb_smooth);
    }

    fs::remove_all(temp_root, ec);
    return 0;
}
