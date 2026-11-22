#define LOGI_TEST_REAL_MOUSE
#include "environment.hpp"

namespace test_routes {extern unsigned writes;extern std::wstring value;}
namespace discovery_fixture {
std::vector<unsigned> requested;
unsigned unavailable{};
bool dpi_only{};
bool identified{};
unsigned rounds{};
std::map<unsigned,std::vector<unsigned>> queried_by_round;
constexpr auto path=L"test-receiver-interface";
}
namespace logi {
std::vector<Interface> enumerate(){++discovery_fixture::rounds;return {{discovery_fixture::path,0xc548,0xff00,1,20,20,true}};}
std::optional<Device> probe(Channel&,Byte slot,DWORD,bool){
    if(slot==1)return Device{slot,2,0,"MX Master 3S"};
    return {};
}
std::string read_device_name(Channel&,Byte slot,bool){CHECK(slot==1);return "MX Master 3S";}
Byte feature(Channel&,Byte slot,unsigned id,DWORD timeout){
    CHECK(slot==1);discovery_fixture::requested.push_back(id);
    discovery_fixture::queried_by_round[discovery_fixture::rounds].push_back(id);
    CHECK(timeout==1200 || timeout==180);
    if(id==discovery_fixture::unavailable)return 0;
    if(id==0x2201)return 9;
    if(discovery_fixture::dpi_only)return 0;
    if(discovery_fixture::identified){
        if(id==0x0003)return 3;
        if(id==0x2111)return 8;
        if(id==0x1004)return 10;
    }
    switch(id){case 0x1b04:return 7;case 0x2121:return 5;case 0x2150:return 6;default:return 0;}
}
}

namespace {
using logi::Byte;
struct Device {
    static constexpr std::array<unsigned,6> ids{0x50,0x52,0x53,0x56,0xc3,0xc4};
    unsigned calls{},ignored_write{},metadata_reads{},reject_read{},count{ids.size()};
    bool duplicate_info{},no_divertable{};
    std::map<unsigned,unsigned> report_reads;
    Byte main_mode{2},thumb_mode{},thumb_flags{1};
    std::map<unsigned,Byte> flags,extended;
    std::map<unsigned,unsigned> targets;
    logi::Reply request(Byte feature,Byte function,std::span<const Byte> parameters) {
        ++calls;
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        auto* data=reply.packet.bytes.data()+4;
        if(feature==5||feature==6) {
            if(function==0) {
                if(feature==5)data[0]=8;
                else {data[1]=18;data[3]=120;}
            } else if(function==1) {
                data[0]=feature==5?main_mode:thumb_mode;
                if(feature==6)data[1]=thumb_flags;
            } else {
                CHECK(function==2);
                if(feature==5)main_mode=parameters[0];
                else {thumb_mode=parameters[0];thumb_flags=parameters[1];}
            }
        } else {
            CHECK(feature==7);
            if(function==0)data[0]=static_cast<Byte>(count);
            else if(function==1) {
                ++metadata_reads;
                const auto id=ids.at(duplicate_info && parameters[0]==1 ? 0 : parameters[0]);
                data[0]=logi::high(id);data[1]=logi::low(id);
                data[4]=no_divertable || id==0x50?0:0x20;data[8]=id==0xc3;
            } else {
                const auto id=logi::word(parameters);
                if(function==3) {
                    if(ignored_write!=id)flags[id]=(flags[id]&~0x11)|(parameters[2]&0x11);
                } else {
                    CHECK(function==2);++report_reads[id];
                    if(reject_read==id){reject_read=0;return {logi::Result::device_error};}
                    data[0]=parameters[0];data[1]=parameters[1];
                    data[2]=flags[id];data[3]=logi::high(targets[id]);data[4]=logi::low(targets[id]);data[5]=extended[id];
                }
            }
        }
        return reply;
    }
};
void verify(bool stale,bool lose_write) {
    std::istringstream text("[global]\n0052=MouseMiddle\n0053=MouseBack\n0056=MouseForward\n00c3=A\n00c3.left=A\n00c4=B\n");
    Mappings mappings;CHECK(parse_mappings(text,mappings));
    unsigned baseline{};
    for(bool reuse:{false,true}) {
        Device model;
        if(stale) {
            model.main_mode=3;model.thumb_mode=1;model.flags[0xc3]=0x11;
            model.targets[0x53]=0x56;model.extended[0x52]=1;
        }
        channel_request_result=[&](Byte f,Byte fn,std::span<const Byte> p){return model.request(f,fn,p);};
        logi::Mouse mouse{};mouse.device.name="MX Master 3S";mouse.wheel=5;mouse.thumb=6;mouse.buttons=7;
        mouse.channel=std::make_unique<logi::Channel>(logi::Interface{});
        logi::StandardReporting state;
        CHECK(logi::restore_standard_input(mouse,reuse?&state:nullptr));
        CHECK(model.main_mode==2&&model.thumb_mode==0&&model.thumb_flags==1&&model.flags[0xc3]==0);
        if(lose_write)model.ignored_write=0xc4;
        std::vector<Control> controls;std::optional<Wheel> thumb;std::optional<MainWheel> main;
        const bool success=enable(mouse,mappings.bindings,controls,state.controls);
        CHECK(success==!lose_write);
        if(!lose_write) {
            CHECK(divert_wheel(mouse,thumb,80,state.thumb)&&divert_main(mouse,main,true,state.wheel));
            CHECK(model.main_mode==3&&model.thumb_mode==1&&model.thumb_flags==0);
            CHECK(model.flags[0xc3]==0x11&&model.flags[0xc4]==1);
            CHECK(model.flags[0x53]==stale&&model.flags[0x52]==stale&&model.flags[0x56]==0);
            if(!reuse)baseline=model.calls;
            else {
                CHECK(model.calls+7==baseline);
                std::printf("%s controls and wheels: %u -> %u HID transactions before cleanup\n",stale?"altered":"standard",baseline,model.calls);
            }
        }
        CHECK(restore(mouse,controls,thumb,true,main));
        CHECK(model.main_mode==2&&model.thumb_mode==0&&model.thumb_flags==1);
        for(auto id:Device::ids)CHECK(model.flags[id]==0);
        if(stale)CHECK(model.targets[0x53]==0x56&&model.extended[0x52]==1);
    }
    channel_request_result={};
}

void discovery() {
    const std::vector<unsigned> required{0x2201,0x1b04,0x2121,0x2150};
    Device model;
    unsigned dpi_lists{};
    channel_request_result=[&](Byte f,Byte fn,std::span<const Byte> p)->logi::Reply {
        if(f!=9)return model.request(f,fn,p);
        CHECK(fn==1);++dpi_lists;
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        reply.packet.bytes[5]=3;reply.packet.bytes[6]=0xe8;return reply;
    };
    logi::discovery::OwnerSearch search;
    for(unsigned i=0;i<required.size();++i) {
        search={};discovery_fixture::requested.clear();discovery_fixture::unavailable=required[i];
        CHECK(!logi::connect(nullptr,false,false,&search));
        std::vector<unsigned> checked;
        for(const auto id:discovery_fixture::requested)if(std::ranges::contains(required,id))checked.push_back(id);
        CHECK(checked==std::vector<unsigned>(required.begin(),required.begin()+i+1));
        CHECK(!test_routes::writes && !model.calls && !dpi_lists);
    }
    // The next owner round must discover the same physical fixture once its features appear.
    discovery_fixture::unavailable=0;tick+=5000;
    auto connected=logi::connect(nullptr,false,false,&search);
    CHECK(connected&&connected->dpi_choices==std::vector<unsigned>{1000});
    CHECK(test_routes::writes==1 && test_routes::value==std::wstring(L"1\n")+discovery_fixture::path);
    logi::StandardReporting state;
    CHECK(logi::restore_standard_input(*connected,&state));
    Mappings mappings;std::istringstream ini("[global]\n00c4=A\n");
    CHECK(parse_mappings(ini,mappings));
    std::vector<Control> controls;CHECK(enable(*connected,mappings.bindings,controls,state.controls));
    CHECK(controls.size()==1 && model.flags[0xc4]==1);

    discovery_fixture::dpi_only=true;
    auto cli=logi::connect(nullptr,false,false);
    CHECK(cli && cli->dpi==9 && !cli->wheel && !cli->thumb && !cli->buttons);
    CHECK(test_routes::writes==1 && dpi_lists==2);
    channel_request_result={};
}

void catalog() {
    Device model;
    channel_request_result=[&](Byte f,Byte fn,std::span<const Byte> p){return model.request(f,fn,p);};
    logi::OwnerIdentity identity{L"test-receiver",1,2,0,{1,2,3,4},{5,6,7,8,9,10},{'R','B','M',1,2,3,4}};
    logi::Mouse mouse{};mouse.device.name="MX Master 3S";mouse.device.owner_identity=identity;
    mouse.wheel=5;mouse.thumb=6;mouse.buttons=7;
    mouse.channel=std::make_unique<logi::Channel>(logi::Interface{});
    logi::StandardReporting state;
    std::optional<logi::ControlCatalog> previous;
    auto restore=[&]{
        state={};model.metadata_reads=0;model.calls=0;model.report_reads.clear();
        return logi::restore_standard_input(mouse,&state,&previous);
    };
    CHECK(restore() && !previous && state.learned && model.metadata_reads==model.count);
    const auto known=*state.learned;
    const auto full_calls=model.calls;
    CHECK(known.divertable==std::vector<unsigned>({0x52,0x53,0x56,0xc3,0xc4}));
    previous=known;
    CHECK(restore() && !state.learned && previous && !model.metadata_reads);
    CHECK(model.calls+model.count==full_calls);
    for(auto id:known.divertable)CHECK(model.report_reads[id]==1);
    std::printf("same verified identity: control normalization %u -> %u HID transactions\n",full_calls,model.calls);

    // A foreign owner changed an unbound control while this owner was away.
    model.flags[0x56]=0x11;model.targets[0x56]=0x53;model.extended[0x56]=1;
    CHECK(restore() && !model.metadata_reads && model.report_reads[0x56]==2);
    CHECK(!model.flags[0x56] && model.targets[0x56]==0x53 && model.extended[0x56]==1);

    // Even a single lost read makes this attempt enumerate every capability.
    model.reject_read=0x53;
    CHECK(restore() && !previous && state.learned && model.metadata_reads==model.count);
    CHECK(state.controls.size()==known.divertable.size() && model.report_reads[0x52]==2);

    for(unsigned changed=0;changed<7;++changed) {
        auto other=identity;
        switch(changed) {
        case 0:other.route+=L"-other";break;
        case 1:++other.slot;break;
        case 2:++other.major;break;
        case 3:++other.minor;break;
        case 4:++other.unit[0];break;
        case 5:++other.model[0];break;
        case 6:++other.firmware[0];break;
        }
        mouse.device.owner_identity=other;previous=known;
        CHECK(restore() && !previous && state.learned && state.learned->owner==other);
        CHECK(model.metadata_reads==model.count);
    }
    mouse.device.owner_identity.reset();previous=known;
    CHECK(restore() && !previous && !state.learned && model.metadata_reads==model.count);
    mouse.device.owner_identity=identity;previous=known;--model.count;
    CHECK(restore() && !previous && state.learned && state.learned->count==model.count);
    CHECK(model.metadata_reads==model.count);
    ++model.count;

    // Missing acknowledgements or inconsistent metadata must never publish a catalog.
    model.ignored_write=0xc4;model.flags[0xc4]=1;previous=known;
    CHECK(!restore() && !previous && !state.learned && state.controls.empty());
    model.ignored_write=0;model.duplicate_info=true;
    CHECK(restore() && !previous && !state.learned && model.metadata_reads==model.count);
    model.duplicate_info=false;model.no_divertable=true;
    CHECK(restore() && !state.learned && model.metadata_reads==model.count);
    channel_request_result={};
}

void dpi_drift(const char* config_path,std::string_view scenario) {
    fixture_path=config_path;
    std::ofstream(fixture_path)<<"[global]\n0052=A\n";
    auto saved=fixture_path;
    saved.replace_extension(L".dpi");
    const bool has_target=scenario!="dpi-unconfigured";
    const bool refuse_first=scenario=="dpi-write-refused";
    if(has_target)std::ofstream(saved)<<"MX Master 3S\n5000\n";
    else CHECK(!std::filesystem::exists(saved));
    const auto original_stamp=has_target?std::filesystem::last_write_time(saved):std::filesystem::file_time_type{};
    Device model;
    unsigned sensor=5000,writes{},sensor_reads{};
    channel_request_result=[&](Byte feature,Byte fn,std::span<const Byte> args) {
        if(feature!=9)return model.request(feature,fn,args);
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        auto* data=reply.packet.bytes.data()+4;
        if(fn==1) {
            data[1]=logi::high(1000);data[2]=logi::low(1000);
            data[3]=logi::high(5000);data[4]=logi::low(5000);
        } else if(fn==2) {
            ++sensor_reads;data[1]=logi::high(sensor);data[2]=logi::low(sensor);
        } else {
            CHECK(fn==3 && args.size()==3 && args[0]==0 && logi::word(args.subspan(1))==5000);
            CHECK(health && health->mappings_ready && !health->settings_ready && !health->ready);
            ++writes;
            if(!refuse_first || writes>1)sensor=5000;
        }
        return reply;
    };
    respect_read_deadlines=true;
    for(auto at:{1000,4000,7000}){Step sample;sample.idle=true;sample.at=at;steps.push_back(sample);}
    before_step=[&](unsigned step) {
        CHECK(health && health->connections==1 && health->mappings_ready);
        if(step==1) {
            CHECK(!desired_dpi && health->ready && sensor==5000 && writes==0);
            first_pid=health->pid;sensor=1000;
        } else {
            const bool pending=step==2 && refuse_first;
            CHECK(health->pid==first_pid && health->ready==!pending && health->settings_ready==!pending);
            CHECK(sensor==((pending || !has_target)?1000u:5000u));
        }
    };
    CHECK(run_agent(fixture_path.c_str(),false)==0);
    CHECK(input_index==3 && discovery_fixture::rounds==1 && sensor_reads>=3);
    CHECK(writes==(has_target?(refuse_first?2u:1u):0u));
    if(has_target)CHECK(std::filesystem::last_write_time(saved)==original_stamp);
    else CHECK(!std::filesystem::exists(saved));
}

void cached_wheel_recovery(const char* config_path) {
    // Drive the actual owner, real connect(), and all reporting calls through a lost link.
    fixture_path=config_path;mode="cached-wheel-recovery";
    std::ofstream(fixture_path)<<"[global]\n0052=A\n";
    Device model;
    discovery_fixture::identified=true;
    unsigned faults{},wheel_reads_after_loss{},recovery_dpi_reads{},ready_observations{};
    std::optional<ULONGLONG> refused_at;
    channel_request_result=[&](Byte feature,Byte fn,std::span<const Byte> args)->logi::Reply {
        const unsigned round=discovery_fixture::rounds;
        CHECK(round<=3 && health);
        if(round==2 || (round==3 && !ready_observations)){
            CHECK(!health->ready && !health->mappings_ready);
            CHECK(health->connections==1 || (round==3 && feature==9 && fn==2 && health->connections==2));
        }
        logi::Reply reply{logi::Result::ok};reply.packet.size=20;
        auto* data=reply.packet.bytes.data()+4;
        if(feature==3) {
            if(fn==0){constexpr std::array<Byte,13> unit{1,1,2,3,4,0,1,5,6,7,8,9,10};std::ranges::copy(unit,data);}
            else {CHECK(fn==1);constexpr std::array<Byte,8> fw{0,'R','B','M',1,2,3,4};std::ranges::copy(fw,data);}
            if(round==3){CHECK(refused_at && *refused_at==tick);}
        } else if(feature==9) {
            CHECK(fn==1 || fn==2);data[1]=3;data[2]=0xe8;
            if(fn==2 && round==3)++recovery_dpi_reads;
        } else if(feature==8) {
            CHECK(fn==1);data[0]=2;data[1]=10;
        } else if(feature==5 && fn==1 && round==2 && ++wheel_reads_after_loss==2) {
            // The first read observes the user's settings. Reject the live
            // check immediately before the owner is allowed to divert buttons.
            ++faults;refused_at=tick;
            return {logi::Result::device_error};
        } else return model.request(feature,fn,args);
        return reply;
    };
    Step lost;lost.disconnect=true;
    Step down;down.down=true;Step up;Step stop;stop.stop=true;
    steps={lost,down,up,stop};
    before_step=[&](unsigned n){
        CHECK(health && health->ready && health->mappings_ready && model.flags[0x52]==1);
        if(n==1)CHECK(discovery_fixture::rounds==1 && health->connections==1);
        else {
            CHECK(discovery_fixture::rounds==3 && health->connections==2);
            ++ready_observations;
        }
    };
    // Account for actual stop waits in our synthetic clock. Retrying immediately
    // is part of this regression; a two-second backoff must fail this test.
    observe_wait=[&](HANDLE event,DWORD duration){
        if(event==reinterpret_cast<HANDLE>(101) && refused_at && !ready_observations && duration<=2000)tick+=duration;
    };
    CHECK(run_agent(fixture_path.c_str(),false)==0);
    CHECK(discovery_fixture::rounds==3 && faults==1 && recovery_dpi_reads==1 && ready_observations==3);
    const auto& cached=discovery_fixture::queried_by_round.at(2);
    CHECK(std::ranges::contains(cached,0x1b04) && !std::ranges::contains(cached,0x2111));
    const auto& refreshed=discovery_fixture::queried_by_round.at(3);
    for(auto id:{0x2201,0x1b04,0x2121,0x2150})CHECK(std::ranges::contains(refreshed,id));
    CHECK(std::ranges::any_of(captured,[](const INPUT& input){return input.type==INPUT_KEYBOARD && input.ki.wVk=='A';}));
}
}
int main(int argc,char** argv){
    if(argc==3 && std::string_view(argv[1]).starts_with("dpi-")){dpi_drift(argv[2],argv[1]);return 0;}
    if(argc==3 && std::string_view(argv[1])=="cached-wheel"){cached_wheel_recovery(argv[2]);return 0;}
    if(argc==2 && std::string_view(argv[1])=="discovery"){discovery();return 0;}
    if(argc==2 && std::string_view(argv[1])=="catalog"){catalog();return 0;}
    CHECK(argc==1);verify(false,false);verify(true,false);verify(true,true);
}
