#pragma once
#include "config_sync_transport.hpp"
#include <span>
#include <string>

namespace config_sync::drive {
struct Client { std::wstring id,secret; };
void authorize(const Client& client);
void disconnect(const Client& client);
std::wstring account_id(const Client& client);
Remote read(const Client& client);
void write(const Client& client,const Remote& expected,std::span<const unsigned char> bytes);
}
