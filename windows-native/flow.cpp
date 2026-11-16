#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "flow.hpp"
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace flow {
namespace {

struct WsaScope {
    bool ok{};
    WsaScope() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WsaScope() {
        if (ok) WSACleanup();
    }
};

void ensure_wsa() {
    static WsaScope scope;
}

std::string hex_encode(std::string_view bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

std::optional<std::string> hex_decode(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = val(text[i]), lo = val(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

ui_settings::FlowEdge opposite_edge(ui_settings::FlowEdge edge) {
    switch (edge) {
    case ui_settings::FlowEdge::left: return ui_settings::FlowEdge::right;
    case ui_settings::FlowEdge::right: return ui_settings::FlowEdge::left;
    case ui_settings::FlowEdge::top: return ui_settings::FlowEdge::bottom;
    default: return ui_settings::FlowEdge::top;
    }
}

std::string narrow(std::wstring_view s) { return ui_settings::wide_to_utf8(s); }
std::wstring wide(std::string_view s) { return ui_settings::utf8_to_wide(s); }

bool set_socket_timeouts(SOCKET s, DWORD timeout_ms) {
    DWORD tv = timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    return true;
}

SOCKET connect_host(std::wstring_view host, unsigned port, DWORD timeout_ms) {
    ensure_wsa();
    const auto host_utf8 = narrow(host);
    const auto port_str = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    if (getaddrinfo(host_utf8.c_str(), port_str.c_str(), &hints, &list) != 0 || !list)
        return INVALID_SOCKET;
    SOCKET result = INVALID_SOCKET;
    for (addrinfo* p = list; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        int rc = connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen));
        if (rc == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                fd_set wfds{};
                FD_ZERO(&wfds);
                FD_SET(s, &wfds);
                timeval tv{};
                tv.tv_sec = static_cast<long>(timeout_ms / 1000);
                tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
                if (select(0, nullptr, &wfds, nullptr, &tv) > 0) {
                    int so_error = 0;
                    int len = sizeof(so_error);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
                    if (so_error == 0) rc = 0;
                }
            }
        }
        nonblock = 0;
        ioctlsocket(s, FIONBIO, &nonblock);
        if (rc == 0) {
            set_socket_timeouts(s, timeout_ms);
            result = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(list);
    return result;
}

bool send_all(SOCKET s, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::optional<std::string> recv_line(SOCKET s, std::size_t max_bytes = 2 * 1024 * 1024) {
    std::string line;
    char ch{};
    while (line.size() < max_bytes) {
        const int n = recv(s, &ch, 1, 0);
        if (n <= 0) return std::nullopt;
        if (ch == '\n') return line;
        if (ch != '\r') line.push_back(ch);
    }
    return std::nullopt;
}

std::string build_nonce() {
    std::uint64_t r[2]{};
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(r), sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char buf[33]{};
    std::snprintf(buf, sizeof(buf), "%016llX%016llX",
                  static_cast<unsigned long long>(r[0]), static_cast<unsigned long long>(r[1]));
    return buf;
}

} // namespace

std::optional<EdgeMatch> evaluate_edge_hit(
    POINT cursor, const RECT& bounds,
    const std::vector<ui_settings::FlowPeerConfig>& peers,
    bool hold_ctrl, bool ctrl_down, bool corner_deadzone) {
    if (peers.empty()) return std::nullopt;
    if (hold_ctrl && !ctrl_down) return std::nullopt;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width < 64 || height < 64) return std::nullopt;

    const int dead = corner_deadzone ? std::clamp(std::min(width, height) / 14, 24, 96) : 0;
    const int margin = 2;
    std::optional<ui_settings::FlowEdge> hit;
    double norm = 0.5;
    if (cursor.x <= bounds.left + margin) {
        if (cursor.y < bounds.top + dead || cursor.y > bounds.bottom - dead) return std::nullopt;
        hit = ui_settings::FlowEdge::left;
        norm = static_cast<double>(cursor.y - bounds.top) / static_cast<double>(height);
    } else if (cursor.x >= bounds.right - 1 - margin) {
        if (cursor.y < bounds.top + dead || cursor.y > bounds.bottom - dead) return std::nullopt;
        hit = ui_settings::FlowEdge::right;
        norm = static_cast<double>(cursor.y - bounds.top) / static_cast<double>(height);
    } else if (cursor.y <= bounds.top + margin) {
        if (cursor.x < bounds.left + dead || cursor.x > bounds.right - dead) return std::nullopt;
        hit = ui_settings::FlowEdge::top;
        norm = static_cast<double>(cursor.x - bounds.left) / static_cast<double>(width);
    } else if (cursor.y >= bounds.bottom - 1 - margin) {
        if (cursor.x < bounds.left + dead || cursor.x > bounds.right - dead) return std::nullopt;
        hit = ui_settings::FlowEdge::bottom;
        norm = static_cast<double>(cursor.x - bounds.left) / static_cast<double>(width);
    }
    if (!hit) return std::nullopt;
    norm = std::clamp(norm, 0.0, 1.0);
    for (std::size_t i = 0; i < peers.size(); ++i) {
        if (peers[i].edge == *hit)
            return EdgeMatch{i, *hit, norm, peers[i].host_slot};
    }
    return std::nullopt;
}

POINT map_arrival_cursor(const RECT& bounds, ui_settings::FlowEdge incoming_edge,
                           double normalized_axis, int inset_px) {
    const double t = std::clamp(normalized_axis, 0.0, 1.0);
    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    const int inset = std::max(2, inset_px);
    switch (incoming_edge) {
    case ui_settings::FlowEdge::left:
        return {bounds.left + inset, bounds.top + static_cast<int>(t * (height - 1))};
    case ui_settings::FlowEdge::right:
        return {bounds.right - inset, bounds.top + static_cast<int>(t * (height - 1))};
    case ui_settings::FlowEdge::top:
        return {bounds.left + static_cast<int>(t * (width - 1)), bounds.top + inset};
    default:
        return {bounds.left + static_cast<int>(t * (width - 1)), bounds.bottom - inset};
    }
}

std::string hmac_sha256_hex(std::string_view key, std::string_view payload) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return {};
    std::array<unsigned char, 32> digest{};
    const NTSTATUS created = BCryptCreateHash(
        alg, &hash, nullptr, 0,
        const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(key.data())),
        static_cast<ULONG>(key.size()), 0);
    if (created == 0) {
        if (BCryptHashData(hash, const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(payload.data())),
                           static_cast<ULONG>(payload.size()), 0) == 0) {
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex_encode({reinterpret_cast<const char*>(digest.data()), digest.size()});
}

Controller::~Controller() { stop(); }

bool Controller::start(unsigned tcp_port, std::string pin_utf8, std::string local_id,
                       std::string local_name, unsigned screen_w, unsigned screen_h,
                       Callbacks callbacks, bool enable_udp_discovery, unsigned udp_port) {
    stop();
    ensure_wsa();
    pin_ = std::move(pin_utf8);
    local_id_ = std::move(local_id);
    local_name_ = std::move(local_name);
    screen_w_ = screen_w;
    screen_h_ = screen_h;
    callbacks_ = std::move(callbacks);

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(tcp_port));
    if (bind(ls, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, 8) == SOCKET_ERROR) {
        closesocket(ls);
        return false;
    }
    sockaddr_in actual{};
    int actual_len = sizeof(actual);
    if (getsockname(ls, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0)
        bound_port_.store(ntohs(actual.sin_port));
    else
        bound_port_.store(tcp_port);

    listen_socket_ = static_cast<std::uintptr_t>(ls);
    running_.store(true);
    accept_thread_ = std::thread(&Controller::server_loop, this);

    if (enable_udp_discovery) {
        SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (us != INVALID_SOCKET) {
            BOOL bcast = TRUE;
            setsockopt(us, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
            setsockopt(us, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
            sockaddr_in uaddr{};
            uaddr.sin_family = AF_INET;
            uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
            uaddr.sin_port = htons(static_cast<u_short>(udp_port));
            if (bind(us, reinterpret_cast<const sockaddr*>(&uaddr), sizeof(uaddr)) == 0) {
                udp_socket_ = static_cast<std::uintptr_t>(us);
                discovery_thread_ = std::thread(&Controller::discovery_loop, this, udp_port);
            } else {
                closesocket(us);
            }
        }
    }
    return true;
}

void Controller::stop() {
    running_.store(false);
    if (listen_socket_ != ~std::uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(listen_socket_));
        listen_socket_ = ~std::uintptr_t(0);
    }
    if (udp_socket_ != ~std::uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(udp_socket_));
        udp_socket_ = ~std::uintptr_t(0);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    if (discovery_thread_.joinable()) discovery_thread_.join();
    bound_port_.store(0);
}

std::vector<DiscoveredPeer> Controller::discovered_peers() const {
    std::lock_guard lock(mutex_);
    return peers_;
}

void Controller::server_loop() {
    const SOCKET ls = static_cast<SOCKET>(listen_socket_);
    while (running_.load()) {
        sockaddr_in client_addr{};
        int len = sizeof(client_addr);
        SOCKET client = accept(ls, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client == INVALID_SOCKET) break;
        set_socket_timeouts(client, 2000);
        handle_client(static_cast<std::uintptr_t>(client));
    }
}

void Controller::handle_client(std::uintptr_t client_handle) {
    const SOCKET client = static_cast<SOCKET>(client_handle);
    const auto line = recv_line(client);
    if (!line) {
        closesocket(client);
        return;
    }
    // Wire format: <MAC>|<NONCE>|<BODY>
    const auto sep1 = line->find('|');
    const auto sep2 = sep1 == std::string::npos ? std::string::npos : line->find('|', sep1 + 1);
    if (sep1 == std::string::npos || sep2 == std::string::npos) {
        send_all(client, "ERR FORMAT\n");
        closesocket(client);
        return;
    }
    const auto mac = line->substr(0, sep1);
    const auto nonce = line->substr(sep1 + 1, sep2 - sep1 - 1);
    const auto body = line->substr(sep2 + 1);
    const auto expected_mac = hmac_sha256_hex(pin_, nonce + "|" + body);
    if (expected_mac.empty() || mac != expected_mac) {
        send_all(client, "ERR AUTH\n");
        closesocket(client);
        return;
    }

    std::istringstream parts(body);
    std::string cmd;
    parts >> cmd;
    if (cmd == "PING") {
        send_all(client, "OK PONG " + local_id_ + "\n");
    } else if (cmd == "EDGE") {
        std::string peer_id, peer_name_hex, edge_str, slot_str, norm_str, clip_hex;
        parts >> peer_id >> peer_name_hex >> edge_str >> slot_str >> norm_str >> clip_hex;
        EdgeEvent ev;
        ev.peer_id = wide(peer_id);
        ev.peer_name = wide(hex_decode(peer_name_hex).value_or(peer_id));
        ev.edge = opposite_edge(ui_settings::parse_edge(wide(edge_str)));
        ev.target_slot = static_cast<unsigned>(std::max(1, std::atoi(slot_str.c_str())));
        ev.normalized_axis = std::clamp(std::atof(norm_str.c_str()), 0.0, 1.0);
        if (!clip_hex.empty() && clip_hex != "-")
            ev.clipboard_text = wide(hex_decode(clip_hex).value_or(""));
        send_all(client, "OK EDGE\n");
        if (callbacks_.on_edge_enter) callbacks_.on_edge_enter(ev);
    } else if (cmd == "CLIP") {
        std::string clip_hex;
        parts >> clip_hex;
        const auto text = wide(hex_decode(clip_hex).value_or(""));
        send_all(client, "OK CLIP\n");
        if (callbacks_.on_clipboard) callbacks_.on_clipboard(text);
    } else {
        send_all(client, "ERR CMD\n");
    }
    closesocket(client);
}

void Controller::discovery_loop(unsigned udp_port) {
    (void)udp_port;
    const SOCKET us = static_cast<SOCKET>(udp_socket_);
    std::array<char, 1024> buf{};
    while (running_.load()) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int n = recvfrom(us, buf.data(), static_cast<int>(buf.size() - 1), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) break;
        buf[static_cast<std::size_t>(n)] = '\0';
        std::istringstream ss(buf.data());
        std::string tag, id, name_hex, port_str, w_str, h_str;
        if (!(ss >> tag >> id >> name_hex >> port_str >> w_str >> h_str) || tag != "LOGIFLOW1")
            continue;
        if (id == local_id_) continue;
        char ip_buf[64]{};
        inet_ntop(AF_INET, &from.sin_addr, ip_buf, sizeof(ip_buf));
        DiscoveredPeer peer;
        peer.id = wide(id);
        peer.name = wide(hex_decode(name_hex).value_or(id));
        peer.host = wide(ip_buf);
        peer.port = static_cast<unsigned>(std::atoi(port_str.c_str()));
        peer.screen_width = static_cast<unsigned>(std::max(640, std::atoi(w_str.c_str())));
        peer.screen_height = static_cast<unsigned>(std::max(480, std::atoi(h_str.c_str())));
        peer.last_seen_ms = GetTickCount64();
        {
            std::lock_guard lock(mutex_);
            auto it = std::ranges::find(peers_, peer.id, &DiscoveredPeer::id);
            if (it != peers_.end()) *it = peer;
            else peers_.push_back(peer);
        }
        if (callbacks_.on_discovered) callbacks_.on_discovered(peer);
    }
}

void Controller::broadcast_presence(unsigned udp_port) {
    ensure_wsa();
    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (us == INVALID_SOCKET) return;
    BOOL bcast = TRUE;
    setsockopt(us, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    addr.sin_port = htons(static_cast<u_short>(udp_port));
    const unsigned port = bound_port_.load() ? bound_port_.load() : 59835u;
    const std::string id_tok = local_id_.empty() ? std::string("local") : local_id_;
    const std::string name_tok = local_name_.empty() ? id_tok : local_name_;
    const std::string payload = "LOGIFLOW1 " + id_tok + " " + hex_encode(name_tok) + " " +
        std::to_string(port) + " " + std::to_string(screen_w_) + " " + std::to_string(screen_h_);
    sendto(us, payload.data(), static_cast<int>(payload.size()), 0,
           reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    closesocket(us);
}

bool Controller::send_packet(std::wstring_view host, unsigned port, std::string_view message_body,
                             std::string_view pin_override, DWORD timeout_ms) {
    const SOCKET s = connect_host(host, port, timeout_ms);
    if (s == INVALID_SOCKET) return false;
    const std::string nonce = build_nonce();
    const std::string_view key = pin_override.empty() ? std::string_view(pin_) : pin_override;
    const std::string mac = hmac_sha256_hex(key, nonce + "|" + std::string(message_body));
    const std::string frame = mac + "|" + nonce + "|" + std::string(message_body) + "\n";
    const bool ok = send_all(s, frame);
    const auto reply = ok ? recv_line(s, 4096) : std::nullopt;
    closesocket(s);
    return reply && reply->starts_with("OK ");
}

bool Controller::send_edge_switch(const ui_settings::FlowPeerConfig& peer, double normalized_axis,
                                  std::wstring_view clipboard_text, std::string_view pin_override,
                                  DWORD timeout_ms) {
    const std::string clip_utf8 = narrow(clipboard_text);
    const std::string clip_hex = clip_utf8.empty() ? "-" : hex_encode(clip_utf8);
    char norm_buf[32]{};
    std::snprintf(norm_buf, sizeof(norm_buf), "%.4f", std::clamp(normalized_axis, 0.0, 1.0));
    const std::string id_tok = local_id_.empty() ? std::string("local") : local_id_;
    const std::string name_tok = local_name_.empty() ? id_tok : local_name_;
    const std::string body = "EDGE " + id_tok + " " + hex_encode(name_tok) + " " +
        narrow(ui_settings::edge_name(peer.edge)) + " " + std::to_string(peer.host_slot) + " " +
        norm_buf + " " + clip_hex;
    return send_packet(peer.host, peer.port, body, pin_override, timeout_ms);
}

bool Controller::send_clipboard(const ui_settings::FlowPeerConfig& peer, std::wstring_view clipboard_text,
                                std::string_view pin_override, DWORD timeout_ms) {
    const std::string clip_hex = hex_encode(narrow(clipboard_text));
    return send_packet(peer.host, peer.port, "CLIP " + clip_hex, pin_override, timeout_ms);
}

bool Controller::ping_peer(std::wstring_view host, unsigned port, std::string_view pin_override,
                           DWORD timeout_ms) {
    return send_packet(host, port, "PING", pin_override, timeout_ms);
}

std::optional<std::wstring> read_clipboard_text() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (OpenClipboard(nullptr)) {
            std::optional<std::wstring> result;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* ptr = static_cast<const wchar_t*>(GlobalLock(h))) {
                    result = std::wstring(ptr);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            return result;
        }
        Sleep(15);
    }
    return std::nullopt;
}

bool write_clipboard_text(std::wstring_view text) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!mem) {
                CloseClipboard();
                return false;
            }
            auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
            if (dst) {
                std::copy(text.begin(), text.end(), dst);
                dst[text.size()] = L'\0';
                GlobalUnlock(mem);
            }
            const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            if (!ok) GlobalFree(mem);
            CloseClipboard();
            return ok;
        }
        Sleep(15);
    }
    return false;
}

std::wstring local_computer_name() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameW(buf, &len) && len > 0) return buf;
    return L"Windows-PC";
}

std::string local_computer_name_utf8() {
    return narrow(local_computer_name());
}

} // namespace flow
