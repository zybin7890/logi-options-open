#pragma once

#include <windows.h>

#include <span>
#include <optional>
#include <string>

struct RingIcon {
    std::wstring glyph, image;
    unsigned color{0xfbfbfb};
    std::optional<unsigned> background;
    std::wstring text{};
    unsigned text_color{0xfbfbfb};
    int layout{}, font_size{27}, scale{100}, offset_x{}, offset_y{};
    bool show_icon{true}, show_text{}, bold{};
    std::wstring font_family{L"Brown Logitech Pan Light"};
    bool operator==(const RingIcon&) const = default;
};

inline constexpr int ring_back = -2;
int choose_ring_action(std::span<const std::wstring> labels, HANDLE stop, bool submenu = false,
                       std::span<const RingIcon> icons = {}, float scale = 1.0f);
