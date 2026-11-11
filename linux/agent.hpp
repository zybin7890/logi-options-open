#pragma once
#include <filesystem>
#include <string>
namespace loo_linux {
std::filesystem::path config_path();
bool running();
bool control(const std::string& command);
int run(const std::filesystem::path& config);
}
