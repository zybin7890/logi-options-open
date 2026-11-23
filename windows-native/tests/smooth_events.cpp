#define LOGI_CAPTURE_VOLUME_STEPS
#include "environment.hpp"
#include <chrono>

static std::atomic<unsigned> thumb_volume_steps{};
static unsigned confirmed_generation{};
static unsigned generation_for_test() noexcept {return confirmed_generation;}
static std::function<system_actions::VolumeResult(bool,unsigned,const ActionContext&)> volume_effect;
static std::atomic<unsigned> fallback_huds{};
static void observe_volume_hud(std::wstring_view title,std::wstring_view subtitle,notifications_hud::Kind,unsigned,bool) {
    CHECK(title==L"音量"&&subtitle==L"37% · 已静音");
    ++fallback_huds;
}
system_actions::VolumeResult system_actions::step_output_volume(bool up,unsigned count,const ActionContext& context) {
    if(volume_effect)return volume_effect(up,count,context);
    CHECK(up&&count&&context.active());
    thumb_volume_steps+=count;
    return {count,S_OK,37,true};
}

int main(int argc,char** argv){try{
    CHECK(argc==3);const std::string scenario=argv[1];fixture_path=argv[2];
    mode="smooth";restore_ok=true;respect_read_deadlines=true;fixture_app_name=L"C:\\Apps\\firefox.exe";
    const bool thumb_volume=scenario=="thumb-volume"||scenario=="thumb-volume-media";
    const bool thumb=scenario=="thumb"||thumb_volume||scenario=="thumb-default";
    const bool same_pid_recovery=scenario=="input-generation"||scenario=="input-generation-pending";
    const bool native_only=scenario=="input-rejected"||scenario=="input-cancelled-current-owner";
    const bool clash_scope=scenario=="clash-browser"||scenario=="clash-protected-all";
    const bool disabled=scenario=="default"||scenario=="reload-enable"||thumb;
    const auto config=[&](bool enabled) {
        return std::string(scenario=="main-only"||scenario=="main-only-loss"?"[global]\nscroll.smooth=":"[global]\n0052=A\nscroll.smooth=")+(enabled?"1\n":"0\n")+
            (scenario=="input-recovery"||scenario=="clash-protected-all"?"scroll.smooth.all=1\n":"")+
            (thumb?thumb_volume?"thumb.smooth=1\nthumb.left=VolumeDown\nthumb.right=VolumeUp\n":
             scenario=="thumb-default"?"thumb.left=ScrollLeft\nthumb.right=ScrollRight\n":
             "thumb.smooth=1\nthumb.left=ScrollLeft\nthumb.right=ScrollRight\n":"");
    };
    std::ofstream(fixture_path)<<config(!disabled);
    unsigned mode_value=scenario=="invert"?6:2,capability=8;
    std::vector<unsigned> mode_writes;unsigned mode_reads{},control_restores{};
    bool fail_restore{},bad_verify{};
    unsigned rejected_wheel{},native_readbacks{},resumed_readbacks{},rejected_keys{};
    const RequestOverride wheel_request=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> parameters)->std::optional<logi::Reply>{
        if(feature!=5)return {};
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        if(function==0){reply.packet.bytes[4]=static_cast<logi::Byte>(capability);reply.packet.bytes[5]=12;}
        else if(function==1){++mode_reads;if(rejected_wheel && !(mode_value&1))++native_readbacks;
            if(same_pid_recovery && confirmed_generation==18 && (mode_value&1))++resumed_readbacks;
            reply.packet.bytes[4]=static_cast<logi::Byte>(mode_value);if(bad_verify&&mode_value&1)reply.packet.bytes[4]^=1;}
        else if(function==2){CHECK(parameters.size()==1);mode_value=parameters[0];mode_writes.push_back(mode_value);reply.packet.bytes[4]=static_cast<logi::Byte>(mode_value);
            if(fail_restore&&!(mode_value&1)){fail_restore=false;return logi::Reply{logi::Result::timeout};}}
        else throw std::runtime_error("unexpected main wheel command");
        return reply;
    };
    mouse_request_result=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> parameters)->std::optional<logi::Reply>{
        if(feature==7&&function==3&&parameters[2]==0x23)++control_restores;
        return wheel_request(feature,function,parameters);
    };
    bool rejected_readback{};
    channel_request_result=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> parameters)->std::optional<logi::Reply>{
        auto reply=wheel_request(feature,function,parameters);
        if(feature!=5 || !reply || !rejected_wheel || mode_value!=2)return reply;
        if(scenario=="input-generation-pending" && function==1 && native_readbacks==1)async_due=tick+500;
        if((scenario=="input-preempt"||scenario=="input-stop-pending") && function==2)async_due=tick+120;
        if(scenario=="input-readback" && function==1 && !rejected_readback) {
            rejected_readback=true;reply->packet.bytes[4]^=1;
        }
        return reply;
    };
    configure_mouse=[&](logi::Mouse& mouse){mouse.wheel=5;if(thumb)mouse.thumb=6;if(connects>1)mode_value=2;};
    if(scenario=="thumb-volume-queue") {
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        auto until=[&](auto&& ready){while(!ready()){CHECK(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}};
        for(const auto& [code,expected]:std::vector<std::pair<std::wstring,int>>{
                {L"VolumeUp",1},{L"VolumeDown",-1},{L"Ctrl+VolumeUp",0},{L"VolumeUp;VolumeDown",0},{L"None",0}}) {
            Binding binding{};CHECK(parse_actions(code,binding.actions));CHECK(thumb_volume_direction(&binding)==expected);
        }
        std::atomic<bool> entered{},allow{};std::vector<int> applied;
        volume_effect=[&](bool up,unsigned count,const ActionContext& context){
            entered=true;until([&]{return allow.load();});CHECK(context.active()&&!context.window);
            applied.push_back(up?static_cast<int>(count):-static_cast<int>(count));
            return system_actions::VolumeResult{count,S_OK};
        };
        bool failed{};ThumbVolumeOutput output;output.add(true,1);until([&]{return entered.load();});
        output.add(true,5);output.add(true,10);output.add(false,2);output.add(true,3);
        CHECK(output.jobs.size()==4&&output.service(failed)==16);
        // The owner can dispatch a second physical binding while the Core Audio call is blocked.
        Mappings mappings;logi::Mouse desktop;Sequences seq{{},mappings};Binding key_binding{};CHECK(parse_actions(L"A",key_binding.actions));
        seq.start(key_binding,desktop);CHECK(captured.size()==2&&captured.front().ki.wVk=='A');
        Binding volume_key{};CHECK(parse_actions(L"VolumeUp",volume_key.actions));
        seq.start(volume_key,desktop);CHECK(captured.size()==4&&captured[2].ki.wVk==VK_VOLUME_UP);
        allow=true;until([&]{return output.service(failed)==INFINITE;});
        CHECK(!failed&&applied==std::vector<int>({1,15,-2,3}));
        volume_effect=[](bool,unsigned,const ActionContext&){return system_actions::VolumeResult{1,E_FAIL};};
        output.add(false,4);until([&]{return output.service(failed)==INFINITE;});CHECK(failed);
        entered=false;std::atomic<unsigned> calls{};
        volume_effect=[&](bool,unsigned,const ActionContext& context){
            ++calls;entered=true;until([&]{return !context.active();});return system_actions::VolumeResult{0,E_ABORT};
        };
        output.add(true,1);until([&]{return entered.load();});output.add(false,2);
        auto first=output.jobs.front().task,second=output.jobs.back().task;
        output.cancel(failed);
        until([&]{return first->status.load()!=slow_actions::Status::pending&&second->status.load()!=slow_actions::Status::pending;});
        CHECK(calls==1&&first->status==slow_actions::Status::cancelled&&second->status==slow_actions::Status::cancelled);
        // Explicitly clean up the test effect only after the last worker finished.
        volume_effect={};
    } else if(scenario=="thumb-volume-policy") {
        ThumbVolumeOutput output;bool failed{};InputContext owner{fake_foreground(),1,{}};
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        const auto until=[&](auto&& ready){while(!ready()){CHECK(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}};
        notifications_hud::notify_hook()=observe_volume_hud;
        physical_keys[VK_VOLUME_UP]=true;output.submit(true,1,owner,failed);physical_keys[VK_VOLUME_UP]=false;
        CHECK(captured.empty()&&output.jobs.empty());
        // A partial down must receive a cleanup up; neither gets a second volume change.
        input_result=[](UINT n,LPINPUT events){CHECK(events[0].ki.wVk==VK_VOLUME_UP);return n==2?1u:n;};
        output.submit(true,2,owner,failed);
        CHECK(failed&&captured.size()==2&&(captured[1].ki.dwFlags&KEYEVENTF_KEYUP)&&output.jobs.empty());
        failed=false;
        input_result=[](UINT,LPINPUT){foreground_app=2;return 0u;};
        output.submit(true,1,owner,failed);CHECK(output.jobs.empty());foreground_app=1;
        stop_event=reinterpret_cast<HANDLE>(101);
        input_result=[](UINT,LPINPUT){stopped=true;return 0u;};
        output.submit(true,1,owner,failed);CHECK(output.jobs.empty());stopped=false;

        std::atomic<bool> entered{},release{};std::vector<int> order;
        volume_effect=[&](bool up,unsigned n,const ActionContext& ctx){
            CHECK(up&&ctx.window==owner.window&&ctx.pid==1);entered=true;
            until([&]{return release.load();});
            order.push_back(static_cast<int>(n));return system_actions::VolumeResult{n,S_OK,37,true};
        };
        unsigned calls{};
        input_result=[&](UINT n,LPINPUT events){
            CHECK(n==2);++calls;
            if(calls==1)return 0u;
            CHECK(events[0].ki.wVk==VK_VOLUME_DOWN&&fallback_huds==1);
            order.push_back(-1);return n;
        };
        output.submit(true,2,owner,failed);until([&]{return entered.load();});
        output.submit(false,1,owner,failed);
        CHECK(calls==1&&output.deferred_steps==1&&output.service(failed)==16);
        release=true;until([&]{return output.service(failed)==INFINITE;});
        CHECK(!failed&&order==std::vector<int>({2,-1})&&fallback_huds==1);

        entered=false;release=false;calls=0;
        output.submit(true,1,owner,failed);until([&]{return entered.load();});
        for(unsigned i=0;i<40;++i)output.submit(false,1,owner,failed);
        CHECK(calls==1&&output.deferred_steps==32);
        tick+=251;output.service(failed);
        CHECK(output.deferred.empty()&&output.deferred_steps==0);
        release=true;until([&]{return output.service(failed)==INFINITE;});
        CHECK(calls==1&&fallback_huds==2);
        volume_effect={};notifications_hud::notify_hook()=nullptr;
    } else if(scenario=="parser") {
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
        if(scenario=="thumb-volume") {
            notifications_hud::notify_hook()=observe_volume_hud;
            input_result=[](UINT n,LPINPUT events){CHECK(n==2&&events[0].ki.wVk==VK_VOLUME_UP);return 0u;};
            before_step=[&](unsigned index){if(index==2){
                const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!fallback_huds){CHECK(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
            }};
        }
        else if(scenario=="firefox-fine-stop") {
            steps.clear();
            for(auto [at,flags,count]:std::array<std::tuple<unsigned,unsigned,int>,5>{
                    {{100,1,1},{10100,1,1},{15100,1,-1},{20100,0x11,1},{25100,0x11,-1}}}) {
                movement.at=at;movement.wheel_flags=flags;movement.wheel_delta=count;
                steps.push_back(movement);
            }
            end.at=30000;steps.push_back(end);
        }
        else if(scenario=="nonbrowser")fixture_app_name=L"C:\\Apps\\notepad.exe";
        else if(scenario=="reload-scope") {
            fixture_app_name=L"C:\\Apps\\notepad.exe";
            Step enable;enable.idle=true;enable.at=200;
            Step disable=enable;disable.at=600;
            Step enabled_wheel=movement;enabled_wheel.at=350;
            Step native_wheel=movement;native_wheel.at=800;
            steps={steps.front(),enable,enabled_wheel,disable,native_wheel,end};
            before_step=[&](unsigned index) {
                if(index==2||index==4){std::ofstream(fixture_path)<<config(true)<<"scroll.smooth.all="<<(index==2?1:0)<<"\n";reload_signal=true;}
                if(index==1||index==5)CHECK(mode_value==2);
                if(index==3)CHECK(mode_value==3);
            };
        }
        else if(clash_scope) {
            fixture_app_name=L"C:\\Apps\\Clash Party.exe";second_app_name=L"C:\\Apps\\firefox.exe";
            if(scenario=="clash-protected-all")blocked_wheel_app=1;
            Step to_browser;to_browser.idle=true;to_browser.app=2;to_browser.at=200;
            Step hover_clash;hover_clash.idle=true;hover_clash.at=500;
            Step in_clash=movement;in_clash.at=650;
            Step hover_browser=hover_clash;hover_browser.at=700;
            Step in_browser=movement;in_browser.at=900;
            Step to_clash=to_browser;to_clash.app=1;to_clash.at=1200;
            Step final_wheel=movement;final_wheel.at=1400;
            movement.at=350;
            steps={steps.front(),to_browser,movement,hover_clash,in_clash,hover_browser,in_browser,to_clash,final_wheel,end};
            before_step=[&](unsigned index) {
                if(index==4)hovered_app=1;
                // A browser renderer child can have a different PID from its top-level window.
                if(index==6){hovered_app=3;hovered_root=2;}
                if(index==8){hovered_app=0;hovered_root=0;}
                if(index==1 || index==5 || index==9)CHECK(mode_value==2);
                if(index==3 || index==7)CHECK(mode_value==3);
            };
        }
        else if(scenario=="packet-resolution"){movement.wheel_flags=1;movement.wheel_delta=1;movement.at=110;steps.insert(steps.begin()+1,movement);}
        else if(scenario=="negative"){steps[0].wheel_delta=-8;}
        else if(scenario=="malformed"){steps[0].wheel_flags=0x91;}
        else if(native_only) {
            input_result=[&](UINT count,LPINPUT events){
                CHECK(count==1&&events->type==INPUT_MOUSE&&events->mi.dwFlags==MOUSEEVENTF_WHEEL);
                if(scenario=="input-cancelled-current-owner")fixture_last_error=ERROR_CANCELLED;
                ++rejected_wheel;return 0u;
            };
            movement.at=500;steps.insert(steps.end()-1,movement);end.at=4000;steps.back()=end;
            before_step=[&](unsigned index){
                CHECK(foreground_app==1&&foreground_pid_override==0);
                if(index>=2)CHECK(mode_value==2&&native_readbacks>0);
            };
        }
        else if(same_pid_recovery) {
            confirmed_generation=17;
            Step first_press;first_press.at=170;first_press.down=true;
            Step first_release;first_release.at=180;
            Step stale=movement;stale.at=240;
            Step confirming=first_press;confirming.at=400;
            Step release=first_release;release.at=420;
            movement.at=900;
            steps={steps.front(),first_press,first_release,stale,confirming,release,movement,end};
            input_result=[&](UINT count,LPINPUT events) {
                if(events->type==INPUT_KEYBOARD) {
                    CHECK(events->ki.wVk=='A'&&foreground_app==1);
                    if(tick<400){++rejected_keys;return 0u;}
                    confirmed_generation=18;
                    return count;
                }
                CHECK(events->type==INPUT_MOUSE&&events->mi.dwFlags==MOUSEEVENTF_WHEEL);
                if(confirmed_generation==17){++rejected_wheel;return 0u;}
                CHECK(tick>=900&&resumed_readbacks>0);
                return count;
            };
            before_step=[&](unsigned index) {
                CHECK(foreground_app==1&&foreground_pid_override==0);
                if(index>=2&&index<=5)CHECK(mode_value==2&&native_readbacks>0&&confirmed_generation==17);
                if(index==6&&scenario=="input-generation-pending")CHECK(confirmed_generation==18&&mode_value==2&&resumed_readbacks==0);
                if(index==7)CHECK(mode_value==3&&resumed_readbacks>0);
            };
        }
        else if(scenario=="input-recovery"||scenario=="input-recovery-direct") {
            // Rejection by a privileged foreground must not release the device.
            // A fresh side-button press and wheel movement must work on return.
            input_result=[&](UINT count,LPINPUT){if(tick>=200)return count;++rejected_wheel;return 0u;};
            Step press;press.at=250;press.down=true;press.app=2;
            Step release;release.at=260;
            movement.at=300;steps={steps.front(),press,release,movement,end};
            before_step=[&](unsigned index){if(index==2)CHECK(mode_value==2&&native_readbacks>0);if(index==4)CHECK(mode_value==(scenario=="input-recovery-direct"?2u:3u));};
        }
        else if(scenario=="input-preempt"||scenario=="input-stop-pending"||scenario=="input-readback"||scenario=="input-reload") {
            input_result=[&](UINT count,LPINPUT input){
                if(input->type==INPUT_MOUSE && !rejected_wheel){++rejected_wheel;return 0u;}
                return count;
            };
            Step press;press.down=true;press.at=140;
            Step release;release.at=150;
            Step change;change.idle=true;change.at=170;
            Step resume;resume.idle=true;resume.at=240;
            movement.at=400;end.at=4000;
            steps={steps.front(),press,release,change,resume,movement,end};
            if(scenario=="input-stop-pending")steps[3].stop=true;
            before_step=[&](unsigned index) {
                if(index==2 && (scenario=="input-preempt"||scenario=="input-stop-pending"))CHECK(native_readbacks==0 && mode_value==2);
                if(index==4 && (scenario=="input-preempt"||scenario=="input-reload")) {
                    std::ofstream(fixture_path)<<config(scenario=="input-preempt");reload_signal=true;
                }
                if(index==5 && scenario=="input-reload"){CHECK(mode_value==2);std::ofstream(fixture_path)<<config(true);reload_signal=true;}
                if(index==6)CHECK(mode_value==(scenario=="input-reload"?3u:2u));
                if(index==7 && scenario=="input-readback")CHECK(rejected_readback && native_readbacks>=2);
            };
        }
        else if(scenario=="focus"||scenario=="pid"||scenario=="stop"||scenario=="disconnect"||scenario=="reload"||scenario=="reload-enable"||scenario=="loss"||scenario=="loss-no-neutral"||scenario=="main-only-loss") {
            Step cancel;cancel.at=115;cancel.idle=scenario=="focus"||scenario=="pid"||scenario=="reload"||scenario=="reload-enable";
            cancel.app=scenario=="focus"?2:0;cancel.stop=scenario=="stop";cancel.disconnect=scenario=="disconnect";cancel.loss=scenario=="loss"||scenario=="loss-no-neutral"||scenario=="main-only-loss";
            steps.insert(steps.begin()+1,cancel);
            if(scenario=="pid")before_step=[](unsigned index){if(index==2)foreground_pid_override=42;};
            if(scenario=="reload")before_step=[&](unsigned index){if(index==2){std::ofstream(fixture_path)<<config(false);reload_signal=true;}};
            if(scenario=="reload-enable")before_step=[&](unsigned index){if(index==2){std::ofstream(fixture_path)<<config(true);reload_signal=true;}};
            if(scenario=="disconnect"||scenario=="loss"||scenario=="loss-no-neutral"||scenario=="reload-enable"||scenario=="main-only-loss") {
                movement.at=scenario=="loss-no-neutral"?700u:150u;steps.insert(steps.end()-1,movement);
                if(scenario=="loss"){Step neutral;neutral.at=130;steps.insert(steps.begin()+2,neutral);}
            }
        }
        else if(scenario=="health-repair") {
            steps[0].at=3100;steps[0].idle=true;end.at=3200;steps[1]=end;movement.at=3300;end.at=5000;steps.push_back(movement);steps.push_back(end);
            before_step=[&](unsigned index){if(index==1)mode_value=2;};
        }
        else if(scenario=="browser"||scenario=="default"||scenario=="invert"||scenario=="main-only"||thumb){}
        else throw std::runtime_error("unknown smooth scenario");
        if(same_pid_recovery) {
            LiveHealth live;
            Mappings mappings;CHECK(load(fixture_path.c_str(),mappings));
            auto mouse=logi::connect(nullptr);CHECK(mouse);
            std::vector<Control> controls;std::optional<Wheel> side;std::optional<MainWheel> main;
            CHECK(enable(*mouse,mappings.bindings,controls)&&divert_main(*mouse,main));
            std::size_t registered{};DWORD blocked_pid{};
            CHECK(events(*mouse,controls,side,mappings,fixture_path.c_str(),registered,nullptr,&main,nullptr,&blocked_pid,
                         generation_for_test)==EventExit::stopped);
            CHECK(blocked_pid==0&&restore(*mouse,controls,side,true,main));
            CHECK(rejected_wheel==1&&rejected_keys==1&&confirmed_generation==18);
            CHECK(mode_writes==std::vector<unsigned>({3,2,3,2})&&observed_mappings==1);
            CHECK(std::ranges::count_if(captured,[](const INPUT& event){return event.type==INPUT_KEYBOARD;})==2);
        } else CHECK(run_agent(fixture_path.c_str(),false)==0);
        int total{};unsigned scroll_count{};
        for(const auto& input:captured)if(input.type==INPUT_MOUSE&&(input.mi.dwFlags&(MOUSEEVENTF_WHEEL|MOUSEEVENTF_HWHEEL))) {
            CHECK(input.mi.dwFlags==(thumb?MOUSEEVENTF_HWHEEL:MOUSEEVENTF_WHEEL));
            total+=static_cast<int>(input.mi.mouseData);++scroll_count;
        }
        if(scenario=="firefox-fine-stop") {
            const std::array<int,5> expected{120,120,-120,15,-15};
            std::array<int,5> received{};
            for(std::size_t i=0;i<captured.size();++i) {
                const auto& input=captured[i];
                CHECK(input.type==INPUT_MOUSE && input.mi.dwFlags==MOUSEEVENTF_WHEEL);
                const auto report=std::ranges::find_if(steps,[&](const Step& s) {
                    return s.main_wheel && s.at<=capture_times[i] && capture_times[i]-s.at<=120;
                });
                CHECK(report!=steps.end());
                const auto index=static_cast<std::size_t>(report-steps.begin());
                int units=static_cast<int>(input.mi.mouseData);
                CHECK((units>0)==(expected[index]>0));
                received[index]+=units;
            }
            CHECK(received==expected && connects==1 && observed_ready==1);
        }
        else if(scenario=="default"||scenario=="nonbrowser"||scenario=="input-recovery-direct"||scenario=="malformed"||native_only||scenario=="input-preempt"||
           scenario=="input-stop-pending"||scenario=="input-readback")CHECK(scroll_count==0);
        else if(clash_scope) {
            CHECK(total==240 && scroll_count>4 && mode_writes==std::vector<unsigned>({3,2,3,2}));
            for(const auto at:capture_times)CHECK((at>=350&&at<500)||(at>=900&&at<1200));
        }
        else if(scenario=="thumb-volume"){CHECK(captured.empty()&&thumb_volume_steps==1&&fallback_huds==1);}
        else if(scenario=="thumb-volume-media"){
            CHECK(scroll_count==0&&thumb_volume_steps==0&&fallback_huds==0&&captured.size()==2);
            CHECK(captured[0].ki.wVk==VK_VOLUME_UP&&captured[0].ki.dwFlags==KEYEVENTF_EXTENDEDKEY);
            CHECK(captured[1].ki.wVk==VK_VOLUME_UP&&captured[1].ki.dwFlags==(KEYEVENTF_EXTENDEDKEY|KEYEVENTF_KEYUP));
        }
        else if(scenario=="focus"||scenario=="pid"||scenario=="stop"||scenario=="reload")CHECK(total==72&&scroll_count==1);
        else if(scenario=="disconnect"||scenario=="loss"||scenario=="loss-no-neutral"||scenario=="main-only-loss")CHECK(total==192&&scroll_count>2);
        else if(scenario=="invert"||scenario=="negative")CHECK(total==-120&&scroll_count>2);
        else if(scenario=="packet-resolution")CHECK(total==240&&scroll_count>2);
        else {CHECK(total==120);if(scenario=="thumb-default")CHECK(scroll_count==1);else CHECK(scroll_count>2);}
        if(scenario=="default"||scenario=="nonbrowser"||thumb)CHECK(mode_writes.empty());
        else {
            CHECK(!mode_writes.empty()&&!(mode_writes.back()&1));CHECK(mode_reads>=3);
            CHECK(mode_value==(scenario=="invert"?6u:2u));
        }
        if(scenario=="browser"||scenario=="thumb") {
            CHECK(capture_times.front()==110);for(std::size_t i=1;i<capture_times.size();++i)CHECK(capture_times[i]-capture_times[i-1]>=10);
        }
        if(scenario=="disconnect")CHECK(connects==2&&std::ranges::count(mode_writes,2u)==1);
        if(native_only||scenario=="input-recovery"||scenario=="input-recovery-direct") {
            CHECK(rejected_wheel==1&&connects==1&&std::ranges::count(mode_writes,2u)==(native_only||scenario=="input-recovery-direct"?1:2)&&control_restores==1);
            CHECK(observed_ready==1&&observed_mappings==1);
            if(native_only)CHECK(mode_writes==std::vector<unsigned>({3,2}));
            else {
                unsigned key_count{};
                for(std::size_t i=0;i<captured.size();++i) {
                    CHECK(capture_times[i]>=250);
                    if(captured[i].type==INPUT_KEYBOARD){CHECK(captured[i].ki.wVk=='A');++key_count;}
                }
                CHECK(key_count==2);
            }
        }
        if(scenario=="reload")CHECK(connects==1&&std::ranges::count(mode_writes,3u)==1);
        if(scenario.starts_with("input-")) {
            const auto expected=config(true);
            std::ifstream file(fixture_path,std::ios::binary);
            std::string line,actual;
            while(std::getline(file,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();actual+=line+"\n";}
            CHECK(actual==expected&&connects==1);
        }
        if(scenario=="input-preempt"||scenario=="input-stop-pending"||scenario=="input-readback"||scenario=="input-reload") {
            CHECK(rejected_wheel==1&&observed_ready==1&&observed_mappings==1);
            CHECK(std::ranges::count_if(captured,[](const INPUT& input){return input.type==INPUT_KEYBOARD&&input.ki.wVk=='A';})==2);
            if(scenario=="input-preempt")CHECK(async_cancels>0&&native_readbacks>0);
            if(scenario=="input-reload")CHECK(std::ranges::count(mode_writes,3u)==2);
        }
        if(scenario=="reload-enable")CHECK(connects==1&&std::ranges::count(mode_writes,3u)==1&&capture_times.front()==160);
        if(scenario=="health-repair")CHECK(std::ranges::count(mode_writes,3u)==2&&async_begins>0&&capture_times.front()==3310);
        std::cout<<" units="<<total<<" outputs="<<scroll_count<<" writes="<<mode_writes.size();
    }
    std::cout<<" PASS production smooth "<<scenario<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
