// Isolated environment derived from tests/dpi-ready.cpp, SHA256 fb1e7ff871036d056999b84786335a0026951ce6722663ba4554de0c746119a2.
// Production remap.cpp is included below. Only OS/HID fixture code is copied.
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <map>
#include <functional>
#include <array>
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
static bool stopped,restore_ok;
static unsigned foreground_app=1;
static std::function<void(unsigned)> before_step;
static std::function<unsigned(UINT,LPINPUT)> input_result;
static std::array<bool,256> physical_keys{};
static std::string mode;
static bool fixture_has_xy;
static bool fixture_xy(){return fixture_has_xy;}
static ULONGLONG tick=100;
static unsigned actual_dpi=1000;
static bool dpi_read_failed;
static std::filesystem::path fixture_path;
static std::vector<INPUT> captured;
static std::vector<ULONGLONG> capture_times;
static int observed_ready=-1;
static int observed_mappings=-1,observed_settings=-1;
static unsigned connects,restores,read_calls,requests;
static unsigned input_index;
static unsigned reporting_requests;
static unsigned injected_loss;
static unsigned dpi_writes{},dpi_saves{};
static unsigned async_begins{},async_cancels{},async_writes{};
static unsigned report_main=1,report_button{},report_thumb{};
static unsigned report_shift_mode=2;
static ULONGLONG async_due{};
static bool reload_signal{},cancel_delayed{};
static unsigned wheel_writes{},nonzero_cancel_waits{};
static unsigned sync_dpi_reads{};
static unsigned reload_faults{},reload_acks{},reload_hotkeys{};
static unsigned reload_window_count{};
static std::map<int,UINT> registered_hotkeys;
static std::string reload_bytes;
static bool reload_failed_ready{};
static std::filesystem::path saved_path;
static DWORD input_stamp=100;
static LONG first_pid{},last_pid{},observed_failures{},observed_connections{};
static LONG64 observed_checked{},idle_age{};
alignas(16) static unsigned char health_buffer[1024];
alignas(16) static unsigned char owner_buffer[4096];
alignas(16) static unsigned char reload_buffer[4096];
static LONG speed_buffer;
static unsigned created_events;
HANDLE WINAPI fake_mutex(LPSECURITY_ATTRIBUTES,BOOL,LPCWSTR){return reinterpret_cast<HANDLE>(11);}
DWORD WINAPI fake_error(){return 0;}
HANDLE WINAPI fake_snapshot(DWORD,DWORD){return reinterpret_cast<HANDLE>(12);}
BOOL WINAPI fake_processfirst(HANDLE,LPPROCESSENTRY32W){return FALSE;}
BOOL WINAPI fake_processnext(HANDLE,LPPROCESSENTRY32W){return FALSE;}
HANDLE WINAPI fake_event(LPSECURITY_ATTRIBUTES,BOOL,BOOL,LPCWSTR){return reinterpret_cast<HANDLE>(100+(++created_events));}
BOOL WINAPI fake_close(HANDLE){return TRUE;}
HANDLE WINAPI fake_mapping(HANDLE,LPSECURITY_ATTRIBUTES,DWORD,DWORD,DWORD,LPCWSTR name){if(wcsstr(name,L".Settings"))return nullptr;if(wcsstr(name,L".ReloadAck"))return mode.starts_with("reload-")?reinterpret_cast<HANDLE>(24):nullptr;if(wcsstr(name,L".Dpi"))return mode.starts_with("owner-")?reinterpret_cast<HANDLE>(23):nullptr;return reinterpret_cast<HANDLE>(wcsstr(name,L"Health")?21:22);}
LPVOID WINAPI fake_map(HANDLE handle,DWORD,DWORD,DWORD,SIZE_T){if(handle==reinterpret_cast<HANDLE>(24))return reload_buffer;if(handle==reinterpret_cast<HANDLE>(23))return owner_buffer;return handle==reinterpret_cast<HANDLE>(21)?static_cast<void*>(health_buffer):static_cast<void*>(&speed_buffer);}
BOOL WINAPI fake_event_set(HANDLE){return TRUE;}
BOOL WINAPI fake_event_reset(HANDLE h){if(h==reinterpret_cast<HANDLE>(102))reload_signal=false;return TRUE;}
BOOL WINAPI fake_unmap(LPCVOID){return TRUE;}
BOOL WINAPI fake_handler(PHANDLER_ROUTINE,BOOL){return TRUE;}
DWORD WINAPI fake_wait(HANDLE h,DWORD){if(h==reinterpret_cast<HANDLE>(11))return WAIT_OBJECT_0;if(h==reinterpret_cast<HANDLE>(102)&&reload_signal)return WAIT_OBJECT_0;return h==reinterpret_cast<HANDLE>(101)&&stopped?WAIT_OBJECT_0:WAIT_TIMEOUT;}
BOOL WINAPI fake_release_mutex(HANDLE){return TRUE;}
ULONGLONG WINAPI fake_clock(){return tick;}
BOOL WINAPI fake_lastinput(PLASTINPUTINFO input){input->dwTime=mode=="idle-resume"?input_stamp:static_cast<DWORD>(tick);return TRUE;}
BOOL WINAPI fake_peek(LPMSG,HWND,UINT,UINT,UINT){return FALSE;}
UINT WINAPI fake_input(UINT n,LPINPUT input,int){const auto accepted=input_result?input_result(n,input):n;CHECK(accepted<=n);captured.insert(captured.end(),input,input+accepted);for(unsigned i=0;i<accepted;++i)capture_times.push_back(tick);return accepted;}
HWND WINAPI fake_foreground(){return reinterpret_cast<HWND>(static_cast<uintptr_t>(foreground_app));}
static DWORD foreground_pid_override;
DWORD WINAPI fake_window_pid(HWND window,LPDWORD pid){*pid=foreground_pid_override?foreground_pid_override:static_cast<DWORD>(reinterpret_cast<uintptr_t>(window));return *pid;}
HANDLE WINAPI fake_open_process(DWORD,BOOL,DWORD pid){return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1000+pid));}
BOOL WINAPI fake_process_path(HANDLE handle,DWORD,LPWSTR path,PDWORD size){const auto pid=reinterpret_cast<uintptr_t>(handle)-1000;const wchar_t* name=pid==1?L"C:\\Apps\\a.exe":L"C:\\Apps\\b.exe";const auto n=std::wcslen(name);CHECK(*size>n);std::wcscpy(path,name);*size=static_cast<DWORD>(n);return TRUE;}
SHORT WINAPI fake_key(int key){return key>=0&&key<256&&physical_keys[key]?SHORT(0x8000):0;}
static bool allow_open,fail_open;
HINSTANCE WINAPI fake_shell(HWND,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,INT){CHECK(allow_open);if(fail_open)return reinterpret_cast<HINSTANCE>(31);foreground_app=2;return reinterpret_cast<HINSTANCE>(33);}
BOOL WINAPI fake_show(HWND,INT){throw std::runtime_error("unexpected window mutation");}
BOOL WINAPI fake_lock(){throw std::runtime_error("unexpected desktop lock");}
void WINAPI fake_sleep(DWORD duration){tick+=duration;}
BOOL WINAPI fake_terminate(HANDLE,UINT){throw std::runtime_error("unexpected process mutation");}
BOOL WINAPI fake_createprocess(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION){throw std::runtime_error("unexpected process spawn");}
BOOL WINAPI fake_register(HWND,int id,UINT,UINT key){++reload_hotkeys;if(mode=="reload-hotkey-fail"&&input_index>0&&!reload_faults){++reload_faults;return FALSE;}registered_hotkeys[id]=key;return TRUE;}
BOOL WINAPI fake_unregister(HWND,int id){registered_hotkeys.erase(id);return TRUE;}
#define CreateMutexW fake_mutex
#define GetLastError fake_error
#define CreateToolhelp32Snapshot fake_snapshot
#define Process32FirstW fake_processfirst
#define Process32NextW fake_processnext
#define CreateEventW fake_event
#define CloseHandle fake_close
#define CreateFileMappingW fake_mapping
#define SetEvent fake_event_set
#define ResetEvent fake_event_reset
#define MapViewOfFile fake_map
#define UnmapViewOfFile fake_unmap
#define SetConsoleCtrlHandler fake_handler
#define WaitForSingleObject fake_wait
#define ReleaseMutex fake_release_mutex
#define GetTickCount64 fake_clock
#define GetLastInputInfo fake_lastinput
#define PeekMessageW fake_peek
#define SendInput fake_input
#define GetForegroundWindow fake_foreground
#define GetWindowThreadProcessId fake_window_pid
#define OpenProcess fake_open_process
#define QueryFullProcessImageNameW fake_process_path
#define GetAsyncKeyState fake_key
#define TerminateProcess fake_terminate
#define ShellExecuteW fake_shell
#define ShowWindowAsync fake_show
#define LockWorkStation fake_lock
#define Sleep fake_sleep
#define CreateProcessW fake_createprocess
#define RegisterHotKey fake_register
#define UnregisterHotKey fake_unregister
#include "../remap.cpp"
static std::function<int()> ring_choice;
int choose_ring_action(std::span<const std::wstring>,HANDLE,bool){CHECK(ring_choice);return ring_choice();}
struct Step {bool down{};int x{};bool motion{},change{},loss{};ULONGLONG at{};bool thumb{};int y{};unsigned app{};bool disconnect{},stop{};};
static std::vector<Step> steps;
namespace logi {
Channel::Channel(const Interface&,HANDLE){}
Channel::~Channel(){}
void Channel::finish_shutdown(){control_active_=false;control_result_.reset();discard_notifications();pending_loss_=0;}
bool Channel::cancel_control(){
 if(control_active_&&mode=="async-wheel-cancel"&&!cancel_delayed){cancel_delayed=true;async_due=tick+20;return false;}
 if(control_active_&&cancel_delayed&&tick<async_due)return false;
 if(control_active_)++async_cancels;control_active_=false;control_result_.reset();return true;
}
Result Channel::begin_control(Byte slot,Byte feature,Byte function,std::initializer_list<Byte> params,DWORD timeout){
 CHECK(!control_active_);++async_begins;if(mode=="async-busy"&&async_begins==1)return Result::busy;control_active_=true;async_due=tick;
 control_result_=request(slot,feature,function,params,timeout);
 if(mode.starts_with("async-") && async_begins==1)async_due=tick+1000;
 if(mode=="owner-async-preempt"&&async_begins==1)async_due=tick+1000;
 if((mode=="owner-async-preempt"&&async_begins==2)||(mode=="owner-repair-preempt"&&async_begins==3))async_due=4000;
 if(mode.find("repair-")!=std::string::npos && !mode.starts_with("fsm-") && async_begins==1)async_due=tick;
 if(mode.find("repair-")!=std::string::npos && !mode.starts_with("fsm-") && async_begins==2)async_due=tick+1000;
 if(mode=="async-timeout"&&async_begins==1)control_result_=Reply{Result::timeout};
 return Result::ok;
}
std::optional<Reply> Channel::poll_control(){if(!control_active_||tick<async_due)return {};auto result=control_result_;control_result_.reset();control_active_=false;return result;}
Reply Channel::read(DWORD timeout,bool,HANDLE){
 if(mode=="async-busy"&&async_begins==1){CHECK(timeout>0&&timeout<=50);tick+=timeout;return {Result::timeout};}
 if(control_active_&&cancel_delayed&&tick<async_due){CHECK(timeout!=0);++nonzero_cancel_waits;tick=async_due;return {Result::control};}
 if(control_active_&&input_index<steps.size()&&steps[input_index].at>=async_due&&steps[input_index].at!=0){tick=async_due;return {Result::control};}
 if(control_active_ && (tick>=async_due || input_index>=steps.size())){tick=std::max(tick,async_due);return {Result::control};}
 ++read_calls;CHECK(health);observed_ready=health->ready;observed_mappings=health->mappings_ready;observed_settings=health->settings_ready;
 observed_checked=health->checked;observed_failures=health->failures;observed_connections=health->connections;last_pid=health->pid;
 if(mode=="reconnect" && read_calls==1){first_pid=health->pid;return {Result::io_error};}
 if(mode=="idle-resume") {
     if(read_calls==1){tick=3100;return {Result::timeout};}
     if(read_calls==2){tick=288257;return {Result::timeout};}
     if(read_calls==3){CHECK(reporting_requests==0 && observed_checked==100 && observed_ready==1 && health->events==0);
         idle_age=tick-observed_checked;input_stamp=288257;tick=291257;return {Result::timeout};}
 }
 if(queued_) {const auto i=(next_+notifications_.size()-queued_)%notifications_.size();--queued_;return {Result::ok,notifications_[i]};}
 if(!steps.empty() && input_index<steps.size()) {
     const auto s=steps[input_index++];
     if(s.app)foreground_app=s.app;
     if(before_step)before_step(input_index);
     if(s.disconnect)return {Result::io_error};
     if(s.stop){stopped=true;return {Result::stopped};}
     if(s.at)tick=std::max(tick,s.at);
     if(mode.starts_with("reload-")&&input_index==1) {
         auto& command=*reinterpret_cast<reload_ipc::Shared*>(reload_buffer);
         CHECK(command.running&&command.version==reload_ipc::protocol);
         command.request_id=55;command.request_instance=command.instance;command.revision=9;
         CHECK(mapping_fingerprint(reload_bytes,command.fingerprint));
         std::ofstream(fixture_path,std::ios::binary)<< (mode=="reload-superseded"?"[global]\n0052=C\n":reload_bytes);
         if(mode=="reload-read-fail")std::filesystem::remove(fixture_path);
         command.phase=reload_ipc::pending;reload_signal=true;
         if(mode=="reload-not-ready")mapping_readiness(false);
     }
     if(mode.starts_with("reload-")&&input_index==2) {
         const auto& command=*reinterpret_cast<reload_ipc::Shared*>(reload_buffer);
         CHECK(command.phase==reload_ipc::done);++reload_acks;
         CHECK(registered_hotkeys.size()==1&&registered_hotkeys[1]==(command.status==static_cast<LONG>(ReloadStatus::applied)?VK_F11:VK_F10));
     }
     if((mode=="async-reload"&&input_index==2)||(mode=="async-repair-reload"&&input_index==3)) {
         std::ofstream(fixture_path)<<"[global]\n0052=B\n";reload_signal=true;
     }
     if(mode.starts_with("owner-")&&input_index==(mode=="owner-macro"?2u:3u)) {
         auto& command=*reinterpret_cast<dpi_ipc::Shared*>(owner_buffer);
         command.request_id=42;command.request_instance=command.instance;command.requested=5000;command.phase=dpi_ipc::pending;
         if(mode=="owner-priority")tick=3100;
     }
     if(s.loss){++pending_loss_;++lost_notifications_;++injected_loss;return {Result::notification_loss};}
     if(s.change){actual_dpi=5000;auto file=fixture_path;file.replace_extension(L".dpi");
         std::ofstream(file)<<"MX Master 3S\n5000\n";
         std::filesystem::last_write_time(file,std::filesystem::file_time_type::clock::now()+std::chrono::seconds(read_calls));}
     if((mode=="gesture-health-deferred"&&read_calls==3)||(mode=="gesture-readback-recovery"&&read_calls==5))tick=3100;
     if(mode=="macro-loss"&&read_calls==3)tick=500;
     if(mode=="owner-macro"&&input_index==3)tick=500;
     if(mode=="gesture-health-bounded"&&input_index==2)tick=3100;
     if(mode=="gesture-health-bounded"&&input_index==3)tick=8100;
     if(mode=="gesture-watchdog-loss"&&input_index==2)tick=3100;
     if(mode=="gesture-watchdog-loss"&&input_index==3)tick=8100;
     Reply r{Result::ok};r.packet.size=20;r.packet.bytes[0]=0x11;r.packet.bytes[1]=1;r.packet.bytes[2]=s.thumb?6:7;r.packet.bytes[3]=s.motion?0x10:0;
     auto value=s.thumb?1u:s.motion?unsigned(s.x):(s.down?0x52u:0u);r.packet.bytes[4]=high(value);r.packet.bytes[5]=low(value);if(s.motion){r.packet.bytes[6]=high(unsigned(s.y));r.packet.bytes[7]=low(unsigned(s.y));}return r;
 }
 if((mode=="retry"||mode=="reporting")&&read_calls==1){tick=3100;return {Result::timeout};}
 stopped=true;return {Result::stopped};
}
Reply Channel::request(Byte,Byte feature,Byte function,std::initializer_list<Byte> parameters,DWORD){
 ++reporting_requests;
 if(feature==8){Reply r{Result::ok};r.packet.size=20;r.packet.bytes[4]=static_cast<Byte>(report_shift_mode);r.packet.bytes[5]=10;return r;}
 if(mode=="gesture-watchdog-loss" && read_calls==3){++pending_loss_;++lost_notifications_;++injected_loss;return {Result::notification_loss};}
 if(feature==6 && !mode.starts_with("fsm-")){Reply r{Result::ok};r.packet.size=20;r.packet.bytes[4]=1;return r;}
 if(feature==7 && mode.find("repair-")!=std::string::npos && !mode.starts_with("fsm-")) {
     Reply r{Result::ok};r.packet.size=20;r.packet.bytes[5]=0x52;
     if(function==3){report_button=1;++async_writes;}
     r.packet.bytes[6]=static_cast<Byte>(report_button);return r;
 }
 if(feature==9){Reply r{Result::ok};r.packet.size=20;
     if(function==3){actual_dpi=word(std::span(parameters.begin()+1,2));return r;}
     if(mode=="gesture-unknown"||mode=="unknown-side")return {Result::io_error};
     if(mode=="gesture-readback-recovery"&&read_calls==4&&!dpi_read_failed){dpi_read_failed=true;return {Result::io_error};}
     r.packet.bytes[5]=high(actual_dpi);r.packet.bytes[6]=low(actual_dpi);return r;}
 if(mode.starts_with("fsm-")) {
     Reply r{Result::ok};r.packet.size=20;auto& flags=feature==5?report_main:feature==6?report_thumb:report_button;
     if((feature==7&&function==3)||(feature!=7&&function==2)) {
         ++async_writes;if(mode!="fsm-bad-verify")flags=feature==5?0:feature==6?1:0x11;return r;
     }
     if(feature==7){r.packet.bytes[4]=0;r.packet.bytes[5]=0x52;r.packet.bytes[6]=static_cast<Byte>(flags);}
     else r.packet.bytes[4]=static_cast<Byte>(flags);return r;
 }
 CHECK(feature==7&&function==2&&parameters.size()==2);Reply r{Result::ok};r.packet.size=20;
 r.packet.bytes[4]=*parameters.begin();r.packet.bytes[5]=*(parameters.begin()+1);r.packet.bytes[6]=fixture_xy()?0x11:1;return r;
}
Result Channel::send(Byte,Byte,Byte,std::initializer_list<Byte>,DWORD) {
 CHECK(mode=="owner-window-released"||mode=="owner-window-held"||mode.starts_with("reload-window"));
 const auto push=[&](bool down,bool motion,int x){Packet p{};p.size=20;p.bytes[0]=0x11;p.bytes[1]=1;p.bytes[2]=7;p.bytes[3]=motion?0x10:0;
     unsigned value=motion?unsigned(x):(down?0x52u:0u);p.bytes[4]=high(value);p.bytes[5]=low(value);
     notifications_[next_]=p;next_=(next_+1)%notifications_.size();++queued_;if(mode.starts_with("reload-window"))++reload_window_count;};
 push(true,false,0);push(false,true,0);push(false,true,1000);
 if(mode=="owner-window-released"||mode=="reload-window-released")push(false,false,0);
 return Result::ok;
}
std::optional<Mouse> connect(HANDLE,bool){++connects;if(mode.starts_with("reload-")&&connects==2){CHECK(health&&health->mappings_ready==0);reload_failed_ready=true;report_button=0;}if(mode=="reconnect" && connects==2)CHECK(health && health->ready==0 && health->mappings_ready==0 && health->pid==first_pid);Mouse m{};m.device={1,2,0,"MX Master 3S"};m.buttons=7;m.dpi=9;if(mode=="async-wheel-cancel"||mode=="async-past-wheel")m.shift=8;if(mode=="async-repair-thumb")m.thumb=6;m.dpi_choices={1000,5000};m.channel=std::make_unique<Channel>(Interface{});return m;}
bool restore_standard_input(Mouse&){return true;}
bool restore_saved_dpi(const std::filesystem::path&,Mouse&){++restores;return restore_ok||(mode=="retry"&&restores>1)||(mode=="reconnect"&&connects>1);}
bool read_saved_settings(const std::filesystem::path&,const Mouse&,SavedSettings&){return true;}
bool remember_settings(const std::filesystem::path&,const Mouse&,const SavedSettings&){return true;}
std::optional<unsigned> get(Mouse& mouse,std::wstring_view key){
 if(key==L"ratchet"){CHECK(!mouse.channel->control_pending());return 1;}
 CHECK(key==L"dpi");
 ++sync_dpi_reads;if(mode=="async-sidecar")CHECK(sync_dpi_reads==1);
 if(mode=="gesture-unknown"||mode=="unknown-side")return {};
 if(mode=="gesture-readback-recovery"&&read_calls==4&&!dpi_read_failed){dpi_read_failed=true;return {};}
 return actual_dpi;
}
bool set(Mouse& mouse,std::wstring_view key,unsigned value){if(key==L"ratchet"){CHECK(!mouse.channel->control_pending());++wheel_writes;report_shift_mode=value?2:1;return true;}CHECK(mode.starts_with("owner-")&&key==L"dpi");CHECK(!health->settings_ready);if(mode=="owner-priority")CHECK(reporting_requests==0);if(mode=="owner-async-preempt"||mode=="owner-repair-preempt")CHECK(async_cancels==1&&!mouse.channel->control_pending());++dpi_writes;actual_dpi=value;if(mode.starts_with("owner-window"))mouse.channel->send(1,1,1);return true;}
bool remember_dpi(const std::filesystem::path& path,const Mouse&,unsigned value){CHECK(mode.starts_with("owner-")&&value==actual_dpi&&dpi_writes==1);saved_path=path;++dpi_saves;return true;}
Reply Mouse::request(Byte feature,Byte function,std::initializer_list<Byte> parameters){
 if(feature==8){Reply r{Result::ok};r.packet.size=20;r.packet.bytes[4]=static_cast<Byte>(report_shift_mode);r.packet.bytes[5]=10;return r;}
 if(mode.starts_with("reload-")&&feature==7) {
     CHECK(parameters.size()>=2);auto p=parameters.begin();Reply r{Result::ok};r.packet.size=20;r.packet.bytes[4]=p[0];r.packet.bytes[5]=p[1];
     if(function==3) {
         const bool restoring=p[2]==0x22;
         const bool injecting=input_index>0&&connects==1&&reload_faults==0;
         if(injecting&&((mode=="reload-restore-fail"&&restoring)||(mode=="reload-enable-fail"&&p[2]==0x33))){++reload_faults;return {Result::io_error};}
         if(injecting&&mode=="reload-stop"&&p[2]==0x33){++reload_faults;stopped=true;return {Result::stopped};}
         if(injecting&&mode=="reload-enable-verify"&&p[2]==0x33){++reload_faults;report_button=1;return r;}
         if(injecting&&mode=="reload-restore-verify"&&restoring){++reload_faults;}
         else report_button=p[2]&0x11;
         if(input_index>0&&connects==1&&p[2]==0x33&&mode.starts_with("reload-window"))channel->send(1,1,1);
     }
     r.packet.bytes[6]=static_cast<Byte>(report_button);return r;
 }
 if(feature==6){Reply r{Result::ok};r.packet.size=20;
     if(function==0){r.packet.bytes[5]=1;r.packet.bytes[7]=1;r.packet.bytes[8]=1;}
     if(function==1)r.packet.bytes[4]=static_cast<Byte>(report_thumb);
     if(function==2)report_thumb=*parameters.begin();return r;}
 ++requests;CHECK(feature==7 && (function==2||function==3));CHECK(parameters.size()>=2);
 Reply r{Result::ok};r.packet.size=20;r.packet.bytes[4]=*parameters.begin();r.packet.bytes[5]=*(parameters.begin()+1);r.packet.bytes[6]=fixture_xy()?0x11:1;return r;
}
unsigned word(std::span<const Byte> b){return (unsigned(b[0])<<8)|b[1];}
Byte high(unsigned n){return static_cast<Byte>(n>>8);}
Byte low(unsigned n){return static_cast<Byte>(n);}
std::optional<unsigned> number(std::wstring_view text,int base){wchar_t* end;std::wstring copy(text);auto v=wcstoul(copy.c_str(),&end,base);return *end?std::optional<unsigned>{}:std::optional<unsigned>{unsigned(v)};}
}
