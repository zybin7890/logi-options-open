#include "agent.hpp"
#include "hid_diversion.hpp"
#include "desktop.hpp"
#include "mouse.hpp"
#include <linux/uinput.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <deque>
#include <sstream>
#include <set>
#include <thread>
namespace loo_linux {
namespace {
using namespace logi;
using Clock=std::chrono::steady_clock;
std::atomic_bool stopped{};
void signal_stop(int){stopped.store(true,std::memory_order_relaxed);}
std::string json(std::string_view value){
    std::string out="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=c;}return out+'"';
}
std::wstring wide(std::string_view value){return {value.begin(),value.end()};}
struct Fd{
    int value{-1};explicit Fd(int fd=-1):value(fd){}~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other)noexcept:value(other.value){other.value=-1;}
    Fd& operator=(Fd&& other)noexcept{if(this!=&other){if(value>=0)::close(value);value=other.value;other.value=-1;}return *this;}
};
std::filesystem::path runtime_dir(){
    std::filesystem::path dir;
    if(const char* env=std::getenv("XDG_RUNTIME_DIR");env&&*env)dir=env;else dir="/tmp";
    dir/="logi-options-open-"+std::to_string(getuid());
    if(::mkdir(dir.c_str(),0700)<0&&errno!=EEXIST)throw std::runtime_error("Cannot create runtime directory");
    struct stat info{};
    if(lstat(dir.c_str(),&info)||!S_ISDIR(info.st_mode)||info.st_uid!=getuid()||(info.st_mode&077))throw std::runtime_error("Runtime directory must be owned by current user with mode 0700");
    return dir;
}
Fd acquire_lock(){
    Fd lock(::open((runtime_dir()/"agent.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600));
    if(lock.value<0||flock(lock.value,LOCK_EX|LOCK_NB))throw std::runtime_error("Mouse backend is busy; another command or daemon owns the device");return lock;
}
sockaddr_un address(){
    sockaddr_un addr{};addr.sun_family=AF_UNIX;const auto path=(runtime_dir()/"control.sock").string();
    if(path.size()>=sizeof(addr.sun_path))throw std::runtime_error("Runtime socket path is too long");
    std::memcpy(addr.sun_path,path.c_str(),path.size()+1);return addr;
}
void respond(int fd,bool success,const std::string& body)noexcept{
    const auto message=(success?"OK\n":"ERR\n")+body;send(fd,message.data(),message.size(),MSG_NOSIGNAL);
}
struct Request{Fd peer;std::string body;};
struct Server{
    Fd lock{acquire_lock()},socket{::socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0)};
    struct Client{Fd fd;Clock::time_point deadline;};std::vector<Client> clients;std::string path;
    Server(){
        if(socket.value<0)throw std::runtime_error("Cannot create control socket");
        auto addr=address();path=addr.sun_path;unlink(path.c_str());
        if(bind(socket.value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||listen(socket.value,16))throw std::runtime_error("Cannot bind/listen control socket");
    }
    ~Server(){if(!path.empty())unlink(path.c_str());}
    std::optional<Request> receive(){
        for(;;){
            Fd peer(accept4(socket.value,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC));if(peer.value<0)break;
            ucred credentials{};socklen_t size=sizeof(credentials);
            if(getsockopt(peer.value,SOL_SOCKET,SO_PEERCRED,&credentials,&size)||credentials.uid!=getuid())continue;
            if(clients.size()>=16){respond(peer.value,false,"Too many pending clients");continue;}
            clients.push_back({std::move(peer),Clock::now()+std::chrono::seconds(5)});
        }
        for(auto it=clients.begin();it!=clients.end();){
            char buffer[65536];const auto count=recv(it->fd.value,buffer,sizeof(buffer),MSG_DONTWAIT|MSG_TRUNC);
            if(count>0&&count<static_cast<ssize_t>(sizeof(buffer))){Request request{std::move(it->fd),std::string(buffer,count)};clients.erase(it);return request;}
            if(count==0||count>=static_cast<ssize_t>(sizeof(buffer))||(count<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)||Clock::now()>it->deadline)it=clients.erase(it);else ++it;
        }return {};
    }
};
struct SupervisedWorker {
    Server& server;
    int notice;
    pid_t parent;
    bool possible_diversion,cleanup_only;

    void notify(char event) const {
        // Each lifecycle phase is reported at most once. The channel is CLOEXEC.
        while(send(notice,&event,1,MSG_NOSIGNAL)<0 && errno==EINTR){}
    }
};
SupervisedWorker* supervision{};

struct SupervisorSignals {
    sigset_t original{};
    Fd events;
    bool stopping{};
    SupervisorSignals(){
        sigset_t watched{};sigemptyset(&watched);
        for(int s:{SIGTERM,SIGINT,SIGHUP,SIGCHLD})sigaddset(&watched,s);
        if(sigprocmask(SIG_BLOCK,&watched,&original)<0)throw std::runtime_error("Cannot block supervisor signals");
        events=Fd(signalfd(-1,&watched,SFD_NONBLOCK|SFD_CLOEXEC));
        if(events.value<0){sigprocmask(SIG_SETMASK,&original,nullptr);throw std::runtime_error("Cannot monitor supervisor signals");}
    }
    ~SupervisorSignals(){sigprocmask(SIG_SETMASK,&original,nullptr);}
    void child(){
        events=Fd{};
        for(int s:{SIGTERM,SIGINT,SIGHUP})signal(s,signal_stop);
        sigprocmask(SIG_SETMASK,&original,nullptr);
    }
    void wait(int timeout){
        pollfd event{events.value,POLLIN,0};
        if(poll(&event,1,timeout)<0 && errno!=EINTR)throw std::runtime_error("Supervisor signal wait failed");
        signalfd_siginfo info{};
        while(read(events.value,&info,sizeof(info))==sizeof(info))
            if(info.ssi_signo!=SIGCHLD)stopping=true;
    }
};

int restore_crashed_owner(DeviceBackend& device){
    if(!device.needs_crash_restore()&&!supervision->possible_diversion)return 0;
    if(!device.connect(stopped))throw std::runtime_error("Cannot confirm restoration after worker loss: device unavailable");
    device.restore();device.disconnect();
    if(device.needs_crash_restore())throw std::runtime_error("Input restoration still pending for the original device");
    return 0;
}

class UInput final:public InputSink{
    Fd fd{::open("/dev/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC)};bool created{};std::set<unsigned> held;std::string kernel_name;
    void capability(unsigned request,int value){if(ioctl(fd.value,request,value)<0)throw std::runtime_error("uinput capability setup failed");}
    void emit(unsigned type,unsigned code,int value){
        input_event event{};event.type=type;event.code=code;event.value=value;
        for(;;){
            const auto size=write(fd.value,&event,sizeof(event));if(size==sizeof(event))return;
            if(size<0&&errno==EINTR)continue;
            if(size<0&&errno==EAGAIN){pollfd wait{fd.value,POLLOUT,0};if(poll(&wait,1,100)>0)continue;}
            throw std::runtime_error("uinput write failed");
        }
    }
public:
    UInput(){
        if(fd.value<0)throw std::runtime_error("Cannot open /dev/uinput; install opt-in permissions and log in again");
        capability(UI_SET_EVBIT,EV_KEY);capability(UI_SET_EVBIT,EV_REL);
        for(int key=KEY_ESC;key<=KEY_F24;++key)capability(UI_SET_KEYBIT,key);
        for(int button:{BTN_LEFT,BTN_RIGHT,BTN_MIDDLE,BTN_SIDE,BTN_EXTRA})capability(UI_SET_KEYBIT,button);
        for(int axis:{REL_WHEEL,REL_HWHEEL,REL_X,REL_Y})capability(UI_SET_RELBIT,axis);
        uinput_setup setup{};setup.id.bustype=BUS_VIRTUAL;setup.id.vendor=0x1209;setup.id.product=1;
        std::strncpy(setup.name,"Logi Options Open virtual input",sizeof(setup.name)-1);
        if(ioctl(fd.value,UI_DEV_SETUP,&setup)<0||ioctl(fd.value,UI_DEV_CREATE)<0)throw std::runtime_error("Cannot create uinput device");
        created=true;char name[128]{};if(ioctl(fd.value,UI_GET_SYSNAME(sizeof(name)),name)>=0)kernel_name=name;
    }
    ~UInput()override{if(created){try{for(auto code:held)emit(EV_KEY,code,0);sync();}catch(...){}ioctl(fd.value,UI_DEV_DESTROY);}}
    void key(unsigned code,bool down)override{emit(EV_KEY,code,down?1:0);if(down)held.insert(code);else held.erase(code);}
    void relative(unsigned code,int value)override{emit(EV_REL,code,value);}
    void sync()override{emit(EV_SYN,SYN_REPORT,0);}
    std::string sysname()const override{return kernel_name;}
};
void check_competing_agents(){
    for(const auto& entry:std::filesystem::directory_iterator("/proc")){
        auto name=entry.path().filename().string();if(name.empty()||name.find_first_not_of("0123456789")!=name.npos)continue;
        std::ifstream comm(entry.path()/"comm");std::getline(comm,name);
        if(name=="solaar"||name=="logid")throw std::runtime_error("Another mouse manager ("+name+") is running; stop it explicitly before starting this daemon");
    }
}
class HidDevice final:public DeviceBackend{
    std::optional<Mouse> mouse;HidDiversions diversions;WheelInfo wheel_info;
    unsigned current_dpi{1000};std::string device_name,cached_status;
    std::optional<std::uint32_t> device_unit() {
        const auto index=feature(*mouse->channel,mouse->device.slot,0x0003);
        if(!index)return {};
        const auto info=mouse->request(index,0);
        if(!info || info.packet.data().size()<5)return {};
        const auto bytes=info.packet.data();
        const auto id=(std::uint32_t{bytes[1]}<<24)|(std::uint32_t{bytes[2]}<<16)|
                      (std::uint32_t{bytes[3]}<<8)|bytes[4];
        if(id==0 || id==UINT32_MAX)return {};
        return id;
    }
    std::optional<unsigned> diversion(unsigned id) {
        if(id==thumb) {
            const auto reply=mouse->request(mouse->thumb,1);
            if(reply && reply.packet.data().size()>=2)return word(reply.packet.data());
        }else{
            const auto reply=mouse->request(mouse->buttons,2,{high(id),low(id)});
            if(reply && reply.packet.data().size()>=3 && word(reply.packet.data())==id)
                return reply.packet.data()[2]&0x11;
        }
        return {};
    }
    void snapshot(){
        std::ostringstream out;out<<"{\"device\":"<<json(mouse->device.name)<<",\"platform\":\"linux\",\"battery\":";
        auto battery=mouse->request(mouse->battery,mouse->unified_battery?1:0);
        if(battery&&battery.packet.data()[0]<=100)out<<unsigned(battery.packet.data()[0]);else out<<"null";
        out<<",\"dpi_choices\":[";bool first=true;
        for(auto dpi:mouse->dpi_choices){if(!first)out<<',';first=false;out<<dpi;}
        out<<"],\"settings\":{";first=true;
        for(auto name:{"dpi","smartshift","ratchet","hires","invert","thumb-invert","host"}){
            if(!first)out<<',';first=false;out<<json(name)<<':';auto value=logi::get(*mouse,wide(name));
            if(value){out<<*value;if(std::string_view(name)=="dpi")current_dpi=*value;}else out<<"null";
        }out<<"},\"cached\":true}";cached_status=out.str();
    }
public:
    bool connect(std::atomic_bool& stop)override{
        check_competing_agents();mouse=logi::connect(&stop);if(!mouse)return false;
        if(!device_name.empty()&&device_name!=mouse->device.name&&!diversions.empty()){
            mouse.reset();throw std::runtime_error("Waiting for the original device to restore its diverted controls");
        }
        device_name=mouse->device.name;
        // Do not make the destructor act on a new device if binding its identity fails.
        try{diversions.bind(runtime_dir(),device_unit());}
        catch(...){mouse.reset();throw;}
        // A disconnect can leave diversion latched: restore our saved flags on
        // the same device before enabling the current mappings again.
        restore();snapshot();return true;
    }
    void disconnect()noexcept override{mouse.reset();}
    void restore()override{
        if(!mouse){if(!diversions.empty())throw std::runtime_error("Device disconnected; saved input state awaits reconnect");return;}
        diversions.restore([this](unsigned id){return diversion(id);},[this](unsigned id,unsigned value){
            if(id==thumb)return bool(mouse->request(mouse->thumb,2,{high(value),low(value)}));
            return bool(mouse->request(mouse->buttons,3,{high(id),low(id),Byte(0x22|value)}));
        });
    }
    void configure(const Config& config)override{
        if(!mouse)throw std::runtime_error("Mouse is disconnected");
        for(const auto& [id,xy]:layout(config)){
            if(id==thumb){
                auto info=mouse->request(mouse->thumb,0),state=mouse->request(mouse->thumb,1);
                if(!info||!state)throw std::runtime_error("Thumb wheel feature unavailable");
                auto data=info.packet.data(),mode=state.packet.data();
                wheel_info.native=word(data);wheel_info.diverted=word(data.subspan(2));
                if(!wheel_info.native||!wheel_info.diverted)throw std::runtime_error("Invalid thumb wheel resolution");
                if(mode[0])throw std::runtime_error("Thumb wheel already diverted by another manager; recover input first");
                diversions.remember(id,word(mode),0x0100);wheel_info.direction=((data[4]&1)!=(mode[1]&1))?1:-1;
                if(!mouse->request(mouse->thumb,2,{1,0}))throw std::runtime_error("Cannot divert thumb wheel");
                if(diversion(id)!=0x0100)throw std::runtime_error("Thumb diversion was not acknowledged");
            }else{
                auto before=mouse->request(mouse->buttons,2,{high(id),low(id)});
                if(!before||word(before.packet.data())!=id)throw std::runtime_error("Unsupported control: "+std::to_string(id));
                const auto flags=before.packet.data()[2];if(flags&0x11)throw std::runtime_error("Control already diverted by another manager; recover input first");
                const Byte desired=xy?0x33:0x03;
                diversions.remember(id,flags&0x11,desired&0x11);
                if(!mouse->request(mouse->buttons,3,{high(id),low(id),desired}))throw std::runtime_error("Cannot divert control");
                auto checked=mouse->request(mouse->buttons,2,{high(id),low(id)});
                if(!checked||(checked.packet.data()[2]&0x11)!=(desired&0x11))throw std::runtime_error("Control diversion was not acknowledged");
            }
        }mouse->channel->keep_notifications();
    }
    DeviceEvent read(unsigned timeout)override{
        if(!mouse)return {DeviceEvent::Kind::disconnected};auto reply=mouse->channel->read(timeout);
        if(reply.result==Result::timeout||reply.result==Result::stopped)return {};
        if(!reply)return {DeviceEvent::Kind::disconnected};
        const auto& packet=reply.packet;if(packet.size!=20||packet.bytes[1]!=mouse->device.slot)return {};auto data=packet.data();
        if(packet.bytes[2]==mouse->thumb&&packet.bytes[3]==0)return {DeviceEvent::Kind::wheel,{},0,0,static_cast<std::int16_t>(word(data)),data[4]==1};
        if(packet.bytes[2]==mouse->buttons&&packet.bytes[3]==0){
            DeviceEvent event{DeviceEvent::Kind::buttons};for(unsigned i=0;i<8;i+=2)if(auto id=word(data.subspan(i));id)event.buttons.push_back(id);return event;
        }
        if(packet.bytes[2]==mouse->buttons&&packet.bytes[3]==0x10)return {DeviceEvent::Kind::motion,{},static_cast<std::int16_t>(word(data)),static_cast<std::int16_t>(word(data.subspan(2)))};
        return {};
    }
    std::string command(const std::string& command)override{
        if(!mouse)throw std::runtime_error("Mouse is disconnected");
        if(command=="status"||command=="status-json"||command=="list")return cached_status;
        if(command=="hosts"||command=="hosts-json"){
            auto slots=logi::hosts(*mouse);if(slots.empty())throw std::runtime_error("Host switching is unsupported");
            std::ostringstream out;out<<'[';bool first=true;
            for(auto slot:slots){if(!first)out<<',';first=false;out<<"{\"slot\":"<<slot.slot<<",\"current\":"<<(slot.current?"true":"false")<<",\"paired\":"<<(slot.paired?(*slot.paired?"true":"false"):"null")<<'}';}return out.str()+']';
        }
        if(command=="recover-input"){
            if(!logi::restore_standard_input(*mouse))throw std::runtime_error("Standard input recovery was not acknowledged");
            return "{\"restored\":true}";
        }
        if(command.starts_with("switch-host ")){
            if(!logi::switch_host(*mouse,number(std::string_view(command).substr(12))))throw std::runtime_error("Host switch failed or pairing unavailable");return "{\"sent\":true}";
        }
        if(command=="toggle-wheel"){
            auto ratchet=logi::get(*mouse,L"ratchet");if(!ratchet)throw std::runtime_error("Cannot read wheel mode");
            return this->command("set ratchet "+std::to_string(!*ratchet));
        }
        if(command.starts_with("set ")){
            const auto split=command.find(' ',4);if(split==command.npos)throw std::runtime_error("Expected set NAME VALUE");
            const auto name=command.substr(4,split-4);const auto value=number(std::string_view(command).substr(split+1));
            if(!logi::set(*mouse,wide(name),value))throw std::runtime_error("Unsupported setting/value or HID write failed");
            auto checked=logi::get(*mouse,wide(name));if(!checked||*checked!=value)throw std::runtime_error("Setting readback differs from requested value");
            // Cache readbacks rather than doing a slow full query for each slider event.
            auto token=json(name)+':';auto begin=cached_status.find(token);
            if(begin!=std::string::npos){begin+=token.size();auto end=cached_status.find_first_of(",}",begin);cached_status.replace(begin,end-begin,std::to_string(value));}
            if(name=="dpi")current_dpi=value;return "{\"setting\":"+json(name)+",\"value\":"+std::to_string(value)+'}';
        }throw std::runtime_error("Unsupported device command");
    }
    bool needs_crash_restore()const override{return !diversions.empty()||HidDiversions::any_pending(runtime_dir());}
    WheelInfo wheel()const override{return wheel_info;}
    unsigned threshold()const override{return std::max(20u,current_dpi/12);}
    ~HidDevice()override{if(mouse){mouse->channel->finish_shutdown();try{restore();}catch(const std::exception& e){std::fprintf(stderr,"Input restoration pending: %s\n",e.what());}}}
};
void launch(const std::vector<std::string>& arguments){
    int pipes[2];if(pipe2(pipes,O_CLOEXEC)<0)throw std::runtime_error("Cannot create launch status pipe");
    Fd reader(pipes[0]),writer(pipes[1]);const auto pid=fork();if(pid<0)throw std::runtime_error("Cannot launch application");
    if(pid==0){
        ::close(reader.value);std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(const_cast<char*>(argument.c_str()));argv.push_back(nullptr);
        execvp(argv[0],argv.data());const int error=errno;static_cast<void>(write(writer.value,&error,sizeof(error)));_exit(127);
    }
    writer=Fd();int error{};ssize_t size;do{size=read(reader.value,&error,sizeof(error));}while(size<0&&errno==EINTR);
    if(size>0){waitpid(pid,nullptr,0);throw std::runtime_error("Cannot execute "+arguments.front()+": "+std::strerror(error));}
    if(size<0)throw std::runtime_error("Cannot read application launch result");
}
class DesktopHooks final:public RuntimeHooks{
    Desktop desktop;
public:
    explicit DesktopHooks(InputSink& input):desktop([&input](const std::vector<unsigned>& keys){
        std::vector<unsigned> pressed;
        try{
            for(auto key:keys){input.key(key,true);pressed.push_back(key);}input.sync();
            for(auto it=pressed.rbegin();it!=pressed.rend();++it)input.key(*it,false);input.sync();
        }catch(...){for(auto it=pressed.rbegin();it!=pressed.rend();++it){try{input.key(*it,false);}catch(...){}}try{input.sync();}catch(...){}throw;}
    }){}
    void validate(const Config& config)override{desktop.validate(config);}
    void configure(const Config& config)override{desktop.configure_hotkeys(config);}
    std::vector<std::string> foreground()override{return desktop.foreground();}
    std::vector<std::vector<Action>> hotkeys()override{return desktop.poll_hotkeys();}
    bool busy()const override{return desktop.busy();}
    void cancel_actions()override{desktop.cancel_actions();}
    void suspend_hotkeys(bool value)override{desktop.suspend_hotkeys(value);}
    void action(const Action& action,const std::filesystem::path& config)override{
        if(action.operation==Op::text){desktop.type_text(action.text);return;}
        if(action.operation==Op::open){launch({"xdg-open",action.text});return;}
        if(action.operation==Op::ring||action.operation==Op::folder){preview_ring(config,action.operation==Op::folder?action.text:std::string{});return;}
        throw std::runtime_error("Unsupported desktop action");
    }
};
struct Pending{std::vector<Action> actions;std::size_t next{};Clock::time_point due{};unsigned id{};};
struct State{bool down{},skip_first{},gesture_active{};int x{},y{};unsigned held_button{};std::vector<std::string> context;};
std::vector<Action> fallback(unsigned id){
    if(id==0x52)return actions("MouseMiddle");if(id==0x53)return actions("MouseBack");if(id==0x56)return actions("MouseForward");if(id==0xc4)return actions("WheelMode");return {};
}
unsigned held_mouse(const std::vector<Action>& sequence){
    if(sequence.size()!=1||sequence[0].operation!=Op::keys||sequence[0].keys.size()!=1)return 0;
    const auto key=sequence[0].keys[0];return key>=BTN_LEFT&&key<=BTN_EXTRA?key:0;
}
}
void RuntimeHooks::action(const Action&,const std::filesystem::path&){throw std::runtime_error("Desktop action is unavailable");}
std::unique_ptr<InputSink> make_uinput_sink(){return std::make_unique<UInput>();}
std::filesystem::path config_path(){
    if(auto value=std::getenv("LOGI_OPTIONS_OPEN_CONFIG");value&&*value)return value;
    if(auto value=std::getenv("XDG_CONFIG_HOME");value&&*value)return std::filesystem::path(value)/"logi-options-open/mappings.ini";
    if(auto value=std::getenv("HOME");value&&*value)return std::filesystem::path(value)/".config/logi-options-open/mappings.ini";
    throw std::runtime_error("HOME or XDG_CONFIG_HOME is required");
}
bool running(){
    Fd lock(::open((runtime_dir()/"agent.lock").c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW));if(lock.value<0)return false;
    if(flock(lock.value,LOCK_EX|LOCK_NB)==0)return false;return errno==EWOULDBLOCK;
}
std::string transact(const std::string& command,unsigned timeout_ms){
    if(command.empty()||command.size()>=65536)throw std::runtime_error("Invalid IPC command length");
    const auto deadline=Clock::now()+std::chrono::milliseconds(timeout_ms);
    auto remaining=[&]{return std::max(0,static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now()).count()));};
    Fd socket;auto addr=address();
    for(;;){
        socket=Fd(::socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));
        if(socket.value<0)throw std::runtime_error("Cannot create daemon client socket");
        if(::connect(socket.value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0)break;
        const int error=errno;
        // The daemon owns the lock before it binds/listens. A previous socket
        // can also remain after a crash. Wait only for this startup window,
        // within the caller's original budget; no command has been sent yet.
        if((error!=ENOENT&&error!=ECONNREFUSED)||!running())
            throw std::runtime_error("Linux daemon IPC is unavailable");
        const auto left=remaining();
        if(!left)throw std::runtime_error("Daemon IPC startup timed out; command was not sent");
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(left,10)));
    }
    if(!remaining())throw std::runtime_error("Daemon IPC startup timed out; command was not sent");
    if(send(socket.value,command.data(),command.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(command.size()))throw std::runtime_error("Cannot send daemon command");
    // Once sent, never reconnect/replay a possibly applied setting or action.
    pollfd wait{socket.value,POLLIN,0};int result;do{result=poll(&wait,1,remaining());}while(result<0&&errno==EINTR&&remaining());
    if(result<=0)throw std::runtime_error("Daemon command timed out; completion is unknown");
    char buffer[65536];const auto size=recv(socket.value,buffer,sizeof(buffer),MSG_TRUNC);
    if(size<=0||size>=static_cast<ssize_t>(sizeof(buffer)))throw std::runtime_error("Daemon closed without a valid acknowledgement");
    std::string reply(buffer,size);if(reply.starts_with("OK\n"))return reply.substr(3);if(reply.starts_with("ERR\n"))throw std::runtime_error(reply.substr(4));
    throw std::runtime_error("Unrecognized daemon reply");
}
std::vector<Action> load_actions(const std::filesystem::path& file){
    const auto config=load(file);
    if(config.hotkeys.size()!=1||!config.bindings.empty()||!config.profiles.empty()||!config.ring.empty()||!config.folders.empty())throw std::runtime_error("Action file must contain exactly one [hotkeys] sequence and no mappings");
    return config.hotkeys.front().actions;
}
int preview_ring(const std::filesystem::path& file,const std::string& folder){
    const auto config=load(file);if(folder.empty()?config.ring.empty():!config.folders.contains(folder))throw std::runtime_error("Ring or folder is empty/missing");
    std::error_code error;const auto executable=std::filesystem::read_symlink("/proc/self/exe",error).parent_path()/"logi-options-open";
    if(error||!std::filesystem::is_regular_file(executable))throw std::runtime_error("Native Ring GUI executable is unavailable");
    std::vector<std::string> args{executable.string(),"--ring",std::filesystem::absolute(file).string()};
    if(!folder.empty()){args.push_back("--folder");args.push_back(folder);}launch(args);return 0;
}
int run_with(const std::filesystem::path& config_file,DeviceBackend& device,InputSink& input,RuntimeHooks& hooks){
    if(supervision && supervision->cleanup_only)return restore_crashed_owner(device);
    auto config=load(config_file);hooks.validate(config);
    std::optional<Server> standalone;
    if(!supervision){standalone.emplace();stopped=false;signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);}
    Server& server=supervision?supervision->server:*standalone;
    bool connected=false,ready=false,quit=false,reported_device=false;unsigned revision=1,next_action=1,last_completed=0;
    std::string last_error,last_action_error;auto retry=Clock::now();
    auto controls=layout(config);std::map<unsigned,State> state;WheelAccumulator wheel;
    std::deque<Pending> pending;std::map<unsigned,unsigned> held_counts;
    auto key_down=[&](unsigned key){if(held_counts[key]++==0)input.key(key,true);input.sync();};
    auto key_up=[&](unsigned key){auto it=held_counts.find(key);if(it!=held_counts.end()&&--it->second==0){input.key(key,false);held_counts.erase(it);}input.sync();};
    auto release=[&]{for(const auto& [key,count]:held_counts)input.key(key,false);held_counts.clear();input.sync();state.clear();wheel={};};
    auto cancel=[&](const std::string& why){if(!pending.empty()||hooks.busy())last_action_error=why;pending.clear();hooks.cancel_actions();release();};
    auto queue=[&](std::vector<Action> sequence){if(pending.size()>=128)throw std::runtime_error("Action queue is full");const auto id=next_action++;pending.push_back({std::move(sequence),0,Clock::now(),id});return id;};
    auto choose=[&](Binding binding,const std::vector<std::string>& context){if(auto found=lookup(config,binding,context))return *found;return binding.second==Direction::tap?fallback(binding.first):std::vector<Action>{};};
    auto execute=[&](const Action& action){
        switch(action.operation){
        case Op::keys:
            for(auto key:action.keys)key_down(key);
            for(auto it=action.keys.rbegin();it!=action.keys.rend();++it)key_up(*it);
            break;
        case Op::wheel_mode:device.command("toggle-wheel");break;
        case Op::scroll_x:input.relative(REL_HWHEEL,action.value);input.sync();break;
        case Op::scroll_y:input.relative(REL_WHEEL,action.value);input.sync();break;
        case Op::wait:break;
        default:hooks.action(action,config_file);break;
        }
    };
    auto lose=[&](const std::string& why){cancel(why);ready=false;connected=false;last_error=why;device.disconnect();retry=Clock::now()+std::chrono::seconds(1);};
    std::optional<Request> ending;
    try{
        hooks.configure(config);
        if(supervision)supervision->notify('R');
        while(!quit&&!stopped.load(std::memory_order_relaxed)){
            while(auto request=server.receive()){
                const auto& command=request->body;
                try{
                    std::string response;
                    if(command=="stop"||command=="recover-input"){
                        if(supervision)supervision->notify('S');
                        ending=std::move(request);quit=true;break;
                    }
                    if(command=="agent-status"||command=="agent-json"){
                        response="{\"native\":true,\"options_plus\":false,\"ready\":"+std::string(ready?"true":"false")+",\"connected\":"+(connected?"true":"false")+
                            ",\"pid\":"+std::to_string(getpid())+",\"supervisor_pid\":"+std::to_string(supervision?supervision->parent:0)+
                            ",\"state\":"+json(ready?"ready":"reconnecting")+",\"config_revision\":"+std::to_string(revision)+",\"pending_actions\":"+std::to_string(pending.size())+
                            ",\"last_completed_action\":"+std::to_string(last_completed)+",\"last_action_error\":"+json(last_action_error)+",\"last_error\":"+json(last_error)+'}';
                    }else if(command.starts_with("reload ")){
                        const std::filesystem::path path(command.substr(7));
                        if(std::filesystem::weakly_canonical(path)!=std::filesystem::weakly_canonical(config_file))throw std::runtime_error("Reload must use the running configuration path");
                        auto fresh=load(path);hooks.validate(fresh);if(!ready)throw std::runtime_error("Device disconnected; configuration has not been applied");
                        cancel("Pending actions cancelled by configuration reload");
                        try{device.restore();device.configure(fresh);hooks.configure(fresh);}
                        catch(const std::exception& error){
                            const auto reason=std::string(error.what());
                            try{device.restore();device.configure(config);hooks.configure(config);}
                            catch(const std::exception& rollback){lose("Reload rollback failed: "+std::string(rollback.what()));}
                            throw std::runtime_error("Reload rejected; previous configuration retained: "+reason);
                        }
                        config=std::move(fresh);controls=layout(config);++revision;
                        response="{\"reload_requested\":true,\"reloaded\":true,\"config_revision\":"+std::to_string(revision)+'}';
                    }else if(command.starts_with("play-actions ")){
                        auto sequence=load_actions(std::filesystem::path(command.substr(13)));
                        Config action_config;action_config.bindings.emplace(Binding{0x52,Direction::tap},sequence);hooks.validate(action_config);
                        if(!ready)throw std::runtime_error("Device is not ready; action was not queued");
                        const auto id=queue(std::move(sequence));last_action_error.clear();response="{\"accepted\":true,\"action_id\":"+std::to_string(id)+'}';
                    }else{
                        if(!ready)throw std::runtime_error("Device is disconnected or not yet configured");
                        const bool reconfigure=command.starts_with("set thumb-invert ")||command.starts_with("switch-host ");
                        if(reconfigure){cancel("Pending actions cancelled by device setting change");device.restore();}
                        try{response=device.command(command);}
                        catch(...){if(reconfigure){try{device.configure(config);}catch(const std::exception& e){lose(e.what());}}throw;}
                        if(command.starts_with("switch-host "))lose("Waiting for device after host switch");
                        else if(reconfigure){try{device.configure(config);}catch(const std::exception& e){lose(e.what());throw;}}
                    }
                    respond(request->peer.value,true,response);
                }catch(const std::exception& error){respond(request->peer.value,false,error.what());}
            }
            if(quit||stopped.load(std::memory_order_relaxed))break;
            if(!connected&&Clock::now()>=retry){
                try{
                    if(device.connect(stopped)){
                        connected=true;
                        if(supervision && !reported_device){supervision->notify('D');reported_device=true;}
                        try{device.configure(config);ready=true;last_error.clear();}
                        catch(...){try{device.restore();}catch(...){}throw;}
                    }else last_error="Waiting for an accessible HID++ mouse";
                }catch(const std::exception& error){last_error=error.what();ready=false;connected=false;device.disconnect();}
                retry=Clock::now()+std::chrono::seconds(1);
            }
            try{for(auto& sequence:hooks.hotkeys())if(ready)queue(std::move(sequence));}
            catch(const std::exception& error){last_action_error=error.what();pending.clear();hooks.cancel_actions();release();}
            hooks.suspend_hotkeys(!pending.empty()||hooks.busy());
            auto now=Clock::now();unsigned timeout=20;
            if(ready&&!hooks.busy()){
                for(auto it=pending.begin();it!=pending.end();){
                    try{
                        while(it->next<it->actions.size()&&it->due<=now&&!hooks.busy()){
                            const auto& action=it->actions[it->next++];
                            if(action.operation==Op::wait){it->due=now+std::chrono::milliseconds(action.value);break;}
                            execute(action);
                        }
                        if(it->next==it->actions.size()&&it->due<=now&&!hooks.busy()){last_completed=it->id;it=pending.erase(it);}else ++it;
                    }catch(const std::exception& error){last_action_error=error.what();release();it=pending.erase(it);}
                }
            }
            while(waitpid(-1,nullptr,WNOHANG)>0){}
            if(!ready){std::this_thread::sleep_for(std::chrono::milliseconds(timeout));continue;}
            const auto event=device.read(timeout);
            if(event.kind==DeviceEvent::Kind::disconnected){lose("Device disconnected; mappings will resume after reconnect");continue;}
            if(event.kind==DeviceEvent::Kind::wheel&&controls.contains(thumb)){
                const auto context=hooks.foreground();const auto info=device.wheel();
                const auto steps=wheel.steps(event.rotation,info.native,info.diverted,info.direction,speed_for(config,context),event.begin);
                for(int n=0;n<std::abs(steps);++n){
                    const Binding binding{thumb,steps<0?Direction::left:Direction::right};
                    if(auto found=lookup(config,binding,context))queue(*found);else{input.relative(REL_HWHEEL,steps<0?-1:1);input.sync();}
                }
            }else if(event.kind==DeviceEvent::Kind::buttons){
                for(const auto& [id,xy]:controls){
                    if(id==thumb)continue;auto& s=state[id];const bool down=std::ranges::find(event.buttons,id)!=event.buttons.end();
                    if(down&&!s.down){
                        s={true,true,false,0,0,0,hooks.foreground()};
                        for(auto direction:{Direction::left,Direction::right,Direction::up,Direction::down})
                            s.gesture_active|=lookup(config,{id,direction},s.context)!=nullptr;
                        auto sequence=choose({id,Direction::tap},s.context);
                        if(auto button=held_mouse(sequence);button&&!s.gesture_active){s.held_button=button;key_down(button);}
                        else if(!s.gesture_active&&!sequence.empty())queue(std::move(sequence));
                    }
                    if(!down&&s.down){
                        if(s.held_button)key_up(s.held_button);
                        if(s.gesture_active){auto sequence=choose({id,gesture(s.x,s.y,device.threshold())},s.context);if(!sequence.empty())queue(std::move(sequence));}s={};
                    }
                }
            }else if(event.kind==DeviceEvent::Kind::motion){
                for(auto& [id,s]:state)if(s.down&&controls[id]){
                    if(s.skip_first){s.skip_first=false;continue;}
                    if(s.gesture_active){s.x=std::clamp(s.x+event.x,-100000,100000);s.y=std::clamp(s.y+event.y,-100000,100000);}
                    else{input.relative(REL_X,event.x);input.relative(REL_Y,event.y);input.sync();}
                }
            }
        }
        cancel("Pending actions cancelled by shutdown");hooks.configure(Config{});
        const bool signalled=!ending&&stopped.load();
        // A stop queued on the persistent socket can arrive before this
        // replacement worker's first device probe. Reclaim and restore first.
        const bool must_restore=device.needs_crash_restore()||(supervision&&supervision->possible_diversion)
            ||(ending&&ending->body=="recover-input");
        if(!connected && (ending||signalled) && must_restore){
            stopped=false;
            connected=device.connect(stopped);
            if(!connected)throw std::runtime_error("Cannot confirm restoration after worker loss: device unavailable");
        }
        if(connected){
            // Stop cancels reads; restoration still needs an uncancelled device.
            stopped=false;device.restore();if(ending&&ending->body=="recover-input")device.command("recover-input");
        }else if(ending&&ending->body=="recover-input")throw std::runtime_error("Cannot restore input while device is disconnected");
        device.disconnect();
        if(device.needs_crash_restore())throw std::runtime_error("Input restoration still pending for the original device");
        if(supervision)supervision->notify('C');
        if(ending)respond(ending->peer.value,true,ending->body=="recover-input"?"{\"restored\":true,\"stopped\":true}":"{\"stopped\":true}");
        return supervision&&signalled?75:0;
    }catch(const std::exception& error){
        try{release();}catch(...){}stopped=false;try{device.restore();}catch(...){}device.disconnect();
        if(ending)respond(ending->peer.value,false,error.what());throw;
    }
}
int run_device(const std::filesystem::path& config,DeviceBackend& device){
    if(supervision && supervision->cleanup_only)return restore_crashed_owner(device);
    auto input=make_uinput_sink();DesktopHooks hooks(*input);return run_with(config,device,*input,hooks);
}
int supervise(const std::filesystem::path& config,int (*worker)(const std::filesystem::path&)){
    // One open file description for the exclusive HID lock and one listening
    // socket survive worker death. Pending clients stay in the kernel backlog.
    SupervisorSignals signals;Server server;
    struct ProcessName {
        char original[16]{};
        ProcessName(){
            if(prctl(PR_GET_NAME,original)==0)prctl(PR_SET_NAME,"logi-supervisor");
        }
        void restore() const{if(*original)prctl(PR_SET_NAME,original);}
        ~ProcessName(){restore();}
    } name;
    const auto parent=getpid();
    bool recovering=false,cleanup_only=false,possible_diversion=false;
    int retry_ms=150,final_error=0;
    for(;;){
        int channel[2];
        if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,channel)<0)
            throw std::runtime_error("Cannot create worker lifecycle channel");
        Fd reader(channel[0]),writer(channel[1]);
        const auto started=Clock::now();
        const auto child=fork();
        if(child<0)throw std::runtime_error("Cannot start mouse worker");
        if(!child){
            reader=Fd{};
            // A dead parent must not leave a live HID owner. Check getppid()
            // after prctl to cover parent death between fork and registration.
            if(prctl(PR_SET_PDEATHSIG,SIGTERM)<0||getppid()!=parent)_exit(1);
            name.restore();
            SupervisedWorker context{server,writer.value,parent,possible_diversion,cleanup_only};
            supervision=&context;stopped=false;signals.child();
            try{_exit(worker(config));}
            catch(const std::exception& e){std::fprintf(stderr,"Mouse worker: %s\n",e.what());_exit(1);}
        }
        writer=Fd{};
        int status{};bool forwarded=false;
        for(;;){
            const auto result=waitpid(child,&status,WNOHANG);
            if(result==child)break;
            if(result<0 && errno!=EINTR)throw std::runtime_error("Cannot wait for mouse worker");
            signals.wait(-1);
            if(signals.stopping&&!forwarded&&!cleanup_only){kill(child,SIGTERM);forwarded=true;}
        }
        signals.wait(0);
        bool initialized=false,requested=false;
        char note{};
        while(recv(reader.value,&note,1,0)==1){
            if(note=='R')initialized=true;
            if(note=='S')requested=true;
            if(note=='D')possible_diversion=true;
            if(note=='C')possible_diversion=false;
        }
        const bool exited=WIFEXITED(status);
        const int code=exited?WEXITSTATUS(status):128+WTERMSIG(status);
        if(cleanup_only)return final_error?final_error:code;
        if(requested){
            if(exited)return code;
            // A stop was accepted but the worker died while releasing input.
            // Honour the stop: make one restoration-only attempt, never remap.
            std::fprintf(stderr,"Mouse worker pid=%ld died during explicit stop; checking input restoration.\n",
                         static_cast<long>(child));
            cleanup_only=true;continue;
        }
        if(exited&&code==0)return 0;
        if(signals.stopping&&exited&&code==75)return 0;
        if(exited && code!=75 && !initialized){
            // Bad configuration, unavailable uinput or unsupported desktop:
            // report once. If a prior worker crashed, restore its journal.
            if(!recovering)return code;
            cleanup_only=true;final_error=code;continue;
        }
        if(Clock::now()-started>std::chrono::seconds(10))retry_ms=150;
        if(!signals.stopping){
            std::fprintf(stderr,"Mouse worker pid=%ld exited (status=%d); retrying in %d ms.\n",
                         static_cast<long>(child),code,retry_ms);
            const auto due=Clock::now()+std::chrono::milliseconds(retry_ms);
            while(!signals.stopping&&Clock::now()<due)
                signals.wait(static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(due-Clock::now()).count()));
            retry_ms=std::min(retry_ms*2,2000);
        }
        recovering=true;cleanup_only=signals.stopping;
    }
}
int run(const std::filesystem::path& config){
    return supervise(config,[](const std::filesystem::path& path){HidDevice device;return run_device(path,device);});
}
std::string dispatch(const std::string& command){
    if(running())return transact(command);
    // A status probe does not access HID and need not take device ownership.
    // The daemon may acquire its lock immediately after running() returned false.
    if(command=="agent-status"||command=="agent-json")return "{\"native\":false,\"ready\":false,\"connected\":false,\"options_plus\":false}";
    // Same exclusive lock closes the check/start race with a newly started daemon.
    Fd lock;
    try{lock=acquire_lock();}catch(...){if(running())return transact(command);throw;}
    if(command=="stop")return "{\"stopped\":true}";
    if(command.starts_with("reload "))throw std::runtime_error("Linux daemon is not running");
    stopped=false;signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);HidDevice device;
    if(!device.connect(stopped))throw std::runtime_error("No accessible HID++ mouse; check hidraw permissions and connection");
    if(command.starts_with("play-actions ")){
        auto sequence=load_actions(std::filesystem::path(command.substr(13)));auto input=make_uinput_sink();DesktopHooks hooks(*input);
        Config config;config.bindings.emplace(Binding{0x52,Direction::tap},sequence);hooks.validate(config);hooks.suspend_hotkeys(true);
        for(const auto& action:sequence){
            if(stopped.load())throw std::runtime_error("Action playback cancelled");
            if(action.operation==Op::wait){
                const auto until=Clock::now()+std::chrono::milliseconds(action.value);
                while(Clock::now()<until&&!stopped.load()){hooks.hotkeys();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            }else if(action.operation==Op::keys){
                for(auto key:action.keys)input->key(key,true);input->sync();
                for(auto it=action.keys.rbegin();it!=action.keys.rend();++it)input->key(*it,false);input->sync();
            }else if(action.operation==Op::wheel_mode)device.command("toggle-wheel");
            else if(action.operation==Op::scroll_x||action.operation==Op::scroll_y){input->relative(action.operation==Op::scroll_x?REL_HWHEEL:REL_WHEEL,action.value);input->sync();}
            else hooks.action(action,config_path());
            while(hooks.busy()&&!stopped.load()){hooks.hotkeys();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        }return "{\"accepted\":true,\"completed\":true}";
    }return device.command(command);
}
}
