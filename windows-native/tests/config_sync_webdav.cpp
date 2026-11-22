#include "../config_sync_webdav.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace config_sync;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void expect_failure(const auto& operation,std::string_view reason={}) {
    try {operation();}
    catch(const std::exception& error) {
        const std::string_view text=error.what();
        check(text.find("sample-user")==text.npos && text.find("sample-password")==text.npos,
              "WebDAV error exposed credentials");
        check(text.find(reason)!=text.npos,"Unexpected WebDAV error");
        return;
    }
    throw std::runtime_error("WebDAV operation unexpectedly succeeded");
}
}

int main(int argc,char** argv) {
    try {
        check(argc==2,"Expected loopback server port");
        const std::string port=argv[1];
        const auto origin=L"http://127.0.0.1:"+std::wstring(port.begin(),port.end());
        const auto target=[&](std::wstring_view path) {
            return WebDav{origin+std::wstring(path),L"sample-user",L"sample-password"};
        };
        const auto file=target(L"/dav/My%20Mouse.bin?version=1");
        const std::vector<unsigned char> one{0,0xff,42,10},two{1,97,0,2,3};
        check(!read_webdav(file).exists,"Missing file should be reported as absent");
        write_webdav(file,one,{},true);
        auto first=read_webdav(file);
        check(first.exists && first.bytes==one && first.etag==L"\"v1\"","Incorrect read after create");
        expect_failure([&]{write_webdav(file,two,{},true);},"412");
        write_webdav(file,two,first.etag,false);
        expect_failure([&]{write_webdav(file,one,first.etag,false);},"412");
        const auto second=read_webdav(file);
        check(second.exists && second.bytes==two && second.etag==L"\"v2\"","Stale write overwrote competing update");
        for(const auto path:{L"/weak",L"/missing-etag",L"/multiple-tags"}) {
            const auto remote=read_webdav(target(path));
            check(remote.exists && remote.etag.empty() && !remote.bytes.empty(),"Unsafe download validator");
            expect_failure([&]{write_webdav(target(path),one,remote.etag,false);},"strong");
        }
        for(const auto tag:{L"*",L"W/\"v1\"",L"\"v1\", \"v2\"",L"\"a\r\nInjected: value\""}) {
            expect_failure([&]{write_webdav(file,one,tag,false);},"strong");
        }
        auto unauthorized=file;unauthorized.password=L"invalid";
        expect_failure([&]{read_webdav(unauthorized);},"401");
        expect_failure([&]{read_webdav(target(L"/redirect"));},"307");
        expect_failure([&]{write_webdav(target(L"/redirect"),one,{},true);},"307");
        expect_failure([&]{read_webdav(target(L"/large-stream"));});
        expect_failure([&]{write_webdav(file,std::vector<unsigned char>(max_bytes+1),{},true);},"size");
        expect_failure([&]{read_webdav(WebDav{L"http://example.invalid/config",L"sample-user",L"sample-password"});});
        std::cout<<"PASS authenticated GET, conditional PUT, conflict, response limit and redirect checks\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
