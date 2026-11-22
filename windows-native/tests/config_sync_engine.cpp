#include "../config_sync_engine.hpp"
#include "../config_sync_drive.hpp"
#include "../config_sync_onedrive.hpp"
#include "../config_sync_webdav.hpp"
#include "../configuration_backup.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace config_sync;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void rejected(const auto& call){
    try{call();}catch(const std::exception&){return;}
    throw std::runtime_error("Expected operation to be rejected");
}
using Bytes=std::vector<unsigned char>;
Bytes package(char key){
    loo_backup::Configuration config;
    config.mappings="[global]\n0053="+std::string(1,key)+"\n";config.settings[0]=5000;
    const auto encoded=loo_backup::encode(config);
    return {encoded.begin(),encoded.end()};
}
Bytes mapping_only(char key){
    const std::string mapping="[global]\n0053="+std::string(1,key)+"\n";
    return {mapping.begin(),mapping.end()};
}
struct Fixture {
    fs::path root;
    explicit Fixture(const fs::path& parent):root(parent/(L"engine-case-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()))){
        check(fs::create_directory(root),"Cannot create isolated test directory");
    }
    ~Fixture(){std::error_code error;fs::remove_all(root,error);}
};
Remote forked;
unsigned resolved{};
std::wstring drive_account=L"account-a";
}

// OneDrive uses the production filesystem implementation. Simulate the Drive API
// boundary for forks that first arise during concurrent creation in appDataFolder.
namespace config_sync {
std::wstring read_secret(std::wstring_view){return L"isolated";}
Remote read_webdav(const WebDav&){throw std::runtime_error("Unexpected WebDAV call");}
void write_webdav(const WebDav&,std::span<const unsigned char>,const std::wstring&,bool){throw std::runtime_error("Unexpected WebDAV call");}
namespace drive {
std::wstring account_id(const Client& client){check(client.secret.empty(),"Account lookup requested a client secret");return drive_account;}
Remote read(const Client& client){check(client.secret==L"isolated","Wrong credential");return forked;}
void write(const Client&,const Remote& expected,std::span<const unsigned char> bytes){
    check(expected.conflict && expected.etag==forked.etag,"Lost fork precondition");
    ++resolved;Bytes saved(bytes.begin(),bytes.end());forked={std::move(saved),L"next",true,false};
}
}
}

int main(int argc,char** argv){try{
    check(argc==2,"Expected build directory");
    Fixture fixture(fs::absolute(argv[1]));
    const auto client_a=fixture.root/"first.ini",client_b=fixture.root/"second.ini";
    const auto one=package('A'),two=package('B'),three=package('C'),four=package('D');
    Options options;options.folder=fixture.root/L"驱动器";fs::create_directory(options.folder);
    options.url=L"https://example.invalid/dav/配置.bin";options.user=L"用户";options.client_id=L"client";options.automatic=true;
    check(!load_options(client_a).automatic,"Fresh profile unexpectedly enabled");
    save_options(client_a,options);const auto saved=load_options(client_a);
    check(saved.automatic && saved.folder==options.folder && saved.user==options.user &&
          saved.url==options.url && saved.client_id==options.client_id,"Options did not round trip");
    auto invalid=options;invalid.url=L"https://name:secret@example.invalid/a";
    rejected([&]{save_options(client_a,invalid);});
    check(load_options(client_a).url==saved.url,"Rejected settings damaged previous settings");
    Options web=options;web.provider=Provider::webdav;
    const auto key=credential_key(web);auto changed=web;changed.user+=L"2";
    check(key!=credential_key(changed) && !key.contains(web.user),"WebDAV credential scope was not isolated");
    changed=web;changed.url+=L"2";check(key!=credential_key(changed),"Two WebDAV targets shared a secret");

    const auto sync=[&](const fs::path& profile,const Bytes& data,Mode mode=Mode::sync){return synchronize(profile,options,data,mode);};
    check(sync(client_a,one).status==Status::uploaded,"First upload failed");
    check(sync(client_a,one).status==Status::unchanged,"Idempotent sync rewrote the cloud");
    check(sync(client_b,two).status==Status::conflict,"New client silently overwrote an existing file");
    const auto pending=sync(client_b,two,Mode::prefer_remote);
    check(pending.status==Status::download && pending.package==one,"Wrong remote package");
    check(sync(client_b,two).status==Status::conflict,"Unapplied download advanced the baseline");
    rejected([&]{acknowledge_download(client_a,options,pending);});
    acknowledge_download(client_b,options,pending);
    // Previous versions saved a single shared digest after applying identical bytes.
    const auto legacy_state=fixture.root/(".logi-sync-"+pending.target_id+".state");
    std::string legacy;
    {std::ifstream state(legacy_state);std::getline(state,legacy);}
    check(legacy.size()==64,"Unexpected saved baseline");
    {std::ofstream state(legacy_state,std::ios::binary|std::ios::trunc);state<<legacy;}
    check(sync(client_b,one).status==Status::unchanged,"Accepted package was not remembered");
    check(sync(client_b,two).status==Status::uploaded,"Local-only edit failed to upload");
    const auto remote_only=sync(client_a,one);
    check(remote_only.status==Status::download && remote_only.package==two,"Remote-only edit was not downloaded");
    check(sync(client_a,three).status==Status::conflict && read_onedrive(options.folder).bytes==two,
          "Two-sided edits did not preserve both configurations");
    check(sync(client_a,three,Mode::prefer_local).status==Status::uploaded,"Local conflict resolution failed");
    rejected([&]{acknowledge_download(client_a,options,remote_only);});
    check(sync(client_a,three).status==Status::unchanged,"Stale acknowledgement damaged newer baseline");
    fs::remove(options.folder/L"Logi Options Open"/L"settings.logi");
    check(sync(client_a,three).status==Status::conflict && !read_onedrive(options.folder).exists,
          "Unexpectedly recreated a deleted shared configuration");
    check(sync(client_a,three,Mode::prefer_local).status==Status::uploaded,"Explicit recreation failed");
    auto other=options;other.folder=fixture.root/"other";fs::create_directory(other.folder);
    write_onedrive(other.folder,two,{});
    check(synchronize(client_a,other,three).status==Status::conflict,"Independent target inherited an old baseline");
    const auto without_dpi=mapping_only('E'),retained_dpi=package('E');
    write_onedrive(other.folder,without_dpi,read_onedrive(other.folder));
    const auto partial=synchronize(client_a,other,three,Mode::prefer_remote);
    acknowledge_download(client_a,other,partial,retained_dpi);
    check(synchronize(client_a,other,retained_dpi).status==Status::unchanged &&
          read_onedrive(other.folder).bytes==without_dpi,"Retained DPI caused a spurious upload");
    const auto next_legacy=mapping_only('F');
    write_onedrive(other.folder,next_legacy,read_onedrive(other.folder));
    const auto next_partial=synchronize(client_a,other,retained_dpi);
    check(next_partial.status==Status::download && next_partial.package==next_legacy,"Remote edit after normalized apply was lost");
    acknowledge_download(client_a,other,next_partial,package('F'));
    check(synchronize(client_a,other,four).status==Status::uploaded &&
          read_onedrive(other.folder).bytes==four,"Local edit after normalized apply was lost");
    write_onedrive(other.folder,next_legacy,read_onedrive(other.folder));
    check(synchronize(client_a,other,one).status==Status::conflict,"Two-sided edit was silently overwritten");
    write_onedrive(options.folder,Bytes{0,255},read_onedrive(options.folder));
    rejected([&]{sync(client_a,three);});
    check(sync(client_a,four,Mode::prefer_local).status==Status::uploaded,"Malformed cloud file damaged local state");

    Options drive;drive.provider=Provider::google_drive;drive.client_id=L"fixture-client";
    forked={one,L"fork1",true,true};
    check(synchronize(client_a,drive,one).status==Status::conflict && !resolved,"Matching selected branch hid a competing file");
    auto choice=synchronize(client_a,drive,two,Mode::prefer_remote);
    check(resolved==1 && forked.bytes==one && choice.status==Status::download && choice.package==one,"Failed to consolidate remote fork");
    acknowledge_download(client_a,drive,choice);
    forked={one,L"fork2",true,true};
    check(synchronize(client_a,drive,two,Mode::prefer_local).status==Status::uploaded && resolved==2 &&
          forked.bytes==two,"Local resolution did not replace both Drive branches");
    const auto client_key=credential_key(drive);
    drive_account=L"account-b";forked={three,L"other-account",true,false};
    check(synchronize(client_a,drive,two).status==Status::conflict && credential_key(drive)==client_key,
          "Switching Google accounts reused a baseline or lost the desktop client secret");
    rejected([&]{acknowledge_download(client_a,drive,choice);});
    drive_account.clear();rejected([&]{synchronize(client_a,drive,one);});
    std::cout<<"PASS independent targets, two-client sync, conditional conflicts and delayed apply acknowledgement\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
