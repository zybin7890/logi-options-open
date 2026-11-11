#include "fixture.hpp"
#include <iostream>
int main(int argc,char** argv){
    try{
        if(argc!=2)throw std::runtime_error("Usage: logi-linux-fixture-daemon CONFIG (simulation only)");
        const auto config=std::filesystem::absolute(argv[1]);loo_linux::fixture::Device device(config);loo_linux::fixture::Input input(config);loo_linux::fixture::Hooks hooks(config);
        return loo_linux::run_with(config,device,input,hooks);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
