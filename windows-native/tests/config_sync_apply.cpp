// Exercise the UI-thread download state machine without opening HID devices or
// injecting input. The native snapshot and recovery-file routines use real files.
#include "../configuration_backup.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <span>
#include <vector>
#include <iostream>
namespace fs=std::filesystem;
using Bytes=std::vector<unsigned char>;
using Values=std::array<std::optional<unsigned>,6>;
struct CloudJob {
    enum class ApplyStage{prepare,probe,reload,settings} apply_stage{ApplyStage::prepare};
    loo_backup::Configuration download;
    unsigned long long apply_reload_revision{},apply_dpi_revision{};
    std::array<unsigned long long,6> apply_setting_revision{};
    Bytes local;
    struct {Bytes package;} result;
};
enum class ReloadStatus {applied,hardware_failed};
struct ReloadResult {unsigned long long revision{};ReloadStatus status{};};
struct App {
    fs::path directory;std::wstring source;
    struct {Values values{};std::vector<unsigned> dpi{1000,2000};} device;
    HWND window{};
    bool busy{},reload_busy{},pending_reload{},pending_binding{},pending_ring{},pending_thumb_speed{},pending_sensitivity{};
    bool ring_dragging{},ring_slot_dragging{},waiting_agent{},waiting_stop{},closing{},dpi_unconfirmed{};
    std::vector<std::pair<unsigned,unsigned>> pending_values;
    std::array<bool,6> setting_unconfirmed{};
    std::array<unsigned long long,6> setting_revision{};
    unsigned long long reload_revision{},dpi_revision{};
    std::optional<ReloadResult> last_reload_result;
    fs::path mapping()const{return directory/"mappings.ini";}
};
static std::wstring from_utf8(std::string_view s){
    if(s.starts_with("\xef\xbb\xbf"))s.remove_prefix(3);
    auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring text(n,0);if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),text.data(),n);
    return text;
}
static std::string to_utf8(std::wstring_view s){
    auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(n,0);if(n)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),bytes.data(),n,nullptr,nullptr);
    return bytes;
}
static void write(const fs::path& p,std::string_view bytes){std::ofstream f(p,std::ios::binary);f<<bytes;if(!f)throw std::runtime_error("write");}
static std::wstring read(const fs::path& p){std::ifstream f(p,std::ios::binary);return from_utf8(std::string{std::istreambuf_iterator<char>(f),{}});}
static void read_config(App& a){a.source=read(a.mapping());}
static bool ready=true,receipt=true,write_parameters=true;
struct Health{bool available,mappings_ready;};
static Health read_agent_status(){return{ready,ready};}
static bool options_running(){return false;}
static void io(App& a){a.device.values[0]=1000;a.device.values[3]=0;}
static bool is_current_reload_result(const ReloadResult&){return receipt;}
static bool save(App& a,const std::wstring& source){
    if(!source.starts_with(L"[global]"))return false;
    write(a.mapping(),to_utf8(source));a.last_reload_result=ReloadResult{++a.reload_revision,ReloadStatus::applied};return true;
}
static void queue_parameter(App& a,unsigned id,unsigned value,bool){
    a.pending_values.emplace_back(id,value);if(id)++a.setting_revision[id];else ++a.dpi_revision;
}
static void flush_parameters(App& a){
    for(auto [id,value]:a.pending_values)if(write_parameters){
        if(id==0){a.device.values[id]=value;a.dpi_unconfirmed=false;write(a.directory/"mappings.dpi","MX\r\n"+std::to_string(value)+"\r\n");}
        else if(id==3){a.device.values[id]=value;write(a.directory/"mappings.settings","LogiNative settings 1\r\nMX\r\nhires "+std::to_string(value)+"\r\n");}
    }
    a.pending_values.clear();
}
#include "../native_cloud_apply.hpp"
static void require(bool yes){if(!yes)throw std::runtime_error("assertion failed");}
static void step(App& a,CloudJob& j){require(apply_cloud_download(a,j)==ApplyProgress::waiting);}
static void rejected(App& a,CloudJob& j){try{apply_cloud_download(a,j);}catch(const std::exception&){return;}throw std::runtime_error("unexpected apply");}
int main(int argc,char** argv)try{
    if(argc!=2)return 2;
    const auto dir=fs::absolute(argv[1]);if(!fs::create_directory(dir))throw std::runtime_error("fixture root must be new");
    struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove_all(p,ec);}}cleanup{dir};
    const std::string a="[global]\n00C4=Win+D\n",b="[global]\n00C4=Win+Tab\n";
    auto initialize=[&](App& app,CloudJob& job){
        app.directory=dir;
        write(app.mapping(),"\xef\xbb\xbf"+a);
        write(dir/"mappings.dpi","MX\r\n1000\r\n");
        write(dir/"mappings.settings","LogiNative settings 1\r\nMX\r\nhires 0\r\n");
        job.local=cloud_snapshot(app);
        loo_backup::Configuration cfg{b};cfg.settings[0]=2000;const auto bytes=loo_backup::encode(cfg);
        job.result.package.assign(bytes.begin(),bytes.end());
    };
    {
        App app;CloudJob job;initialize(app,job);app.pending_binding=true;require(cloud_busy(app));step(app,job);
        app.pending_binding=false;step(app,job);step(app,job);step(app,job);require(apply_cloud_download(app,job)==ApplyProgress::done);
        require(read(app.mapping())==from_utf8(b) && app.device.values[0]==2000);
        const auto recovery=fs::directory_iterator(dir/"sync-backups")->path();
        std::ifstream f(recovery,std::ios::binary);const Bytes prior{std::istreambuf_iterator<char>(f),{}};require(prior==job.local);
        auto pkg=cloud_snapshot(app);const auto decoded=loo_backup::decode({reinterpret_cast<const char*>(pkg.data()),pkg.size()});
        require(decoded.settings[3]==0 && decoded.settings[0]==2000);
    }
    {
        App app;CloudJob job;initialize(app,job);write(app.mapping(),a+"# new edit\n");rejected(app,job);
        require(read(app.mapping())==from_utf8(a+"# new edit\n"));
    }
    {
        App app;CloudJob job;initialize(app,job);step(app,job);step(app,job);receipt=false;rejected(app,job);receipt=true;
        require(read(dir/"mappings.dpi")==L"MX\r\n1000\r\n");
    }
    {
        App app;CloudJob job;initialize(app,job);step(app,job);step(app,job);write_parameters=false;step(app,job);
        rejected(app,job);write_parameters=true;
    }
    {
        App app;CloudJob job;initialize(app,job);fs::remove(dir/"mappings.settings");fs::remove(dir/"mappings.dpi");const auto pkg=cloud_snapshot(app);
        const auto data=loo_backup::decode({reinterpret_cast<const char*>(pkg.data()),pkg.size()});require(!data.settings[0] && !data.settings[3]);
        write(dir/"mappings.dpi","MX\r\ngarbage");try{cloud_snapshot(app);return 3;}catch(const std::runtime_error&){}
    }
    std::cout<<"PASS cloud snapshot, safe apply, local drift, real recovery file, unavailable ACK, failed readback and missing sidecars\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
