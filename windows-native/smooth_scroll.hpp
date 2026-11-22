#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace smooth_scroll {
// The reconstructed default filter (see SMOOTH-SCROLL-MECHANISM.md). This is
// intentionally not a claim of parity with the original's other profiles,
// linear input controller or browser-specific horizontal handler.
struct Filter {
    double position{},target{},gain{.2},limit{};
    void feed(double delta,double velocity) {
        constexpr double slope=(1.-.01)/(12.-.06);
        gain=std::clamp(std::abs(velocity)*slope+.01-slope*.06,.01,1.);
        target+=delta;limit=(target-position)*gain*.6;
    }
    double tick(double minimum_fraction=0.) {
        const auto remaining=target-position;
        auto amount=remaining*gain;
        if(amount>0&&limit>0)amount=std::min(amount,limit);
        if(amount<0&&limit<0)amount=std::max(amount,limit);
        const auto minimum_step=remaining*minimum_fraction;
        if(std::abs(amount)<std::abs(minimum_step))amount=minimum_step;
        gain=std::min(gain,.2);position+=amount;
        return amount;
    }
};

class Axis {
    static constexpr std::uint64_t frame_ms=10, tail_ms=120;
    Filter filter_;
    double remainder_{};
    std::uint64_t due_{},last_feed_{};
    bool running_{},smooth_{};
    bool expired(std::uint64_t now)const {
        return running_ && now>=due_ && now-due_>tail_ms && now-last_feed_>tail_ms;
    }
    int take(double quantum) {
        // Compensate only binary floating point error at an integer boundary.
        const auto scaled=remainder_/quantum;
        const auto whole=std::trunc(scaled+std::copysign(1e-9,scaled));
        const auto units=static_cast<int>(whole*quantum);
        remainder_-=units;return units;
    }
public:
    void reset(){*this={};}
    bool active()const{return running_;}
    std::uint64_t due()const{return due_;}
    int feed(double delta,std::uint64_t now,bool smooth) {
        if(!std::isfinite(delta)||delta==0)return 0;
        if(smooth_!=smooth || expired(now))reset();
        smooth_=smooth;
        if(!smooth){remainder_+=delta;return take(120.);}
        // A burst after an idle period starts at full responsiveness.
        const auto interval=running_ && now>last_feed_?now-last_feed_:frame_ms;
        filter_.feed(delta,delta/static_cast<double>(std::max(frame_ms,interval)));
        last_feed_=now;
        if(!running_)due_=now+frame_ms;
        running_=true;return 0;
    }
    int tick(std::uint64_t now) {
        if(!running_||now<due_)return 0;
        if(expired(now)){reset();return 0;}
        // Give a slow tick its share of the remaining distance so that the
        // last physical report has a finite tail in either direction.
        const auto age=now-last_feed_;
        const auto frames_left=age>=tail_ms?0:(tail_ms-age+frame_ms-1)/frame_ms;
        remainder_+=filter_.tick(1./(1+frames_left));due_=now+frame_ms;
        // Once less than one Windows wheel unit remains, finish it exactly;
        // an asymptotic fraction must not keep the owner waking indefinitely.
        if(std::abs(filter_.target-filter_.position)<1.) {
            remainder_+=filter_.target-filter_.position;
            filter_={};running_=false;
        }
        return take(1.);
    }
};

struct Movement {double units{};unsigned periods{};bool high_resolution{};};
inline std::optional<Movement> decode(std::span<const std::uint8_t> data,unsigned multiplier,bool inverted) {
    if(data.size()<3||!multiplier||(data[0]&0xe0))return {};
    const auto delta=static_cast<std::int16_t>((static_cast<unsigned>(data[1])<<8)|data[2]);
    const bool high=(data[0]&0x10)!=0;
    // HID++ ignores the hardware invert setting. Apply it exactly once here.
    return Movement{delta*(inverted?-120.:120.)/(high?multiplier:1),data[0]&15u,high};
}
}
