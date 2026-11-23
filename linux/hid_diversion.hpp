#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace loo_linux {
// HID++ diversion survives the process that enabled it. The daemon's ownership
// lock serializes this per-device journal. A crash before rename cannot expose
// a partial journal; a crash after rename is safe even before the HID write.
class HidDiversions {
    struct Change { unsigned original, requested; };
    using Changes = std::map<unsigned, Change>;
    static constexpr std::string_view prefix = "hid-diversion-";
    Changes entries_;
    std::filesystem::path path_;

    void save(const Changes& entries) const {
        if (path_.empty()) return;
        if (entries.empty()) {
            std::filesystem::remove(path_);
            return;
        }
        auto temporary = path_;
        temporary += ".tmp";
        std::ofstream out(temporary, std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        for (auto [id, change] : entries)
            out << id << ' ' << change.original << ' ' << change.requested << '\n';
        out.close();
        std::filesystem::rename(temporary, path_);
    }
public:
    bool empty() const { return entries_.empty(); }
    static bool any_pending(const std::filesystem::path& directory) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(prefix) || !std::filesystem::is_regular_file(entry.symlink_status())) continue;
            const std::string_view id(name.data() + prefix.size(), name.size() - prefix.size());
            std::uint32_t unit{};
            const auto parsed = std::from_chars(id.data(), id.data()+id.size(), unit);
            if (parsed.ec == std::errc{} && parsed.ptr == id.data()+id.size() &&
                unit != 0 && unit != UINT32_MAX && std::to_string(unit) == id) return true;
        }
        return false;
    }
    void bind(const std::filesystem::path& directory, std::optional<std::uint32_t> unit) {
        // Devices without a unique ID retain only this connection's in-memory
        // information. A model name or a receiver slot may be shared by mice.
        const auto next = unit ? directory / (std::string(prefix) + std::to_string(*unit)) : std::filesystem::path{};
        if (path_ == next) return;
        if (!entries_.empty() && (path_.empty() || next.empty()))
            throw std::runtime_error("Waiting for the previous mouse to restore its input reporting");
        Changes stored;
        if (!next.empty() && std::filesystem::exists(next)) {
            std::ifstream input(next);
            if (!input) throw std::runtime_error("Cannot open HID diversion journal");
            while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
                unsigned id{}, original{}, requested{};
                if (!(input >> id >> original >> requested) || id > 0x10000 ||
                    (id == 0x10000 ? (original > 255 || requested != 256)
                                   : (original != 0 || (requested != 1 && requested != 17))) ||
                    !stored.emplace(id, Change{original, requested}).second)
                    throw std::runtime_error("Invalid HID diversion journal");
            }
            if (input.bad()) throw std::runtime_error("Cannot read HID diversion journal");
        }
        path_ = next;
        entries_ = std::move(stored);
    }

    void remember(unsigned id, unsigned original, unsigned requested) {
        auto next = entries_;
        if (!next.emplace(id, Change{original, requested}).second)
            throw std::runtime_error("Previous HID diversion has not been restored");
        save(next);
        entries_ = std::move(next);
    }

    template<class Read, class Write>
    void restore(Read read, Write write) {
        if (entries_.empty()) return;
        auto pending = entries_;
        for (auto [id, change] : entries_) {
            const auto current = read(id);
            if (!current) continue;
            // An untouched intent, a hardware reset, or a different manager's
            // setting requires no write. Only undo a state we actually requested.
            if (*current == change.requested &&
                (!write(id, change.original) || read(id) != change.original)) continue;
            pending.erase(id);
        }
        save(pending);
        entries_ = std::move(pending);
        if (!entries_.empty()) throw std::runtime_error("Device did not confirm input restoration");
    }
};
}
