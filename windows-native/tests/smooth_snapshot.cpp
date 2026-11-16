#define LOGI_TEST_SETTINGS_SNAPSHOT
#include "environment.hpp"
#include <io.h>
#define wmain unused_cli_entry
#include "../main.cpp"
#undef wmain
namespace logi {
bool switch_host(Mouse&,unsigned){throw std::runtime_error("unexpected host switch");}
std::vector<HostSlot> hosts(Mouse&){throw std::runtime_error("unexpected host enumeration");}
}

int main(int argc,char** argv){try{
    CHECK(argc==2);settings_fixture=true;fixture_path=argv[1];
    settings_ipc::Host host(fixture_path.c_str());setting_commands=&host;
    SettingResult settings;settings.observed={40,1,1,0,1};settings.observed_mask=31;settings.software=true;
    host.observe(settings,true,true);host.observe_wheel(3);
    auto snapshot=read_agent_settings();CHECK(snapshot.available&&snapshot.ready&&snapshot.saved);
    CHECK(snapshot.observed==settings.observed&&snapshot.observed_mask==31&&snapshot.software_mask==16);
    CHECK(snapshot.wheel_mode_valid&&snapshot.wheel_mode==3&&snapshot.wheel_mode_sampled_at_ms==100);
    auto& shared=*reinterpret_cast<settings_ipc::Shared*>(settings_buffer);
    ++shared.snapshot_sequence;CHECK(!read_agent_settings().available);++shared.snapshot_sequence;
    const auto version=shared.version;shared.version=1;CHECK(!read_agent_settings().available);shared.version=version;
    tick=120;host.observe_wheel(6);snapshot=read_agent_settings();CHECK(snapshot.wheel_mode==6&&snapshot.wheel_mode_sampled_at_ms==120);
    CHECK(snapshot.observed==settings.observed&&snapshot.sampled_at_ms[0]==100);
    host.observe_wheel({});snapshot=read_agent_settings();CHECK(snapshot.available&&!snapshot.wheel_mode_valid);
    CHECK(snapshot.observed==settings.observed);host.observe_wheel(3);

    logi::Mouse mouse;mouse.device={1,2,0,"MX Master 3S"};mouse.battery=10;mouse.host=11;mouse.dpi_choices={1000,5000};
    get_override=[](logi::Mouse&,std::wstring_view key)->std::optional<unsigned>{
        if(key==L"dpi")return 5000;if(key==L"host")return 2;if(key==L"smartshift")return 40;
        if(key==L"ratchet"||key==L"hires")return 1;if(key==L"invert")return 0;
        throw std::runtime_error("unexpected direct setting read");
    };
    mouse_request_result=[](logi::Byte feature,logi::Byte function,std::span<const logi::Byte>)->std::optional<logi::Reply>{
        CHECK(feature==10&&function==0);logi::Reply reply{logi::Result::ok};reply.packet.size=20;reply.packet.bytes[4]=87;return reply;
    };
    auto output=fixture_path;output+=L".json";
    std::fflush(stdout);const auto saved=_dup(_fileno(stdout));CHECK(saved>=0);FILE* file=_wfopen(output.c_str(),L"wb");CHECK(file);
    CHECK(_dup2(_fileno(file),_fileno(stdout))==0);print_status(mouse);std::fflush(stdout);
    CHECK(_dup2(saved,_fileno(stdout))==0);_close(saved);std::fclose(file);
    std::ifstream in(output);const std::string json{std::istreambuf_iterator<char>(in),{}};
    for(const auto* fragment:{"\"battery\":87","\"dpi\":5000","\"host\":2","\"thumb-invert\":1",
          "\"dpi_choices\":[1000,5000]","\"wheel_mode\":3","\"wheel_target\":1","\"wheel_mode_sampled_at_ms\":120"})
        CHECK(json.find(fragment)!=std::string::npos);
    CHECK(json.ends_with("}\n"));host.shutdown();CHECK(!read_agent_settings().available);setting_commands=nullptr;
    std::cout<<"PASS owner wheel snapshot validity/time/version and existing status-json battery/DPI/host/direction compatibility\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
