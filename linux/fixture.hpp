#pragma once
// Test-only simulated hardware. Never linked into the installed CLI/GUI.
#include "agent.hpp"
#include <fstream>
#include <sstream>
#include <thread>
namespace loo_linux::fixture {
inline void append(const std::filesystem::path& path,const std::string& line){std::ofstream(path,std::ios::app)<<line<<'\n';}
class Device final:public DeviceBackend{
    std::filesystem::path base;bool connected{};unsigned dpi{1600};std::streamoff cursor{};
public:
    explicit Device(std::filesystem::path path):base(std::move(path)){}
    void log(const std::string& text){append(base.string()+".trace",text);}
    bool connect(std::atomic_bool&)override{
        if(std::filesystem::exists(base.string()+".offline"))return false;
        connected=true;log("connect");return true;
    }
    void disconnect()noexcept override{connected=false;try{log("disconnect");}catch(...){}}
    void configure(const Config& config)override{
        if(!connected)throw std::runtime_error("Simulated device disconnected");
        log("configure");if(layout(config).contains(0xfffe))throw std::runtime_error("Simulated configure rejection");
    }
    void restore()override{
        log("restore");if(std::filesystem::exists(base.string()+".restore-fail"))throw std::runtime_error("Simulated restoration failure");
    }
    DeviceEvent read(unsigned timeout)override{
        if(std::filesystem::exists(base.string()+".offline"))return {DeviceEvent::Kind::disconnected};
        std::ifstream events(base.string()+".events");events.seekg(cursor);std::string line;
        if(std::getline(events,line)){
            cursor+=static_cast<std::streamoff>(line.size()+1);std::istringstream stream(line);std::string command;stream>>command;
            if(command=="buttons"){
                DeviceEvent event{DeviceEvent::Kind::buttons};unsigned id;while(stream>>id)event.buttons.push_back(id);return event;
            }
            if(command=="motion"){DeviceEvent event{DeviceEvent::Kind::motion};stream>>event.x>>event.y;return event;}
            if(command=="wheel"){DeviceEvent event{DeviceEvent::Kind::wheel};stream>>event.rotation;return event;}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));return {};
    }
    std::string command(const std::string& command)override{
        log("command "+command);
        if(command=="status-json"||command=="status"||command=="list")
            return "{\"device\":\"Simulated HID++ mouse\",\"simulation\":true,\"battery\":90,\"dpi_choices\":[800,1600,1800,2400],\"settings\":{\"dpi\":"+std::to_string(dpi)+",\"smartshift\":25,\"ratchet\":1,\"hires\":0,\"invert\":0,\"thumb-invert\":0}}";
        if(command.starts_with("set ")){
            std::istringstream stream(command);std::string op,name,value;stream>>op>>name>>value;const auto number=loo_linux::number(value);
            if(name=="dpi"){if(number<100||number>8000)throw std::runtime_error("Simulated DPI rejection");dpi=number;}
            return "{\"setting\":\""+name+"\",\"value\":"+value+'}';
        }
        if(command=="hosts-json"||command=="hosts")return "[{\"slot\":1,\"current\":true,\"paired\":true},{\"slot\":2,\"current\":false,\"paired\":true}]";
        if(command=="toggle-wheel")return "{\"changed\":true}";
        if(command=="recover-input")return "{\"restored\":true}";
        if(command.starts_with("switch-host "))return "{\"sent\":true}";
        throw std::runtime_error("Unsupported simulated command");
    }
    WheelInfo wheel()const override{return {};}
    unsigned threshold()const override{return 20;}
};
class Input final:public InputSink{
    std::filesystem::path trace;
public:
    explicit Input(const std::filesystem::path& base):trace(base.string()+".trace"){}
    void key(unsigned code,bool down)override{append(trace,"key "+std::to_string(code)+" "+(down?"1":"0"));}
    void relative(unsigned code,int value)override{append(trace,"relative "+std::to_string(code)+" "+std::to_string(value));}
    void sync()override{append(trace,"sync");}
};
class Hooks final:public RuntimeHooks{
    std::filesystem::path base;
public:
    explicit Hooks(std::filesystem::path path):base(std::move(path)){}
    std::vector<std::string> foreground()override{std::ifstream file(base.string()+".focus");std::string name;std::getline(file,name);return name.empty()?std::vector<std::string>{}:std::vector<std::string>{name};}
    void action(const Action& action,const std::filesystem::path&)override{
        if(action.operation==Op::text){append(base.string()+".trace","text "+action.text);return;}
        throw std::runtime_error("Desktop action intentionally unavailable in simulated fixture");
    }
};
}
