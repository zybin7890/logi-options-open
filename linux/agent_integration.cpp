#include "fixture.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <future>
#include <iostream>
using namespace loo_linux;
namespace {
void require(bool okay,const char* why){if(!okay)throw std::runtime_error(why);}
void write_file(const std::filesystem::path& path,const std::string& text){std::ofstream(path)<<text;}
std::string contents(const std::filesystem::path& path){std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};}
bool has(const std::string& value,const std::string& part){return value.find(part)!=std::string::npos;}
template<class F>void until(F check,const char* why){
    for(unsigned attempt=0;attempt<500;++attempt){try{if(check())return;}catch(...){}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    throw std::runtime_error(why);
}
// Simulated asynchronous desktop sink: stage Text until the test permits its
// dispatch. The daemon/IPC/cancellation path remains the production runtime.
struct QueuedTextHooks final:RuntimeHooks {
    std::filesystem::path base;std::vector<std::string> pending;
    explicit QueuedTextHooks(std::filesystem::path path):base(std::move(path)){}
    bool busy()const override{return !pending.empty();}
    void action(const Action& action,const std::filesystem::path&)override{
        if(action.operation!=Op::text)throw std::runtime_error("Expected fixture Text");
        pending.push_back(action.text);fixture::append(base.string()+".trace","staged "+action.text);
    }
    std::vector<std::vector<Action>> hotkeys()override{
        if(std::filesystem::exists(base.string()+".release-text")){
            for(const auto& text:pending)fixture::append(base.string()+".trace","delivered "+text);
            pending.clear();
        }
        return {};
    }
    void cancel_actions()override{
        for(const auto& text:pending)fixture::append(base.string()+".trace","cancelled "+text);
        pending.clear();
    }
};
struct Child {
    pid_t pid{-1};
    explicit Child(const std::filesystem::path& config,bool queued_text=false){
        pid=fork();if(pid<0)throw std::runtime_error("Cannot fork daemon fixture");
        if(pid==0){try{fixture::Device device(config);fixture::Input input(config);
            if(queued_text){QueuedTextHooks hooks(config);_exit(run_with(config,device,input,hooks));}
            fixture::Hooks hooks(config);_exit(run_with(config,device,input,hooks));}catch(...){_exit(1);}}
    }
    int join(){int status{};waitpid(pid,&status,0);pid=-1;return WIFEXITED(status)?WEXITSTATUS(status):255;}
    ~Child(){if(pid>0){kill(pid,SIGTERM);waitpid(pid,nullptr,0);}}
};
}
int main(){
    char temp[]="/tmp/loo-agent-test-XXXXXX";auto root=mkdtemp(temp);
    if(!root)return 1;setenv("XDG_RUNTIME_DIR",root,1);
    const auto config=std::filesystem::path(root)/"mappings.ini";
    const auto trace=config.string()+".trace",events=config.string()+".events",focus=config.string()+".focus";
    const std::string original="[global]\n0052=MouseMiddle\n0053=MouseBack\nthumb.left=VolumeDown\nthumb.right=VolumeUp\n[editor]\n0052=MouseForward\n[gestures]\n0052=MouseMiddle\n0052.left=Ctrl+C\n";
    try{
        write_file(config,original);Child child(config);
        until([]{return has(transact("agent-status"),"\"ready\":true");},"Daemon did not become ready");
        require(has(dispatch("status-json"),"Simulated HID++ mouse"),"Status did not traverse daemon IPC");
        std::vector<std::future<std::string>> clients;
        for(unsigned n=0;n<8;++n)clients.push_back(std::async(std::launch::async,[n]{return transact("set dpi "+std::to_string(1600+n*50));}));
        for(unsigned n=0;n<8;++n)require(has(clients[n].get(),"\"value\":"+std::to_string(1600+n*50)),"Concurrent reply mixed with another request");
        require(has(transact("agent-status"),"\"config_revision\":1"),"Initial revision mismatch");
        // Hold uses the profile selected on press. Changing foreground must not
        // release a different button or turn a held click into an immediate tap.
        fixture::append(events,"buttons 82");
        until([&]{return has(contents(trace),"key 274 1");},"Middle button down was not injected");
        require(!has(contents(trace),"key 274 0"),"Held mouse button was released prematurely");
        fixture::append(events,"motion 1 1");fixture::append(events,"motion 20 5");
        until([&]{return has(contents(trace),"relative 0 20")&&has(contents(trace),"relative 1 5");},"App-only gesture diversion froze normal drag motion in another app");
        write_file(focus,"editor");fixture::append(events,"buttons");
        until([&]{return has(contents(trace),"key 274 0");},"Profile switch released wrong held button");
        fixture::append(events,"buttons 82");
        until([&]{return has(contents(trace),"key 276 1");},"Application mapping did not apply");
        // Reload while held releases the old mapped button before replacement.
        write_file(config,"[global]\n0052=MouseBack\n0053=MouseBack\nthumb.left=VolumeDown\nthumb.right=VolumeUp\n");
        const auto ack=transact("reload "+config.string());
        require(has(ack,"\"reloaded\":true")&&has(ack,"\"config_revision\":2"),"Reload did not confirm applied revision");
        require(has(contents(trace),"key 276 0"),"Reload left old profile mouse button held");
        write_file(config,"[global]\nFFFE=MouseBack\n");bool rejected=false;
        try{transact("reload "+config.string());}catch(const std::exception&){rejected=true;}
        require(rejected,"Device configure failure reported reload success");
        require(has(transact("agent-status"),"\"config_revision\":2"),"Rejected config incremented revision");
        require(has(transact("agent-status"),"\"ready\":true"),"Rollback did not retain ready state");
        write_file(config,original);transact("reload "+config.string());write_file(focus,"");
        const auto action_file=std::filesystem::path(root)/"actions.ini";
        write_file(action_file,"[hotkeys]\nCtrl+Alt+F10=Ctrl+C;Wait(180);Ctrl+V\n");
        require(load_actions(action_file).size()==3,"Windows-format action container failed");
        const auto action_ack=transact("play-actions "+action_file.string());require(has(action_ack,"\"accepted\":true"),"Playback not acknowledged as queued");
        until([&]{return has(contents(trace),"key 46 1");},"First sequence step not executed");
        require(!has(contents(trace),"key 47 1"),"Wait step did not delay following chord");
        until([&]{return has(contents(trace),"key 47 1");},"Sequence did not resume after wait");
        until([]{return has(transact("agent-status"),"\"pending_actions\":0");},"Sequence remained pending");
        // Disconnect must release virtual held buttons, retain the live config,
        // and automatically reconfigure once the simulated device reappears.
        fixture::append(events,"buttons 83");
        until([&]{return has(contents(trace),"key 275 1");},"Back button was not held");
        write_file(config.string()+".offline","1");
        until([]{return has(transact("agent-status"),"\"ready\":false");},"Disconnect did not clear readiness");
        require(has(contents(trace),"key 275 0"),"Disconnect did not release held button");
        rejected=false;try{transact("set dpi 2400");}catch(...){rejected=true;}require(rejected,"Disconnected settings reported success");
        std::filesystem::remove(config.string()+".offline");
        until([]{return has(transact("agent-status"),"\"ready\":true");},"Device did not reconnect");
        require(has(transact("set dpi 2400"),"2400"),"Reconnected device not usable");
        require(has(transact("stop"),"\"stopped\":true"),"Stop not acknowledged");require(child.join()==0,"Daemon exited unsuccessfully");
        require(!running(),"Daemon left ownership lock held");
        // Reload/stop must also cancel Text already staged by RuntimeHooks,
        // not only the daemon's Pending action list.
        write_file(events,"");Child queued(config,true);
        until([]{return has(transact("agent-status"),"\"ready\":true");},"Text fixture not ready");
        write_file(action_file,"[hotkeys]\nCtrl+Alt+F10=Text(CancelOnReload)\n");
        transact("play-actions "+action_file.string());
        until([&]{return has(contents(trace),"staged CancelOnReload");},"Text was not staged");
        require(has(transact("reload "+config.string()),"\"reloaded\":true"),"Text cancellation reload failed");
        require(has(contents(trace),"cancelled CancelOnReload"),"Reload did not cancel staged desktop Text");
        write_file(config.string()+".release-text","1");
        write_file(action_file,"[hotkeys]\nCtrl+Alt+F10=Text(AfterReload)\n");
        transact("play-actions "+action_file.string());
        until([&]{return has(contents(trace),"delivered AfterReload");},"Fresh Text did not work after cancellation");
        require(!has(contents(trace),"delivered CancelOnReload"),"Cancelled Text was dispatched after reload");
        std::filesystem::remove(config.string()+".release-text");
        write_file(action_file,"[hotkeys]\nCtrl+Alt+F10=Text(CancelOnStop)\n");
        transact("play-actions "+action_file.string());
        until([&]{return has(contents(trace),"staged CancelOnStop");},"Stop Text was not staged");
        require(has(transact("stop"),"\"stopped\":true"),"Queued Text stop failed");
        require(queued.join()==0,"Queued Text daemon exited unsuccessfully");
        require(has(contents(trace),"cancelled CancelOnStop")&&!has(contents(trace),"delivered CancelOnStop"),"Stop failed to cancel staged desktop Text");
        // A restoration failure must be returned to the requester, not masked by
        // a successful stop acknowledgement.
        write_file(events,"");write_file(config,original);Child failure(config);
        until([]{return has(transact("agent-status"),"\"ready\":true");},"Second daemon not ready");
        write_file(config.string()+".restore-fail","1");rejected=false;
        try{transact("stop");}catch(...){rejected=true;}require(rejected,"Restoration failure was reported as successful stop");
        require(failure.join()!=0,"Restoration failure did not propagate exit status");
        std::filesystem::remove(config.string()+".restore-fail");
        std::cout<<"Simulated-device real IPC integration passed: concurrent settings, profile-held release, transactional reload, action timing, staged Text cancellation on reload/stop, disconnect/reconnect, restoration errors.\n";
        std::filesystem::remove_all(root);return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<" (fixture artifacts: "<<root<<")\n";return 1;}
}
