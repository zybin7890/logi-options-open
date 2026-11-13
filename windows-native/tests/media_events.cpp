#include "environment.hpp"

static const std::string media_config=
    "[global]\n0052=GestureMedia\n0052.left=PreviousTrack\n0052.right=NextTrack\n"
    "0052.up=VolumeUp\n0052.down=VolumeDown\n";
static Step press(){Step s;s.down=true;return s;}
static Step move(int x,int y=0,ULONGLONG at=0){Step s;s.motion=true;s.x=x;s.y=y;s.at=at;return s;}
static Step release(){return {};}
static std::vector<WORD> keyboard(bool up){
    std::vector<WORD> keys;
    for(const auto& input:captured)if(input.type==INPUT_KEYBOARD&&bool(input.ki.dwFlags&KEYEVENTF_KEYUP)==up)keys.push_back(input.ki.wVk);
    return keys;
}
static bool parses(const std::string& text){std::istringstream input(text);Mappings mappings;return parse_mappings(input,mappings);}

int main(int argc,char** argv){try{
    CHECK(argc==3);const std::string scenario=argv[1];fixture_path=argv[2];
    restore_ok=true;mode="continuous-media";fixture_has_xy=true;
    if(scenario=="parser") {
        CHECK(parses(media_config));
        CHECK(!parses("[global]\n0052=GestureMedia\n"));
        CHECK(!parses(media_config+"[hotkeys]\nCtrl+Alt+F10=GestureMedia\n"));
        CHECK(!parses(media_config+"[ring]\nMedia=GestureMedia\n"));
        CHECK(!parses(media_config+"[ring/Media]\nItem=GestureMedia\n"));
        CHECK(!parses("[global]\n0052=GestureMedia;A\n"));
        CHECK(!parses("[global]\n0052=A\n0052.left=GestureMedia\n"));
        CHECK(!parses("[global]\nthumb.left=GestureMedia\nthumb.right=NextTrack\n"));
        CHECK(!parses(media_config+"[a.exe]\n0052.right=A\n"));
        CHECK(parses(media_config+"[a.exe]\n0052=PlayPause\n0052.right=A\n"));
        CHECK(parses(media_config+"[a.exe]\n0052.right=NextTrack\n"));
        CHECK(!parses(media_config+"[a.exe]\n0052.right=NextTrack\n[C:\\Apps\\a.exe]\n0052.up=A\n"));
        std::istringstream input(media_config);Mappings mappings;CHECK(parse_mappings(input,mappings));
        CHECK(needs_raw_motion(mappings.bindings.front()));
        std::cout<<"PASS parser accepts supported complete preset; rejects invalid contexts/inheritance/macros\n";return 0;
    }
    std::string config=media_config;
    steps={press(),move(0),move(83),move(84),release()};
    std::vector<WORD> expected{VK_MEDIA_NEXT_TRACK};
    if(scenario=="tap"){steps={press(),release()};expected={VK_MEDIA_PLAY_PAUSE};}
    else if(scenario=="first-packet"){steps={press(),move(30000),release()};expected={VK_MEDIA_PLAY_PAUSE};}
    else if(scenario=="below-start"){steps={press(),move(0),move(82),release()};expected={VK_MEDIA_PLAY_PAUSE};}
    else if(scenario=="start-equal"){steps={press(),move(0),move(83),release()};expected={};}
    else if(scenario=="action-equal"){steps={press(),move(0),move(83),move(83),release()};expected={};}
    else if(scenario=="action-above"){
        before_step=[](unsigned i){if(i==4)CHECK(captured.empty());if(i==5)CHECK(captured.size()==2);};
    }
    else if(scenario=="left"){steps={press(),move(0),move(-83),move(-84),release()};expected={VK_MEDIA_PREV_TRACK};}
    else if(scenario=="up"){steps={press(),move(0),move(0,-83),move(0,-84),release()};expected={VK_VOLUME_UP};}
    else if(scenario=="down"){steps={press(),move(0),move(0,83),move(0,84),release()};expected={VK_VOLUME_DOWN};}
    else if(scenario=="negative-equal"){steps={press(),move(0),move(-83),move(-83),release()};expected={};}
    else if(scenario=="tie"){steps={press(),move(0),move(83,83),release()};expected={VK_MEDIA_PLAY_PAUSE};}
    else if(scenario=="accumulate"){steps={press(),move(0),move(40),move(43),move(40),move(44),release()};}
    else if(scenario=="reverse"){steps={press(),move(0),move(83),move(-84),release()};expected={VK_MEDIA_PREV_TRACK};}
    else if(scenario=="large-packet"){steps={press(),move(0),move(30000),move(30000),release()};}
    else if(scenario=="restart"){
        steps={press(),move(0),move(83),move(84),move(84,0,500),release()};
        before_step=[](unsigned i){if(i==6)CHECK(captured.size()==2);};
    }
    else if(scenario=="duration-boundary"){
        steps={press(),move(0),move(83),move(84),move(30000,0,499),move(84,0,500),move(84,0,500),release()};
        expected={VK_MEDIA_NEXT_TRACK,VK_MEDIA_NEXT_TRACK};
        before_step=[](unsigned i){if(i==7)CHECK(captured.size()==2);};
    }
    else if(scenario=="duration-after"){
        steps={press(),move(0),move(83),move(84),move(-84,0,501),move(-84,0,501),release()};
        expected={VK_MEDIA_NEXT_TRACK,VK_MEDIA_PREV_TRACK};
    }
    else if(scenario=="duration-vertical"){
        steps={press(),move(0),move(83),move(84),move(0,30000,499),move(0,84,500),move(0,84,500),release()};
        expected={VK_MEDIA_NEXT_TRACK,VK_VOLUME_DOWN};
    }
    else if(scenario=="vertical-repeat"){
        steps={press(),move(0),move(0,83),move(0,84),move(0,84),move(0,84),release()};
        expected={VK_VOLUME_DOWN,VK_VOLUME_DOWN};
    }
    else if(scenario=="idle"){
        Step held=press();held.at=1500;steps={press(),move(0),move(83),move(84),held,move(0,0,1600),release()};
    }
    else if(scenario=="focus-before"){
        auto other=move(84);other.app=2;steps={press(),move(0),move(83),other,release()};expected={};
    }
    else if(scenario=="focus-after"){
        auto other=move(30000,0,500);other.app=2;auto back=move(30000,0,900);back.app=1;
        steps={press(),move(0),move(83),move(84),other,back,release(),press(),release()};
        expected={VK_MEDIA_NEXT_TRACK,VK_MEDIA_PLAY_PAUSE};
    }
    else if(scenario=="loss"){
        Step loss;loss.loss=true;
        steps={press(),move(0),move(83),move(84),loss,release(),release(),press(),release()};
        expected={VK_MEDIA_NEXT_TRACK,VK_MEDIA_PLAY_PAUSE};
    }
    else if(scenario=="stop"){
        Step stop;stop.stop=true;steps={press(),move(0),move(83),move(84),stop};
    }
    else if(scenario=="reconnect"){
        Step failure;failure.disconnect=true;
        steps={press(),move(0),move(83),move(84),failure,release(),press(),release()};
        expected={VK_MEDIA_NEXT_TRACK,VK_MEDIA_PLAY_PAUSE};
    }
    else if(scenario=="reload"){
        steps={press(),move(0),move(83),move(84),press(),release(),press(),release()};
        expected={VK_MEDIA_NEXT_TRACK,'N'};
        before_step=[](unsigned i){if(i==5){std::ofstream(fixture_path)<<"[global]\n0052=N\n";reload_signal=true;}};
    }
    else if(scenario=="dpi"){
        mode="owner-gesture";steps={press(),move(0),move(83),move(84),release(),release(),press(),move(0),move(416),move(417),release()};
    }
    else if(scenario=="partial"){
        unsigned calls{};input_result=[calls](UINT n,LPINPUT)mutable{return ++calls==1?1u:n;};
    }
    else if(scenario=="rejected"){
        input_result=[](UINT,LPINPUT){return 0u;};expected={};
    }
    else if(scenario=="physical-key"){
        physical_keys[VK_MEDIA_NEXT_TRACK]=true;expected={};
    }
    else throw std::runtime_error("unknown scenario");
    std::ofstream(fixture_path)<<config;
    CHECK(run_agent(fixture_path.c_str(),false)==0);
    const auto downs=keyboard(false),ups=keyboard(true);
    if(downs!=expected){std::cerr<<"down keys:";for(auto key:downs)std::cerr<<' '<<key;std::cerr<<'\n';}
    CHECK(downs==expected&&ups==expected);
    CHECK(captured.size()==expected.size()*2);
    CHECK(connects==(scenario=="reconnect"?2u:1u));
    if(scenario=="duration-boundary"||scenario=="duration-vertical")CHECK(capture_times[2]==500);
    if(scenario=="duration-after")CHECK(capture_times[2]==501);
    if(scenario=="physical-key")CHECK(physical_keys[VK_MEDIA_NEXT_TRACK]);
    std::cout<<"PASS continuous_media "<<scenario<<" accepted_inputs="<<captured.size()<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<argv[1]<<": "<<error.what()<<'\n';return 1;}}
