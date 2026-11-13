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
}
