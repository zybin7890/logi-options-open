#pragma once
#include "config_sync_transport.hpp"
#include <span>
#include <string>

namespace config_sync {

struct WebDav {
    std::wstring url, user, password;
};

Remote read_webdav(const WebDav& endpoint);
void write_webdav(const WebDav& endpoint, std::span<const unsigned char> bytes,
                  const std::wstring& expected_etag, bool create);

}
