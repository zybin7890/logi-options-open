#include "fixture.hpp"
#include <iostream>

namespace {
int isolated_worker(const std::filesystem::path& config) {
    loo_linux::fixture::Device device(config, true);
    loo_linux::fixture::Input input(config);
    loo_linux::fixture::Hooks hooks(config);
    return loo_linux::run_with(config, device, input, hooks);
}

int wayland_worker(const std::filesystem::path& config) {
    // The HID report stream lives only as long as this worker. Do not replay
    // simulated button reports from a previous incarnation of the device.
    loo_linux::fixture::Device device(config, true);
    return loo_linux::run_device(config, device);
}
}

int main(int argc,char** argv){
    try{
        if (argc == 3 && std::string_view(argv[1]) == "--wayland-worker")
            return loo_linux::supervise(std::filesystem::absolute(argv[2]), wayland_worker);
        if(argc!=2)throw std::runtime_error("Usage: logi-linux-fixture-daemon [--wayland-worker] CONFIG (simulation only)");
        return loo_linux::supervise(std::filesystem::absolute(argv[1]), isolated_worker);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
