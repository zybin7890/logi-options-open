#include "config_sync_transport.hpp"
#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace config_sync {
namespace {
struct Internet {
    HINTERNET value;
    explicit Internet(HINTERNET h):value(h) {if(!h)throw std::runtime_error("Network connection failed ("+std::to_string(GetLastError())+")");}
    ~Internet(){WinHttpCloseHandle(value);}
    Internet(const Internet&)=delete;
};
void checked(BOOL ok) {
    if(!ok)throw std::runtime_error("Network request failed ("+std::to_string(GetLastError())+")");
}
std::wstring credential(std::wstring_view key) {
    if(key.empty() || key.size()>1024 || key.find(L'\0')!=key.npos)throw std::runtime_error("Invalid credential identifier");
    return L"LogiOptionsOpen/Sync/"+std::wstring(key);
}
}
HttpReply request(std::wstring_view method,std::wstring_view url,std::wstring_view headers,
                  std::span<const unsigned char> body,std::wstring_view user,std::wstring_view password) {
    if(body.size()>max_bytes+8192 || url.size()>8192 || url.empty() || url.find_first_of(L"#\r\n")!=url.npos ||
       url.find(L'\0')!=url.npos || url.find(L'\\')!=url.npos)throw std::runtime_error("Invalid sync URL or request size");
    std::wstring address(url),verb(method);
    URL_COMPONENTS c{};c.dwStructSize=sizeof(c);
    c.dwSchemeLength=c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=c.dwUserNameLength=c.dwPasswordLength=DWORD(-1);
    if(!WinHttpCrackUrl(address.c_str(),0,0,&c) || !c.dwHostNameLength || c.dwUserNameLength || c.dwPasswordLength)
        throw std::runtime_error("Invalid sync URL");
    std::wstring host(c.lpszHostName,c.dwHostNameLength);
    const bool secure=c.nScheme==INTERNET_SCHEME_HTTPS;
    if(!secure && !(c.nScheme==INTERNET_SCHEME_HTTP && (host==L"127.0.0.1"||host==L"[::1]"||host==L"::1"||_wcsicmp(host.c_str(),L"localhost")==0)))
        throw std::runtime_error("Sync requires HTTPS");
    std::wstring path=c.dwUrlPathLength?std::wstring(c.lpszUrlPath,c.dwUrlPathLength):L"/";
    if(c.dwExtraInfoLength)path.append(c.lpszExtraInfo,c.dwExtraInfoLength);
    Internet session(WinHttpOpen(L"LogiOptionsOpen/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    checked(WinHttpSetTimeouts(session.value,5000,6000,10000,10000));
    Internet connection(WinHttpConnect(session.value,host.c_str(),c.nPort,0));
    Internet req(WinHttpOpenRequest(connection.value,verb.c_str(),path.c_str(),nullptr,WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,secure?WINHTTP_FLAG_SECURE:0));
    DWORD disable=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES,policy=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    checked(WinHttpSetOption(req.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)));
    checked(WinHttpSetOption(req.value,WINHTTP_OPTION_AUTOLOGON_POLICY,&policy,sizeof(policy)));
    if(!headers.empty())checked(WinHttpAddRequestHeaders(req.value,headers.data(),static_cast<DWORD>(headers.size()),WINHTTP_ADDREQ_FLAG_ADD));
    const auto send=[&](){
        checked(WinHttpSendRequest(req.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,body.empty()?nullptr:const_cast<unsigned char*>(body.data()),
                                   static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0));
        checked(WinHttpReceiveResponse(req.value,nullptr));
        DWORD status{},size=sizeof(status);
        checked(WinHttpQueryHeaders(req.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr));
        return status;
    };
    HttpReply result;
    result.status=send();
    if(result.status==401 && !user.empty()){
        DWORD supported{},first{},target{};
        if(WinHttpQueryAuthSchemes(req.value,&supported,&first,&target) && target==WINHTTP_AUTH_TARGET_SERVER && (supported&WINHTTP_AUTH_SCHEME_BASIC)){
            std::wstring login(user),secret(password);
            const BOOL set=WinHttpSetCredentials(req.value,WINHTTP_AUTH_TARGET_SERVER,WINHTTP_AUTH_SCHEME_BASIC,login.c_str(),secret.c_str(),nullptr);
            SecureZeroMemory(secret.data(),secret.size()*sizeof(wchar_t));
            checked(set);result.status=send();
        }
    }
    std::array<wchar_t,2048> tag{};DWORD tag_size=sizeof(tag);
    if(WinHttpQueryHeaders(req.value,WINHTTP_QUERY_CUSTOM,L"ETag",tag.data(),&tag_size,nullptr))
        result.etag.assign(tag.data(),tag_size/sizeof(wchar_t));
    std::array<unsigned char,16384> chunk{};
    for(;;){
        DWORD length{};
        checked(WinHttpReadData(req.value,chunk.data(),static_cast<DWORD>(chunk.size()),&length));
        if(!length)break;
        if(result.bytes.size()+length>max_bytes)throw std::runtime_error("Sync response exceeds size limit");
        result.bytes.insert(result.bytes.end(),chunk.begin(),chunk.begin()+length);
    }
    return result;
}
std::wstring read_secret(std::wstring_view key){
    const auto name=credential(key); PCREDENTIALW entry{};
    if(!CredReadW(name.c_str(),CRED_TYPE_GENERIC,0,&entry)){
        if(GetLastError()==ERROR_NOT_FOUND)return {};
        throw std::runtime_error("Cannot read Windows Credential Manager");
    }
    std::wstring value;
    if(entry->CredentialBlobSize)value.assign(reinterpret_cast<const wchar_t*>(entry->CredentialBlob),entry->CredentialBlobSize/sizeof(wchar_t));
    CredFree(entry);return value;
}
void write_secret(std::wstring_view key,std::wstring_view value){
    auto name=credential(key);
    if(value.size()*sizeof(wchar_t)>CRED_MAX_CREDENTIAL_BLOB_SIZE)throw std::runtime_error("Credential exceeds size limit");
    CREDENTIALW entry{};entry.Type=CRED_TYPE_GENERIC;entry.TargetName=name.data();entry.Persist=CRED_PERSIST_LOCAL_MACHINE;
    entry.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(value.data()));
    entry.CredentialBlobSize=static_cast<DWORD>(value.size()*sizeof(wchar_t));
    if(!CredWriteW(&entry,0))throw std::runtime_error("Cannot save Windows credential");
}
void erase_secret(std::wstring_view key){
    if(!CredDeleteW(credential(key).c_str(),CRED_TYPE_GENERIC,0) && GetLastError()!=ERROR_NOT_FOUND)
        throw std::runtime_error("Cannot delete Windows credential");
}
}
