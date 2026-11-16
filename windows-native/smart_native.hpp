#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

struct SmartLibraryItem {
    std::wstring name, sequence;
};

std::vector<SmartLibraryItem> read_smart_library(const std::filesystem::path& mapping);

void show_smart_actions(HWND owner, const std::filesystem::path& directory);
