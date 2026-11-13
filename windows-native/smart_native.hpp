#pragma once

#include <windows.h>
#include <filesystem>

void show_smart_actions(HWND owner, const std::filesystem::path& directory);
