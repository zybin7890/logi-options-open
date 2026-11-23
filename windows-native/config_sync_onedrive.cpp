#include "config_sync_onedrive.hpp"

#include <windows.h>
#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace config_sync {
namespace fs=std::filesystem;
namespace {
constexpr wchar_t config_name[]=L"settings.logi";
constexpr wchar_t old_name[]=L"settings.previous.tmp";
constexpr wchar_t temp_name[]=L".settings-upload.tmp";

[[noreturn]] void failed(const char* operation,DWORD error=GetLastError()) {
    throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}
struct Handle {
    HANDLE h{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE value=INVALID_HANDLE_VALUE):h(value){}
    ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
bool directory(const fs::path& path) {
    std::error_code ec;
    return path.is_absolute() && fs::is_directory(path,ec);
}
fs::path account_folder(std::wstring_view account) {
    const auto key=L"Software\\Microsoft\\OneDrive\\Accounts\\"+std::wstring{account};
    DWORD size{};
    constexpr auto flags=RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ;
    if(RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"UserFolder",flags,nullptr,nullptr,&size)!=ERROR_SUCCESS ||
       !size || size>65536)return {};
    std::wstring path((size+sizeof(wchar_t)-1)/sizeof(wchar_t),L'\0');
    if(RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"UserFolder",flags,nullptr,path.data(),&size)!=ERROR_SUCCESS)return {};
    path.resize(std::wcslen(path.c_str()));
    return directory(path)?fs::path(path):fs::path{};
}
void verify_entry(const fs::path& path,bool want_directory) {
    Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(h.h==INVALID_HANDLE_VALUE)failed("Inspect OneDrive sync path");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if(!GetFileInformationByHandleEx(h.h,FileAttributeTagInfo,&info,sizeof(info)))failed("Inspect OneDrive path type");
    // OneDrive cloud placeholders are reparse points too. Only redirects such as
    // symlinks and junctions are unsafe at the managed location.
    if(((info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=want_directory ||
       ((info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) && (info.ReparseTag&0x20000000u)))
        throw std::runtime_error("Unexpected file type or redirected OneDrive sync path");
}
fs::path sync_folder(const fs::path& root,bool create) {
    if(!directory(root))throw std::runtime_error("OneDrive local sync root is unavailable");
    const auto folder=root/L"Logi Options Open";
    if(create && !CreateDirectoryW(folder.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
        failed("Create OneDrive configuration folder");
    const auto attributes=GetFileAttributesW(folder.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        const auto err=GetLastError();
        if(!create && err==ERROR_FILE_NOT_FOUND)return {};
        failed("Find OneDrive configuration folder",err);
    }
    verify_entry(folder,true);
    return folder;
}

// A per-folder, delete-on-close lock also serializes other sessions on this
// machine. Windows releases the handle if a sync process exits unexpectedly.
HANDLE lock_folder(const fs::path& folder) {
    const auto path=folder/L".logi-sync-lock.tmp";
    const auto deadline=GetTickCount64()+3000;
    for(;;) {
        const auto h=CreateFileW(path.c_str(),GENERIC_READ|DELETE,0,nullptr,OPEN_ALWAYS,
            FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_OPEN_REPARSE_POINT|FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if(h!=INVALID_HANDLE_VALUE)return h;
        const auto error=GetLastError();
        if(error!=ERROR_SHARING_VIOLATION || GetTickCount64()>=deadline)failed("Lock OneDrive configuration folder",error);
        Sleep(20);
    }
}
void check_conflicts(const fs::path& folder) {
    for(const auto& item:fs::directory_iterator(folder)) {
        auto name=item.path().filename().wstring();
        CharLowerBuffW(name.data(),static_cast<DWORD>(name.size()));
        if(name==config_name)continue;
        if(name==old_name || name==temp_name ||
           (name.starts_with(L"settings") && (name.ends_with(L".logi") || name.starts_with(L"settings.logi"))))
            throw std::runtime_error("OneDrive conflict or interrupted save: inspect the preserved copies in Logi Options Open");
    }
}
Remote read_file(const fs::path& path,FILE_ATTRIBUTE_TAG_INFO* tag=nullptr) {
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES) {
        const auto error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND)return {};
        failed("Find OneDrive configuration",error);
    }
    verify_entry(path,false);
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(h.h==INVALID_HANDLE_VALUE)failed("Read OneDrive configuration");
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(h.h,&size))failed("Measure OneDrive configuration");
    if(size.QuadPart<0 || static_cast<unsigned long long>(size.QuadPart)>max_bytes)
        throw std::runtime_error("OneDrive configuration exceeds the size limit");
    Remote remote;
    remote.exists=true;remote.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    if(!remote.bytes.empty()) {
        DWORD count{};
        if(!ReadFile(h.h,remote.bytes.data(),static_cast<DWORD>(remote.bytes.size()),&count,nullptr))failed("Read OneDrive configuration bytes");
        if(count!=remote.bytes.size())throw std::runtime_error("Incomplete local OneDrive configuration");
    }
    if(tag && !GetFileInformationByHandleEx(h.h,FileAttributeTagInfo,tag,sizeof(*tag))) *tag={};
    return remote;
}

void stage_file(const fs::path& path,std::span<const unsigned char> bytes) {
    Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(h.h==INVALID_HANDLE_VALUE)failed("Create OneDrive staging file");
    DWORD written{};
    if(!bytes.empty() && !WriteFile(h.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr))failed("Stage OneDrive configuration");
    if(written!=bytes.size())throw std::runtime_error("Incomplete OneDrive configuration write");
    if(!FlushFileBuffers(h.h))failed("Flush OneDrive configuration");
}
}

fs::path onedrive_folder() {
    if(auto path=account_folder(L"Personal");!path.empty())return path;
    HKEY accounts{};
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\OneDrive\\Accounts",0,KEY_ENUMERATE_SUB_KEYS,&accounts)==ERROR_SUCCESS) {
        fs::path result;
        for(DWORD index=0;result.empty();++index) {
            wchar_t account[256]{};DWORD count=256;
            if(RegEnumKeyExW(accounts,index,account,&count,nullptr,nullptr,nullptr,nullptr)!=ERROR_SUCCESS)break;
            result=account_folder({account,count});
        }
        RegCloseKey(accounts);
        if(!result.empty())return result;
    }
    for(const auto* variable:{L"OneDrive",L"OneDriveConsumer",L"OneDriveCommercial"}) {
        const auto* value=_wgetenv(variable);
        if(value && directory(value))return value;
    }
    return {};
}

Remote read_onedrive(const fs::path& root) {
    const auto folder=sync_folder(root,false);
    if(folder.empty())return {};
    Handle lock(lock_folder(folder));
    check_conflicts(folder);
    return read_file(folder/config_name);
}

void write_onedrive(const fs::path& root,std::span<const unsigned char> bytes,const Remote& expected) {
    if(bytes.size()>max_bytes)throw std::runtime_error("OneDrive upload exceeds the configuration size limit");
    const auto folder=sync_folder(root,true);
    Handle lock(lock_folder(folder));
    check_conflicts(folder);
    const auto path=folder/config_name,staged=folder/temp_name,old=folder/old_name;
    const auto before=read_file(path);
    if(before.exists!=expected.exists || (before.exists && before.bytes!=expected.bytes))
        throw std::runtime_error("OneDrive configuration changed since the last read; both versions preserved");
    if(before.exists && std::ranges::equal(before.bytes,bytes))return;
    stage_file(staged,bytes);
    if(!before.exists) {
        // No replacement flag: a file concurrently materialized by OneDrive wins.
        if(!MoveFileExW(staged.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)) {
            const auto error=GetLastError();
            DeleteFileW(staged.c_str());
            failed("Another file appeared or the OneDrive commit failed",error);
        }
    } else {
        // The cloud client does not share our application lock. Verify the actual
        // displaced file; if another writer won the race, keep its exact bytes.
        if(!ReplaceFileW(path.c_str(),staged.c_str(),old.c_str(),0,nullptr,nullptr))
            failed("Replace OneDrive configuration; staged copies preserved");
        if(read_file(old).bytes!=expected.bytes)
            throw std::runtime_error("Concurrent OneDrive replacement: previous and incoming copies preserved");
        if(!DeleteFileW(old.c_str()))failed("Remove verified previous OneDrive configuration");
    }
}

bool onedrive_in_sync(const fs::path& root,std::span<const unsigned char> expected) {
    const auto folder=sync_folder(root,false);
    if(folder.empty())return false;
    Handle lock(lock_folder(folder));
    check_conflicts(folder);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const auto snapshot=read_file(folder/config_name,&tag);
    if(!snapshot.exists || !std::ranges::equal(snapshot.bytes,expected) || !(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    const auto dll=LoadLibraryExW(L"cldapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll)return false;
    using Query=DWORD(WINAPI*)(DWORD,DWORD);
    const auto query=reinterpret_cast<Query>(GetProcAddress(dll,"CfGetPlaceholderStateFromAttributeTag"));
    const auto state=query?query(tag.FileAttributes,tag.ReparseTag):0;
    FreeLibrary(dll);
    // CF_PLACEHOLDER_STATE_PLACEHOLDER (1) and IN_SYNC (8), excluding INVALID.
    return state!=0xffffffffu && (state&9u)==9u;
}
}
