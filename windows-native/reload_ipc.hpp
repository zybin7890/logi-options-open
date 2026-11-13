#pragma once
#include "remap.hpp"
#include "dpi_ipc.hpp"
#include <wincrypt.h>
#include <limits>

namespace reload_ipc {
constexpr auto memory_name=L"Local\\LogiNative.ReloadAck.v1";
constexpr auto done_name=L"Local\\LogiNative.ReloadAck.Done.v1";
constexpr auto client_name=L"Local\\LogiNative.ReloadAck.Client.v1";
constexpr LONG protocol=1;
enum Phase : LONG { idle,pending,processing,done };
using dpi_ipc::Handle;
using dpi_ipc::canonical;
struct alignas(8) Shared {
    LONG version{},running{},pid{},phase{};
    LONG64 instance{},serial{},request_id{},request_instance{},revision{};
    ReloadFingerprint fingerprint{};
    LONG status{};
    // Separate committed identity: a newer pending request is not an applied version.
    LONG64 commit_sequence{},committed_id{},committed_revision{};
    ReloadFingerprint committed_fingerprint{};
    wchar_t mapping[1024]{};
};
struct Request {unsigned long long id{},revision{};ReloadFingerprint fingerprint{};};
class Host {
    Handle memory_,client_,done_;
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
        done_.value=CreateEventW(nullptr,FALSE,FALSE,done_name);
        if(!client_.value || !done_.value){UnmapViewOfFile(shared_);shared_=nullptr;return;}
        const auto locked=WaitForSingleObject(client_.value,5000);
        if(locked!=WAIT_OBJECT_0 && locked!=WAIT_ABANDONED){UnmapViewOfFile(shared_);shared_=nullptr;return;}
        struct Unlock {HANDLE value;~Unlock(){ReleaseMutex(value);}} unlock{client_.value};
        InterlockedExchange(&shared_->running,0);InterlockedExchange(&shared_->version,0);
        LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
        instance_=counter.QuadPart^(LONG64(GetCurrentProcessId())<<32);
        InterlockedExchange64(&shared_->instance,instance_);
        shared_->pid=static_cast<LONG>(GetCurrentProcessId());
        std::wcscpy(shared_->mapping,file.c_str());
        InterlockedExchange64(&shared_->committed_id,0);
        InterlockedExchange64(&shared_->commit_sequence,0);
        InterlockedExchange(&shared_->phase,idle);ResetEvent(done_.value);
        InterlockedExchange(&shared_->version,protocol);InterlockedExchange(&shared_->running,1);
    }
    ~Host(){shutdown();}
    void shutdown() {
        if(!shared_)return;
        if(InterlockedCompareExchange64(&shared_->instance,0,0)==instance_) {
            InterlockedExchange(&shared_->running,0);
            if(shared_->phase==pending || shared_->phase==processing) {
                shared_->status=static_cast<LONG>(ReloadStatus::stopped);
                InterlockedExchange(&shared_->phase,done);
            }
            SetEvent(done_.value);
        }
        UnmapViewOfFile(shared_);shared_=nullptr;
    }
    bool waiting() const{return shared_ && InterlockedCompareExchange(&shared_->phase,0,0)==pending;}
    std::optional<Request> take() {
        if(!shared_ || InterlockedCompareExchange(&shared_->phase,processing,pending)!=pending)return {};
        if(shared_->request_instance!=instance_) {
            shared_->status=static_cast<LONG>(ReloadStatus::stopped);
            InterlockedExchange(&shared_->phase,done);SetEvent(done_.value);return {};
        }
        return Request{static_cast<unsigned long long>(shared_->request_id),
            static_cast<unsigned long long>(shared_->revision),shared_->fingerprint};
    }
    void finish(const Request& request,ReloadStatus status) {
        if(!shared_ || InterlockedCompareExchange64(&shared_->instance,0,0)!=instance_ ||
           shared_->request_id!=static_cast<LONG64>(request.id) || shared_->phase!=processing)return;
        if(status==ReloadStatus::applied) {
            InterlockedIncrement64(&shared_->commit_sequence);
            InterlockedExchange64(&shared_->committed_id,static_cast<LONG64>(request.id));
            InterlockedExchange64(&shared_->committed_revision,static_cast<LONG64>(request.revision));
            for(std::size_t i=0;i<request.fingerprint.size();++i)
                std::atomic_ref<unsigned char>(shared_->committed_fingerprint[i]).store(request.fingerprint[i],std::memory_order_relaxed);
            InterlockedIncrement64(&shared_->commit_sequence);
        }
        shared_->status=static_cast<LONG>(status);
        InterlockedExchange(&shared_->phase,done);SetEvent(done_.value);
    }
    // A legacy reload also supersedes the prior committed ACK, but has no revision ACK.
    void legacy_commit() {
        if(!shared_)return;
        InterlockedIncrement64(&shared_->commit_sequence);
        InterlockedExchange64(&shared_->committed_id,0);
        InterlockedIncrement64(&shared_->commit_sequence);
    }
};
}

bool mapping_fingerprint(std::string_view bytes,ReloadFingerprint& result) {
    HCRYPTPROV provider{};HCRYPTHASH hash{};
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))return false;
    struct Provider {HCRYPTPROV value;~Provider(){CryptReleaseContext(value,0);}} owner{provider};
    if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash))return false;
    struct Hash {HCRYPTHASH value;~Hash(){CryptDestroyHash(value);}} digest{hash};
    while(!bytes.empty()) {
        const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size(),std::numeric_limits<DWORD>::max()));
        if(!CryptHashData(hash,reinterpret_cast<const BYTE*>(bytes.data()),count,0))return false;
        bytes.remove_prefix(count);
    }
    DWORD size=static_cast<DWORD>(result.size());
    return CryptGetHashParam(hash,HP_HASHVAL,result.data(),&size,0) && size==result.size();
}

const char* reload_status_name(ReloadStatus status) {
    switch(status) {
    case ReloadStatus::applied:return "applied";
    case ReloadStatus::superseded:return "superseded";
    case ReloadStatus::read_failed:return "read_failed";
    case ReloadStatus::invalid_mapping:return "invalid_mapping";
    case ReloadStatus::hotkey_failed:return "hotkey_failed";
    case ReloadStatus::hardware_failed:return "hardware_failed";
    case ReloadStatus::device_unavailable:return "device_unavailable";
    case ReloadStatus::stopped:return "stopped";
    case ReloadStatus::unsupported:return "unsupported";
    case ReloadStatus::not_running:return "not_running";
    case ReloadStatus::busy:return "busy";
    case ReloadStatus::timeout:return "timeout";
    case ReloadStatus::cancelled:return "cancelled";
    case ReloadStatus::configuration_mismatch:return "configuration_mismatch";
    default:return "ipc_error";
    }
}

ReloadResult reload_agent_confirmed(const wchar_t* path,unsigned long long revision,
    const ReloadFingerprint& fingerprint,unsigned timeout_ms,void* cancel_event) {
    using namespace reload_ipc;
    ReloadResult result;result.revision=revision;result.fingerprint=fingerprint;
    Handle agent(OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\LogiNative.Agent"));
    const auto agent_error=agent.value?ERROR_SUCCESS:GetLastError();
    if(!agent_running() && !agent.value){result.status=agent_error==ERROR_FILE_NOT_FOUND?ReloadStatus::not_running:ReloadStatus::ipc_error;return result;}
    Handle memory(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,memory_name));
    if(!memory.value){result.status=ReloadStatus::unsupported;return result;}
    auto* shared=static_cast<Shared*>(MapViewOfFile(memory.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    if(!shared)return result;
    struct View {Shared* value;~View(){UnmapViewOfFile(value);}} view{shared};
    if(InterlockedCompareExchange(&shared->version,0,0)!=protocol){result.status=ReloadStatus::unsupported;return result;}
    const LONG64 instance=InterlockedCompareExchange64(&shared->instance,0,0);
    const DWORD pid=static_cast<DWORD>(shared->pid);
    result.instance_id=static_cast<unsigned long long>(instance);result.pid=pid;
    Handle owner(OpenProcess(SYNCHRONIZE,FALSE,pid));
    Handle client(OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,client_name));
    Handle completed(OpenEventW(SYNCHRONIZE,FALSE,done_name));
    if(!owner.value || !client.value || !completed.value)return result;
    const auto start=GetTickCount64();
    const auto remaining=[&]{const auto elapsed=GetTickCount64()-start;return elapsed>=timeout_ms?0u:timeout_ms-static_cast<unsigned>(elapsed);};
    const HANDLE locks[]{client.value,owner.value,cancel_event};
    const auto locked=WaitForMultipleObjects(cancel_event?3:2,locks,FALSE,remaining());
    if(locked!=WAIT_OBJECT_0 && locked!=WAIT_ABANDONED_0) {
        result.status=locked==WAIT_OBJECT_0+2?ReloadStatus::cancelled:locked==WAIT_OBJECT_0+1?ReloadStatus::stopped:
            locked==WAIT_TIMEOUT?ReloadStatus::timeout:ReloadStatus::ipc_error;return result;
    }
    struct Unlock {HANDLE value;~Unlock(){ReleaseMutex(value);}} unlock{client.value};
    const auto alive=[&]{return InterlockedCompareExchange(&shared->running,0,0) &&
        InterlockedCompareExchange64(&shared->instance,0,0)==instance && shared->pid==static_cast<LONG>(pid) &&
        WaitForSingleObject(owner.value,0)==WAIT_TIMEOUT;};
    if(!alive()){result.status=ReloadStatus::stopped;return result;}
    if(cancel_event && WaitForSingleObject(cancel_event,0)==WAIT_OBJECT_0){result.status=ReloadStatus::cancelled;return result;}
    if(!path || canonical(path)!=shared->mapping){result.status=ReloadStatus::configuration_mismatch;return result;}
    const auto phase=InterlockedCompareExchange(&shared->phase,0,0);
    if(phase==pending || phase==processing){result.status=ReloadStatus::busy;return result;}
    result.request_id=static_cast<unsigned long long>(InterlockedIncrement64(&shared->serial));
    shared->request_id=static_cast<LONG64>(result.request_id);shared->request_instance=instance;
    shared->revision=static_cast<LONG64>(revision);shared->fingerprint=fingerprint;
    InterlockedExchange(&shared->phase,pending);
    if(!alive()){result.status=ReloadStatus::stopped;return result;}
    // Existing path-specific reload wake is already integrated into the sole HID reader.
    if(!reload_agent(path)) {
        // Failure to wake can reclaim only an unclaimed request. A daemon that has
        // already taken it retains the processing slot until its real completion.
        if(InterlockedCompareExchange(&shared->phase,done,pending)==pending)
            shared->status=static_cast<LONG>(ReloadStatus::ipc_error);
        return result;
    }
    const HANDLE events[]{completed.value,owner.value,cancel_event};
    for(;;) {
        if(!alive()){result.status=ReloadStatus::stopped;return result;}
        if(cancel_event && WaitForSingleObject(cancel_event,0)==WAIT_OBJECT_0){result.status=ReloadStatus::cancelled;return result;}
        if(InterlockedCompareExchange(&shared->phase,0,0)==done && shared->request_id==static_cast<LONG64>(result.request_id)) {
            result.status=static_cast<ReloadStatus>(shared->status);return result;
        }
        const auto wait=WaitForMultipleObjects(cancel_event?3:2,events,FALSE,remaining());
        if(wait==WAIT_TIMEOUT){result.status=alive()?ReloadStatus::timeout:ReloadStatus::stopped;return result;}
        if(wait==WAIT_OBJECT_0+1){result.status=ReloadStatus::stopped;return result;}
        if(cancel_event && wait==WAIT_OBJECT_0+2){result.status=ReloadStatus::cancelled;return result;}
        if(wait!=WAIT_OBJECT_0){result.status=ReloadStatus::ipc_error;return result;}
    }
}

bool is_current_reload_result(const ReloadResult& result) {
    using namespace reload_ipc;
    if(result.status!=ReloadStatus::applied || !result.request_id)return false;
    Handle memory(OpenFileMappingW(FILE_MAP_READ,FALSE,memory_name));
    if(!memory.value)return false;
    auto* shared=static_cast<Shared*>(MapViewOfFile(memory.value,FILE_MAP_READ,0,0,sizeof(Shared)));
    if(!shared)return false;
    struct View {Shared* value;~View(){UnmapViewOfFile(value);}} view{shared};
    const auto load=[]<class T>(T& field){return std::atomic_ref<T>(field).load(std::memory_order_acquire);};
    const auto sequence=load(shared->commit_sequence);
    if(sequence&1)return false;
    ReloadFingerprint actual{};
    for(std::size_t i=0;i<actual.size();++i)actual[i]=load(shared->committed_fingerprint[i]);
    const bool matches=load(shared->version)==protocol && load(shared->running) &&
        load(shared->instance)==static_cast<LONG64>(result.instance_id) && load(shared->pid)==static_cast<LONG>(result.pid) &&
        load(shared->committed_id)==static_cast<LONG64>(result.request_id) &&
        load(shared->committed_revision)==static_cast<LONG64>(result.revision) && actual==result.fingerprint;
    if(!matches || load(shared->commit_sequence)!=sequence)return false;
    Handle process(OpenProcess(SYNCHRONIZE,FALSE,result.pid));
    return process.value && WaitForSingleObject(process.value,0)==WAIT_TIMEOUT;
}
