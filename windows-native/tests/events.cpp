#include "environment.hpp"
static std::vector<WORD> keyboard(bool up){
    std::vector<WORD> result;
    for(const auto& input:captured)if(input.type==INPUT_KEYBOARD&&bool(input.ki.dwFlags&KEYEVENTF_KEYUP)==up)result.push_back(input.ki.wVk);
    return result;
}
static unsigned mouse_events(DWORD flag){return unsigned(std::ranges::count_if(captured,[&](const INPUT& i){return i.type==INPUT_MOUSE&&(i.mi.dwFlags&flag); }));}
static std::string directions="[global]\n0052=A\n0052.left=L\n0052.right=R\n0052.up=U\n0052.down=D\n";
static Step press(){Step s;s.down=true;return s;}
static Step move(int x,int y=0){Step s;s.motion=true;s.x=x;s.y=y;return s;}
static Step release(){return {};}
int main(int argc,char** argv){try{
    CHECK(argc==3);const std::string scenario=argv[1];fixture_path=argv[2];restore_ok=true;mode="gesture-lifecycle";fixture_has_xy=true;
    std::string config=directions;
    bool foreign_modifier_release{};
    if(scenario=="phase-contract"){
        using pointer_gesture::Phase;pointer_gesture::Lifecycle lifecycle;
        CHECK(lifecycle.phase()==Phase::idle&&!lifecycle.active()&&!lifecycle.progress()&&!lifecycle.end()&&!lifecycle.cancel());
        CHECK(lifecycle.start()&&lifecycle.phase()==Phase::START&&lifecycle.active()&&!lifecycle.start());
        CHECK(lifecycle.progress()&&lifecycle.phase()==Phase::PROGRESS&&lifecycle.progress());
        CHECK(lifecycle.end()&&lifecycle.phase()==Phase::END&&!lifecycle.active()&&!lifecycle.end()&&!lifecycle.cancel());
        CHECK(lifecycle.start()&&lifecycle.cancel()&&lifecycle.phase()==Phase::CANCELED&&!lifecycle.end()&&!lifecycle.cancel());
        Control control{0x52,true,0x22};control.session.start();control.session.progress();control.down=true;control.panning=true;control.x=100;
        cancel_gesture(control);CHECK(control.session.phase()==Phase::CANCELED&&control.wait_release&&!control.down&&!control.panning&&control.x==0);
        cancel_gesture(control);CHECK(control.session.phase()==Phase::CANCELED&&control.wait_release&&captured.empty());
        std::cout<<"PASS production phase transitions and idempotent cancellation without END action\n";return 0;
    }
    if(scenario=="smart-open"){
        allow_open=true;std::ofstream(fixture_path)<<"[hotkeys]\nCtrl+Alt+F10=A;Open(fake.exe);Wait(200);B\n";
        CHECK(play_actions(fixture_path.c_str(),false)==0&&foreground_app==2);
        CHECK((keyboard(false)==std::vector<WORD>{'A','B'}));
        std::cout<<"PASS production play_actions preserves explicit Open flow\n";return 0;
    }
    steps={press(),move(0),move(100),release()};
    std::vector<WORD> expected{'R'};
    if(scenario=="tap"){steps={press(),release()};expected={'A'};}
    else if(scenario=="direction"){before_step=[](unsigned index){if(index<=4)CHECK(captured.empty());};}
    else if(scenario=="diagonal"){steps={press(),move(0),move(100,100),release()};}
    else if(scenario=="multi-direction-final"){steps={press(),move(0),move(100),move(0,200),release()};expected={'D'};}
    else if(scenario=="app-switch"){
        config+="[a.exe]\n0052.right=X\n[b.exe]\n0052.right=Y\n";
        Step switched=move(100);switched.app=2;
        steps={press(),move(0),switched,release(),press(),move(0),move(100),release()};expected={'Y'};
    }
    else if(scenario=="macro-app-switch"){
        fixture_has_xy=false;config="[global]\n0052=A;Wait(200);B\n";
        auto switched=release();switched.app=2;switched.at=500;
        steps={press(),switched,release()};expected={'A'};
    }
    else if(scenario=="mouse-open"){
        fixture_has_xy=false;allow_open=true;config="[global]\n0052=A;Open(fake.exe);Wait(200);B\n";
        auto after=release();after.at=500;steps={press(),after,release()};expected={'A','B'};
    }
    else if(scenario=="failed-open-handoff"){
        fixture_has_xy=false;allow_open=fail_open=true;config="[global]\n0052=A;Open(fake.exe);Wait(200);B\n";
        auto after=release();after.at=500;after.app=2;steps={press(),after,release()};expected={'A','B'};
    }
    else if(scenario=="concurrent-runs-physical-ctrl"){
        fixture_has_xy=false;physical_keys[VK_LCONTROL]=true;config="[global]\n0052=Ctrl+A;Wait(200);Ctrl+B\n";
        auto first=press();first.at=100;auto first_up=release();first_up.at=150;
        auto second=press();second.at=200;auto second_up=release();second_up.at=250;
        auto due=release();due.at=450;steps={first,first_up,second,second_up,due};expected={'A','A','B','B'};
    }
    else if(scenario=="switch-away-back"){
        Step away=move(100);away.app=2;Step back=release();back.app=1;
        steps={press(),move(0),away,back};expected={};
    }
    else if(scenario=="same-window-new-pid"){
        before_step=[](unsigned index){if(index==3)foreground_pid_override=2;};expected={};
    }
    else if(scenario=="null-capture"||scenario=="null-to-valid-held"){
        foreground_app=0;expected={};
        if(scenario=="null-to-valid-held"){
            auto still_held=press();still_held.app=1;steps={press(),still_held,move(0),move(100),release()};
        }
    }
    else if(scenario=="foreground-lost"){
        before_step=[](unsigned index){if(index==3)foreground_app=0;};expected={};
    }
    else if(scenario=="ring-unreturned"||scenario=="ring-restored"){
        fixture_has_xy=false;config="[global]\n0052=Ring\n[ring]\nChosen=K\n";steps={press(),release()};
        const bool restored=scenario=="ring-restored";expected=restored?std::vector<WORD>{'K'}:std::vector<WORD>{};
        ring_choice=[restored]{foreground_app=2;if(restored)foreground_app=1;return 0;};
    }
    else if(scenario=="loss-cancel"){
        Step loss;loss.loss=true;
        steps={press(),move(0),move(100),loss,release(),release(),press(),move(0),move(100),release()};
    }
    else if(scenario=="stop-cancel"){
        Step stop;stop.stop=true;steps={press(),move(0),move(100),stop};expected={};
    }
    else if(scenario=="reconnect-cancel"){
        Step failed;failed.disconnect=true;
        steps={press(),move(0),move(100),failed,release(),press(),move(0),move(100),release()};
    }
    else if(scenario=="reload-cancel"){
        steps={press(),move(0),move(100),press(),release(),press(),move(0),move(100),release()};expected={'N'};
        before_step=[](unsigned index){if(index==4){std::ofstream(fixture_path)<<"[global]\n0052=B\n0052.right=N\n";reload_signal=true;}};
    }
    else if(scenario=="dpi-cancel"){
        mode="owner-gesture";steps={press(),move(0),move(100),release(),release(),press(),move(0),move(600),release()};
    }
    else if(scenario=="dpi-window-released"||scenario=="dpi-window-held"){
        const bool held=scenario=="dpi-window-held";mode=held?"owner-window-held":"owner-window-released";
        steps={release(),release(),release(),press(),move(0),move(600),release()};
        if(held){steps.push_back(press());steps.push_back(move(0));steps.push_back(move(600));steps.push_back(release());}
    }
    else if(scenario=="pan-tap"){
        config="[global]\n0052=Pan\n";steps={press(),release()};expected={};
    }
    else if(scenario=="pan-progress"){
        config="[global]\n0052=Pan\n";steps={press(),move(0),move(124),move(-124),release()};expected={};
    }
    else if(scenario=="pan-cancel"){
        config="[global]\n0052=Pan\n";Step loss;loss.loss=true;
        steps={press(),move(0),loss,release(),release()};expected={};
    }
    else if(scenario=="physical-modifier"){
        fixture_has_xy=false;config="[global]\n0052=Ctrl+A\n";physical_keys[VK_LCONTROL]=true;steps={press(),release()};expected={'A'};
    }
    else if(scenario=="partial-keyboard"||scenario=="partial-keyboard-two"||scenario=="partial-keyboard-three"||scenario=="rejected-input"){
        fixture_has_xy=false;config="[global]\n0052=Ctrl+A\n";steps={press(),release()};expected={VK_LCONTROL};
        const unsigned prefix=scenario=="rejected-input"?0:scenario=="partial-keyboard"?1:scenario=="partial-keyboard-two"?2:3;
        if(prefix==0)expected={};else if(prefix>=2)expected={VK_LCONTROL,'A'};
        unsigned calls{};input_result=[calls,prefix](UINT n,LPINPUT)mutable{return ++calls==1?prefix:n;};
    }
    else if(scenario=="partial-mouse"){
        fixture_has_xy=false;config="[global]\n0052=MouseMiddle\n";steps={press(),release()};expected={};
        unsigned calls{};input_result=[calls](UINT n,LPINPUT)mutable{return ++calls==1?1u:n;};
    }
    else if(scenario=="failed-cleanup-new-physical-hold"){
        fixture_has_xy=false;config="[global]\n0052=Ctrl+A\n";steps={press(),release()};expected={VK_LCONTROL};
        unsigned calls{};input_result=[calls,&foreign_modifier_release](UINT n,LPINPUT input)mutable{
            ++calls;if(calls==1)return 1u;
            if(calls==2){physical_keys[VK_LCONTROL]=true;return 0u;} // Physical release/re-press after blocked cleanup.
            for(unsigned i=0;i<n;++i)if(input[i].type==INPUT_KEYBOARD&&input[i].ki.wVk==VK_LCONTROL&&
                (input[i].ki.dwFlags&KEYEVENTF_KEYUP)&&physical_keys[VK_LCONTROL])foreign_modifier_release=true;
            return n;
        };
    }
    else if(scenario!="direction")throw std::runtime_error("unknown scenario");
    std::ofstream(fixture_path)<<config;
    const auto result=run_agent(fixture_path.c_str(),false);CHECK(result==0);
    const auto downs=keyboard(false),ups=keyboard(true);
    if(downs!=expected){std::cerr<<"down keys:";for(auto key:downs)std::cerr<<' '<<key;std::cerr<<'\n';}
    CHECK(downs==expected);
    if(scenario=="partial-keyboard")CHECK((ups==std::vector<WORD>{VK_LCONTROL}));
    if(scenario=="partial-keyboard-two"||scenario=="partial-keyboard-three")CHECK((ups==std::vector<WORD>{'A',VK_LCONTROL}));
    if(scenario=="rejected-input")CHECK(ups.empty());
    if(scenario=="physical-modifier")CHECK((ups==std::vector<WORD>{'A'}));
    if(scenario=="partial-mouse"||scenario=="pan-tap")CHECK(mouse_events(MOUSEEVENTF_MIDDLEDOWN)==1&&mouse_events(MOUSEEVENTF_MIDDLEUP)==1);
    if(scenario=="pan-progress")CHECK(mouse_events(MOUSEEVENTF_HWHEEL)==2&&mouse_events(MOUSEEVENTF_MIDDLEDOWN)==0&&mouse_events(MOUSEEVENTF_MIDDLEUP)==0);
    if(scenario=="pan-cancel")CHECK(captured.empty());
    if(scenario=="failed-cleanup-new-physical-hold")CHECK(!foreign_modifier_release);
    if(scenario=="concurrent-runs-physical-ctrl")CHECK(ups==expected&&physical_keys[VK_LCONTROL]);
    CHECK(connects==(scenario=="reconnect-cancel"?2u:1u));
    std::cout<<"PASS actual_events "<<scenario<<" accepted_inputs="<<captured.size()<<" reconnects="<<(connects-1)<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<argv[1]<<": "<<error.what()<<'\n';return 1;}}
