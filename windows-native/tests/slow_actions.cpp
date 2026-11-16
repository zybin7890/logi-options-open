#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <reason.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <thread>
#undef interface

BOOL WINAPI slow_post(HWND,UINT,WPARAM,LPARAM);
#define PostMessageW slow_post
#define LOGI_CAPTURE_SYSTEM_ACTIONS
#define LOGI_CAPTURE_SCREEN_ACTIONS
#include "environment.hpp"

static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool entered{},allowed{},readable{true};
static std::atomic<unsigned> brightness_writes{},monitor_cleanup{},capture_writes{},posts{};
static HWND closed{};
static bool close_changes_focus{};
static bool wait_gate() {
    std::unique_lock lock(gate_mutex);entered=true;gate_cv.notify_all();
    CHECK(gate_cv.wait_for(lock,std::chrono::seconds(3),[]{return allowed;}));return readable;
}
static void await_entered() {
    std::unique_lock lock(gate_mutex);CHECK(gate_cv.wait_for(lock,std::chrono::seconds(3),[]{return entered;}));
}
static void unblock(){std::lock_guard lock(gate_mutex);allowed=true;gate_cv.notify_all();}
static void await_task(const std::shared_ptr<slow_actions::Task>& task) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(task->status.load()==slow_actions::Status::pending) {
        CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void drain(){auto barrier=slow_actions::submit({},[](const ActionContext&){return true;});await_task(barrier);CHECK(barrier->status.load()==slow_actions::Status::accepted);}
HRESULT WINAPI slow_initialize(LPVOID,DWORD){return S_OK;}
void WINAPI slow_uninitialize(){}
HRESULT WINAPI slow_create(REFCLSID clsid,LPUNKNOWN,DWORD,REFIID,LPVOID*){CHECK(clsid==CLSID_WbemLocator);return E_NOINTERFACE;}
BOOL WINAPI slow_token(HANDLE,DWORD,PHANDLE){throw std::runtime_error("unexpected power action");}
BOOL WINAPI slow_parameters(UINT,UINT,PVOID,UINT){throw std::runtime_error("unexpected contrast action");}
BOOL WINAPI slow_post(HWND window,UINT message,WPARAM wp,LPARAM lp){CHECK(message==WM_CLOSE&&!wp&&!lp);closed=window;++posts;if(close_changes_focus)foreground_app=2;return TRUE;}
HMONITOR WINAPI slow_monitor(HWND window,DWORD flags){CHECK(window==reinterpret_cast<HWND>(1)&&flags==MONITOR_DEFAULTTOPRIMARY);return reinterpret_cast<HMONITOR>(4);}
BOOL WINAPI slow_count(HMONITOR,LPDWORD count){*count=1;return TRUE;}
BOOL WINAPI slow_physical(HMONITOR,DWORD count,LPPHYSICAL_MONITOR output){CHECK(count==1);output->hPhysicalMonitor=reinterpret_cast<HANDLE>(5);return TRUE;}
BOOL WINAPI slow_brightness(HANDLE,LPDWORD minimum,LPDWORD current,LPDWORD maximum){*minimum=0;*current=50;*maximum=100;return wait_gate();}
BOOL WINAPI slow_set_brightness(HANDLE,DWORD value){CHECK(value==60);++brightness_writes;return TRUE;}
BOOL WINAPI slow_destroy(DWORD,LPPHYSICAL_MONITOR){++monitor_cleanup;return TRUE;}
#define CoInitializeEx slow_initialize
#define CoUninitialize slow_uninitialize
#define CoCreateInstance slow_create
#define OpenProcessToken slow_token
#define SystemParametersInfoW slow_parameters
#define MonitorFromWindow slow_monitor
#define GetNumberOfPhysicalMonitorsFromHMONITOR slow_count
#define GetPhysicalMonitorsFromHMONITOR slow_physical
#define GetMonitorBrightness slow_brightness
#define SetMonitorBrightness slow_set_brightness
#define DestroyPhysicalMonitors slow_destroy
#include "../system_actions.cpp"
bool screen_capture::capture(Area,Destination,const ActionContext& context) {
    if(!wait_gate()||!context.active())return false;++capture_writes;return true;
}
static Binding binding(std::wstring_view code){Binding result;CHECK(parse_actions(code,result.actions));return result;}
static void events_case(const std::string& scenario) {
    restore_ok=true;mode="async-repair-thumb";
    std::ofstream(fixture_path)<<"[global]\n0052=BrightnessUp;B\nthumb.right=C\nthumb.left=D\n";
    Step press;press.down=true;Step thumb;thumb.thumb=true;Step stop;stop.stop=true;
    steps={press,thumb,{},stop};
    const bool success=scenario=="event-complete";
    if(scenario=="event-stop")steps[2].stop=true;
    else if(scenario=="event-disconnect")steps[2].disconnect=true;
    else if(scenario=="event-loss")steps[2].loss=true;
    else if(scenario=="event-focus")steps[2].app=2;
    else CHECK(success||scenario=="event-reload");
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    before_step=[&](unsigned index) {
        if(index==2)await_entered();
        if(index==3) {
            CHECK(captured.size()==2&&captured[0].ki.wVk=='C');
            CHECK(brightness_writes==0); // Genuine production DDC call is still blocked.
            if(success)unblock();
            if(scenario=="event-reload") {std::ofstream(fixture_path)<<"[global]\n0052=None\n";reload_signal=true;}
        }
        if(success&&index>=3) {
            CHECK(std::chrono::steady_clock::now()<end);
            if(captured.size()<4) {Step next;next.at=tick+10;steps.back()=next;steps.push_back(stop);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        }
    };
    CHECK(run_agent(fixture_path.c_str(),false)==0);
    if(!success)unblock();drain();
    CHECK(monitor_cleanup==1);
    CHECK(brightness_writes==(success?1u:0u));
    CHECK(captured.size()==(success?4u:2u));
    if(success)CHECK(captured[2].ki.wVk=='B');
}
static void sequence_case(const std::string& scenario) {
    Mappings mappings;logi::Mouse mouse;Sequences sequences{{},mappings};
    const bool capture=scenario=="capture-cancel";
    auto first=binding(capture?L"ScreenCapture(Screen,File);B":L"BrightnessUp;B");
    if(scenario=="close-success") {
        first=binding(L"CloseWindow;B");close_changes_focus=true;
        sequences.start(first,mouse);auto task=sequences.runs[0].pending;await_task(task);
        sequences.service(mouse);CHECK(sequences.runs.empty()&&!sequences.cancelled&&!sequences.failed);
        CHECK(posts==1&&closed==reinterpret_cast<HWND>(1)&&captured.size()==2&&captured[0].ki.wVk=='B');return;
    }
    sequences.start(first,mouse);auto task=sequences.runs[0].pending;await_entered();
    if(scenario=="failure")readable=false;
    else if(scenario=="queued-close"||scenario=="queued-pid") {
        auto second=binding(L"CloseWindow;C");sequences.start(second,mouse);auto queued=sequences.runs[1].pending;
        if(scenario=="queued-pid")foreground_pid_override=2;else foreground_app=2;
        sequences.service(mouse);CHECK(sequences.runs.empty()&&sequences.cancelled);unblock();await_task(task);await_task(queued);
        CHECK(posts==0&&brightness_writes==0&&captured.empty()&&queued->status.load()==slow_actions::Status::cancelled);return;
    } else {sequences.cancel();CHECK(sequences.runs.empty());}
    unblock();await_task(task);tick+=20;sequences.service(mouse);
    CHECK(captured.empty()&&brightness_writes==0&&capture_writes==0);
    if(scenario=="failure")CHECK(sequences.failed&&!sequences.cancelled);
    else CHECK(sequences.cancelled&&task->status.load()==slow_actions::Status::cancelled);
}
int main(int argc,char** argv) {try {
    CHECK(argc==3);fixture_path=argv[2];const std::string scenario=argv[1];
    if(scenario.starts_with("event-"))events_case(scenario);else sequence_case(scenario);
    drain();std::cout<<"PASS production async system/capture lifecycle "<<scenario<<'\n';return 0;
}catch(const std::exception& e){unblock();std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
