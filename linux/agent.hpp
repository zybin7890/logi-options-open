#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include "mapping.hpp"
namespace loo_linux {
struct DeviceEvent {
    enum class Kind {idle,buttons,motion,wheel,disconnected};
    Kind kind{Kind::idle};
    std::vector<unsigned> buttons;
    int x{},y{},rotation{};
    bool begin{};
};
struct WheelInfo {unsigned native{1},diverted{1};int direction{1};};
// Narrow device seam for deterministic Linux integration tests. Production uses
// HID++/uinput; test executables supply an explicitly simulated device.
struct DeviceBackend {
    virtual ~DeviceBackend()=default;
    virtual bool connect(std::atomic_bool& stop)=0;
    virtual void disconnect() noexcept=0;
    virtual void configure(const Config&)=0;
    virtual void restore()=0;
    virtual DeviceEvent read(unsigned timeout_ms)=0;
    virtual std::string command(const std::string&)=0;
    virtual WheelInfo wheel() const=0;
    virtual unsigned threshold() const=0;
};
struct InputSink {
    virtual ~InputSink()=default;
    virtual void key(unsigned code,bool down)=0;
    virtual void relative(unsigned code,int value)=0;
    virtual void sync()=0;
    virtual std::string sysname() const{return {};}
};
struct RuntimeHooks {
    virtual ~RuntimeHooks()=default;
    virtual void validate(const Config&){}
    virtual void configure(const Config&){}
    virtual std::vector<std::string> foreground(){return {};}
    virtual std::vector<std::vector<Action>> hotkeys(){return {};}
    virtual bool busy() const{return false;}
    virtual void suspend_hotkeys(bool){}
    virtual void action(const Action&,const std::filesystem::path&);
};
std::unique_ptr<InputSink> make_uinput_sink();
std::filesystem::path config_path();
bool running();
std::string transact(const std::string& command,unsigned timeout_ms=30000);
std::string dispatch(const std::string& command);
std::vector<Action> load_actions(const std::filesystem::path& file);
int preview_ring(const std::filesystem::path& file,const std::string& folder={});
int run(const std::filesystem::path& config);
int run_with(const std::filesystem::path& config,DeviceBackend&,InputSink&,RuntimeHooks&);
}
