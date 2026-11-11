#include "agent.hpp"
#include "mapping.hpp"
#include "mouse.hpp"
#include "first_run.hpp"
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::string json(std::string_view value) {
    std::string out="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value) {if(c=='"' || c=='\\'){out+='\\';out+=c;}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=c;}
    return out+'"';
}
std::wstring wide(std::string_view value){return {value.begin(),value.end()};}
void status(logi::Mouse& mouse) {
    std::cout<<"{\"device\":"<<json(mouse.device.name)<<",\"platform\":\"linux\",\"battery\":";
    auto battery=mouse.request(mouse.battery,mouse.unified_battery?1:0);
    if(battery)std::cout<<unsigned(battery.packet.data()[0]);else std::cout<<"null";
    std::cout<<",\"dpi_choices\":[";
    bool first=true;for(auto dpi:mouse.dpi_choices){if(!first)std::cout<<',';first=false;std::cout<<dpi;}
    std::cout<<"],\"settings\":{";first=true;
    for(auto name:{"dpi","smartshift","ratchet","hires","invert","thumb-invert","host"}) {
        if(!first)std::cout<<',';first=false;std::cout<<json(name)<<':';
        auto value=logi::get(mouse,wide(name));if(value)std::cout<<*value;else std::cout<<"null";
    }
    std::cout<<"}}\n";
}
bool consent() {
    if(!loo::notice_required())return true;
    std::cerr<<loo::notice_text<<"\n\nRead the notice, then run: logi-cli accept-disclaimer\n";return false;
}
void help() {
    std::cout<<"Logi Options Open - experimental Linux backend\n"
        "logi-cli disclaimer | accept-disclaimer | check-disclaimer\n"
        "logi-cli list | status | status-json | hosts | hosts-json | agent-status\n"
        "logi-cli set <dpi|smartshift|ratchet|hires|invert|thumb-invert> <value>\n"
        "logi-cli switch-host <1..3> | recover-input\n"
        "logi-cli check-config [file] | run [file] | reload [file] | stop\n"
        "Default mappings: $LOGI_OPTIONS_OPEN_CONFIG or $XDG_CONFIG_HOME/logi-options-open/mappings.ini\n"
        "Global mappings only. App profiles, global trigger hotkeys, text injection, Ring and Flow are not supported on Linux.\n";
}
}
int main(int argc,char** argv) {
    try {
        const std::string command=argc>1?argv[1]:"help";
        if(command=="help" || command=="--help" || command=="-h"){help();return 0;}
        if(command=="disclaimer"){std::cout<<loo::notice_text<<'\n';return 0;}
        if(command=="accept-disclaimer") {
            if(!loo::accept_notice())throw std::runtime_error("Cannot persist first-use acknowledgement");
            std::cout<<"Acknowledged third-party experimental software notice.\n";return 0;
        }
        if(command=="check-disclaimer")return consent()?0:2;
        if(command=="agent-status" || command=="agent-json") {std::cout<<"{\"native\":"<<(loo_linux::running()?"true":"false")<<",\"options_plus\":false,\"platform\":\"linux\"}\n";return 0;}
        if(command=="check-config" || command=="run" || command=="reload") {
            auto path=std::filesystem::absolute(argc>2?std::filesystem::path(argv[2]):loo_linux::config_path());
            auto config=loo_linux::load(path);
            if(command=="check-config"){std::cout<<"Configuration valid: "<<config.bindings.size()<<" mappings\n";return 0;}
            if(!consent())return 2;
            if(command=="run")return loo_linux::run(path);
            if(!loo_linux::control("reload "+path.string()))throw std::runtime_error("Linux daemon is not running or reload request failed");
            std::cout<<"{\"reload_requested\":true}\n";return 0;
        }
        if(command=="stop") {
            if(loo_linux::running() && !loo_linux::control("stop"))throw std::runtime_error("Cannot send stop request");
            for(unsigned n=0;n<100 && loo_linux::running();++n)std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if(loo_linux::running())throw std::runtime_error("Daemon is still stopping; input restoration has not completed");
            std::cout<<"{\"stopped\":true}\n";return 0;
        }
        const bool mutation=command=="set" || command=="switch-host";
        if(mutation && !consent())return 2;
        if(command!="list" && command!="status" && command!="status-json" && command!="hosts" && command!="hosts-json" && command!="recover-input" && !mutation)
            throw std::runtime_error("Unknown or unsupported Linux command: "+command);
        if(command=="set" && argc!=4)throw std::runtime_error("Usage: logi-cli set <setting> <value>");
        if(command=="switch-host" && argc!=3)throw std::runtime_error("Usage: logi-cli switch-host <1..3>");
        if((command=="recover-input" || command=="switch-host" || (command=="set" && std::string_view(argv[2])=="thumb-invert")) && loo_linux::running())
            throw std::runtime_error("Stop the Linux daemon before changing host, restoring input, or changing thumb inversion");
        if(command=="list"){logi::connect(nullptr,true);return 0;}
        auto mouse=logi::connect();if(!mouse)throw std::runtime_error("No accessible HID++ mouse; check hidraw permissions and connection");
        if(command=="status-json"){status(*mouse);return 0;}
        if(command=="status"){logi::describe(*mouse);return 0;}
        if(command=="hosts" || command=="hosts-json") {
            const auto slots=logi::hosts(*mouse);if(slots.empty())throw std::runtime_error("Host switching not supported by device");
            std::cout<<'[';bool first=true;
            for(const auto& slot:slots){if(!first)std::cout<<',';first=false;std::cout<<"{\"slot\":"<<slot.slot<<",\"current\":"<<(slot.current?"true":"false")<<",\"paired\":"<<(slot.paired?(*slot.paired?"true":"false"):"null")<<'}';}
            std::cout<<"]\n";return 0;
        }
        if(command=="recover-input") {
            if(!logi::restore_standard_input(*mouse))throw std::runtime_error("Device did not acknowledge complete input recovery");
            std::cout<<"Standard input restored.\n";return 0;
        }
        if(command=="switch-host") {
            if(!logi::switch_host(*mouse,loo_linux::number(argv[2])))throw std::runtime_error("Host switch failed or target pairing unavailable");
            std::cout<<"Host switch sent; verify the target host.\n";return 0;
        }
        const auto name=wide(argv[2]);const auto value=loo_linux::number(argv[3]);
        if(!logi::set(*mouse,name,value))throw std::runtime_error("Unsupported setting/value or HID++ write failure");
        auto checked=logi::get(*mouse,name);
        if(!checked || *checked!=value)throw std::runtime_error("Setting write did not match device readback");
        std::cout<<"{\"setting\":"<<json(argv[2])<<",\"value\":"<<value<<"}\n";return 0;
    } catch(const std::exception& e){std::cerr<<"Error: "<<e.what()<<'\n';return 1;}
}
