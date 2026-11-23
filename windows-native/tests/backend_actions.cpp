#define LOGI_CAPTURE_SYSTEM_ACTIONS
#define LOGI_CAPTURE_SCREEN_ACTIONS
#include "environment.hpp"
static std::vector<system_actions::Action> system_calls;
static bool system_accepted=true;
bool system_actions::perform(Action action,const ActionContext&){system_calls.push_back(action);return system_accepted;}
static std::vector<std::pair<screen_capture::Area,screen_capture::Destination>> captures;
static bool capture_accepted=true;
bool screen_capture::capture(Area area,Destination destination,const ActionContext&){captures.emplace_back(area,destination);return capture_accepted;}

static Action action(std::wstring_view text){Action result;CHECK(parse_action(text,result));return result;}
static void clicks() {
    for(const auto [name,count,down,up]:{
        std::tuple{L"Single",1u,DWORD(MOUSEEVENTF_LEFTDOWN),DWORD(MOUSEEVENTF_LEFTUP)},
        std::tuple{L"Double",2u,DWORD(MOUSEEVENTF_LEFTDOWN),DWORD(MOUSEEVENTF_LEFTUP)},
        std::tuple{L"Triple",3u,DWORD(MOUSEEVENTF_LEFTDOWN),DWORD(MOUSEEVENTF_LEFTUP)},
        std::tuple{L"Right",1u,DWORD(MOUSEEVENTF_RIGHTDOWN),DWORD(MOUSEEVENTF_RIGHTUP)},
        std::tuple{L"Middle",1u,DWORD(MOUSEEVENTF_MIDDLEDOWN),DWORD(MOUSEEVENTF_MIDDLEUP)}}) {
        for(unsigned mask=0;mask<16;++mask) {
            captured.clear();auto value=action(L"AdvancedClick("+std::wstring(name)+L","+std::to_wstring(mask)+L")");
            CHECK(execute(value,nullptr));
            constexpr WORD modifiers[]{VK_LSHIFT,VK_LCONTROL,VK_LMENU,VK_LWIN};
            std::vector<WORD> held;for(unsigned n=0;n<4;++n)if(mask&(1u<<n))held.push_back(modifiers[n]);
            CHECK(captured.size()==held.size()*2+count*2);
            for(unsigned n=0;n<held.size();++n){CHECK(captured[n].type==INPUT_KEYBOARD&&captured[n].ki.wVk==held[n]);CHECK(!(captured[n].ki.dwFlags&KEYEVENTF_KEYUP));}
            for(unsigned n=0;n<count;++n){CHECK(captured[held.size()+n*2].mi.dwFlags==down);CHECK(captured[held.size()+n*2+1].mi.dwFlags==up);}
            for(unsigned n=0;n<held.size();++n){const auto& e=captured[captured.size()-1-n];CHECK(e.ki.wVk==held[n]&&(e.ki.dwFlags&KEYEVENTF_KEYUP));}
        }
    }
    captured.clear();physical_keys[VK_LCONTROL]=true;
    CHECK(execute(action(L"AdvancedClick(Double,2)"),nullptr));CHECK(captured.size()==4);
    physical_keys[VK_LBUTTON]=true;captured.clear();CHECK(execute(action(L"AdvancedClick(Single,15)"),nullptr));CHECK(captured.empty());
    physical_keys.fill(false);
    // Each accepted prefix is cleaned up, including a left or right down.
    for(const auto mode:{L"Triple",L"Right",L"Middle"})for(unsigned prefix=0;prefix<10;++prefix) {
        captured.clear();unsigned calls{};
        input_result=[&](UINT size,LPINPUT){return calls++?size:std::min(size-1,prefix);};
        CHECK(!execute(action(L"AdvancedClick("+std::wstring(mode)+L",15)"),nullptr));
        std::array<int,256> keys{};int left{},right{},middle{};
        for(const auto& input:captured) {
            if(input.type==INPUT_KEYBOARD)keys[input.ki.wVk]+=(input.ki.dwFlags&KEYEVENTF_KEYUP)?-1:1;
            if(input.mi.dwFlags==MOUSEEVENTF_LEFTDOWN&&input.type==INPUT_MOUSE)++left;
            if(input.mi.dwFlags==MOUSEEVENTF_LEFTUP&&input.type==INPUT_MOUSE)--left;
            if(input.mi.dwFlags==MOUSEEVENTF_RIGHTDOWN&&input.type==INPUT_MOUSE)++right;
            if(input.mi.dwFlags==MOUSEEVENTF_RIGHTUP&&input.type==INPUT_MOUSE)--right;
            if(input.mi.dwFlags==MOUSEEVENTF_MIDDLEDOWN&&input.type==INPUT_MOUSE)++middle;
            if(input.mi.dwFlags==MOUSEEVENTF_MIDDLEUP&&input.type==INPUT_MOUSE)--middle;
        }
        CHECK(std::ranges::all_of(keys,[](int n){return n==0;})&&!left&&!right&&!middle);
    }
    input_result={};
    for(const auto invalid:{L"AdvancedClick()",L"AdvancedClick(Double)",L"AdvancedClick(Unknown,0)",L"AdvancedClick(Single,16)",L"AdvancedClick(Single,-1)",L"AdvancedClick(Single,1,2)",L"DpiToggle(0,5000)",L"DpiToggle(1000,9000)",L"DpiToggle(1000,)",L"DpiToggle(-1000,5000)"}) {
        Action value;CHECK(!parse_action(invalid,value));
    }
}
static void dpi() {
    logi::Mouse mouse;mouse.channel=std::make_unique<logi::Channel>(logi::Interface{});mouse.dpi_choices={1000,5000};
    unsigned current=1000,writes{},saves{},before{},after{};bool fail_save=false,fail_read=false,fail_write=false;
    agent_mapping=L"isolated.ini";
    get_override=[&](logi::Mouse&,std::wstring_view key)->std::optional<unsigned>{CHECK(key==L"dpi");return fail_read?std::nullopt:std::optional(current);};
    set_override=[&](logi::Mouse& m,std::wstring_view key,unsigned value){CHECK(!m.channel->control_pending()&&key==L"dpi"&&!dpi_restored);++writes;if(!fail_write)current=value;return !fail_write;};
    remember_dpi_override=[&](const std::filesystem::path& path,const logi::Mouse&,unsigned value){CHECK(path==L"isolated.ini"&&value==current);++saves;return !fail_save;};
    const auto toggle=action(L"DpiToggle(1000,5000)");
    CHECK(execute(toggle,&mouse)&&current==5000&&desired_dpi==5000&&desired_dpi_saved&&dpi_restored);
    CHECK(execute(toggle,&mouse)&&current==1000&&writes==2&&saves==2);
    CHECK(!execute(action(L"DpiToggle(1200,5000)"),&mouse)&&writes==2);
    CHECK(!execute(toggle,nullptr)&&writes==2);
    fail_read=true;CHECK(!execute(toggle,&mouse)&&writes==2);fail_read=false;
    fail_write=true;CHECK(!execute(toggle,&mouse)&&current==1000&&writes==3&&saves==2&&!dpi_restored);fail_write=false;
    fail_save=true;CHECK(!execute(toggle,&mouse)&&current==5000&&desired_dpi==5000&&!desired_dpi_saved&&!dpi_restored);fail_save=false;
    std::optional<unsigned> observed;
    CHECK(restore_owner_dpi(agent_mapping,mouse,observed)&&desired_dpi_saved&&observed==current&&current==5000);
    Mappings mappings;Binding binding{{},0x52,Gesture::tap,{toggle}};Sequences sequences{{},mappings};
    sequences.before_dpi=[&]{++before;};sequences.after_dpi=[&](std::optional<unsigned> value){++after;CHECK(value==current);};
    CHECK(Sequences::needs_mouse(Sequences::Run{&binding}));
    // Active async control must be cancelled by the event owner before a DPI run.
    mode="async-repair-thumb";mouse.channel->begin_control(1,6,1,{});sequences.start(binding,mouse);
    CHECK(sequences.runs.size()==1&&before==0);mouse.channel->cancel_control();sequences.service(mouse);
    CHECK(sequences.runs.empty()&&before==1&&after==1&&current==1000);
    get_override={};set_override={};remember_dpi_override={};
}
static void systems() {
    for(const auto area:{L"Screen",L"Window"})for(const auto destination:{L"File",L"Clipboard"}) {
        const auto value=action(L"ScreenCapture("+std::wstring(area)+L","+destination+L")");
        CHECK(execute(value,nullptr)&&captures.size()>0);
        CHECK(captures.back().first==(std::wstring_view(area)==L"Screen"?screen_capture::Area::screen:screen_capture::Area::window));
        CHECK(captures.back().second==(std::wstring_view(destination)==L"File"?screen_capture::Destination::file:screen_capture::Destination::clipboard));
        capture_accepted=false;CHECK(!execute(value,nullptr));capture_accepted=true;
    }
    for(const auto text:{L"ScreenCapture(Region,File)",L"ScreenCapture(Screen,Unknown)",L"ScreenCapture(Screen)",L"ScreenCapture(Screen,File,extra)"}){Action value;CHECK(!parse_action(text,value));}
    // Validate the complete UI catalogue without executing its destructive or
    // application-launch actions. The separate API capture tests cover effects.
    std::vector<Action> catalogue;
    CHECK(parse_actions(L"Lock;Win+Shift+S;PrintScreen;Win+Tab;Win+D;Win+E;Win;VolumeDown;VolumeUp;Mute;PlayPause;NextTrack;PreviousTrack;Alt+Tab;Ctrl+C;Ctrl+V;Win+A;Ctrl+Z;ThisPC;Ctrl+P;CloseWindow;Ctrl+W;Alt+Left;Open(calc.exe);Ctrl+X;Alt+Right;Win+PrintScreen;Win+I;Win+Space;F5;Win+S;Ctrl+T;Apps;Ctrl+Y;BrowserHome;Win+Ctrl+Right;Win+Ctrl+Left;Maximize;Minimize;Delete;End;Home;PageDown;PageUp;BrightnessUp;BrightnessDown;MicrophoneMute;Shutdown;Sleep;HighContrast;Mail;Media;Hanja;MediaStop",catalogue));
    CHECK(catalogue.size()==54);
    for(const auto text:{L"Win+H",L"Win+Shift+F23",L"RightCtrl",L"Insert"})CHECK(parse_action(text,catalogue.emplace_back()));
    for(const auto code:{L"BrightnessUp",L"BrightnessDown",L"MicrophoneMute",L"Shutdown",L"Sleep",L"HighContrast",L"CloseWindow",L"ThisPC"}) {
        auto value=action(code);CHECK(value.operation==Operation::system);const auto prior=system_calls.size();
        CHECK(execute(value,nullptr)&&system_calls.size()==prior+1);system_accepted=false;CHECK(!execute(value,nullptr));system_accepted=true;
    }
    for(const auto [name,code]:{std::pair{L"Apps",VK_APPS},std::pair{L"Hanja",VK_HANJA},std::pair{L"Mail",VK_LAUNCH_MAIL},std::pair{L"Media",VK_LAUNCH_MEDIA_SELECT},std::pair{L"BrowserHome",VK_BROWSER_HOME}}) {
        captured.clear();CHECK(execute(action(name),nullptr));CHECK(captured.size()==2&&captured[0].ki.wVk==code&&(captured[1].ki.dwFlags&KEYEVENTF_KEYUP));
        if(code!=VK_HANJA)CHECK(captured[0].ki.dwFlags&KEYEVENTF_EXTENDEDKEY);
    }
}
int main(){try{clicks();dpi();systems();std::puts("Production advanced-click (80 modes, 30 short prefixes), device DPI ownership/persistence and system dispatch passed.");return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
