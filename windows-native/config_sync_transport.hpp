#pragma once
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace config_sync {
inline constexpr std::size_t max_bytes=4*1024*1024;
struct Remote {std::vector<unsigned char> bytes; std::wstring etag; bool exists{}; bool conflict{};};
struct HttpReply {unsigned status{}; std::wstring etag; std::vector<unsigned char> bytes;};
HttpReply request(std::wstring_view method,std::wstring_view url,std::wstring_view headers={},
                  std::span<const unsigned char> body={},std::wstring_view user={},std::wstring_view password={});
std::wstring read_secret(std::wstring_view key);
void write_secret(std::wstring_view key,std::wstring_view value);
void erase_secret(std::wstring_view key);
}
