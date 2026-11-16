#pragma once
#include "action_context.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace slow_actions {
enum class Status { pending, accepted, failed, cancelled };
struct Task {
    std::stop_source cancel;
    std::atomic<Status> status{Status::pending};
    ActionContext context;
    std::function<bool(const ActionContext&)> perform;
};
struct Queue {
    std::mutex mutex;
    std::deque<std::shared_ptr<Task>> tasks;
    bool running{};
};
inline std::shared_ptr<Task> submit(ActionContext context,std::function<bool(const ActionContext&)> perform) {
    // One lazy worker serializes system side effects across event-loop reloads.
    // Its copies never reference a Mouse, Mappings, Run, or event-loop object.
    static const auto queue=std::make_shared<Queue>();
    auto task=std::make_shared<Task>();
    context.stop=task->cancel.get_token();task->context=std::move(context);task->perform=std::move(perform);
    std::lock_guard lock(queue->mutex);
    queue->tasks.push_back(task);
    if(queue->running)return task;
    queue->running=true;
    try {
        std::thread([state=queue] {
            for(;;) {
                std::shared_ptr<Task> item;
                {
                    std::lock_guard lock(state->mutex);
                    if(state->tasks.empty()){state->running=false;return;}
                    item=std::move(state->tasks.front());state->tasks.pop_front();
                }
                auto result=Status::cancelled;
                try {
                    if(item->context.active()) {
                        const bool accepted=item->perform(item->context);
                        result=item->cancel.stop_requested()?Status::cancelled:
                            accepted?Status::accepted:item->context.active()?Status::failed:Status::cancelled;
                    }
                } catch(...) {result=Status::failed;}
                item->status.store(result,std::memory_order_release);
            }
        }).detach();
    } catch(...) {
        queue->running=false;queue->tasks.pop_back();task->status.store(Status::failed,std::memory_order_release);
    }
    return task;
}
}
