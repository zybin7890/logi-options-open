#include "config_sync_engine.hpp"
#include "config_sync_drive.hpp"
#include "config_sync_onedrive.hpp"
#include "config_sync_webdav.hpp"
#include "configuration_backup.hpp"
#include "ui_settings.hpp"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace config_sync {
namespace fs=std::filesystem;
namespace {

std::string digest(std::span<const unsigned char> bytes) {
    std::array<unsigned char,32> value{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<unsigned char*>(bytes.data()),
                  static_cast<ULONG>(bytes.size()),value.data(),static_cast<ULONG>(value.size()))<0)
        throw std::runtime_error("Cannot hash sync data");
    constexpr char digits[]="0123456789abcdef";
    std::string out;
    for(const auto byte:value){out.push_back(digits[byte>>4]);out.push_back(digits[byte&15]);}
    return out;
}

std::string digest(std::string_view bytes) {
    return digest({reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size()});
}

std::string field(std::wstring_view value) {
    if(value.size()>32767 || value.find_first_of(L"\r\n")!=value.npos || value.find(L'\0')!=value.npos)
        throw std::runtime_error("Invalid sync option");
    const auto encoded=ui_settings::wide_to_utf8(value);
    if(ui_settings::utf8_to_wide(encoded)!=value)throw std::runtime_error("Invalid sync option text");
    return encoded;
}

const char* provider_name(Provider provider) {
    switch(provider) {
    case Provider::onedrive:return "onedrive";
    case Provider::webdav:return "webdav";
    case Provider::google_drive:return "google_drive";
    }
    throw std::runtime_error("Unknown sync provider");
}

void check_url(std::wstring_view url) {
    // Userinfo in a WebDAV URL would otherwise be written to the options file.
    const auto scheme=url.find(L"://");
    if(scheme==url.npos)return;
    const auto authority=url.substr(scheme+3);
    if(authority.substr(0,authority.find_first_of(L"/?#")).find(L'@')!=url.npos)
        throw std::runtime_error("Use the separate WebDAV username and password fields");
}

std::string endpoint_id(const Options& options) {
    std::string identity=provider_name(options.provider);
    identity+='\0';
    switch(options.provider) {
    case Provider::webdav:
        check_url(options.url);
        identity+=field(options.url);identity+='\0';identity+=field(options.user);break;
    case Provider::google_drive:identity+=field(options.client_id);break;
    case Provider::onedrive:
        identity+=field(fs::weakly_canonical(fs::absolute(options.folder.empty()?onedrive_folder():options.folder)).native());break;
    }
    return digest(identity);
}

fs::path options_path(const fs::path& mapping) {
    auto path=fs::absolute(mapping);
    path+=L".sync-options";
    return path;
}

std::optional<std::string> read_file(const fs::path& path,std::size_t limit) {
    if(!fs::exists(path))return {};
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    const auto end=in.tellg();
    if(!in || end<0 || static_cast<std::uintmax_t>(end)>limit)throw std::runtime_error("Invalid sync state or settings");
    std::string value(static_cast<std::size_t>(end),'\0');
    in.seekg(0);
    if(!in.read(value.data(),static_cast<std::streamsize>(value.size())))throw std::runtime_error("Cannot read sync state or settings");
    return value;
}

void write_file(const fs::path& path,std::string_view text) {
    wchar_t temp[MAX_PATH]{};
    if(!GetTempFileNameW(path.parent_path().c_str(),L"LSY",0,temp))throw std::runtime_error("Cannot create temporary sync file");
    std::ofstream out(fs::path(temp),std::ios::binary|std::ios::trunc);
    out.write(text.data(),static_cast<std::streamsize>(text.size()));
    out.close();
    if(!out || !MoveFileExW(temp,path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temp);
        throw std::runtime_error("Cannot save sync state or settings");
    }
}

struct State {
    fs::path path;
    std::string id,prior,local_digest,remote_digest;
    HANDLE mutex{};
    State(const fs::path& mapping,const Options& options) {
        const auto file=fs::weakly_canonical(fs::absolute(mapping));
        auto identity=field(file.native())+std::string(1,'\0')+endpoint_id(options);
        if(options.provider==Provider::google_drive) {
            const auto account=drive::account_id({options.client_id,{}});
            if(account.empty())throw std::runtime_error("Authorize a Google Drive account before syncing");
            identity+='\0';identity+=field(account);
        }
        id=digest(identity);
        const std::wstring wide(id.begin(),id.end());
        path=file.parent_path()/(L".logi-sync-"+wide+L".state");
        mutex=CreateMutexW(nullptr,FALSE,(L"Local\\LogiOptionsOpen.Sync."+wide).c_str());
        if(!mutex)throw std::runtime_error("Cannot lock sync state");
        const auto wait=WaitForSingleObject(mutex,0);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) {
            CloseHandle(mutex);throw std::runtime_error("Another sync is in progress");
        }
    }
    State(const State&)=delete;
    ~State(){ReleaseMutex(mutex);CloseHandle(mutex);}
    void load(){
        const auto previous=read_file(path,129);
        if(!previous)return;
        local_digest=previous->substr(0,64);
        if(previous->size()==64)remote_digest=local_digest;
        else if(previous->size()==129 && (*previous)[64]=='\n')remote_digest=previous->substr(65);
        const auto valid=[](const std::string& value){
            return value.size()==64 && value.find_first_not_of("0123456789abcdef")==value.npos;
        };
        if(!valid(local_digest) || !valid(remote_digest))
            throw std::runtime_error("Invalid sync baseline");
        prior=local_digest+'\n'+remote_digest;
    }
    void remember(const std::string& local,const std::string& remote) const {
        const auto next=local+'\n'+remote;
        if(next!=prior)write_file(path,next);
    }
};

void validate_package(std::span<const unsigned char> data) {
    if(data.empty() || data.size()>max_bytes)throw std::runtime_error("Invalid configuration sync package size");
    loo_backup::decode({reinterpret_cast<const char*>(data.data()),data.size()});
}

template<class Writer>
SyncResult reconcile(State& state,std::span<const unsigned char> local,Remote remote,Mode mode,Writer write) {
    const auto local_hash=digest(local);
    if(remote.conflict) {
        if(mode==Mode::sync)return {Status::conflict,{},{},{}};
        if(mode==Mode::prefer_remote) {
            validate_package(remote.bytes);
            write(remote,std::span<const unsigned char>{remote.bytes});
            return {Status::download,std::move(remote.bytes),state.prior,state.id};
        }
    }
    const auto remote_hash=remote.exists?digest(remote.bytes):std::string{};
    if(!remote.conflict && remote.exists && (local_hash==remote_hash || (mode==Mode::sync &&
       local_hash==state.local_digest && remote_hash==state.remote_digest))) {
        state.remember(local_hash,remote_hash);
        return {Status::unchanged,{},{},{}};
    }
    if(mode==Mode::prefer_local || (mode==Mode::sync &&
       ((!remote.exists && state.prior.empty()) || (remote.exists && remote_hash==state.remote_digest)))) {
        write(remote,local);
        state.remember(local_hash,local_hash);
        return {Status::uploaded,{},{},{}};
    }
    if(remote.exists && (mode==Mode::prefer_remote || (mode==Mode::sync && local_hash==state.local_digest))) {
        validate_package(remote.bytes);
        return {Status::download,std::move(remote.bytes),state.prior,state.id};
    }
    return {Status::conflict,{},{},{}};
}
}

Options load_options(const fs::path& mapping) {
    const auto content=read_file(options_path(mapping),128*1024);
    if(!content)return {};
    std::istringstream in(*content);
    std::array<std::string,7> lines;
    for(auto& line:lines)if(!std::getline(in,line))throw std::runtime_error("Incomplete sync settings");
    if(in.peek()!=EOF || lines[0]!="Logi Options Open sync 1" || (lines[2]!="0" && lines[2]!="1"))
        throw std::runtime_error("Invalid sync settings");
    Options options;
    if(lines[1]=="webdav")options.provider=Provider::webdav;
    else if(lines[1]=="google_drive")options.provider=Provider::google_drive;
    else if(lines[1]!="onedrive")throw std::runtime_error("Invalid saved sync provider");
    options.automatic=lines[2]=="1";
    const auto wide=[](const std::string& line) {
        auto value=ui_settings::utf8_to_wide(line);
        if(field(value)!=line)throw std::runtime_error("Invalid saved sync option");
        return value;
    };
    options.url=wide(lines[3]);check_url(options.url);
    options.user=wide(lines[4]);options.client_id=wide(lines[5]);options.folder=wide(lines[6]);
    return options;
}

void save_options(const fs::path& mapping,const Options& options) {
    check_url(options.url);
    std::string text="Logi Options Open sync 1\n";
    text+=provider_name(options.provider);text+=options.automatic?"\n1\n":"\n0\n";
    text+=field(options.url)+'\n'+field(options.user)+'\n'+field(options.client_id)+'\n'+field(options.folder.native())+'\n';
    if(text.size()>128*1024)throw std::runtime_error("Sync options are too large");
    write_file(options_path(mapping),text);
}

std::wstring credential_key(const Options& options) {
    if(options.provider==Provider::onedrive)throw std::runtime_error("OneDrive folder sync does not require a credential");
    const auto key=endpoint_id(options);
    const std::wstring prefix=options.provider==Provider::google_drive?L"drive-client/":L"webdav/";
    return prefix+std::wstring(key.begin(),key.end());
}

SyncResult synchronize(const fs::path& mapping,const Options& options,std::span<const unsigned char> local,Mode mode) {
    validate_package(local);
    State state(mapping,options);state.load();
    switch(options.provider) {
    case Provider::onedrive: {
        const auto folder=options.folder.empty()?onedrive_folder():options.folder;
        return reconcile(state,local,read_onedrive(folder),mode,[&](const Remote& old,auto bytes){write_onedrive(folder,bytes,old);});
    }
    case Provider::webdav: {
        const WebDav endpoint{options.url,options.user,read_secret(credential_key(options))};
        return reconcile(state,local,read_webdav(endpoint),mode,[&](const Remote& old,auto bytes){write_webdav(endpoint,bytes,old.etag,!old.exists);});
    }
    case Provider::google_drive: {
        const drive::Client client{options.client_id,read_secret(credential_key(options))};
        return reconcile(state,local,drive::read(client),mode,[&](const Remote& old,auto bytes){drive::write(client,old,bytes);});
    }
    }
    throw std::runtime_error("Unknown sync provider");
}

void acknowledge_download(const fs::path& mapping,const Options& options,const SyncResult& result) {
    acknowledge_download(mapping,options,result,result.package);
}

void acknowledge_download(const fs::path& mapping,const Options& options,const SyncResult& result,
                          std::span<const unsigned char> actual_local) {
    if(result.status!=Status::download)throw std::runtime_error("No sync download is pending");
    State state(mapping,options);state.load();
    if(state.id!=result.target_id)throw std::runtime_error("Sync target changed while applying the download");
    validate_package(result.package);
    validate_package(actual_local);
    const auto local=digest(actual_local),remote=digest(result.package);
    if(state.prior!=result.prior_baseline && (state.local_digest!=local || state.remote_digest!=remote))
        throw std::runtime_error("Sync baseline changed while applying the download");
    state.remember(local,remote);
}

}
