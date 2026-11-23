#include "../config_sync_onedrive.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace config_sync;
namespace {
using Bytes=std::vector<unsigned char>;
void require(bool ok){if(!ok)throw std::runtime_error("OneDrive sync assertion failed");}
template<class Func>void rejects(Func fn){
    try{fn();}catch(const std::exception&){return;}
    throw std::runtime_error("Unexpectedly accepted a OneDrive conflict");
}
}
int main(int argc,char** argv) try {
    if(argc!=2)return 2;
    const auto root=fs::absolute(argv[1]);
    // The test driver supplies a new private directory on each run.
    if(!fs::create_directory(root))throw std::runtime_error("Fixture directory must be new");
    struct Cleanup{fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{root};
    const auto folder=root/L"Logi Options Open",file=folder/L"settings.logi";
    const Bytes a{'A',0,0xff},b{'B',0,'2'},c{'C'};
    const auto missing=read_onedrive(root);require(!missing.exists && !fs::exists(folder));
    write_onedrive(root,a,missing);
    const auto first=read_onedrive(root);require(first.exists && first.bytes==a);
    write_onedrive(root,b,first);
    const auto second=read_onedrive(root);require(second.bytes==b && !onedrive_in_sync(root,b));
    rejects([&]{write_onedrive(root,c,first);});require(read_onedrive(root).bytes==b);
    rejects([&]{write_onedrive(root,c,missing);});
    write_onedrive(root,b,second);
    const auto conflict=folder/L"settings-SecondPC.logi";
    std::ofstream(conflict)<<"preserve this";
    rejects([&]{read_onedrive(root);});
    rejects([&]{write_onedrive(root,c,second);});
    require(fs::file_size(conflict)==13);
    fs::remove(conflict);
    std::ofstream(file,std::ios::binary|std::ios::trunc)<<"C";
    require(read_onedrive(root).bytes==c);
    rejects([&]{write_onedrive(root,a,second);});
    fs::remove(file);
    rejects([&]{write_onedrive(root,b,second);});
    const Bytes oversized(max_bytes+1);
    rejects([&]{write_onedrive(root,oversized,missing);});
    {std::ofstream f(file,std::ios::binary);f.seekp(max_bytes);f.put('!');}
    rejects([&]{read_onedrive(root);});require(fs::file_size(file)==max_bytes+1);
    std::cout<<"PASS local OneDrive create, replace, stale versions, conflicted copies, external updates, size and confirmation\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
