#include "agent.hpp"
#include "first_run.hpp"
#include <iostream>
#include <thread>
namespace {
bool consent(){
    if(!loo::notice_required())return true;
    std::cerr<<loo::notice_text<<"\n\nRead the notice, then run: logi-cli accept-disclaimer\n";return false;
}
void help(){
    std::cout<<"Logi Options Open - experimental Linux backend\n"
        "logi-cli disclaimer | accept-disclaimer | check-disclaimer\n"
        "logi-cli list | status | status-json | hosts | hosts-json | agent-status\n"
        "logi-cli set <dpi|smartshift|ratchet|hires|invert|thumb-invert> <value>\n"
        "logi-cli switch-host <1..3> | recover-input\n"
        "logi-cli check-config [file] | run [file] | reload [file] | stop\n"
        "logi-cli check-actions FILE | play-actions FILE | preview-ring CONFIG\n"
        "Action FILE: one [hotkeys] entry, matching the Windows/API format.\n"
        "Running daemon exclusively owns HID; commands use acknowledged local IPC.\n"
        "play-actions accepted=true means queued, not completed. Inspect agent-status\n"
        "pending_actions, last_completed_action and last_action_error for execution.\n"
        "reload succeeds only after device/configuration application.\n"
        "App profiles, global hotkeys and Text require an X11 desktop; global uinput\n"
        "mappings work independently of X11. Flow/firmware updates are unsupported.\n";
}
}
int main(int argc,char** argv){
    try{
        const std::string command=argc>1?argv[1]:"help";
        if(command=="help"||command=="--help"||command=="-h"){help();return 0;}
        if(command=="disclaimer"){std::cout<<loo::notice_text<<'\n';return 0;}
        if(command=="accept-disclaimer"){
            if(!loo::accept_notice())throw std::runtime_error("Cannot persist first-use acknowledgement");
            std::cout<<"Acknowledged third-party experimental software notice.\n";return 0;
        }
        if(command=="check-disclaimer")return consent()?0:2;
        if(command=="check-config"||command=="run"||command=="reload"){
            if(argc>3)throw std::runtime_error("Expected at most one configuration path");
            auto path=std::filesystem::absolute(argc>2?std::filesystem::path(argv[2]):loo_linux::config_path());
            const auto config=loo_linux::load(path);
            if(command=="check-config"){std::cout<<"Configuration valid\n";return 0;}
            if(!consent())return 2;
            if(command=="run")return loo_linux::run(path);
            std::cout<<loo_linux::dispatch("reload "+path.string())<<'\n';return 0;
        }
        if(command=="check-actions"||command=="play-actions"||command=="preview-ring"){
            if(argc!=3)throw std::runtime_error("Expected one file path");
            const auto path=std::filesystem::absolute(argv[2]);
            if(command=="check-actions"||command=="play-actions"){
                const auto sequence=loo_linux::load_actions(path);
                if(command=="check-actions"){std::cout<<"Action sequence valid: "<<sequence.size()<<" steps\n";return 0;}
            }
            if(!consent())return 2;
            if(command=="preview-ring"){loo_linux::preview_ring(path);std::cout<<"{\"launched\":true}\n";return 0;}
            std::cout<<loo_linux::dispatch("play-actions "+path.string())<<'\n';return 0;
        }
        const bool mutation=command=="set"||command=="switch-host";
        if(mutation&&!consent())return 2;
        if(command=="set"){
            if(argc!=4)throw std::runtime_error("Usage: logi-cli set NAME VALUE");
            const auto value=loo_linux::number(argv[3]);
            std::cout<<loo_linux::dispatch("set "+std::string(argv[2])+" "+std::to_string(value))<<'\n';return 0;
        }
        if(command=="switch-host"){
            if(argc!=3)throw std::runtime_error("Usage: logi-cli switch-host SLOT");
            std::cout<<loo_linux::dispatch("switch-host "+std::to_string(loo_linux::number(argv[2])))<<'\n';return 0;
        }
        if(command!="list"&&command!="status"&&command!="status-json"&&command!="hosts"&&command!="hosts-json"&&command!="recover-input"&&command!="stop"&&command!="agent-status"&&command!="agent-json")
            throw std::runtime_error("Unknown or unsupported Linux command: "+command);
        if(argc!=2)throw std::runtime_error("Unexpected command arguments");
        const auto result=loo_linux::dispatch(command);
        if(command=="stop"||command=="recover-input"){
            for(unsigned n=0;n<100&&loo_linux::running();++n)std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if(loo_linux::running())throw std::runtime_error("Daemon acknowledged shutdown but has not released device ownership");
        }
        std::cout<<result<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"Error: "<<e.what()<<'\n';return 1;}
}
