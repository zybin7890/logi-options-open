#include "config_sync_webdav.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace config_sync {
namespace {

bool strong_etag(std::wstring_view tag) {
    if(tag.size()<2 || tag.front()!=L'"' || tag.back()!=L'"')return false;
    tag.remove_prefix(1);
    tag.remove_suffix(1);
    return std::ranges::all_of(tag,[](wchar_t ch) {
        return ch==0x21 || (ch>=0x23 && ch<=0x7e) || (ch>=0x80 && ch<=0xff);
    });
}

[[noreturn]] void http_error(unsigned status) {
    if(status==412)throw std::runtime_error("WebDAV conflict (HTTP 412): the remote file changed; refresh before uploading");
    if(status==409)throw std::runtime_error("WebDAV conflict (HTTP 409): check the destination folder and refresh");
    throw std::runtime_error("WebDAV request failed (HTTP "+std::to_string(status)+")");
}

}

Remote read_webdav(const WebDav& endpoint) {
    auto reply=request(L"GET",endpoint.url,L"Cache-Control: no-cache\r\n",{},endpoint.user,endpoint.password);
    if(reply.status==404)return {};
    if(reply.status!=200)http_error(reply.status);
    // A server without a strong validator still supports downloading.
    if(!strong_etag(reply.etag))reply.etag.clear();
    return {std::move(reply.bytes),std::move(reply.etag),true};
}

void write_webdav(const WebDav& endpoint,std::span<const unsigned char> bytes,
                  const std::wstring& expected_etag,bool create) {
    if(bytes.size()>max_bytes)throw std::runtime_error("WebDAV upload exceeds the configuration size limit");
    std::wstring headers=L"Content-Type: application/octet-stream\r\n";
    if(create) {
        if(!expected_etag.empty())throw std::runtime_error("WebDAV create requires an absent remote file");
        headers+=L"If-None-Match: *\r\n";
    } else {
        if(!strong_etag(expected_etag))throw std::runtime_error("A strong WebDAV ETag is required to replace the remote file");
        headers+=L"If-Match: "+expected_etag+L"\r\n";
    }
    const auto reply=request(L"PUT",endpoint.url,headers,bytes,endpoint.user,endpoint.password);
    if(reply.status!=200 && reply.status!=201 && reply.status!=204)http_error(reply.status);
}

}
