#include "environment.hpp"

int main(int argc,char** argv) { try {
    CHECK(argc==3);
    const std::string scenario=argv[1];fixture_path=argv[2];restore_ok=true;mode="reconnect-latency";
    const bool restore_fault=scenario=="restore-timeout" || scenario=="restore-io-error";
    if(restore_fault)mode="async-repair-thumb"; // Existing fixture exposes the real thumb-wheel setup/restore path.
    {
        std::ofstream config(fixture_path);
        config<<"[global]\n0052=A\n0053=B\n0056=C\n00C3=D\n00C4=E\n";
        if(restore_fault)config<<"thumb.left=VolumeDown\nthumb.right=VolumeUp\n";
    }
    unsigned restoration_writes{},thumb_restores{},backoffs{},faults{};
    std::vector<unsigned> restored_controls;
    observe_wait=[&](HANDLE event,DWORD duration) {
        if(event==reinterpret_cast<HANDLE>(101) && duration==2000 && !stopped)++backoffs;
    };
    mouse_request_result=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> data)->std::optional<logi::Reply> {
        if(feature==7 && function==3 && data.size()==3 && data[2]==0x23) {
            ++restoration_writes;
            if(restore_fault && restoration_writes==1)
                return logi::Reply{scenario=="restore-timeout"?logi::Result::timeout:logi::Result::io_error};
            restored_controls.push_back(logi::word(data));
        }
        if(feature==6 && function==2 && data.size()==2 && data[0]==0)++thumb_restores;
        return {};
    };
    Step lost;lost.disconnect=true;
    unsigned expected_connections=1,expected_restores=5,expected_backoffs=0;
    if(scenario=="disconnect") {steps={lost};expected_connections=2;}
    else if(scenario=="repeated-loss") {steps={lost,lost,lost};expected_connections=4;expected_backoffs=2;}
    else if(scenario=="input-resets-retry") {steps={lost,{},lost};expected_connections=3;}
    else if(scenario=="failed-probe") {
        steps={lost};expected_connections=3;expected_backoffs=1;
        accept_connection=[](unsigned attempt){return attempt!=2;};
    }
    else if(scenario=="health-timeout" || scenario=="health-protocol") {
        Step active;active.at=3100;steps={active};expected_connections=2;
        if(scenario=="health-protocol"){expected_restores=10;expected_backoffs=1;}
        channel_request_result=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte>)->std::optional<logi::Reply> {
            if(feature!=7 || function!=2 || faults++)return {};
            if(scenario=="health-timeout")return logi::Reply{logi::Result::timeout};
            logi::Reply malformed{logi::Result::ok};malformed.packet.size=20;return malformed;
        };
    }
    else if(restore_fault) {}
    else if(scenario!="stop")throw std::runtime_error("unknown reconnect case");
    CHECK(run_agent(fixture_path.c_str(),false)==0);
    CHECK(connects==expected_connections);
    CHECK(restoration_writes==expected_restores);
    if(restore_fault) {
        CHECK((restored_controls==std::vector<unsigned>{0x53,0x56,0xc3,0xc4}));
        CHECK(thumb_restores==1 && report_thumb==0);
    }
    CHECK(backoffs==expected_backoffs);
    CHECK(captured.empty());
    std::cout<<"PASS production reconnect "<<scenario<<": connects="<<connects
             <<" restore-writes="<<restoration_writes<<" backoffs="<<backoffs<<'\n';
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;} }
