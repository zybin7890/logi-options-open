#define LOGI_TEST_REAL_MOUSE
#include "environment.hpp"
#include "../owner_profile.hpp"

namespace test_routes {extern std::vector<BYTE> metadata;extern unsigned metadata_writes;}
namespace fixture {
using logi::Byte;
logi::OwnerIdentity id{L"fake-owner-interface",1,2,0,{1,2,3,4},{5,6,7,8,9,10},{'M','R','B',1,0,0,17}};
unsigned name_reads{},list_reads{},info_reads{},firmware_reads{},dpi_reads{},wheel_reads{};
unsigned dpi=5000;
Byte buttons=7,wheel_mode=2;
bool truncated{};
std::vector<unsigned> feature_queries;

logi::Reply request(Byte feature,Byte function,std::span<const Byte> args) {
    logi::Reply reply{logi::Result::ok};reply.packet.size=20;
    auto* data=reply.packet.bytes.data()+4;
    if(feature==2 && function==0) {
        ++info_reads;data[0]=1;data[6]=4;
        std::ranges::copy(id.unit,data+1);std::ranges::copy(id.model,data+7);
        if(truncated)reply.packet.size=11;
    } else if(feature==2 && function==1) {
        CHECK(args.size()==1 && args[0]==0);++firmware_reads;
        std::ranges::copy(id.firmware,data+1);
    } else if(feature==9 && function==1) {
        CHECK(args.size()==1 && args[0]==0);++list_reads;
        data[1]=3;data[2]=0xe8;data[3]=0x13;data[4]=0x88;
    } else if(feature==9 && function==2) {
        ++dpi_reads;data[1]=logi::high(dpi);data[2]=logi::low(dpi);
    } else if(feature==5 && function==1) {
        ++wheel_reads;data[0]=wheel_mode;
    } else CHECK(false);
    return reply;
}
}

namespace logi {
std::vector<Interface> enumerate(){return {{fixture::id.route,0xc548,0xff00,1,20,20,true}};}
std::string read_device_name(Channel&,Byte slot,bool) {
    CHECK(slot==fixture::id.slot);++fixture::name_reads;return "Live MX";
}
std::optional<Device> probe(Channel& channel,Byte slot,DWORD,bool with_name) {
    const auto& id=fixture::id;
    if(slot!=id.slot)return {};
    return Device{slot,id.major,id.minor,with_name?read_device_name(channel,slot,true):""};
}
Byte feature(Channel&,Byte slot,unsigned id,DWORD) {
    CHECK(slot==fixture::id.slot);fixture::feature_queries.push_back(id);
    switch(id) {
    case 0x0003:return 2;
    case 0x2201:return 9;
    case 0x1b04:return fixture::buttons;
    case 0x2121:return 5;
    case 0x2150:return 6;
    case 0x2111:return 8;
    case 0x1004:return 11;
    default:CHECK(false);return 0;
    }
}
}

int main() {
    using namespace fixture;
    using logi::owner_profile::encode;
    using logi::owner_profile::decode;
    const logi::owner_profile::Metadata stored{"Previous MX",{1000,5000}};
    auto record=encode(id,stored);
    CHECK(!record.empty() && decode(id,record)->choices==stored.choices);
    auto changed=id;changed.firmware.back()++;
    CHECK(!decode(changed,record));
    CHECK(!decode(id,std::span{record}.first(record.size()-1)));
    record.back()^=255;
    CHECK(!decode(id,record));
    CHECK(encode(id,{"",{1000}}).empty() && encode(id,{"MX",{1000,1000}}).empty());
    test_routes::metadata=encode(id,stored);
    channel_request_result=request;
    logi::discovery::OwnerSearch search;
    auto connect=[&] {
        feature_queries.clear();
        auto mouse=logi::connect(nullptr,false,false,&search);
        CHECK(mouse);return std::move(*mouse);
    };
    const std::vector<unsigned> full{3,0x2201,0x1b04,0x2121,0x2150,0x2111,0x1004};
    const std::vector<unsigned> warm{3,0x1b04};
    auto first=connect();
    CHECK(feature_queries==full && first.device.name==stored.name && first.device.owner_identity==id);
    CHECK(!name_reads && !list_reads && !test_routes::metadata_writes);
    CHECK(logi::get(first,L"dpi")==5000 && logi::get(first,L"hires")==1);
    dpi=1400;wheel_mode=0;
    auto second=connect();
    CHECK(feature_queries==warm && second.device.owner_identity==id);
    CHECK(info_reads==2 && firmware_reads==2 && !name_reads && !list_reads);
    CHECK(logi::get(second,L"dpi")==1400 && logi::get(second,L"hires")==0);
    CHECK(dpi_reads==2 && wheel_reads==2);
    search.require_live_features=true;
    CHECK(connect().device.name==stored.name && feature_queries==full);
    CHECK(!search.require_live_features && !name_reads && !list_reads);
    buttons=12;
    CHECK(connect().buttons==12 && std::ranges::contains(feature_queries,0x2201));
    for(unsigned field=0;field<7;++field) {
        switch(field) {
        case 0:++id.firmware.back();break;
        case 1:++id.model[0];break;
        case 2:++id.unit[0];break;
        case 3:id.route+=L"-other";break;
        case 4:++id.major;break;
        case 5:++id.minor;break;
        case 6:++id.slot;break;
        }
        search={};
        // On a changed receiver slot the first probe intentionally consults the old hint.
        if(field==6)CHECK(!logi::connect(nullptr,false,false,&search));
        auto mouse=connect();
        CHECK(feature_queries==full && mouse.device.owner_identity==id && mouse.device.name=="Live MX");
        CHECK(list_reads==field+1 && name_reads==list_reads && test_routes::metadata_writes==list_reads);
    }
    truncated=true;
    CHECK(!connect().device.owner_identity && feature_queries==full);
    CHECK(!connect().device.owner_identity && feature_queries==full);
    truncated=false;test_routes::metadata.back()^=127;
    const auto before=list_reads;
    CHECK(connect().device.name=="Live MX" && feature_queries==full && list_reads==before+1);
    feature_queries.clear();
    auto cli=logi::connect(nullptr,false,false);
    CHECK(cli && !cli->device.owner_identity && cli->device.name=="Live MX" && list_reads==before+2);
    CHECK(!std::ranges::contains(feature_queries,3) && logi::get(*cli,L"dpi")==1400);
    std::puts("verified owner metadata: persistent cold start, live DPI/wheel, 7 identity changes, partial replies and damaged store passed");
}
