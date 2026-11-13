#pragma once

// Local generic-pointer lifecycle. This deliberately does not implement the
// original's specialized thumbwheel navigation, repeat timers or Alt ownership.
namespace pointer_gesture {
enum class Phase { idle,START,PROGRESS,END,CANCELED };
class Lifecycle {
    Phase phase_{Phase::idle};
public:
    constexpr Phase phase() const{return phase_;}
    constexpr bool active() const{return phase_==Phase::START||phase_==Phase::PROGRESS;}
    constexpr bool start(){if(active())return false;phase_=Phase::START;return true;}
    constexpr bool progress(){if(!active())return false;phase_=Phase::PROGRESS;return true;}
    constexpr bool end(){if(!active())return false;phase_=Phase::END;return true;}
    constexpr bool cancel(){if(!active())return false;phase_=Phase::CANCELED;return true;}
};
}
