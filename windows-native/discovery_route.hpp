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

} // namespace logi::discovery
