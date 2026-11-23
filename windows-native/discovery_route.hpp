#pragma once

#include "hidpp.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

namespace logi::discovery {

// This is only a preferred transport address. Device identity and every feature
// are still read from the hardware each time connect() opens the route.
struct Hint {
    unsigned slot{};
    std::wstring path;
    bool operator==(const Hint&) const = default;
};

inline bool valid_slot(unsigned slot) { return (slot >= 1 && slot <= 6) || slot == 255; }

inline std::optional<Hint> decode(std::wstring_view value) {
    const auto split = value.find(L'\n');
    if (split == value.npos || split == 0 || split > 3 || split + 1 == value.size()) return {};
    unsigned slot{};
    for (const auto digit : value.substr(0, split)) {
        if (digit < L'0' || digit > L'9') return {};
        slot = slot * 10 + static_cast<unsigned>(digit - L'0');
    }
    const auto path = value.substr(split + 1);
    if (!valid_slot(slot) || path.find_first_of(std::wstring_view{L"\r\n\0", 3}) != path.npos) return {};
    return Hint{slot, std::wstring(path)};
}

inline std::wstring encode(const Hint& hint) {
    return std::to_wstring(hint.slot) + L'\n' + hint.path;
}

inline void prioritize(std::vector<Interface>& interfaces, const std::optional<Hint>& hint, bool list) {
    if (list || !hint) return;
    const auto found = std::ranges::find(interfaces, hint->path, &Interface::path);
    if (found != interfaces.end()) std::rotate(interfaces.begin(), found, found + 1);
}

inline std::vector<unsigned> slots(const Interface& interface, const std::optional<Hint>& hint, bool list) {
    const unsigned first = interface.receiver ? 1 : 255;
    const unsigned last = interface.receiver ? 6 : 255;
    const auto preferred = !list && hint && hint->path == interface.path &&
        hint->slot >= first && hint->slot <= last ? hint->slot : 0;
    std::vector<unsigned> result;
    if (preferred) result.push_back(preferred);
    for (unsigned slot = first; slot <= last; ++slot)
        if (slot != preferred) result.push_back(slot);
    return result;
}

struct Attempt {
    std::size_t interface_index{};
    unsigned slot{};
    DWORD ping_ms{1250};
};

inline std::vector<Attempt> targets(const std::vector<Interface>& interfaces,
                                    const std::optional<Hint>& hint,bool list) {
    std::vector<Attempt> result;
    for(std::size_t i=0;i<interfaces.size();++i)
        for(auto slot:slots(interfaces[i],hint,list))result.push_back({i,slot});
    return result;
}

// Owner discovery is incremental. A failed cached slot must not block its
// next probe behind five empty slots and a full discovery backoff.
struct OwnerSearch {
    bool first{true};
    bool require_live_features{};
    std::size_t quick_cursor{},slow_cursor{};
    ULONGLONG next_alternate{},next_slow{};
    std::vector<Attempt> round(std::vector<Attempt> all,bool preferred,ULONGLONG now) {
        if(all.empty())return {};
        constexpr DWORD short_ping=180;
        if(first) {
            first=false;
            if(!preferred) {
                for(auto& target:all)target.ping_ms=short_ping;
                return all;
            }
            // An already paired, slow device keeps the original cold-start timeout.
            next_slow=now+4000;
            return {all.front()};
        }
        std::vector<Attempt> work;
        if(preferred)work.push_back({all.front().interface_index,all.front().slot,short_ping});
        if(now>=next_slow) {
            // Give every route the full ping budget eventually, including the
            // preferred route. Only one slow probe can occupy an owner round.
            auto target=all[slow_cursor++%all.size()];
            next_slow=now+(preferred?4000:900);
            if(preferred && target.interface_index==work.front().interface_index &&
               target.slot==work.front().slot)work.clear();
            work.push_back(target);
        } else if(now>=next_alternate && all.size()>static_cast<unsigned>(preferred)) {
            const auto first_alternate=static_cast<unsigned>(preferred);
            auto target=all[first_alternate+quick_cursor++%(all.size()-first_alternate)];
            target.ping_ms=short_ping;
            work.push_back(target);
        }
        if(!work.empty() && (!preferred || work.size()>1))next_alternate=now+700;
        return work;
    }
};

} // namespace logi::discovery
