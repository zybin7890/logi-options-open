#pragma once
#include "config_sync_transport.hpp"
#include <filesystem>

namespace config_sync {

enum class Provider { onedrive, webdav, google_drive };
struct Options {
    Provider provider{Provider::onedrive};
    std::wstring url,user,client_id;
    std::filesystem::path folder;
    bool automatic{};
};
Options load_options(const std::filesystem::path& mapping);
void save_options(const std::filesystem::path& mapping,const Options&);
std::wstring credential_key(const Options&);

enum class Mode { sync, prefer_local, prefer_remote };
enum class Status { unchanged, uploaded, download, conflict };
struct SyncResult {
    Status status{};
    std::vector<unsigned char> package;
    std::string prior_baseline,target_id;
};
SyncResult synchronize(const std::filesystem::path& mapping,const Options&,
                       std::span<const unsigned char> local,Mode mode=Mode::sync);
void acknowledge_download(const std::filesystem::path& mapping,const Options&,const SyncResult&);
void acknowledge_download(const std::filesystem::path& mapping,const Options&,const SyncResult&,
                          std::span<const unsigned char> actual_local);

}
