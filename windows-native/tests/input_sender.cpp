#include "../input_sender.hpp"
#include <cstdio>
#include <deque>
#include <optional>
#include <vector>

using namespace input_sender;
namespace {
struct Receipt { bool worker; ULONG_PTR marker; };
struct State {
    DWORD caller{GetCurrentThreadId()};
    ULONGLONG clock{100};
    bool allowed{true},different_focus{},cancel_worker{},stopped{};
    unsigned verifications{};
    std::deque<Result> direct,worker;
    std::vector<Receipt> writes;
    std::vector<Change> transitions;
};
// All submissions wait for completion. The router's condition variable gives
// each fixture update a happens-before edge across the real thread handoff.
struct Platform {
    State* s;
    bool worker() const {return s->caller!=GetCurrentThreadId();}
    bool stopped() const {return s->stopped || (worker()&&s->cancel_worker);}
    ULONGLONG now() const {return s->clock;}
    std::optional<Foreground> verify(Target target,Skip& reason) const {
        ++s->verifications;
        if(!s->allowed || !target.window)return {};
        if(target.pid!=1){reason=Skip::foreground_changed;return {};}
        return Foreground{(worker()&&s->different_focus)?HWND_DESKTOP:target.window,1};
    }
    Result write(UINT count,INPUT* input,int) const {
        auto& queue=worker()?s->worker:s->direct;
        s->writes.push_back({worker(),input[0].mi.dwExtraInfo});
        if(queue.empty())return {count,ERROR_SUCCESS};
        const auto result=queue.front();queue.pop_front();return result;
    }
    void changed(Change change,unsigned) const {s->transitions.push_back(change);}
};
struct Fixture {
    State s;
    Router<Platform> router{Platform{&s}};
    Result action(unsigned serial,ULONG_PTR marker,UINT count=1,DWORD pid=1) {
        INPUT input[4]{};
        input[0].mi.dwExtraInfo=marker;
        return router.submit({serial,{HWND_MESSAGE,pid}},count,input,sizeof(INPUT));
    }
    void deny_next(){s.direct.push_back({0,ERROR_ACCESS_DENIED});}
};
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x);return false;} } while(false)
bool next_action() {
    Fixture f;
    CHECK(f.action(1,10).sent==1 && f.s.verifications==0);
    f.deny_next();
    CHECK(f.action(2,20).sent==0 && f.router.generation()==0 && f.s.writes.size()==2);
    CHECK(f.action(2,21).sent==1 && !f.s.writes.back().worker);
    CHECK(f.action(3,30,4).sent==4 && f.router.generation()==1 && f.s.writes.back().worker);
    CHECK(f.action(4,40).sent==1 && f.router.generation()==1);
    const std::vector<Change> expected{Change::armed,Change::recovered};
    CHECK(f.s.transitions==expected && f.s.writes.size()==5);
    CHECK(f.s.writes[1].marker==20 && f.s.writes[2].marker==21 && f.s.writes[3].marker==30);
    return true;
}
bool ineligible() {
    Fixture f;
    f.s.allowed=false;f.deny_next();
    const auto rejected=f.action(1,10);
    CHECK(rejected.sent==0 && rejected.error==ERROR_ACCESS_DENIED && rejected.skip==Skip::none && f.s.transitions.empty());
    f.s.allowed=true;f.s.direct.push_back({2,ERROR_ACCESS_DENIED});
    CHECK(f.action(2,20,4).sent==2 && f.s.transitions.empty());
    f.s.direct.push_back({0,ERROR_INVALID_HANDLE});
    CHECK(f.action(3,30).sent==0 && f.s.transitions.empty());
    f.deny_next();CHECK(f.action(0,40).sent==0 && f.s.transitions.empty());
    f.deny_next();CHECK(f.action(5,50).sent==0);
    f.s.allowed=false;
    CHECK(f.action(6,60).skip==Skip::unverified);
    f.s.allowed=true;
    CHECK(f.action(6,61,1,2).skip==Skip::foreground_changed);
    f.s.allowed=true;f.s.different_focus=true;
    CHECK(f.action(7,70).skip==Skip::foreground_changed && f.router.generation()==0);
    f.s.different_focus=false;f.s.cancel_worker=true;
    CHECK(f.action(8,80).skip==Skip::stopping && f.router.generation()==0);
    f.s.cancel_worker=false;f.s.stopped=true;
    CHECK(f.action(9,90).skip==Skip::stopping && f.s.writes.size()==5);
    return true;
}
bool partial_worker() {
    Fixture f;f.deny_next();
    CHECK(f.action(1,10).sent==0);
    f.s.worker.push_back({1,ERROR_ACCESS_DENIED});
    CHECK(f.action(2,20,4).sent==1 && f.router.generation()==0);
    CHECK(f.action(2,21).sent==1 && f.router.generation()==0);
    CHECK(f.action(3,30).sent==1 && f.router.generation()==1);
    CHECK(f.s.writes.size()==4 && f.s.writes[1].marker==20 && f.s.writes[2].marker==21);
    return true;
}
bool cooldown() {
    Fixture f;f.deny_next();
    CHECK(f.action(1,10).sent==0);
    CHECK(f.action(2,20).sent==1 && f.router.generation()==1);
    f.s.worker.push_back({0,ERROR_ACCESS_DENIED});
    CHECK(f.action(3,30).sent==0 && f.router.generation()==1);
    f.deny_next();
    CHECK(f.action(4,40).sent==0 && !f.s.writes.back().worker);
    f.s.clock+=30000;
    f.s.worker.push_back({0,ERROR_ACCESS_DENIED});
    CHECK(f.action(5,50).sent==0 && f.s.writes.back().worker);
    f.s.clock+=60000;f.deny_next();
    CHECK(f.action(6,60).sent==0 && !f.s.writes.back().worker && f.s.transitions.size()==4);
    f.deny_next();
    CHECK(f.action(7,70).sent==0 && !f.s.writes.back().worker);
    CHECK(f.action(8,80).sent==1 && !f.s.writes.back().worker);
    f.deny_next();CHECK(f.action(9,90).sent==0);
    CHECK(f.action(10,100).sent==1 && f.s.writes.back().worker && f.router.generation()==2);
    return true;
}
}
int main(){return next_action()&&ineligible()&&partial_worker()&&cooldown()?0:1;}
