#pragma once

#include "hidpp.hpp"

#include <filesystem>
#include <array>
#include <memory>
#include <string_view>

namespace logi {
namespace discovery { struct OwnerSearch; }

struct Mouse {
    std::unique_ptr<Channel> channel;
    Device device;
    Byte dpi{}, shift{}, wheel{}, thumb{}, buttons{}, battery{}, host{}, hosts_info{};
    bool enhanced_shift{}, unified_battery{};
    std::vector<unsigned> dpi_choices;

    Reply request(Byte feature, Byte function, std::initializer_list<Byte> data = {});
};

struct HostSlot {
    unsigned slot;
    bool current;
    std::optional<bool> paired;
};

// Mapping owners do not need the display-only serial/firmware queries before
// they restore input. UI and CLI callers retain the complete device information.
std::optional<Mouse> connect(HANDLE stop = nullptr, bool list = false, bool include_device_info = true,
                             discovery::OwnerSearch* search = nullptr);
std::vector<HostSlot> hosts(Mouse& mouse);
bool switch_host(Mouse& mouse, unsigned target);
std::optional<unsigned> get(Mouse& mouse, std::wstring_view setting);
bool set(Mouse& mouse, std::wstring_view setting, unsigned value);
bool remember_dpi(const std::filesystem::path& mapping, const Mouse& mouse, unsigned value);
bool restore_saved_dpi(const std::filesystem::path& mapping, Mouse& mouse, std::optional<unsigned>& observed);
// smartshift, ratchet, hires, invert, thumb-invert; absent fields are not overridden.
using SavedSettings = std::array<std::optional<unsigned>,5>;
bool read_saved_settings(const std::filesystem::path& mapping,const Mouse& mouse,SavedSettings& settings);
bool remember_settings(const std::filesystem::path& mapping,const Mouse& mouse,const SavedSettings& settings);
struct ControlCatalog {
    OwnerIdentity owner;
    unsigned count{};
    std::vector<unsigned> divertable;
};
struct StandardReporting {
    Packet wheel, thumb;
    std::vector<Packet> controls;
    std::optional<ControlCatalog> learned;
};
bool restore_standard_input(Mouse& mouse, StandardReporting* observed = nullptr,
                            std::optional<ControlCatalog>* previous = nullptr);
void describe(Mouse& mouse);
std::optional<unsigned> number(std::wstring_view input, int base = 10);

} // namespace logi
