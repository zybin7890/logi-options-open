#pragma once
#include "config_sync_transport.hpp"
#include <filesystem>

namespace config_sync {
std::filesystem::path onedrive_folder();
Remote read_onedrive(const std::filesystem::path& root);
void write_onedrive(const std::filesystem::path& root,std::span<const unsigned char> bytes,const Remote& expected);
bool onedrive_in_sync(const std::filesystem::path& root,std::span<const unsigned char> expected);
}
