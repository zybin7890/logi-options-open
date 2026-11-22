#pragma once
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <thread>

namespace input_sender {
struct Target { HWND window{}; DWORD pid{}; };
struct Action { std::uint64_t serial{}; Target target; };
struct Result { UINT sent{}; DWORD error{}; bool attempted{true}; };
struct Foreground { HWND focus{}; DWORD thread{},focus_thread{},focus_pid{}; bool operator==(const Foreground&) const=default; };
enum class Change { armed, recovered, retired };

// A single owner submits a bounded synchronous call. Only its *next new action*
// can start a worker; no failed input is retained or queued for later playback.
template<class Platform> class Router {
    Platform platform_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread thread_;
    struct Job { Target target; Foreground foreground; UINT count; INPUT* data; int size; Result result{}; bool done{}; };
    Job* pending_{};
    bool stopping_{},armed_{},confirmed_{},inhibited_{};
    std::uint64_t failed_serial_{},next_create_{},partial_serial_{};
    unsigned generation_{};
    static bool denied(Result r){return r.attempted && !r.sent && r.error==ERROR_ACCESS_DENIED;}

    void run() {
        std::unique_lock lock(mutex_);
        for(;;){
            ready_.wait(lock,[&]{return stopping_||pending_;});
            if(stopping_)return;
            auto& job=*pending_;
            lock.unlock();
            const auto foreground=platform_.verify(job.target);
            // Both threads must still be ordinary input-desktop threads. The
            // foreground window, PID, owning thread and focus child must match.
            const auto result=foreground && *foreground==job.foreground && !platform_.stopped() ?
                platform_.write(job.count,job.data,job.size):Result{0,ERROR_CANCELLED,false};
            lock.lock();
            job.result=result;job.done=true;pending_=nullptr;ready_.notify_all();
        }
    }
    void retire() noexcept {
        if(!thread_.joinable())return;
        {std::lock_guard lock(mutex_);stopping_=true;}
        ready_.notify_all();thread_.join();stopping_=false;
    }
public:
    explicit Router(Platform platform):platform_(platform){}
    ~Router(){retire();}
    Router(const Router&)=delete;
    unsigned generation() const noexcept{return generation_;}
    Result submit(Action action,UINT count,INPUT* inputs,int size) {
        const auto direct=[&] {
            auto result=platform_.write(count,inputs,size);
            if(result.sent==count)inhibited_=false;
            if(!armed_ && !thread_.joinable() && !inhibited_ && action.serial && denied(result) &&
               !platform_.stopped() && platform_.verify(action.target)) {
                failed_serial_=action.serial;armed_=true;platform_.changed(Change::armed,generation_);
            }
            return result;
        };
        if((!armed_ && !thread_.joinable()) || !action.serial || !action.target.window || !action.target.pid ||
           action.serial<=failed_serial_)return direct();
        if(platform_.stopped())return {0,ERROR_CANCELLED,false};
        if(!thread_.joinable() && platform_.now()<next_create_)return direct();
        const auto foreground=platform_.verify(action.target);
        if(!foreground)return {0,ERROR_CANCELLED,false};
        if(!thread_.joinable()) {
            next_create_=platform_.now()+30000;
            try {thread_=std::thread([this]{run();});}
            catch(const std::system_error&){inhibited_=true;armed_=false;return direct();}
            armed_=false;confirmed_=false;partial_serial_=0;
        }
        Job job{action.target,*foreground,count,inputs,size};
        {
            std::unique_lock lock(mutex_);
            pending_=&job;ready_.notify_all();
            ready_.wait(lock,[&]{return job.done;});
        }
        if(job.result.sent && job.result.sent<count)partial_serial_=action.serial;
        if(job.result.sent==count && count && !confirmed_ && action.serial!=partial_serial_){
            confirmed_=true;++generation_;platform_.changed(Change::recovered,generation_);
        } else if(denied(job.result)){
            retire();failed_serial_=action.serial;armed_=confirmed_;inhibited_=!confirmed_;
            confirmed_=false;platform_.changed(Change::retired,generation_);
        }
        return job.result;
    }
};
} // namespace input_sender
