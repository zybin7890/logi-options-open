#pragma once

#include "hidpp.hpp"

#include <memory>
#include <string_view>

namespace logi {

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

std::optional<Mouse> connect(StopHandle stop = nullptr, bool list = false);
std::vector<HostSlot> hosts(Mouse& mouse);
bool switch_host(Mouse& mouse, unsigned target);
std::optional<unsigned> get(Mouse& mouse, std::wstring_view setting);
bool set(Mouse& mouse, std::wstring_view setting, unsigned value);
bool restore_standard_input(Mouse& mouse);
void describe(Mouse& mouse);
std::optional<unsigned> number(std::wstring_view input, int base = 10);

} // namespace logi
