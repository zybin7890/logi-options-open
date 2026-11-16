#include "system_actions.hpp"
#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <reason.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wbemidl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace system_actions {
namespace {
struct ComScope {
    HRESULT result{CoInitializeEx(nullptr,COINIT_MULTITHREADED)};
    ~ComScope(){if(SUCCEEDED(result))CoUninitialize();}
    explicit operator bool() const{return SUCCEEDED(result)||result==RPC_E_CHANGED_MODE;}
};
template<class T> struct Release {void operator()(T* value) const {if(value)value->Release();}};
template<class T> using Com=std::unique_ptr<T,Release<T>>;
struct Bstr {
    BSTR value;
    explicit Bstr(const wchar_t* text):value(SysAllocString(text)){}
    ~Bstr(){SysFreeString(value);}
    operator BSTR() const{return value;}
};
struct Value {
    VARIANT data{};
    ~Value(){VariantClear(&data);}
};
bool microphone(const ActionContext& context) {
    ComScope scope;if(!scope)return false;
    IMMDeviceEnumerator* raw{};
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&raw))))return false;
    Com<IMMDeviceEnumerator> enumerator(raw);
    IMMDevice* device_raw{};
    if(FAILED(enumerator->GetDefaultAudioEndpoint(eCapture,eConsole,&device_raw)))return false;
    Com<IMMDevice> device(device_raw);
    IAudioEndpointVolume* endpoint_raw{};
    if(FAILED(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,
                              reinterpret_cast<void**>(&endpoint_raw))))return false;
    Com<IAudioEndpointVolume> endpoint(endpoint_raw);
    BOOL muted{};
    return SUCCEEDED(endpoint->GetMute(&muted)) && context.active() && SUCCEEDED(endpoint->SetMute(!muted,nullptr));
}
struct ShutdownPrivilege {
    HANDLE token{};TOKEN_PRIVILEGES previous{};DWORD previous_size{sizeof(previous)};
    bool enabled{};
    ShutdownPrivilege() {
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return;
        TOKEN_PRIVILEGES request{};request.PrivilegeCount=1;
        if(!LookupPrivilegeValueW(nullptr,SE_SHUTDOWN_NAME,&request.Privileges[0].Luid))return;
        request.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
        enabled=AdjustTokenPrivileges(token,FALSE,&request,sizeof(previous),&previous,&previous_size) && GetLastError()==ERROR_SUCCESS;
    }
    ~ShutdownPrivilege(){if(token){if(enabled)AdjustTokenPrivileges(token,FALSE,&previous,0,nullptr,nullptr);CloseHandle(token);}}
};
bool power(bool sleep,const ActionContext& context) {
    ShutdownPrivilege privilege;if(!privilege.enabled)return false;
    // Respect unsaved documents and application vetoes. Never force termination.
    if(!context.active())return false;
    return sleep ? SetSuspendState(FALSE,FALSE,FALSE)!=FALSE :
        ExitWindowsEx(EWX_POWEROFF,SHTDN_REASON_MAJOR_APPLICATION|SHTDN_REASON_MINOR_OTHER|SHTDN_REASON_FLAG_PLANNED)!=FALSE;
}
bool high_contrast(const ActionContext& context) {
    HIGHCONTRASTW state{sizeof(HIGHCONTRASTW),0,nullptr};
    if(!SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(state),&state,0))return false;
    state.dwFlags^=HCF_HIGHCONTRASTON;
    return context.active() && SystemParametersInfoW(SPI_SETHIGHCONTRAST,sizeof(state),&state,SPIF_UPDATEINIFILE|SPIF_SENDCHANGE)!=FALSE;
}
Com<IEnumWbemClassObject> query(IWbemServices& services,const wchar_t* text) {
    IEnumWbemClassObject* raw{};
    if(FAILED(services.ExecQuery(Bstr(L"WQL"),Bstr(text),WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&raw)))return {};
    return Com<IEnumWbemClassObject>(raw);
}
Com<IWbemClassObject> next(IEnumWbemClassObject& rows) {
    IWbemClassObject* raw{};ULONG count{};
    if(FAILED(rows.Next(1000,1,&raw,&count))||count!=1)return {};
    return Com<IWbemClassObject>(raw);
}
// Laptop panels expose supported brightness levels through WMI. Move to the
// next supported level instead of inventing a brightness keyboard scan code.
bool panel_brightness(bool up,const ActionContext& context) {
    ComScope scope;if(!scope)return false;
    IWbemLocator* raw{};
    if(FAILED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,reinterpret_cast<void**>(&raw))))return false;
    Com<IWbemLocator> locator(raw);IWbemServices* service_raw{};
    if(FAILED(locator->ConnectServer(Bstr(L"ROOT\\WMI"),nullptr,nullptr,nullptr,0,nullptr,nullptr,&service_raw)))return false;
    Com<IWbemServices> services(service_raw);
    if(!context.active())return false;
    if(FAILED(CoSetProxyBlanket(services.get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,
                              RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE)))return false;
    auto panels=query(*services,L"SELECT * FROM WmiMonitorBrightness WHERE Active=TRUE");
    if(!panels)return false;
    IWbemClassObject* definition_raw{};
    if(FAILED(services->GetObject(Bstr(L"WmiMonitorBrightnessMethods"),0,nullptr,&definition_raw,nullptr)))return false;
    Com<IWbemClassObject> definition(definition_raw);
    bool changed=false;
    while(auto panel=next(*panels)) {
        if(!context.active())return false;
        Value current,instance,levels;
        if(FAILED(panel->Get(L"CurrentBrightness",0,&current.data,nullptr,nullptr)) ||
           FAILED(panel->Get(L"InstanceName",0,&instance.data,nullptr,nullptr)) || instance.data.vt!=VT_BSTR ||
           FAILED(panel->Get(L"Level",0,&levels.data,nullptr,nullptr)) || levels.data.vt!=(VT_ARRAY|VT_UI1))continue;
        Value converted;if(FAILED(VariantChangeType(&converted.data,&current.data,0,VT_UI4)))continue;
        auto target=converted.data.ulVal;LONG first{},last{};
        if(FAILED(SafeArrayGetLBound(levels.data.parray,1,&first))||FAILED(SafeArrayGetUBound(levels.data.parray,1,&last)))continue;
        bool found=false;
        for(LONG index=first;index<=last;++index) {
            BYTE level{};if(FAILED(SafeArrayGetElement(levels.data.parray,&index,&level)))continue;
            if((up&&level>converted.data.ulVal&&(!found||level<target)) ||
               (!up&&level<converted.data.ulVal&&(!found||level>target))){target=level;found=true;}
        }
        if(!found){changed=true;continue;}
        auto methods=query(*services,L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
        if(!methods)continue;
        while(auto method=next(*methods)) {
            if(!context.active())return false;
            Value name,path;
            if(FAILED(method->Get(L"InstanceName",0,&name.data,nullptr,nullptr)) || name.data.vt!=VT_BSTR ||
               std::wstring_view(name.data.bstrVal)!=instance.data.bstrVal ||
               FAILED(method->Get(L"__PATH",0,&path.data,nullptr,nullptr)) || path.data.vt!=VT_BSTR)continue;
            IWbemClassObject* signature_raw{};
            if(FAILED(definition->GetMethod(L"WmiSetBrightness",0,&signature_raw,nullptr)))continue;
            Com<IWbemClassObject> signature(signature_raw);IWbemClassObject* parameters_raw{};
            if(FAILED(signature->SpawnInstance(0,&parameters_raw)))continue;
            Com<IWbemClassObject> parameters(parameters_raw);
            VARIANT timeout{};timeout.vt=VT_I4;timeout.lVal=0;
            VARIANT brightness{};brightness.vt=VT_UI1;brightness.bVal=static_cast<BYTE>(target);
            if(FAILED(parameters->Put(L"Timeout",0,&timeout,0))||FAILED(parameters->Put(L"Brightness",0,&brightness,0)))continue;
            IWbemClassObject* result_raw{};
            if(!context.active())return false;
            if(FAILED(services->ExecMethod(path.data.bstrVal,Bstr(L"WmiSetBrightness"),0,nullptr,parameters.get(),&result_raw,nullptr)))continue;
            Com<IWbemClassObject> result(result_raw);Value returned,number;
            if(result && SUCCEEDED(result->Get(L"ReturnValue",0,&returned.data,nullptr,nullptr)) &&
               SUCCEEDED(VariantChangeType(&number.data,&returned.data,0,VT_UI4)) && number.data.ulVal==0)changed=true;
        }
    }
    return changed;
}
bool monitor_brightness(bool up,const ActionContext& context) {
    if(!context.active())return false;
    const auto monitor=MonitorFromWindow(context.window?context.window:GetForegroundWindow(),MONITOR_DEFAULTTOPRIMARY);
    DWORD count{};
    if(!monitor||!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor,&count)||!count)return false;
    std::vector<PHYSICAL_MONITOR> physical(count);
    if(!GetPhysicalMonitorsFromHMONITOR(monitor,count,physical.data()))return false;
    bool accepted=false;
    for(const auto& screen:physical) {
        if(!context.active())break;
        DWORD minimum{},current{},maximum{};
        if(!GetMonitorBrightness(screen.hPhysicalMonitor,&minimum,&current,&maximum)||maximum<minimum||current<minimum||current>maximum)continue;
        const auto increment=std::max<DWORD>(1,(maximum-minimum)/10);
        const auto value=up?current+std::min(increment,maximum-current):current-std::min(increment,current-minimum);
        if(context.active() && (value==current||SetMonitorBrightness(screen.hPhysicalMonitor,value)))accepted=true;
    }
    DestroyPhysicalMonitors(count,physical.data());
    return accepted;
}
}
bool perform(Action action,const ActionContext& context) {
    if(!context.active())return false;
    switch(action) {
    case Action::brightness_up: case Action::brightness_down:
        return panel_brightness(action==Action::brightness_up,context)||monitor_brightness(action==Action::brightness_up,context);
    case Action::microphone_mute:return microphone(context);
    case Action::shutdown:return power(false,context);
    case Action::sleep:return power(true,context);
    case Action::high_contrast:return high_contrast(context);
    case Action::close_window:{const auto window=context.window?context.window:GetForegroundWindow();return window&&context.active()&&PostMessageW(window,WM_CLOSE,0,0);}
    case Action::this_pc:return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"explorer.exe",
        L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",nullptr,SW_SHOWNORMAL))>32;
    }
    return false;
}
}
