#include "mapping.hpp"
#include "hidpp.hpp"
#include <iostream>
using namespace loo_linux;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Invalid input was accepted");}
int main() {
    try {
        auto config=parse("[global]\n0053=MouseBack\n00C3=None\n00C3.left=Ctrl+Alt+Left\nthumb.left=VolumeDown\nthumb.speed=150\n");
        require(config.bindings.size()==4 && config.speed==150,"Config values");
        require(layout(config).at(0xc3),"Raw XY required for gesture");
        require(!layout(config).at(0x53),"Tap must not divert pointer motion");
        rejects([]{parse("[chrome]\n0053=MouseBack");});
        rejects([]{parse("[global]\n0053=Text(abc)");});
        rejects([]{parse("[global]\n0053=Ctrl+Ctrl");});
        rejects([]{parse("[global]\n0053=Ctrl+");});
        rejects([]{parse("[global]\n0053=None\n0053=None");});
        rejects([]{parse("[global]\n00C3.left=Alt+Tab");});
        rejects([]{parse("[global]\nthumb.up=VolumeUp");});
        rejects([]{parse("[global]\n0053=Wait(60001)");});
        rejects([]{parse("[global]\n0053=Open(--help)");});
        auto sequence=actions("Ctrl+C;Wait(40);Ctrl+V");
        require(sequence.size()==3 && sequence[1].value==40,"Sequence parser");
        require(gesture(19,-19,20)==Direction::tap,"Gesture threshold");
        require(gesture(-20,10,20)==Direction::left,"Gesture horizontal");
        require(gesture(0,-21,20)==Direction::up,"Gesture vertical");
        WheelAccumulator wheel;
        require(wheel.steps(1,1,4,1,100)==0,"Fractional wheel first");
        require(wheel.steps(3,1,4,1,100)==1,"Fractional wheel accumulated");
        require(wheel.steps(-4,1,4,1,100)==-1,"Wheel negative direction");
        require(wheel.steps(2,1,4,-1,200)==-1,"Speed and inversion");
        wheel.steps(3,1,4,1,100);
        require(wheel.steps(1,1,4,1,100,true)==0,"Touch phase reset");
        require(logi::Packet{}.data().empty(),"Empty packet underflow");
        logi::Packet malformed{};malformed.size=100;require(malformed.data().empty(),"Oversized packet guard");
        require(logi::word(std::array<logi::Byte,2>{0x12,0x34})==0x1234,"HID big endian");
        std::cout<<"Linux mapping/protocol tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
