#include "agent.hpp"
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <tuple>

// Calls the production uinput implementation and reads the resulting kernel
// evdev stream. This is real Linux input I/O, but not a physical Logitech test.
int main() {
    if(access("/dev/uinput",W_OK)!=0) {
        std::cerr<<"SKIP: /dev/uinput is unavailable or not writable; kernel input is NOT validated\n";
        return 77;
    }
    int reader=-1;
    try {
        auto sink=loo_linux::make_uinput_sink();
        const auto root=std::filesystem::path("/sys/devices/virtual/input")/sink->sysname();
        for(unsigned attempt=0;attempt<40 && reader<0;++attempt) {
            if(std::filesystem::exists(root)) for(const auto& entry:std::filesystem::directory_iterator(root)) {
                const auto name=entry.path().filename().string();
                if(name.starts_with("event"))reader=open(("/dev/input/"+name).c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            }
            if(reader<0)std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(reader<0)throw std::runtime_error("Created virtual input, but no readable evdev event node appeared");
        unsigned char bits[(KEY_MAX+8)/8]{};
        if(ioctl(reader,EVIOCGBIT(EV_KEY,sizeof(bits)),bits)<0)throw std::runtime_error("Cannot inspect evdev capabilities");
        if(bits[BTN_JOYSTICK/8]&(1<<(BTN_JOYSTICK%8)))throw std::runtime_error("Mouse advertises joystick capabilities");
        sink->key(KEY_LEFTCTRL,true);sink->key(KEY_C,true);sink->sync();
        sink->key(KEY_C,false);sink->key(KEY_LEFTCTRL,false);sink->sync();
        sink->key(BTN_MIDDLE,true);sink->sync();
        sink->relative(REL_X,4);sink->relative(REL_Y,-3);sink->sync();
        sink->key(BTN_MIDDLE,false);sink->sync();
        sink->relative(REL_WHEEL,1);sink->relative(REL_HWHEEL,-2);sink->sync();
        using Item=std::tuple<unsigned,unsigned,int>;
        const std::vector<Item> expected={{EV_KEY,KEY_LEFTCTRL,1},{EV_KEY,KEY_C,1},
            {EV_KEY,KEY_C,0},{EV_KEY,KEY_LEFTCTRL,0},{EV_KEY,BTN_MIDDLE,1},
            {EV_REL,REL_X,4},{EV_REL,REL_Y,-3},{EV_KEY,BTN_MIDDLE,0},
            {EV_REL,REL_WHEEL,1},{EV_REL,REL_HWHEEL,-2}};
        std::vector<Item> observed;unsigned frames=0;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(frames<6 && std::chrono::steady_clock::now()<deadline) {
            pollfd ready{reader,POLLIN,0};if(poll(&ready,1,100)<=0)continue;
            input_event events[32];const auto count=read(reader,events,sizeof(events));
            if(count<0)continue;
            for(std::size_t index=0;index<static_cast<std::size_t>(count)/sizeof(input_event);++index) {
                const auto& event=events[index];
                if(event.type==EV_SYN && event.code==SYN_REPORT)++frames;
                else if(event.type==EV_KEY || event.type==EV_REL)observed.emplace_back(event.type,event.code,event.value);
            }
        }
        if(observed!=expected || frames!=6)throw std::runtime_error("Actual evdev stream does not match keyboard, held drag and wheel actions");
        close(reader);reader=-1;
        std::cout<<"PASS: production uinput -> kernel evdev: 10 ordered key/button/motion/wheel events in 6 frames\n";
        return 0;
    }catch(const std::exception& error) {
        if(reader>=0)close(reader);
        std::cerr<<error.what()<<'\n';return 1;
    }
}
