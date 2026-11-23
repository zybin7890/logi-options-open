// Compile the actual discovery and reporting code with a private route store.
#include <windows.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace test_routes {
unsigned writes{};
std::wstring value;
std::vector<BYTE> metadata;
unsigned metadata_writes{};
static HKEY handle(){return reinterpret_cast<HKEY>(1);}
static LSTATUS WINAPI read(HKEY key,LPCWSTR,LPCWSTR name,DWORD flags,LPDWORD,PVOID out,LPDWORD size){
    if(key!=HKEY_CURRENT_USER || std::wstring_view(name)!=L"SensorMetadata" ||
       flags!=RRF_RT_REG_BINARY || metadata.empty())return ERROR_FILE_NOT_FOUND;
    const DWORD capacity=*size;
    *size=static_cast<DWORD>(metadata.size());
    if(!out)return ERROR_SUCCESS;
    if(capacity<*size)return ERROR_MORE_DATA;
    std::memcpy(out,metadata.data(),*size);return ERROR_SUCCESS;
}
static LSTATUS WINAPI create(HKEY,LPCWSTR,DWORD,LPWSTR,DWORD,REGSAM,LPSECURITY_ATTRIBUTES,PHKEY key,LPDWORD){
    *key=handle();return ERROR_SUCCESS;
}
static LSTATUS WINAPI set(HKEY key,LPCWSTR name,DWORD,DWORD type,const BYTE* data,DWORD size){
    if(key==handle() && std::wstring_view(name)==L"SensorMetadata" && type==REG_BINARY) {
        metadata.assign(data,data+size);++metadata_writes;return ERROR_SUCCESS;
    }
    if(key!=handle() || std::wstring_view(name)!=L"Route" || type!=REG_SZ || size<sizeof(wchar_t))
        throw std::runtime_error("unexpected registry write");
    value.assign(reinterpret_cast<const wchar_t*>(data),size/sizeof(wchar_t)-1);
    ++writes;return ERROR_SUCCESS;
}
static LSTATUS WINAPI close(HKEY key){return key==handle()?ERROR_SUCCESS:ERROR_INVALID_HANDLE;}
}
#define RegGetValueW test_routes::read
#define RegCreateKeyExW test_routes::create
#define RegSetValueExW test_routes::set
#define RegCloseKey test_routes::close
DWORD WINAPI fake_wait(HANDLE,DWORD);
ULONGLONG WINAPI fake_clock();
#define WaitForSingleObject fake_wait
#define GetTickCount64 fake_clock
#include "../mouse.cpp"
