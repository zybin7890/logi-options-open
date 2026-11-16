#pragma once

#include "ui_settings.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace flow {

struct EdgeEvent {
    std::wstring peer_id;
    std::wstring peer_name;
    ui_settings::FlowEdge edge{ui_settings::FlowEdge::right};
    double normalized_axis{0.5};
    unsigned target_slot{2};
    std::wstring clipboard_text;
};

struct DiscoveredPeer {
    std::wstring id;
    std::wstring name;
    std::wstring host;
    unsigned port{59835};
    unsigned screen_width{1920};
    unsigned screen_height{1080};
    std::uint64_t last_seen_ms{};
};

struct EdgeMatch {
    std::size_t peer_index{};
    ui_settings::FlowEdge edge{ui_settings::FlowEdge::right};
    double normalized_axis{0.5};
    unsigned target_slot{2};
};

// Pure deterministic edge detector for Flow screen boundary transitions.
std::optional<EdgeMatch> evaluate_edge_hit(
    POINT cursor, const RECT& bounds,
    const std::vector<ui_settings::FlowPeerConfig>& peers,
    bool hold_ctrl, bool ctrl_down, bool corner_deadzone);

// Maps an arrival edge & normalized coordinate onto concrete screen coordinates.
POINT map_arrival_cursor(const RECT& bounds, ui_settings::FlowEdge incoming_edge,
                           double normalized_axis, int inset_px = 6);

// Computes uppercase hex HMAC-SHA256 using Windows BCrypt.
std::string hmac_sha256_hex(std::string_view key, std::string_view payload);

class Controller {
public:
    struct Callbacks {
        std::function<void(const EdgeEvent&)> on_edge_enter;
        std::function<void(const std::wstring&)> on_clipboard;
        std::function<void(const DiscoveredPeer&)> on_discovered;
    };

    Controller() = default;
    ~Controller();

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    bool start(unsigned tcp_port, std::string pin_utf8, std::string local_id,
               std::string local_name, unsigned screen_w, unsigned screen_h,
               Callbacks callbacks = {}, bool enable_udp_discovery = false,
               unsigned udp_port = 59836);
    void stop();

    bool running() const { return running_.load(); }
    unsigned bound_port() const { return bound_port_.load(); }
    std::vector<DiscoveredPeer> discovered_peers() const;

    bool send_edge_switch(const ui_settings::FlowPeerConfig& peer,
                          double normalized_axis,
                          std::wstring_view clipboard_text,
                          std::string_view pin_override = {},
                          DWORD timeout_ms = 1200);

    bool send_clipboard(const ui_settings::FlowPeerConfig& peer,
                        std::wstring_view clipboard_text,
                        std::string_view pin_override = {},
                        DWORD timeout_ms = 1000);

    bool ping_peer(std::wstring_view host, unsigned port,
                   std::string_view pin_override = {},
                   DWORD timeout_ms = 800);

    void broadcast_presence(unsigned udp_port = 59836);

private:
    void server_loop();
    void discovery_loop(unsigned udp_port);
    void handle_client(std::uintptr_t client_socket);
    bool send_packet(std::wstring_view host, unsigned port,
                     std::string_view message_body,
                     std::string_view pin_override,
                     DWORD timeout_ms);

    std::atomic<bool> running_{false};
    std::atomic<unsigned> bound_port_{0};
    std::uintptr_t listen_socket_{~std::uintptr_t(0)};
    std::uintptr_t udp_socket_{~std::uintptr_t(0)};
    std::string pin_;
    std::string local_id_;
    std::string local_name_;
    unsigned screen_w_{1920};
    unsigned screen_h_{1080};
    Callbacks callbacks_;
    std::thread accept_thread_;
    std::thread discovery_thread_;
    mutable std::mutex mutex_;
    std::vector<DiscoveredPeer> peers_;
};

// Reads/writes CF_UNICODETEXT clipboard safely with short retry.
std::optional<std::wstring> read_clipboard_text();
bool write_clipboard_text(std::wstring_view text);
std::string local_computer_name_utf8();
std::wstring local_computer_name();

} // namespace flow
