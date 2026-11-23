#include "environment.hpp"

static Step thumb(int delta,unsigned phase=1,unsigned app=1) {
    Step event;event.thumb=true;event.thumb_delta=delta;event.thumb_phase=phase;event.app=app;
    return event;
}

int main(int argc,char** argv){try {
    CHECK(argc==3);const std::string scenario=argv[1];fixture_path=argv[2];restore_ok=true;mode="thumb-steps";
    configure_mouse=[](logi::Mouse& mouse){mouse.thumb=6;};
    mouse_request_result=[](logi::Byte feature,logi::Byte function,std::span<const logi::Byte>)->std::optional<logi::Reply> {
        if(feature!=6 || function!=0)return {};
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        reply.packet.bytes[5]=18;reply.packet.bytes[7]=120;reply.packet.bytes[8]=1;
        return reply;
    };
    std::string config="[global]\nthumb.left=X\nthumb.right=Y\n";
    WORD expected='Y';
    if(scenario=="starts" || scenario=="speed") {
        steps={thumb(2),thumb(0,3),thumb(2),thumb(0,3),thumb(2),thumb(0,3),thumb(2)};
        if(scenario=="speed"){config+="thumb.speed=70\n";steps.push_back(thumb(0,3));steps.push_back(thumb(2));}
    } else if(scenario=="reverse") {
        steps={thumb(6),thumb(-1),thumb(-5,2),thumb(-1)};expected='X';
    } else if(scenario=="window") {
        // Both windows inherit the same global action.
        steps={thumb(6),thumb(1,1,2),thumb(5,2,2),thumb(1,1,2)};
    } else if(scenario=="pid") {
        steps={thumb(6),thumb(1),thumb(5,2),thumb(1)};
        before_step=[](unsigned index){if(index==2)foreground_pid_override=2;};
    } else if(scenario=="reload" || scenario=="reconnect") {
        Step gap;gap.idle=scenario=="reload";gap.disconnect=scenario=="reconnect";
        steps={thumb(6),gap,thumb(1),thumb(5,2),thumb(1)};
        if(scenario=="reload"){
            expected='Z';
            before_step=[](unsigned index){if(index==2){
                std::ofstream(fixture_path)<<"[global]\nthumb.left=X\nthumb.right=Z\n";reload_signal=true;
            }};
        }
    } else {
        CHECK(scenario=="binding");
        Wheel wheel{{},18,120,1};
        const Binding first{L"a",thumb_wheel,Gesture::right,{}},second{L"b",thumb_wheel,Gesture::right,{}};
        const InputContext owner{reinterpret_cast<HWND>(1),1,L"app"};
        std::array<logi::Byte,5> packet{0,6,0,0,1};
        CHECK(wheel.steps(packet,&first,100,owner)==0);
        packet[1]=2;CHECK(wheel.steps(packet,&second,100,owner)==0);
        packet[0]=0xff;packet[1]=0xfa;CHECK(wheel.steps(packet,&second,100,owner)==0);
        packet[1]=0xff;CHECK(wheel.steps(packet,&second,100,owner)==-1);
        return 0;
    }
    for(std::size_t i=0;i<steps.size();++i)steps[i].at=110+i*10;
    std::ofstream(fixture_path)<<config;
    CHECK(run_agent(fixture_path.c_str(),false)==0);
    CHECK(connects==(scenario=="reconnect"?2u:1u));
    CHECK(captured.size()==2 && captured[0].type==INPUT_KEYBOARD && captured[1].type==INPUT_KEYBOARD);
    CHECK(captured[0].ki.wVk==expected && captured[1].ki.wVk==expected);
    CHECK(!(captured[0].ki.dwFlags&KEYEVENTF_KEYUP) && (captured[1].ki.dwFlags&KEYEVENTF_KEYUP));
    CHECK(capture_times[0]==steps.back().at && capture_times[1]==steps.back().at);
    std::cout<<"PASS thumb-wheel "<<scenario<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
