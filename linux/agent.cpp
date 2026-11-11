#include "agent.hpp"
#include "mapping.hpp"
#include "mouse.hpp"
#include <linux/uinput.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <deque>
#include <set>

namespace loo_linux {
namespace {
using namespace logi;
using Clock=std::chrono::steady_clock;
std::atomic_bool stopped{};
void signal_stop(int) {stopped.store(true,std::memory_order_relaxed);}
struct Fd {
    int value{-1};
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
};
std::filesystem::path runtime_dir() {
    std::filesystem::path dir;
    if(const char* env=std::getenv("XDG_RUNTIME_DIR");env && *env) dir=env;
    else dir="/tmp";
    dir/="logi-options-open-"+std::to_string(getuid());
    if(::mkdir(dir.c_str(),0700)<0 && errno!=EEXIST) throw std::runtime_error("Cannot create runtime directory");
    struct stat info{};
    if(lstat(dir.c_str(),&info) || !S_ISDIR(info.st_mode) || info.st_uid!=getuid() || (info.st_mode&077))
        throw std::runtime_error("Runtime directory must be owned by current user with mode 0700");
    return dir;
}
sockaddr_un address() {
    sockaddr_un addr{};addr.sun_family=AF_UNIX;
    const auto path=(runtime_dir()/"control.sock").string();
    if(path.size()>=sizeof(addr.sun_path))throw std::runtime_error("Runtime socket path is too long");
    std::memcpy(addr.sun_path,path.c_str(),path.size()+1);return addr;
}
struct Server {
    Fd lock{::open((runtime_dir()/"agent.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600)};
    Fd socket{::socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0)};
    bool bound{};
    Server() {
        if(lock.value<0 || flock(lock.value,LOCK_EX|LOCK_NB))throw std::runtime_error("Linux daemon already running or lock unavailable");
        if(socket.value<0)throw std::runtime_error("Cannot create control socket");
        auto addr=address();unlink(addr.sun_path);
        if(bind(socket.value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))throw std::runtime_error("Cannot bind control socket");
        bound=true;
    }
    ~Server(){if(bound){auto addr=address();unlink(addr.sun_path);}}
    std::string receive() {char data[4096];const auto n=recv(socket.value,data,sizeof(data),0);return n>0?std::string(data,n):std::string{};}
};
struct Input {
    Fd fd{::open("/dev/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC)};
    bool created{};
    void capability(unsigned request,int value) {if(ioctl(fd.value,request,value)<0)throw std::runtime_error("uinput capability setup failed");}
    Input() {
        if(fd.value<0)throw std::runtime_error("Cannot open /dev/uinput; install the opt-in permissions rule and log in again");
        capability(UI_SET_EVBIT,EV_KEY);capability(UI_SET_EVBIT,EV_REL);
        // Only keyboard/media keys and mouse buttons: advertising every BTN_*
        // code also advertises a joystick/tablet and confuses device classifiers.
        for(int key=KEY_ESC;key<=KEY_F24;++key)capability(UI_SET_KEYBIT,key);
        for(int button:{BTN_LEFT,BTN_RIGHT,BTN_MIDDLE,BTN_SIDE,BTN_EXTRA})capability(UI_SET_KEYBIT,button);
        capability(UI_SET_RELBIT,REL_WHEEL);capability(UI_SET_RELBIT,REL_HWHEEL);
        capability(UI_SET_RELBIT,REL_X);capability(UI_SET_RELBIT,REL_Y);
        uinput_setup setup{};setup.id.bustype=BUS_VIRTUAL;setup.id.vendor=0x1209;setup.id.product=0x0001;
        std::strncpy(setup.name,"Logi Options Open virtual input",sizeof(setup.name)-1);
        if(ioctl(fd.value,UI_DEV_SETUP,&setup)<0 || ioctl(fd.value,UI_DEV_CREATE)<0)throw std::runtime_error("Cannot create uinput device");
        created=true;
    }
    ~Input(){if(created)ioctl(fd.value,UI_DEV_DESTROY);}
    void emit(unsigned type,unsigned code,int value) {
        input_event event{};event.type=type;event.code=code;event.value=value;
        if(write(fd.value,&event,sizeof(event))!=sizeof(event))throw std::runtime_error("uinput write failed");
    }
    void sync(){emit(EV_SYN,SYN_REPORT,0);}
    void chord(const std::vector<unsigned>& keys) {
        for(auto key:keys)emit(EV_KEY,key,1);sync();
        for(auto it=keys.rbegin();it!=keys.rend();++it)emit(EV_KEY,*it,0);sync();
    }
};
void check_competing_agents() {
    for(const auto& entry:std::filesystem::directory_iterator("/proc")) {
        auto name=entry.path().filename().string();if(name.empty() || name.find_first_not_of("0123456789")!=name.npos)continue;
        std::ifstream comm(entry.path()/"comm");std::getline(comm,name);
        if(name=="solaar" || name=="logid")throw std::runtime_error("Another mouse manager ("+name+") is running; stop it explicitly before starting this daemon");
    }
}
struct Diversion {
    Mouse& mouse;
    std::map<unsigned,Byte> original;
    std::optional<std::pair<Byte,Byte>> original_thumb;
    unsigned native{1},diverted{1};int direction{1};
    explicit Diversion(Mouse& device):mouse(device){}
    bool restore() {
        bool ok=true;
        for(const auto& [id,flags]:original)if(!mouse.request(mouse.buttons,3,{high(id),low(id),flags}))ok=false;
        original.clear();
        if(original_thumb) {
            if(!mouse.request(mouse.thumb,2,{original_thumb->first,original_thumb->second}))ok=false;
            original_thumb.reset();
        }
        return ok;
    }
    ~Diversion(){mouse.channel->finish_shutdown();if(!restore())std::fprintf(stderr,"Warning: device disconnected or input restoration failed; run recover-input after reconnecting.\n");}
    void enable(const Config& config) {
        for(const auto& [id,xy]:layout(config)) {
            if(id==thumb) {
                const auto info=mouse.request(mouse.thumb,0),state=mouse.request(mouse.thumb,1);
                if(!info || !state)throw std::runtime_error("Thumb wheel feature unavailable");
                auto data=info.packet.data();native=word(data);diverted=word(data.subspan(2));
                if(!native || !diverted)throw std::runtime_error("Invalid thumb wheel resolution");
                const auto mode=state.packet.data();
                if(mode[0])throw std::runtime_error("Thumb wheel already diverted; stop other managers or use recover-input");
                original_thumb={{mode[0],mode[1]}};
                direction=((data[4]&1)!=(mode[1]&1))?1:-1;
                if(!mouse.request(mouse.thumb,2,{1,0}))throw std::runtime_error("Cannot divert thumb wheel");
                auto checked=mouse.request(mouse.thumb,1);
                if(!checked || checked.packet.data()[0]!=1)throw std::runtime_error("Thumb wheel diversion was not acknowledged");
            } else {
                auto before=mouse.request(mouse.buttons,2,{high(id),low(id)});
                if(!before || word(before.packet.data())!=id)throw std::runtime_error("Unsupported control: "+std::to_string(id));
                const auto flags=before.packet.data()[2];
                if(flags&0x11)throw std::runtime_error("Control already diverted; stop other managers or use recover-input");
                original.emplace(id,Byte(0x22|(flags&0x11)));
                const Byte desired=xy?0x33:0x03;
                if(!mouse.request(mouse.buttons,3,{high(id),low(id),desired}))throw std::runtime_error("Cannot divert control");
                auto checked=mouse.request(mouse.buttons,2,{high(id),low(id)});
                if(!checked || (checked.packet.data()[2]&0x11)!=(desired&0x11))throw std::runtime_error("Control diversion was not acknowledged");
            }
        }
    }
};
struct Pending {std::vector<Action> actions;std::size_t next{};Clock::time_point due{};};
struct State {bool down{},skip_first{};int x{},y{};};
void execute(const Action& action,Input& input,Mouse& mouse) {
    switch(action.operation) {
    case Op::keys: input.chord(action.keys);break;
    case Op::wheel_mode: {
        auto mode=logi::get(mouse,L"ratchet");if(!mode || !logi::set(mouse,L"ratchet",!*mode))throw std::runtime_error("Wheel mode change failed");break;
    }
    case Op::scroll_x:input.emit(EV_REL,REL_HWHEEL,action.value);input.sync();break;
    case Op::scroll_y:input.emit(EV_REL,REL_WHEEL,action.value);input.sync();break;
    case Op::open: {
        const auto child=fork();if(child<0)throw std::runtime_error("Cannot start xdg-open");
        if(child==0){execlp("xdg-open","xdg-open",action.text.c_str(),static_cast<char*>(nullptr));_exit(127);}break;
    }
    case Op::wait:break;
    }
}
}
std::filesystem::path config_path() {
    if(auto value=std::getenv("LOGI_OPTIONS_OPEN_CONFIG");value && *value)return value;
    if(auto value=std::getenv("XDG_CONFIG_HOME");value && *value)return std::filesystem::path(value)/"logi-options-open/mappings.ini";
    if(auto value=std::getenv("HOME");value && *value)return std::filesystem::path(value)/".config/logi-options-open/mappings.ini";
    throw std::runtime_error("HOME or XDG_CONFIG_HOME is required");
}
bool running() {
    Fd lock(::open((runtime_dir()/"agent.lock").c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW));
    if(lock.value<0)return false;
    if(flock(lock.value,LOCK_EX|LOCK_NB)==0)return false;
    return errno==EWOULDBLOCK;
}
bool control(const std::string& command) {
    if(!running())return false;
    Fd socket(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0));auto addr=address();
    return sendto(socket.value,command.data(),command.size(),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==static_cast<ssize_t>(command.size());
}
int run(const std::filesystem::path& config_file) {
    auto config=load(config_file);check_competing_agents();Server server;Input input;
    stopped=false;signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);
    auto mouse=logi::connect(&stopped);if(!mouse)throw std::runtime_error("No accessible HID++ mouse; check hidraw permissions and connection");
    Diversion diversion(*mouse);diversion.enable(config);mouse->channel->keep_notifications();
    const auto dpi=logi::get(*mouse,L"dpi").value_or(1000);const auto threshold=std::max(20u,dpi/12);
    auto controls=layout(config);std::map<unsigned,State> state;WheelAccumulator wheel;
    std::deque<Pending> pending;
    auto fire=[&](Binding binding) {
        if(auto found=config.bindings.find(binding);found!=config.bindings.end()) {
            if(pending.size()>=128)throw std::runtime_error("Action queue full; stopping to restore normal input");
            pending.push_back({found->second,0,Clock::now()});
        }
    };
    std::fprintf(stderr,"Linux daemon ready: %s\n",mouse->device.name.c_str());
    while(!stopped.load(std::memory_order_relaxed)) {
        const auto command=server.receive();
        if(command=="stop")break;
        if(command.starts_with("reload ")) {
            bool fatal=false;
            try {
                auto path=std::filesystem::path(command.substr(7));
                if(std::filesystem::weakly_canonical(path)!=std::filesystem::weakly_canonical(config_file))throw std::runtime_error("Reload must use the running configuration path");
                auto fresh=load(path);
                if(layout(fresh)!=controls) {
                    if(!diversion.restore()){fatal=true;throw std::runtime_error("Could not restore old controls before reload");}
                    try {diversion.enable(fresh);}
                    catch(...) {diversion.restore();try{diversion.enable(config);}catch(...){fatal=true;throw;}throw;}
                }
                config=std::move(fresh);controls=layout(config);state.clear();pending.clear();wheel={};
                mouse->channel->discard_notifications();std::fprintf(stderr,"Configuration reloaded\n");
            } catch(const std::exception& e) {std::fprintf(stderr,"Reload failed: %s\n",e.what());}
            if(fatal)throw std::runtime_error("Reload rollback failed; stopping to restore input");
        }
        auto now=Clock::now();unsigned timeout=100;
        for(auto it=pending.begin();it!=pending.end();) {
            while(it->next<it->actions.size() && it->due<=now) {
                const auto& action=it->actions[it->next++];
                if(action.operation==Op::wait){it->due=now+std::chrono::milliseconds(action.value);break;}
                execute(action,input,*mouse);
            }
            if(it->next==it->actions.size())it=pending.erase(it);
            else {timeout=std::min(timeout,static_cast<unsigned>(std::max<std::int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(it->due-now).count())));++it;}
        }
        while(waitpid(-1,nullptr,WNOHANG)>0){}
        auto reply=mouse->channel->read(timeout);
        if(reply.result==Result::timeout)continue;
        if(reply.result==Result::stopped)break;
        if(!reply)throw std::runtime_error("HID input failed/disconnected; daemon stopped");
        const auto& packet=reply.packet;if(packet.size!=20 || packet.bytes[1]!=mouse->device.slot)continue;
        auto data=packet.data();
        if(packet.bytes[2]==mouse->thumb && packet.bytes[3]==0 && controls.contains(thumb)) {
            const auto delta=static_cast<std::int16_t>(word(data));
            const auto steps=wheel.steps(delta,diversion.native,diversion.diverted,diversion.direction,config.speed,data[4]==1);
            for(int n=0;n<std::abs(steps);++n)fire({thumb,steps<0?Direction::left:Direction::right});
        } else if(packet.bytes[2]==mouse->buttons && packet.bytes[3]==0) {
            std::set<unsigned> pressed;for(unsigned offset=0;offset<8;offset+=2)if(auto id=word(data.subspan(offset));id)pressed.insert(id);
            for(const auto& [id,xy]:controls) {
                if(id==thumb)continue;auto& s=state[id];const bool down=pressed.contains(id);
                if(down && !s.down){s={true,true,0,0};if(!xy)fire({id,Direction::tap});}
                if(!down && s.down){if(xy)fire({id,gesture(s.x,s.y,threshold)});s={};}
            }
        } else if(packet.bytes[2]==mouse->buttons && packet.bytes[3]==0x10) {
            const int x=static_cast<std::int16_t>(word(data)),y=static_cast<std::int16_t>(word(data.subspan(2)));
            for(auto& [id,s]:state)if(s.down && controls[id]) {
                if(s.skip_first){s.skip_first=false;continue;}
                s.x=std::clamp(s.x+x,-100000,100000);s.y=std::clamp(s.y+y,-100000,100000);
            }
        }
    }
    return 0;
}
}
