#pragma once
#include "dpi_ipc.hpp"

namespace settings_ipc {
constexpr auto memory_name=L"Local\\LogiNative.Settings.v1";
constexpr auto done_name=L"Local\\LogiNative.Settings.Done.v1";
constexpr auto client_name=L"Local\\LogiNative.Settings.Client.v1";
constexpr LONG protocol=1;
enum Phase : LONG { idle,pending,processing,done };
using dpi_ipc::Handle;
using dpi_ipc::canonical;
constexpr unsigned group(SettingKey key){const auto i=static_cast<unsigned>(key);return i<2?0:i-1;}
struct alignas(8) Commit {
    LONG64 sequence{},request_id{},revision{};
    LONG key{},value{},status{};
    std::array<LONG,5> observed{};
    LONG observed_mask{};
};
struct alignas(8) Shared {
    LONG version{},running{},pid{},phase{};
    LONG64 instance{},serial{},request_id{},request_instance{},revision{};
    LONG key{},value{};
    SettingResult result{};
    std::array<Commit,4> commits{};
    LONG64 snapshot_sequence{};
    std::array<LONG,5> observed{};
    std::array<LONG64,5> sampled_at{};
    LONG observed_mask{},software_mask{},ready{},saved{};
    wchar_t mapping[1024]{};
};
struct Request {unsigned long long id{};SettingKey key{};unsigned value{};unsigned long long revision{};};
class Host {
    Handle memory_,client_,done_;
    Shared* shared_{};
    LONG64 instance_{};
public:
    explicit Host(const wchar_t* path) {
        const auto file=canonical(path);if(file.empty()||file.size()>=1024)return;
        memory_.value=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),memory_name);
        if(!memory_.value)return;
        shared_=static_cast<Shared*>(MapViewOfFile(memory_.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        if(!shared_)return;
        client_.value=CreateMutexW(nullptr,FALSE,client_name);done_.value=CreateEventW(nullptr,FALSE,FALSE,done_name);
        if(!client_.value||!done_.value){UnmapViewOfFile(shared_);shared_=nullptr;return;}
        const auto locked=WaitForSingleObject(client_.value,5000);
        if(locked!=WAIT_OBJECT_0&&locked!=WAIT_ABANDONED){UnmapViewOfFile(shared_);shared_=nullptr;return;}
        struct Unlock{HANDLE value;~Unlock(){ReleaseMutex(value);}} unlock{client_.value};
        InterlockedExchange(&shared_->running,0);InterlockedExchange(&shared_->version,0);
        LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);instance_=counter.QuadPart^(LONG64(GetCurrentProcessId())<<32);
        InterlockedExchange64(&shared_->instance,instance_);shared_->pid=static_cast<LONG>(GetCurrentProcessId());
        std::wcscpy(shared_->mapping,file.c_str());
        for(auto& commit:shared_->commits){InterlockedExchange64(&commit.sequence,0);InterlockedExchange64(&commit.request_id,0);}
        InterlockedExchange64(&shared_->snapshot_sequence,0);
        InterlockedExchange(&shared_->observed_mask,0);InterlockedExchange(&shared_->software_mask,0);
        InterlockedExchange(&shared_->ready,0);InterlockedExchange(&shared_->saved,0);
        InterlockedExchange(&shared_->phase,idle);ResetEvent(done_.value);
        InterlockedExchange(&shared_->version,protocol);InterlockedExchange(&shared_->running,1);
    }
    ~Host(){shutdown();}
    void shutdown() {
        if(!shared_)return;
        if(InterlockedCompareExchange64(&shared_->instance,0,0)==instance_) {
            InterlockedExchange(&shared_->running,0);
            if(shared_->phase==pending||shared_->phase==processing) {
                shared_->result={};shared_->result.status=SettingStatus::stopped;
                InterlockedExchange(&shared_->phase,done);
            }
            SetEvent(done_.value);
        }
        UnmapViewOfFile(shared_);shared_=nullptr;
    }
    bool waiting() const{return shared_&&InterlockedCompareExchange(&shared_->phase,0,0)==pending;}
    void invalidate(SettingKey key) {
        if(!shared_||InterlockedCompareExchange64(&shared_->instance,0,0)!=instance_||static_cast<unsigned>(key)>=5)return;
        auto& commit=shared_->commits[group(key)];InterlockedIncrement64(&commit.sequence);
        InterlockedExchange64(&commit.request_id,0);InterlockedIncrement64(&commit.sequence);
    }
    void observe(const SettingResult& result,bool ready,bool saved,unsigned invalid_mask=0) {
        if(!shared_||InterlockedCompareExchange64(&shared_->instance,0,0)!=instance_)return;
        for(unsigned i=0;i<5;++i)if((invalid_mask&~result.observed_mask)&(1u<<i))invalidate(static_cast<SettingKey>(i));
        for(unsigned i=0;i<5;++i)if(result.observed_mask&(1u<<i)) {
            auto& commit=shared_->commits[group(static_cast<SettingKey>(i))];
            if(commit.request_id&&(commit.observed_mask&(1u<<i))&&commit.observed[i]!=static_cast<LONG>(result.observed[i]))
                invalidate(static_cast<SettingKey>(i));
        }
        InterlockedIncrement64(&shared_->snapshot_sequence);
        auto mask=static_cast<unsigned>(shared_->observed_mask)&~invalid_mask;
        auto software=static_cast<unsigned>(shared_->software_mask)&~invalid_mask;
        for(unsigned i=0;i<5;++i)if(result.observed_mask&(1u<<i)) {
            InterlockedExchange(&shared_->observed[i],static_cast<LONG>(result.observed[i]));
            InterlockedExchange64(&shared_->sampled_at[i],static_cast<LONG64>(GetTickCount64()));
            mask|=1u<<i;
        }
        if(result.observed_mask&16u){if(result.software)software|=16u;else software&=~16u;}
        InterlockedExchange(&shared_->observed_mask,static_cast<LONG>(mask));
        InterlockedExchange(&shared_->software_mask,static_cast<LONG>(software));
        InterlockedExchange(&shared_->ready,ready);InterlockedExchange(&shared_->saved,saved);
        InterlockedIncrement64(&shared_->snapshot_sequence);
    }
    std::optional<Request> take() {
        if(!shared_||InterlockedCompareExchange(&shared_->phase,processing,pending)!=pending)return {};
        if(shared_->request_instance!=instance_) {
            shared_->result={};
            shared_->result.status=SettingStatus::stopped;
            InterlockedExchange(&shared_->phase,done);SetEvent(done_.value);return {};
        }
        return Request{static_cast<unsigned long long>(shared_->request_id),static_cast<SettingKey>(shared_->key),
            static_cast<unsigned>(shared_->value),static_cast<unsigned long long>(shared_->revision)};
    }
    void finish(SettingResult result) {
        if(!shared_||InterlockedCompareExchange64(&shared_->instance,0,0)!=instance_||
            shared_->request_id!=static_cast<LONG64>(result.request_id)||shared_->phase!=processing)return;
        result.instance_id=static_cast<unsigned long long>(instance_);result.pid=static_cast<unsigned long>(shared_->pid);
        const auto index=group(result.key);
        if(index<shared_->commits.size()) {
            auto& commit=shared_->commits[index];InterlockedIncrement64(&commit.sequence);
            InterlockedExchange64(&commit.request_id,static_cast<LONG64>(result.request_id));
            InterlockedExchange64(&commit.revision,static_cast<LONG64>(result.revision));
            InterlockedExchange(&commit.key,static_cast<LONG>(result.key));
            InterlockedExchange(&commit.value,static_cast<LONG>(result.requested));
            InterlockedExchange(&commit.status,static_cast<LONG>(result.status));
            InterlockedExchange(&commit.observed_mask,static_cast<LONG>(result.observed_mask));
            for(unsigned i=0;i<5;++i)InterlockedExchange(&commit.observed[i],static_cast<LONG>(result.observed[i]));
            InterlockedIncrement64(&commit.sequence);
        }
        shared_->result=result;InterlockedExchange(&shared_->phase,done);SetEvent(done_.value);
    }
};
}

const char* setting_status_name(SettingStatus status) {
    switch(status) {
    case SettingStatus::applied:return "applied";
    case SettingStatus::save_failed:return "save_failed";
    case SettingStatus::write_failed:return "write_failed";
    case SettingStatus::readback_failed:return "readback_failed";
    case SettingStatus::invalid_value:return "invalid_value";
    case SettingStatus::device_unavailable:return "device_unavailable";
    case SettingStatus::stopped:return "stopped";
    case SettingStatus::unsupported:return "unsupported";
    case SettingStatus::not_running:return "not_running";
    case SettingStatus::busy:return "busy";
    case SettingStatus::timeout:return "timeout";
    case SettingStatus::cancelled:return "cancelled";
    case SettingStatus::configuration_mismatch:return "configuration_mismatch";
    case SettingStatus::official_conflict:return "official_conflict";
    default:return "ipc_error";
    }
}

SettingResult set_agent_setting(SettingKey key,unsigned value,unsigned long long revision,
    unsigned timeout_ms,const wchar_t* expected_mapping,void* cancel_event) {
    using namespace settings_ipc;
    SettingResult result;result.key=key;result.requested=value;result.revision=revision;
    if(static_cast<unsigned>(key)>=5 || value>(key==SettingKey::smartshift?50u:1u)){result.status=SettingStatus::invalid_value;return result;}
    Handle agent(OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\LogiNative.Agent"));
    const auto agent_error=agent.value?ERROR_SUCCESS:GetLastError();
    if(!agent_running()&&!agent.value){result.status=agent_error==ERROR_FILE_NOT_FOUND?SettingStatus::not_running:SettingStatus::ipc_error;return result;}
    Handle memory(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,memory_name));
    if(!memory.value){result.status=SettingStatus::unsupported;return result;}
    auto* shared=static_cast<Shared*>(MapViewOfFile(memory.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    if(!shared)return result;
    struct View{Shared* value;~View(){UnmapViewOfFile(value);}} view{shared};
    if(InterlockedCompareExchange(&shared->version,0,0)!=protocol){result.status=SettingStatus::unsupported;return result;}
    const auto instance=InterlockedCompareExchange64(&shared->instance,0,0);const auto pid=static_cast<DWORD>(shared->pid);
    result.instance_id=static_cast<unsigned long long>(instance);result.pid=pid;
    Handle process(OpenProcess(SYNCHRONIZE,FALSE,pid));
    Handle client(OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,client_name));
    Handle wake(OpenEventW(EVENT_MODIFY_STATE,FALSE,dpi_ipc::request_name));
    Handle completed(OpenEventW(SYNCHRONIZE,FALSE,done_name));
    if(!process.value||!client.value||!wake.value||!completed.value)return result;
    const auto start=GetTickCount64();
    const auto remaining=[&]{const auto elapsed=GetTickCount64()-start;return elapsed>=timeout_ms?0u:timeout_ms-static_cast<unsigned>(elapsed);};
    const HANDLE locks[]{client.value,process.value,cancel_event};
    const auto locked=WaitForMultipleObjects(cancel_event?3:2,locks,FALSE,remaining());
    if(locked!=WAIT_OBJECT_0&&locked!=WAIT_ABANDONED_0) {
        result.status=locked==WAIT_OBJECT_0+2?SettingStatus::cancelled:locked==WAIT_OBJECT_0+1?SettingStatus::stopped:
            locked==WAIT_TIMEOUT?SettingStatus::timeout:SettingStatus::ipc_error;return result;
    }
    struct Unlock{HANDLE value;~Unlock(){ReleaseMutex(value);}} unlock{client.value};
    const auto alive=[&]{return InterlockedCompareExchange(&shared->running,0,0)&&
        InterlockedCompareExchange64(&shared->instance,0,0)==instance&&shared->pid==static_cast<LONG>(pid)&&
        WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;};
    if(!alive()){result.status=SettingStatus::stopped;return result;}
    if(cancel_event&&WaitForSingleObject(cancel_event,0)==WAIT_OBJECT_0){result.status=SettingStatus::cancelled;return result;}
    if(expected_mapping&&canonical(expected_mapping)!=shared->mapping){result.status=SettingStatus::configuration_mismatch;return result;}
    const auto phase=InterlockedCompareExchange(&shared->phase,0,0);
    if(phase==pending||phase==processing){result.status=SettingStatus::busy;return result;}
    result.request_id=static_cast<unsigned long long>(InterlockedIncrement64(&shared->serial));
    shared->request_id=static_cast<LONG64>(result.request_id);shared->request_instance=instance;
    shared->key=static_cast<LONG>(key);shared->value=static_cast<LONG>(value);shared->revision=static_cast<LONG64>(revision);
    InterlockedExchange(&shared->phase,pending);
    if(!alive()){result.status=SettingStatus::stopped;return result;}
    if(!SetEvent(wake.value)) {
        if(InterlockedCompareExchange(&shared->phase,done,pending)==pending)shared->result=result;
        return result;
    }
    const HANDLE events[]{completed.value,process.value,cancel_event};
    for(;;) {
        if(!alive()){result.status=SettingStatus::stopped;return result;}
        if(cancel_event&&WaitForSingleObject(cancel_event,0)==WAIT_OBJECT_0){result.status=SettingStatus::cancelled;return result;}
        if(InterlockedCompareExchange(&shared->phase,0,0)==done&&shared->request_id==static_cast<LONG64>(result.request_id))return shared->result;
        const auto waited=WaitForMultipleObjects(cancel_event?3:2,events,FALSE,remaining());
        if(waited==WAIT_TIMEOUT){result.status=alive()?SettingStatus::timeout:SettingStatus::stopped;return result;}
        if(waited==WAIT_OBJECT_0+1){result.status=SettingStatus::stopped;return result;}
        if(cancel_event&&waited==WAIT_OBJECT_0+2){result.status=SettingStatus::cancelled;return result;}
        if(waited!=WAIT_OBJECT_0){result.status=SettingStatus::ipc_error;return result;}
    }
}

bool is_current_setting_result(const SettingResult& result) {
    using namespace settings_ipc;
    if(!result.request_id||static_cast<unsigned>(result.key)>=5)return false;
    Handle memory(OpenFileMappingW(FILE_MAP_READ,FALSE,memory_name));if(!memory.value)return false;
    auto* shared=static_cast<Shared*>(MapViewOfFile(memory.value,FILE_MAP_READ,0,0,sizeof(Shared)));if(!shared)return false;
    struct View{Shared* value;~View(){UnmapViewOfFile(value);}} view{shared};
    const auto load=[]<class T>(T& field){return std::atomic_ref<T>(field).load(std::memory_order_acquire);};
    const auto& identity=result;auto& commit=shared->commits[group(result.key)];const auto sequence=load(commit.sequence);
    if(sequence&1)return false;
    const bool matches=load(shared->version)==protocol&&load(shared->running)&&load(shared->instance)==static_cast<LONG64>(identity.instance_id)&&
        load(shared->pid)==static_cast<LONG>(identity.pid)&&load(commit.request_id)==static_cast<LONG64>(identity.request_id)&&
        load(commit.revision)==static_cast<LONG64>(identity.revision)&&load(commit.key)==static_cast<LONG>(identity.key)&&
        load(commit.value)==static_cast<LONG>(identity.requested)&&load(commit.status)==static_cast<LONG>(identity.status);
    if(!matches||load(commit.sequence)!=sequence)return false;
    Handle process(OpenProcess(SYNCHRONIZE,FALSE,result.pid));return process.value&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;
}

AgentSettingsSnapshot read_agent_settings() {
    using namespace settings_ipc;
    AgentSettingsSnapshot result;
    Handle memory(OpenFileMappingW(FILE_MAP_READ,FALSE,memory_name));if(!memory.value)return result;
    auto* shared=static_cast<Shared*>(MapViewOfFile(memory.value,FILE_MAP_READ,0,0,sizeof(Shared)));if(!shared)return result;
    struct View{Shared* value;~View(){UnmapViewOfFile(value);}} view{shared};
    const auto load=[]<class T>(T& field){return std::atomic_ref<T>(field).load(std::memory_order_acquire);};
    const auto sequence=load(shared->snapshot_sequence);if(sequence&1)return result;
    if(load(shared->version)!=protocol||!load(shared->running))return result;
    result.instance_id=static_cast<unsigned long long>(load(shared->instance));result.pid=static_cast<unsigned long>(load(shared->pid));
    result.ready=load(shared->ready)!=0;result.saved=load(shared->saved)!=0;
    result.observed_mask=static_cast<unsigned>(load(shared->observed_mask));result.software_mask=static_cast<unsigned>(load(shared->software_mask));
    for(unsigned i=0;i<5;++i){result.observed[i]=static_cast<unsigned>(load(shared->observed[i]));result.sampled_at_ms[i]=static_cast<unsigned long long>(load(shared->sampled_at[i]));}
    if(load(shared->snapshot_sequence)!=sequence||!load(shared->running)||load(shared->instance)!=static_cast<LONG64>(result.instance_id))return {};
    Handle process(OpenProcess(SYNCHRONIZE,FALSE,result.pid));result.available=process.value&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;
    return result;
}
