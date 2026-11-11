#pragma once

#include <windows.h>

#include <span>
#include <string>

inline constexpr int ring_back = -2;
int choose_ring_action(std::span<const std::wstring> labels, HANDLE stop, bool submenu = false);
