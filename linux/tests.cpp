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
        auto profiles=parse("[global]\n0053=MouseBack\n0056=MouseForward\nthumb.speed=125\n[Firefox]\n0053=Ctrl+Tab\nthumb.speed=150\n[Empty]\n");
        require(profiles.profiles.contains("firefox") && profiles.profiles.contains("empty"),"Profile normalization and empty profile");
        require(lookup(profiles,{0x53,Direction::tap},{"Firefox"})->front().keys.front()==KEY_LEFTCTRL,"Application override");
        require(lookup(profiles,{0x56,Direction::tap},{"firefox"})->front().keys.front()==BTN_EXTRA,"Application inheritance");
        require(speed_for(profiles,{"firefox"})==150 && speed_for(profiles,{"other"})==125,"Profile thumb speed");
        rejects([]{parse("[chrome]\n00C3=Super+Tab");});
        auto rich=parse("[hotkeys]\nCtrl+Alt+F10=Text(hello; (world));Wait(20);Ctrl+V\n[ring]\nMedia=Folder(Media)\n[ring/Media]\nMute=Mute\n");
        require(rich.hotkeys.size()==1 && rich.hotkeys[0].actions.size()==3,"Hotkey sequence");
        require(rich.hotkeys[0].actions[0].text=="hello; (world)","Text delimiters");
        require(rich.folders.at("Media").size()==1,"Ring folder");
        require(actions("TextHex(4f60597dd83dde00)")[0].text=="\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x98\x80","UTF-16 TextHex conversion");
        rejects([]{actions("TextHex(d8000041)");});
        rejects([]{actions("Text(broken(");});
        rejects([]{parse("[hotkeys]\nCtrl=Text(a)");});
        rejects([]{parse("[hotkeys]\nCtrl+F10=Text(a)\nF10+Ctrl=Text(b)");});
        rejects([]{parse("[global]\n0053=Ring");});
        rejects([]{parse("[ring]\nMissing=Folder(Missing)");});
        rejects([]{parse("[ring]\nGo=Folder(A)\n[ring/A]\nCycle=Folder(B)\n[ring/B]\nCycle=Folder(A)");});
        rejects([]{parse("[ring]\nAgain=Ring");});
        rejects([]{actions("Ring;Ctrl+A");});
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
