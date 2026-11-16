#pragma once

#include <algorithm>
#include <vector>

namespace smart_steps {
inline constexpr int row_height = 56;
inline constexpr int viewport_top = 110;

struct Layout {
    int if_heading{}, trigger{}, then_heading{}, steps{}, add{}, tag{};
    int scroll{}, max_scroll{}, bottom{};
};

inline Layout layout(int count, bool trigger_open, bool action_open, int height, int requested_scroll) {
    const int rows = count * row_height;
    const int trigger_extra = trigger_open ? 220 : 0;
    const int action_extra = action_open ? 220 : 0;
    const int growth = rows + trigger_extra + action_extra;
    const int first = std::max(122, 325 - growth / 2);
    const int bottom = std::max(viewport_top + 100, height - 84);
    const int tag = first + 246 + growth;
    const int maximum = std::max(0, tag + 30 - bottom);
    const int scroll = std::clamp(requested_scroll, 0, maximum);
    return {first - scroll, first + 36 - scroll, first + 129 + trigger_extra - scroll,
            first + 165 + trigger_extra - scroll, first + 165 + trigger_extra + rows - scroll,
            tag - scroll, scroll, maximum, bottom};
}

inline int hit_row(int y, int offset, int count) {
    const int position = y + offset;
    return position >= 0 && position < count * row_height ? position / row_height : -1;
}

template<class T> bool reorder(std::vector<T>& values, int from, int to) {
    if (from < 0 || to < 0 || from == to || static_cast<std::size_t>(from) >= values.size() || static_cast<std::size_t>(to) >= values.size()) return false;
    if (from < to) std::rotate(values.begin() + from, values.begin() + from + 1, values.begin() + to + 1);
    else std::rotate(values.begin() + to, values.begin() + from, values.begin() + from + 1);
    return true;
}
}
