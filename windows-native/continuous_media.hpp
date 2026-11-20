#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace pointer_gesture {
enum class MediaDirection { left=1, right, up, down };

// Local raw-count adaptation of the two-stage media gesture. No timer-driven
// repeat: every direction action requires fresh motion after the start gate.
class ContinuousMedia {
    enum class Axis { none, horizontal, vertical };
    Axis axis_{};
    int x_{},y_{};
    bool engaged_{},cooling_{};
    std::uint64_t last_horizontal_{};
public:
    void reset() { *this={}; }
    bool engaged() const { return engaged_; }
    std::optional<MediaDirection> move(int x,int y,std::uint64_t now,
                                      int start_threshold,int action_threshold) {
        if(cooling_) {
            if(now-last_horizontal_<400)return {};
            cooling_=false;
        }
        x_=std::clamp(x_+x,-100000,100000);
        y_=std::clamp(y_+y,-100000,100000);
        if(axis_==Axis::none) {
            if(std::abs(x_)>std::abs(y_) && std::abs(x_)>=start_threshold)axis_=Axis::horizontal;
            else if(std::abs(y_)>std::abs(x_) && std::abs(y_)>=start_threshold)axis_=Axis::vertical;
            else return {};
            engaged_=true;x_=y_=0;
            return {}; // Starting an axis never emits the direction action.
        }
        const int distance=axis_==Axis::horizontal?x_:y_;
        if(std::abs(distance)<=action_threshold)return {};
        const bool horizontal=axis_==Axis::horizontal;
        const auto direction=horizontal?(distance<0?MediaDirection::left:MediaDirection::right):
                                        (distance<0?MediaDirection::up:MediaDirection::down);
        if(horizontal){last_horizontal_=now;cooling_=true;}
        axis_=Axis::none;x_=y_=0; // directionLocked=false: restart recognition.
        return direction;
    }
};

enum class PresetKind {
    zoom_rotate = 1,
    app_nav = 2,
    virtual_desktops = 3,
    window_management = 4,
    arrange_windows = 5
};

// Multi-preset continuous pointer gesture state machine implementing direction locking
// (e.g., Zoom/Rotate) and cooldown-gated continuous stepping (pauseBetweenActionsMs = 300ms).
class ContinuousPreset {
    enum class Axis { none, horizontal, vertical };
    PresetKind kind_{PresetKind::zoom_rotate};
    Axis locked_axis_{Axis::none};
    int x_{}, y_{};
    bool engaged_{};
    std::uint64_t last_action_ms_{};

public:
    void start(PresetKind kind) {
        *this = {};
        kind_ = kind;
    }
    void reset() { *this = {}; }
    PresetKind kind() const { return kind_; }
    bool engaged() const { return engaged_; }
    bool direction_locked() const {
        return kind_ == PresetKind::zoom_rotate || kind_ == PresetKind::app_nav ||
               kind_ == PresetKind::window_management || kind_ == PresetKind::arrange_windows;
    }

    // Returns 1=left, 2=right, 3=up, 4=down (matching Gesture enum values).
    std::optional<int> move(int x, int y, std::uint64_t now, int start_threshold, int action_threshold) {
        constexpr std::uint64_t cooldown_ms = 300;
        x_ = std::clamp(x_ + x, -100000, 100000);
        y_ = std::clamp(y_ + y, -100000, 100000);
        const bool lock_axis = direction_locked();

        if (!engaged_) {
            if (std::abs(x_) >= start_threshold && std::abs(x_) >= std::abs(y_)) {
                engaged_ = true;
                if (lock_axis) locked_axis_ = Axis::horizontal;
                const int dir = x_ < 0 ? 1 : 2;
                x_ = y_ = 0;
                last_action_ms_ = now;
                return dir;
            }
            if (std::abs(y_) >= start_threshold && std::abs(y_) > std::abs(x_)) {
                engaged_ = true;
                if (lock_axis) locked_axis_ = Axis::vertical;
                const int dir = y_ < 0 ? 3 : 4;
                x_ = y_ = 0;
                last_action_ms_ = now;
                return dir;
            }
            return std::nullopt;
        }

        if (now - last_action_ms_ < cooldown_ms) {
            if (lock_axis) {
                if (locked_axis_ == Axis::horizontal) y_ = 0;
                else if (locked_axis_ == Axis::vertical) x_ = 0;
            }
            return std::nullopt;
        }

        if (lock_axis && locked_axis_ == Axis::horizontal) {
            if (std::abs(x_) >= action_threshold) {
                const int dir = x_ < 0 ? 1 : 2;
                x_ = y_ = 0;
                last_action_ms_ = now;
                return dir;
            }
            return std::nullopt;
        }
        if (lock_axis && locked_axis_ == Axis::vertical) {
            if (std::abs(y_) >= action_threshold) {
                const int dir = y_ < 0 ? 3 : 4;
                x_ = y_ = 0;
                last_action_ms_ = now;
                return dir;
            }
            return std::nullopt;
        }

        if (std::abs(x_) >= action_threshold && std::abs(x_) >= std::abs(y_)) {
            const int dir = x_ < 0 ? 1 : 2;
            x_ = y_ = 0;
            last_action_ms_ = now;
            return dir;
        }
        if (std::abs(y_) >= action_threshold && std::abs(y_) > std::abs(x_)) {
            const int dir = y_ < 0 ? 3 : 4;
            x_ = y_ = 0;
            last_action_ms_ = now;
            return dir;
        }
        return std::nullopt;
    }
};
}
