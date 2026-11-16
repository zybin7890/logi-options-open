#include "environment.hpp"

int main(int argc,char** argv){try{
    CHECK(argc==3);const std::string scenario=argv[1];fixture_path=argv[2];
    mode="smooth";restore_ok=true;respect_read_deadlines=true;fixture_app_name=L"C:\\Apps\\firefox.exe";
    const bool thumb=scenario=="thumb"||scenario=="thumb-volume"||scenario=="thumb-default";
    const bool disabled=scenario=="default"||scenario=="reload-enable"||thumb;
    const auto config=[&](bool enabled) {
        return std::string(scenario=="main-only"||scenario=="main-only-loss"?"[global]\nscroll.smooth=":"[global]\n0052=A\nscroll.smooth=")+(enabled?"1\n":"0\n")+
            (thumb?scenario=="thumb-volume"?"thumb.smooth=1\nthumb.left=VolumeDown\nthumb.right=VolumeUp\n":
             scenario=="thumb-default"?"thumb.left=ScrollLeft\nthumb.right=ScrollRight\n":
             "thumb.smooth=1\nthumb.left=ScrollLeft\nthumb.right=ScrollRight\n":"");
    };
    std::ofstream(fixture_path)<<config(!disabled);
    unsigned mode_value=scenario=="invert"?6:2,capability=8;
    std::vector<unsigned> mode_writes;unsigned mode_reads{},control_restores{};
    bool fail_restore{},bad_verify{};
    const RequestOverride wheel_request=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> parameters)->std::optional<logi::Reply>{
        if(feature!=5)return {};
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        if(function==0){reply.packet.bytes[4]=static_cast<logi::Byte>(capability);reply.packet.bytes[5]=12;}
        else if(function==1){++mode_reads;reply.packet.bytes[4]=static_cast<logi::Byte>(mode_value);if(bad_verify&&mode_value&1)reply.packet.bytes[4]^=1;}
        else if(function==2){CHECK(parameters.size()==1);mode_value=parameters[0];mode_writes.push_back(mode_value);reply.packet.bytes[4]=static_cast<logi::Byte>(mode_value);
            if(fail_restore&&!(mode_value&1)){fail_restore=false;return logi::Reply{logi::Result::timeout};}}
        else throw std::runtime_error("unexpected main wheel command");
        return reply;
    };
    mouse_request_result=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> parameters)->std::optional<logi::Reply>{
        if(feature==7&&function==3&&parameters[2]==0x23)++control_restores;
        return wheel_request(feature,function,parameters);
    };
    channel_request_result=wheel_request;
    configure_mouse=[&](logi::Mouse& mouse){mouse.wheel=5;if(thumb)mouse.thumb=6;if(connects>1)mode_value=2;};
    if(scenario=="parser") {
        Mappings mapping;std::istringstream good("[global]\nscroll.smooth=1\nthumb.smooth=0\n");CHECK(parse_mappings(good,mapping));
        CHECK(mapping.scroll_smooth==true&&mapping.thumb_smooth==false);
        for(const auto* text:{"[global]\nscroll.smooth=1\nscroll.smooth=0\n","[chrome.exe]\nscroll.smooth=1\n","[global]\nthumb.smooth=2\n"}) {
            Mappings invalid;std::istringstream stream(text);CHECK(!parse_mappings(stream,invalid));
        }
        Mappings empty;std::istringstream old("[global]\n0052=A\n");CHECK(parse_mappings(old,empty));CHECK(!empty.scroll_smooth&&!empty.thumb_smooth);
    } else if(scenario=="action"||scenario=="preview") {
        std::ofstream(fixture_path)<<"[global]\nthumb.smooth=1\n[hotkeys]\nCtrl+F10=ScrollRight\n";
        if(scenario=="preview")CHECK(play_actions(fixture_path.c_str(),false)==0);
        else {
            Mappings mappings;CHECK(load(fixture_path.c_str(),mappings));logi::Mouse desktop;
            Sequences sequences{{},mappings};Foreground foreground;
            sequences.start(mappings.hotkeys.front().binding,desktop,foreground.capture());
            CHECK(captured.empty()&&sequences.scrolling.active());
            while(sequences.scrolling.active()){tick+=10;sequences.service(desktop);}
            CHECK(!sequences.failed);
        }
        int total{};for(const auto& input:captured){CHECK(input.type==INPUT_MOUSE&&input.mi.dwFlags==MOUSEEVENTF_HWHEEL);total+=static_cast<int>(input.mi.mouseData);}
        CHECK(total==120&&captured.size()>2&&mode_writes.empty());
    } else if(scenario=="acquire-verify"||scenario=="foreign-target"||scenario=="zero-capability"||scenario=="restore-timeout"||scenario=="restore-current-settings") {
        auto mouse=logi::connect(nullptr);CHECK(mouse);std::optional<MainWheel> main;
        if(scenario=="acquire-verify")bad_verify=true;
        if(scenario=="foreign-target")mode_value=3;
        if(scenario=="zero-capability")capability=0;
        const bool activated=divert_main(*mouse,main);
        if(scenario=="foreign-target"||scenario=="zero-capability"){CHECK(!activated&&!main&&mode_writes.empty());}
        else if(scenario=="acquire-verify"){CHECK(!activated&&main&&mode_value==3);bad_verify=false;CHECK(restore(*mouse,{}, {},true,main));CHECK(mode_value==2);}
        else {
            CHECK(activated&&main&&main->original==2&&mode_value==3);
            if(scenario=="restore-current-settings")mode_value=5;
            else fail_restore=true;
            std::vector<Control> controls(1);controls[0].id=0x52;controls[0].restore=0x23;
            const auto restored=restore(*mouse,controls,{},true,main);
            CHECK(restored==(scenario=="restore-current-settings"));CHECK(control_restores==1);
            CHECK(mode_value==(scenario=="restore-current-settings"?4u:2u));
        }
        CHECK(captured.empty());
    } else {
        Step movement;movement.main_wheel=!thumb;movement.thumb=thumb;movement.wheel_delta=8;movement.at=100;
        Step end;end.at=2000;end.idle=true;
        steps={movement,end};
        if(scenario=="nonbrowser")fixture_app_name=L"C:\\Apps\\notepad.exe";
        else if(scenario=="packet-resolution"){movement.wheel_flags=1;movement.wheel_delta=1;movement.at=110;steps.insert(steps.begin()+1,movement);}
        else if(scenario=="negative"){steps[0].wheel_delta=-8;}
        else if(scenario=="malformed"){steps[0].wheel_flags=0x91;}
        else if(scenario=="input-rejected"){input_result=[](UINT,LPINPUT){return 0u;};}
        else if(scenario=="focus"||scenario=="pid"||scenario=="stop"||scenario=="disconnect"||scenario=="reload"||scenario=="reload-enable"||scenario=="loss"||scenario=="main-only-loss") {
            Step cancel;cancel.at=115;cancel.idle=scenario=="focus"||scenario=="pid"||scenario=="reload"||scenario=="reload-enable";
            cancel.app=scenario=="focus"?2:0;cancel.stop=scenario=="stop";cancel.disconnect=scenario=="disconnect";cancel.loss=scenario=="loss"||scenario=="main-only-loss";
            steps.insert(steps.begin()+1,cancel);
            if(scenario=="pid")before_step=[](unsigned index){if(index==2)foreground_pid_override=42;};
            if(scenario=="reload")before_step=[&](unsigned index){if(index==2){std::ofstream(fixture_path)<<config(false);reload_signal=true;}};
            if(scenario=="reload-enable")before_step=[&](unsigned index){if(index==2){std::ofstream(fixture_path)<<config(true);reload_signal=true;}};
            if(scenario=="disconnect"||scenario=="loss"||scenario=="reload-enable"||scenario=="main-only-loss") {
                movement.at=150;steps.insert(steps.end()-1,movement);
                if(scenario=="loss"){Step neutral;neutral.at=130;steps.insert(steps.begin()+2,neutral);}
            }
        }
        else if(scenario=="health-repair") {
            steps[0].at=3100;steps[0].idle=true;end.at=3200;steps[1]=end;movement.at=3300;end.at=5000;steps.push_back(movement);steps.push_back(end);
            before_step=[&](unsigned index){if(index==1)mode_value=2;};
        }
        else if(scenario=="browser"||scenario=="default"||scenario=="invert"||scenario=="main-only"||thumb){}
        else throw std::runtime_error("unknown smooth scenario");
        CHECK(run_agent(fixture_path.c_str(),false)==0);
        int total{};unsigned scroll_count{};
        for(const auto& input:captured)if(input.type==INPUT_MOUSE&&(input.mi.dwFlags&(MOUSEEVENTF_WHEEL|MOUSEEVENTF_HWHEEL))) {
            CHECK(input.mi.dwFlags==(thumb?MOUSEEVENTF_HWHEEL:MOUSEEVENTF_WHEEL));
            total+=static_cast<int>(input.mi.mouseData);++scroll_count;
        }
        if(scenario=="default"||scenario=="malformed"||scenario=="input-rejected")CHECK(scroll_count==0);
        else if(scenario=="thumb-volume"){CHECK(scroll_count==0);CHECK(captured.size()==2&&captured[0].ki.wVk==VK_VOLUME_UP);}
        else if(scenario=="focus"||scenario=="pid"||scenario=="stop"||scenario=="reload")CHECK(total==72&&scroll_count==1);
        else if(scenario=="disconnect"||scenario=="loss"||scenario=="main-only-loss")CHECK(total==192&&scroll_count>2);
        else if(scenario=="invert"||scenario=="negative")CHECK(total==-120&&scroll_count>2);
        else if(scenario=="packet-resolution")CHECK(total==240&&scroll_count>2);
        else {CHECK(total==120);if(scenario=="nonbrowser"||scenario=="thumb-default")CHECK(scroll_count==1);else CHECK(scroll_count>2);}
        if(scenario=="default"||thumb)CHECK(mode_writes.empty());
        else {
            CHECK(!mode_writes.empty()&&!(mode_writes.back()&1));CHECK(mode_reads>=3);
            CHECK(mode_value==(scenario=="invert"?6u:2u));
        }
        if(scenario=="browser"||scenario=="thumb") {
            CHECK(capture_times.front()==110);for(std::size_t i=1;i<capture_times.size();++i)CHECK(capture_times[i]-capture_times[i-1]>=10);
        }
        if(scenario=="disconnect")CHECK(connects==2&&std::ranges::count(mode_writes,2u)==1);
        if(scenario=="input-rejected")CHECK(connects==2&&std::ranges::count(mode_writes,2u)==2&&control_restores==2);
        if(scenario=="reload")CHECK(connects==1&&std::ranges::count(mode_writes,3u)==1);
        if(scenario=="reload-enable")CHECK(connects==1&&std::ranges::count(mode_writes,3u)==1&&capture_times.front()==160);
        if(scenario=="health-repair")CHECK(std::ranges::count(mode_writes,3u)==2&&async_begins>0&&capture_times.front()==3310);
        std::cout<<" units="<<total<<" outputs="<<scroll_count<<" writes="<<mode_writes.size();
    }
    std::cout<<" PASS production smooth "<<scenario<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
