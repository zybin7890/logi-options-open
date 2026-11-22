#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <reason.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <cassert>
#include <cstdio>
#include <string>
static unsigned releases{},uninitializes{},mute_sets{},power_calls{},adjusts{},token_closes{},monitor_destroys{},brightness_sets{};
static bool muted{},fail_mute_read{},fail_mute_set{},deny_token{},deny_privilege{},accept_power=true,accept_setting=true,accept_brightness=true;
static bool has_window=true,has_monitor=true,monitor_readable=true;
static DWORD brightness=60,contrast=HCF_AVAILABLE|HCF_HOTKEYACTIVE;
static UINT exit_flags{};static BOOLEAN sleep_hibernate{},sleep_force{},sleep_wake{};
static HRESULT init_result=S_OK;
static DWORD last_error=ERROR_SUCCESS;
static std::wstring shell_target,shell_parameters;
static INT_PTR shell_result=33;
static HWND closed_window{};
static int references{};
#define BASIC_COM \
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;} \
 ULONG STDMETHODCALLTYPE AddRef() override{++references;return 2;} \
 ULONG STDMETHODCALLTYPE Release() override{--references;++releases;return 1;}
struct Volume final:IAudioEndpointVolume {
 BASIC_COM
 int position{40},fail_at{-1};
 HRESULT STDMETHODCALLTYPE RegisterControlChangeNotify(IAudioEndpointVolumeCallback*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE UnregisterControlChangeNotify(IAudioEndpointVolumeCallback*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetChannelCount(UINT*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetMasterVolumeLevel(float,LPCGUID) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetMasterVolumeLevelScalar(float,LPCGUID) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetMasterVolumeLevel(float*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetMasterVolumeLevelScalar(float* scalar) override{*scalar=position/100.f;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetChannelVolumeLevel(UINT,float,LPCGUID) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetChannelVolumeLevelScalar(UINT,float,LPCGUID) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetChannelVolumeLevel(UINT,float*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetChannelVolumeLevelScalar(UINT,float*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetMute(BOOL value,LPCGUID context) override{assert(!context);++mute_sets;if(fail_mute_set)return E_FAIL;muted=value;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetMute(BOOL* value) override{if(fail_mute_read)return E_FAIL;*value=muted;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetVolumeStepInfo(UINT*,UINT*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE VolumeStepUp(LPCGUID id) override{assert(!id);if(position==fail_at)return E_FAIL;++position;return S_OK;}
 HRESULT STDMETHODCALLTYPE VolumeStepDown(LPCGUID id) override{assert(!id);--position;return S_OK;}
 HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetVolumeRange(float*,float*,float*) override{return E_NOTIMPL;}
} volume,speaker,headset;
struct Device final:IMMDevice {
 BASIC_COM
 Volume* endpoint{&volume};
 HRESULT STDMETHODCALLTYPE Activate(REFIID id,DWORD context,PROPVARIANT* parameters,void** output) override{
    assert(id==__uuidof(IAudioEndpointVolume)&&context==CLSCTX_ALL&&!parameters);*output=endpoint;endpoint->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD,IPropertyStore**) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetState(DWORD*) override{return E_NOTIMPL;}
} device,render_device;
static EDataFlow expected_flow=eCapture;
struct Enumerator final:IMMDeviceEnumerator {
 BASIC_COM
 HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow,DWORD,IMMDeviceCollection**) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow direction,ERole role,IMMDevice** output) override{
    assert(direction==expected_flow&&role==eConsole);*output=direction==eRender?&render_device:&device;(*output)->AddRef();return S_OK;}
 HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR,IMMDevice**) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient*) override{return E_NOTIMPL;}
} enumerator;
HRESULT WINAPI fake_initialize(LPVOID reserved,DWORD mode){assert(!reserved&&mode==COINIT_MULTITHREADED);return init_result;}
void WINAPI fake_uninitialize(){++uninitializes;}
HRESULT WINAPI fake_create(REFCLSID clsid,LPUNKNOWN outer,DWORD context,REFIID id,LPVOID* output) {
    assert(!outer);if(clsid==CLSID_WbemLocator)return E_NOINTERFACE;
    assert(clsid==__uuidof(MMDeviceEnumerator)&&context==CLSCTX_ALL&&id==__uuidof(IMMDeviceEnumerator));
    *output=&enumerator;enumerator.AddRef();return S_OK;
}
BOOL WINAPI fake_token(HANDLE,DWORD access,PHANDLE output){assert(access==(TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY));if(deny_token)return FALSE;*output=reinterpret_cast<HANDLE>(2);return TRUE;}
BOOL WINAPI fake_lookup(LPCWSTR system,LPCWSTR name,PLUID id){assert(!system&&std::wstring(name)==SE_SHUTDOWN_NAME);id->LowPart=42;return TRUE;}
BOOL WINAPI fake_adjust(HANDLE token,BOOL all,PTOKEN_PRIVILEGES requested,DWORD size,PTOKEN_PRIVILEGES previous,PDWORD required) {
    assert(token==reinterpret_cast<HANDLE>(2)&&!all);++adjusts;
    if(previous){assert(size==sizeof(TOKEN_PRIVILEGES)&&requested->PrivilegeCount==1&&requested->Privileges[0].Luid.LowPart==42&&requested->Privileges[0].Attributes==SE_PRIVILEGE_ENABLED);previous->PrivilegeCount=0;*required=sizeof(TOKEN_PRIVILEGES);last_error=deny_privilege?ERROR_NOT_ALL_ASSIGNED:ERROR_SUCCESS;}
    else assert(!requested->PrivilegeCount&&!size&&!required);
    return TRUE;
}
DWORD WINAPI fake_error(){return last_error;}
BOOL WINAPI fake_close(HANDLE token){assert(token==reinterpret_cast<HANDLE>(2));++token_closes;return TRUE;}
BOOL WINAPI fake_exit(UINT flags,DWORD reason){++power_calls;exit_flags=flags;assert(reason==(SHTDN_REASON_MAJOR_APPLICATION|SHTDN_REASON_MINOR_OTHER|SHTDN_REASON_FLAG_PLANNED));return accept_power;}
BOOLEAN WINAPI fake_suspend(BOOLEAN hibernate,BOOLEAN force,BOOLEAN wake){++power_calls;sleep_hibernate=hibernate;sleep_force=force;sleep_wake=wake;return accept_power;}
BOOL WINAPI fake_parameters(UINT action,UINT size,PVOID value,UINT flags) {
    assert(size==sizeof(HIGHCONTRASTW));auto& state=*static_cast<HIGHCONTRASTW*>(value);assert(state.cbSize==size);
    if(action==SPI_GETHIGHCONTRAST){assert(!flags);state.dwFlags=contrast;return TRUE;}
    assert(action==SPI_SETHIGHCONTRAST&&flags==(SPIF_UPDATEINIFILE|SPIF_SENDCHANGE));if(accept_setting)contrast=state.dwFlags;return accept_setting;
}
HWND WINAPI fake_front(){return has_window?reinterpret_cast<HWND>(3):nullptr;}
BOOL WINAPI fake_post(HWND window,UINT message,WPARAM wp,LPARAM lp){assert(message==WM_CLOSE&&!wp&&!lp);closed_window=window;return TRUE;}
HINSTANCE WINAPI fake_shell(HWND owner,LPCWSTR verb,LPCWSTR target,LPCWSTR args,LPCWSTR directory,INT show){assert(!owner&&std::wstring(verb)==L"open"&&!directory&&show==SW_SHOWNORMAL);shell_target=target;shell_parameters=args;return reinterpret_cast<HINSTANCE>(shell_result);}
HMONITOR WINAPI fake_monitor(HWND window,DWORD flags){assert(window==fake_front()&&flags==MONITOR_DEFAULTTOPRIMARY);return has_monitor?reinterpret_cast<HMONITOR>(4):nullptr;}
BOOL WINAPI fake_count(HMONITOR monitor,LPDWORD count){assert(monitor==reinterpret_cast<HMONITOR>(4));*count=1;return TRUE;}
BOOL WINAPI fake_physical(HMONITOR monitor,DWORD count,LPPHYSICAL_MONITOR output){assert(monitor==reinterpret_cast<HMONITOR>(4)&&count==1);output->hPhysicalMonitor=reinterpret_cast<HANDLE>(5);return TRUE;}
BOOL WINAPI fake_get_brightness(HANDLE monitor,LPDWORD minimum,LPDWORD current,LPDWORD maximum){assert(monitor==reinterpret_cast<HANDLE>(5));*minimum=10;*maximum=110;*current=brightness;return monitor_readable;}
BOOL WINAPI fake_set_brightness(HANDLE monitor,DWORD value){assert(monitor==reinterpret_cast<HANDLE>(5)&&value>=10&&value<=110);++brightness_sets;if(accept_brightness)brightness=value;return accept_brightness;}
BOOL WINAPI fake_destroy(DWORD count,LPPHYSICAL_MONITOR physical){assert(count==1&&physical->hPhysicalMonitor==reinterpret_cast<HANDLE>(5));++monitor_destroys;return TRUE;}
#define CoInitializeEx fake_initialize
#define CoUninitialize fake_uninitialize
#define CoCreateInstance fake_create
#define OpenProcessToken fake_token
#define LookupPrivilegeValueW fake_lookup
#define AdjustTokenPrivileges fake_adjust
#define GetLastError fake_error
#define CloseHandle fake_close
#define ExitWindowsEx fake_exit
#define SetSuspendState fake_suspend
#define SystemParametersInfoW fake_parameters
#define GetForegroundWindow fake_front
#define PostMessageW fake_post
#define ShellExecuteW fake_shell
#define MonitorFromWindow fake_monitor
#define GetNumberOfPhysicalMonitorsFromHMONITOR fake_count
#define GetPhysicalMonitorsFromHMONITOR fake_physical
#define GetMonitorBrightness fake_get_brightness
#define SetMonitorBrightness fake_set_brightness
#define DestroyPhysicalMonitors fake_destroy
#include "../system_actions.cpp"
int main() {
 using system_actions::Action;using system_actions::perform;
 assert(perform(Action::microphone_mute)&&muted&&references==0&&releases==3&&uninitializes==1);
 assert(perform(Action::microphone_mute)&&!muted&&references==0);
 fail_mute_read=true;const auto previous_sets=mute_sets;assert(!perform(Action::microphone_mute)&&mute_sets==previous_sets&&references==0);fail_mute_read=false;
 fail_mute_set=true;assert(!perform(Action::microphone_mute)&&!muted&&references==0);fail_mute_set=false;
 const auto previous_uninit=uninitializes;init_result=RPC_E_CHANGED_MODE;assert(perform(Action::microphone_mute)&&uninitializes==previous_uninit);init_result=E_FAIL;assert(!perform(Action::microphone_mute));init_result=S_OK;
 expected_flow=eRender;render_device.endpoint=&speaker;
 muted=true;
 auto stepped=system_actions::step_output_volume(true,3,{});
 assert(stepped.status==S_OK&&stepped.completed==3&&speaker.position==43&&stepped.percent==43&&stepped.muted&&references==0);
 muted=false;
 render_device.endpoint=&headset;
 stepped=system_actions::step_output_volume(false,2,{});
 assert(stepped.status==S_OK&&stepped.completed==2&&headset.position==38&&speaker.position==43&&references==0);
 headset.fail_at=39;stepped=system_actions::step_output_volume(true,4,{});
 assert(stepped.status==E_FAIL&&stepped.completed==1&&headset.position==39&&references==0);
 std::stop_source stop;stop.request_stop();
 stepped=system_actions::step_output_volume(false,4,{nullptr,0,stop.get_token()});
 assert(stepped.status==E_ABORT&&stepped.completed==0&&headset.position==39&&references==0);
 expected_flow=eCapture;
 assert(perform(Action::shutdown)&&power_calls==1&&exit_flags==EWX_POWEROFF&&adjusts==2&&token_closes==1);
 assert(perform(Action::sleep)&&power_calls==2&&!sleep_hibernate&&!sleep_force&&!sleep_wake&&adjusts==4);
 deny_token=true;assert(!perform(Action::shutdown)&&power_calls==2);deny_token=false;
 deny_privilege=true;assert(!perform(Action::sleep)&&power_calls==2&&token_closes==3);deny_privilege=false;
 accept_power=false;assert(!perform(Action::shutdown)&&adjusts==7&&token_closes==4);accept_power=true;
 assert(perform(Action::high_contrast)&&(contrast&HCF_HIGHCONTRASTON));assert(perform(Action::high_contrast)&&contrast==(HCF_AVAILABLE|HCF_HOTKEYACTIVE));accept_setting=false;assert(!perform(Action::high_contrast));
 assert(perform(Action::close_window)&&closed_window==reinterpret_cast<HWND>(3));has_window=false;assert(!perform(Action::close_window));has_window=true;
 assert(perform(Action::this_pc)&&shell_target==L"explorer.exe"&&shell_parameters==L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");shell_result=31;assert(!perform(Action::this_pc));
 assert(perform(Action::brightness_up)&&brightness==70&&monitor_destroys==1);assert(perform(Action::brightness_down)&&brightness==60&&monitor_destroys==2);
 brightness=109;assert(perform(Action::brightness_up)&&brightness==110);const auto writes=brightness_sets;assert(perform(Action::brightness_up)&&brightness_sets==writes);
 brightness=11;assert(perform(Action::brightness_down)&&brightness==10);assert(perform(Action::brightness_down)&&brightness==10);
 accept_brightness=false;assert(!perform(Action::brightness_up));monitor_readable=false;assert(!perform(Action::brightness_up));has_monitor=false;assert(!perform(Action::brightness_down));
 assert(references==0);std::puts("Production system APIs: captured CoreAudio toggle, privilege restoration/non-forced power, contrast flags, This PC arguments, WM_CLOSE, DDC bounds/cleanup and unavailable results passed. WMI panel hardware unverified.");
}
