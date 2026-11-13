#pragma once
#include "remap.hpp"
#include <windows.h>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>

namespace dpi_ipc {
constexpr auto memory_name=L"Local\\LogiNative.Dpi.v1";
constexpr auto request_name=L"Local\\LogiNative.Dpi.Request.v1";
constexpr auto done_name=L"Local\\LogiNative.Dpi.Done.v1";
constexpr auto client_name=L"Local\\LogiNative.Dpi.Client.v1";
constexpr LONG protocol=1;
enum Phase : LONG { idle,pending,processing,done };
struct alignas(8) Shared {
    LONG version{},running{},pid{},phase{};
    LONG64 instance{},serial{},request_id{},request_instance{};
    unsigned requested{},observed{};
    LONG status{},observed_valid{},applied{},saved{};
    wchar_t mapping[1024]{};
};
struct Handle {
    HANDLE value{};
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
inline std::wstring canonical(const wchar_t* path) {
    if(!path)return {};
    std::error_code error;auto result=std::filesystem::weakly_canonical(path,error).wstring();
    if(error)return {};
    CharLowerBuffW(result.data(),static_cast<DWORD>(result.size()));return result;
}
struct Request {unsigned long long id{};unsigned value{};};
class Host {
    Handle memory_,client_,request_,done_;
    Shared* shared_{};
    LONG64 instance_{};
public:
    explicit Host(const wchar_t* path) {
        const auto file=canonical(path);
        if(file.empty() || file.size()>=1024)return;
        memory_.value=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),memory_name);
        if(!memory_.value)return;
        shared_=static_cast<Shared*>(MapViewOfFile(memory_.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        if(!shared_)return;
        client_.value=CreateMutexW(nullptr,FALSE,client_name);
        request_.value=CreateEventW(nullptr,TRUE,FALSE,request_name);
        done_.value=CreateEventW(nullptr,FALSE,FALSE,done_name);
        if(!client_.value || !request_.value || !done_.value){UnmapViewOfFile(shared_);shared_=nullptr;return;}
        // Initialization alone takes the client mutex: old clients cannot write a new
        // incarnation between their identity check and publishing their request.
        // Command service never takes this mutex (the waiting client owns it).
        const auto locked=WaitForSingleObject(client_.value,5000);
        if(locked!=WAIT_OBJECT_0 && locked!=WAIT_ABANDONED){UnmapViewOfFile(shared_);shared_=nullptr;return;}
        struct Unlock {HANDLE value;~Unlock(){ReleaseMutex(value);}} unlock{client_.value};
        InterlockedExchange(&shared_->running,0);
        InterlockedExchange(&shared_->version,0);
        LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
        instance_=counter.QuadPart ^ (LONG64(GetCurrentProcessId())<<32);
        InterlockedExchange64(&shared_->instance,instance_);
        shared_->pid=static_cast<LONG>(GetCurrentProcessId());
        std::wcscpy(shared_->mapping,file.c_str());
        InterlockedExchange(&shared_->phase,idle);
        ResetEvent(request_.value);ResetEvent(done_.value);
        InterlockedExchange(&shared_->version,protocol);
        InterlockedExchange(&shared_->running,1);
    }
    ~Host() {shutdown();}
    void shutdown() {
        if(!shared_)return;
        if(shared_->instance==instance_) {
            InterlockedExchange(&shared_->running,0);
            if(shared_->phase==pending || shared_->phase==processing) {
                shared_->status=static_cast<LONG>(DpiCommandStatus::stopped);
                shared_->observed_valid=shared_->applied=shared_->saved=0;
                InterlockedExchange(&shared_->phase,done);
            }
            SetEvent(done_.value);
        }
        UnmapViewOfFile(shared_);shared_=nullptr;
    }
    HANDLE wake() const{return shared_?request_.value:nullptr;}
    bool waiting() const{return shared_ && InterlockedCompareExchange(&shared_->phase,0,0)==pending;}
    std::optional<Request> take() {
        if(!shared_)return {};
        ResetEvent(request_.value);
        if(InterlockedCompareExchange(&shared_->phase,processing,pending)!=pending)return {};
        if(shared_->request_instance!=instance_) {
            shared_->status=static_cast<LONG>(DpiCommandStatus::stopped);
            shared_->observed_valid=shared_->applied=shared_->saved=0;
            InterlockedExchange(&shared_->phase,done);SetEvent(done_.value);return {};
        }
        return Request{static_cast<unsigned long long>(shared_->request_id),shared_->requested};
    }
    void finish(const DpiCommandResult& result) {
        if(!shared_ || shared_->instance!=instance_ || shared_->request_id!=static_cast<LONG64>(result.request_id))return;
        shared_->observed=result.observed;shared_->status=static_cast<LONG>(result.status);
        shared_->observed_valid=result.observed_valid;shared_->applied=result.applied;shared_->saved=result.saved;
        InterlockedExchange(&shared_->phase,done);SetEvent(done_.value);
    }
};
}

const char* dpi_command_status_name(DpiCommandStatus status) {
    switch(status) {
    case DpiCommandStatus::applied:return "applied";
    case DpiCommandStatus::save_failed:return "save_failed";
    case DpiCommandStatus::write_failed:return "write_failed";
    case DpiCommandStatus::readback_failed:return "readback_failed";
    case DpiCommandStatus::invalid_value:return "invalid_value";
    case DpiCommandStatus::device_unavailable:return "device_unavailable";
    case DpiCommandStatus::stopped:return "stopped";
    case DpiCommandStatus::unsupported:return "unsupported";
    case DpiCommandStatus::not_running:return "not_running";
    case DpiCommandStatus::busy:return "busy";
    case DpiCommandStatus::timeout:return "timeout";
    case DpiCommandStatus::configuration_mismatch:return "configuration_mismatch";
    default:return "ipc_error";
    }
}

DpiCommandResult set_agent_dpi(unsigned value,unsigned timeout_ms,const wchar_t* expected_mapping) {
    using namespace dpi_ipc;
    DpiCommandResult result;result.requested=value;
    Handle agent(OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\LogiNative.Agent"));
    const auto agent_error=agent.value?ERROR_SUCCESS:GetLastError();
    if(!agent_running() && !agent.value){result.status=agent_error==ERROR_FILE_NOT_FOUND?DpiCommandStatus::not_running:DpiCommandStatus::ipc_error;return result;}
    Handle memory(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,memory_name));
    if(!memory.value){result.status=DpiCommandStatus::unsupported;return result;}
    auto* shared=static_cast<Shared*>(MapViewOfFile(memory.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    if(!shared)return result;
    struct View {Shared* value;~View(){UnmapViewOfFile(value);}} view{shared};
    if(InterlockedCompareExchange(&shared->version,0,0)!=protocol){result.status=DpiCommandStatus::unsupported;return result;}
    const LONG64 instance=InterlockedCompareExchange64(&shared->instance,0,0);
    const DWORD pid=static_cast<DWORD>(shared->pid);
    Handle owner(OpenProcess(SYNCHRONIZE,FALSE,pid));
    Handle client(OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,client_name));
    Handle request(OpenEventW(EVENT_MODIFY_STATE,FALSE,request_name));
    Handle completed(OpenEventW(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,done_name));
    if(!owner.value || !client.value || !request.value || !completed.value)return result;
    const auto start=GetTickCount64();
    const auto remaining=[&] {const auto elapsed=GetTickCount64()-start;return elapsed>=timeout_ms?0u:timeout_ms-static_cast<unsigned>(elapsed);};
    const auto locked=WaitForSingleObject(client.value,remaining());
    if(locked!=WAIT_OBJECT_0 && locked!=WAIT_ABANDONED){result.status=locked==WAIT_TIMEOUT?DpiCommandStatus::timeout:DpiCommandStatus::ipc_error;return result;}
    struct Unlock {HANDLE value;~Unlock(){ReleaseMutex(value);}} unlock{client.value};
    const auto alive=[&] {return InterlockedCompareExchange(&shared->running,0,0) &&
        InterlockedCompareExchange64(&shared->instance,0,0)==instance && shared->pid==static_cast<LONG>(pid) &&
        WaitForSingleObject(owner.value,0)==WAIT_TIMEOUT;};
    if(!alive()){result.status=DpiCommandStatus::stopped;return result;}
    if(expected_mapping && canonical(expected_mapping)!=shared->mapping){result.status=DpiCommandStatus::configuration_mismatch;return result;}
    const auto phase=InterlockedCompareExchange(&shared->phase,0,0);
    if(phase==pending || phase==processing){result.status=DpiCommandStatus::busy;return result;}
    result.request_id=static_cast<unsigned long long>(InterlockedIncrement64(&shared->serial));
    shared->request_id=static_cast<LONG64>(result.request_id);shared->request_instance=instance;shared->requested=value;
    shared->observed_valid=shared->applied=shared->saved=0;
    ResetEvent(completed.value);
    InterlockedExchange(&shared->phase,pending);
    if(!alive()){result.status=DpiCommandStatus::stopped;return result;}
    if(!SetEvent(request.value))return result;
    const HANDLE events[]{completed.value,owner.value};
    for(;;) {
        if(!alive()){result.status=DpiCommandStatus::stopped;return result;}
        if(InterlockedCompareExchange(&shared->phase,0,0)==done && shared->request_id==static_cast<LONG64>(result.request_id)) {
            result.status=static_cast<DpiCommandStatus>(shared->status);result.observed=shared->observed;
            result.observed_valid=shared->observed_valid!=0;result.applied=shared->applied!=0;result.saved=shared->saved!=0;return result;
        }
        const auto wait=WaitForMultipleObjects(2,events,FALSE,remaining());
        if(wait==WAIT_TIMEOUT){result.status=alive()?DpiCommandStatus::timeout:DpiCommandStatus::stopped;return result;}
        if(wait==WAIT_OBJECT_0+1){result.status=DpiCommandStatus::stopped;return result;}
        if(wait!=WAIT_OBJECT_0){result.status=DpiCommandStatus::ipc_error;return result;}
        // A signalled completion with another incarnation must never be our success.
        if(remaining()==0){result.status=DpiCommandStatus::timeout;return result;}
    }
}
