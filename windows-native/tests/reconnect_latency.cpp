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
    unsigned restoration_writes{},thumb_restores{},backoffs{},faults{},ready_checks{};
    std::vector<unsigned> restored_controls;
    observe_wait=[&](HANDLE event,DWORD duration) {
        if(event==reinterpret_cast<HANDLE>(101) && duration==2000 && !stopped)++backoffs;
        if(scenario=="ready-stop"&&stopped&&health)CHECK(health->ready==0);
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
    else if(scenario.starts_with("ready-")) {
        mode="ready-publication";
        get_override=[&](logi::Mouse&,std::wstring_view key)->std::optional<unsigned> {
            CHECK(key==L"dpi"&&health&&health->ready==0&&health->mappings_ready==0);
            tick+=1200;
            if(scenario=="ready-stop")stopped=true;
            return scenario=="ready-unknown"?std::optional<unsigned>{}:std::optional<unsigned>{actual_dpi};
        };
        observe_lastinput=[&] {
            if(!ready_checks++)CHECK(health&&health->ready==0&&health->mappings_ready==0);
        };
    }
    else if(scenario.starts_with("health-")) {
        Step active;active.at=3100;
        Step end;end.at=4400;end.idle=true;
        steps={active,end};respect_read_deadlines=true;expected_connections=2;
        if(scenario=="health-timeout"||scenario=="health-timeout-input")expected_connections=1;
        if(scenario=="health-timeout-input") {
            mode="async-health-timeout";
            Step press;press.at=3200;press.down=true;
            Step release;release.at=3300;
            steps={active,press,release,end};
        }
        if(scenario=="health-protocol"){expected_restores=10;expected_backoffs=1;}
        channel_request_result=[&](logi::Byte feature,logi::Byte function,std::span<const logi::Byte> parameters)->std::optional<logi::Reply> {
            if(feature!=7)return {};
            if(scenario=="health-write-timeout") {
                if(function==2&&!faults) {
                    logi::Reply reply{logi::Result::ok};reply.packet.size=20;
                    reply.packet.bytes[4]=parameters[0];reply.packet.bytes[5]=parameters[1];
                    return reply; // Existing reporting flag lost: force one repair write.
                }
                if(function==3){++faults;return logi::Reply{logi::Result::timeout};}
                return {};
            }
            if(function!=2)return {};
            const auto misses=scenario=="health-timeout-persistent"?2u:1u;
            if(faults++>=misses)return {};
            if(scenario.starts_with("health-timeout"))return logi::Reply{logi::Result::timeout};
            if(scenario=="health-io-error")return logi::Reply{logi::Result::io_error};
            CHECK(scenario=="health-protocol");
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
    if(scenario=="health-timeout-input") {
        CHECK(captured.size()==2&&captured[0].ki.wVk=='A'&&captured[1].ki.wVk=='A');
        CHECK(capture_times[0]==3200&&capture_times[1]==3200);
    } else CHECK(captured.empty());
    if(scenario=="health-timeout"||scenario=="health-timeout-input") {
        CHECK(observed_ready==1&&observed_mappings==1&&observed_failures==0);
        CHECK(async_begins>=7); // Retried read + five controls + DPI.
    }
    if(scenario=="health-timeout-persistent")CHECK(faults==2);
    if(scenario=="health-write-timeout")CHECK(faults==1);
    if(scenario.starts_with("ready-")) {
        CHECK(ready_checks==1);
        if(scenario=="ready-stop")CHECK(read_calls==0);
        else {
            CHECK(observed_mappings==1&&observed_ready==(scenario=="ready-unknown"?0:1));
            CHECK(observed_checked==1300);
        }
    }
    std::cout<<"PASS production reconnect "<<scenario<<": connects="<<connects
             <<" restore-writes="<<restoration_writes<<" backoffs="<<backoffs<<'\n';
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;} }
