// CI transport only: observe the real production uinput device through evdev,
// then forward its events into an isolated virtual KWin compositor. This is not
// used or installed as the production mouse input backend.
#include "fake-input-client-protocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QTimer>
#include <wayland-client.h>
#include <linux/input.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>

namespace {volatile std::sig_atomic_t stopped{};void stop(int){stopped=1;}}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    std::signal(SIGTERM,stop);std::signal(SIGINT,stop);std::signal(SIGPIPE,SIG_IGN);
    auto* display=wl_display_connect(nullptr);
    if(!display){std::cerr<<"No native Wayland display\n";return 1;}
    auto* registry=wl_display_get_registry(display);org_kde_kwin_fake_input* input{};
    static const wl_registry_listener listener{
        [](void* data,wl_registry* registry,uint32_t name,const char* interface,uint32_t version){
            if(std::strcmp(interface,org_kde_kwin_fake_input_interface.name)==0 && version>=4)
                *static_cast<org_kde_kwin_fake_input**>(data)=static_cast<org_kde_kwin_fake_input*>(wl_registry_bind(registry,name,&org_kde_kwin_fake_input_interface,std::min(version,6u)));
        },[](void*,wl_registry*,uint32_t){}
    };
    wl_registry_add_listener(registry,&listener,&input);wl_display_roundtrip(display);
    if(!input){std::cerr<<"KWin fake-input keyboard protocol unavailable\n";wl_display_disconnect(display);return 1;}
    org_kde_kwin_fake_input_authenticate(input,"Logi Options Open isolated CI","Forward observed production uinput events to virtual KWin for native Wayland acceptance");
    wl_display_roundtrip(display);
    struct Device {int fd{-1};std::unique_ptr<QSocketNotifier> notifier;~Device(){notifier.reset();if(fd>=0)::close(fd);}};
    std::map<QString,std::unique_ptr<Device>> devices;unsigned long events{},frames{};
    QTimer scan;scan.setInterval(25);
    QObject::connect(&scan,&QTimer::timeout,&app,[&]{
        if(stopped){app.quit();return;}
        for(const auto& event:QDir("/dev/input").entryList({"event*"},QDir::System|QDir::Files)) {
            const auto path="/dev/input/"+event;if(devices.contains(path))continue;
            QFile name("/sys/class/input/"+event+"/device/name");if(!name.open(QIODevice::ReadOnly))continue;
            if(name.readAll().trimmed()!="Logi Options Open virtual input")continue;
            const int fd=::open(path.toLocal8Bit().constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)continue;
            auto device=std::make_unique<Device>();device->fd=fd;
            device->notifier=std::make_unique<QSocketNotifier>(fd,QSocketNotifier::Read);
            QObject::connect(device->notifier.get(),&QSocketNotifier::activated,&app,[&,fd,path]{
                input_event received[64];ssize_t length{};
                while((length=::read(fd,received,sizeof(received)))>0)for(ssize_t i=0;i<length/static_cast<ssize_t>(sizeof(input_event));++i) {
                    const auto& e=received[i];
                    if(e.type==EV_KEY && (e.value==0 || e.value==1)) {
                        if(e.code>=BTN_MOUSE && e.code<=BTN_TASK)org_kde_kwin_fake_input_button(input,e.code,e.value);
                        else org_kde_kwin_fake_input_keyboard_key(input,e.code,e.value);
                        ++events;
                    } else if(e.type==EV_REL) {
                        if(e.code==REL_X)org_kde_kwin_fake_input_pointer_motion(input,wl_fixed_from_int(e.value),0);
                        if(e.code==REL_Y)org_kde_kwin_fake_input_pointer_motion(input,0,wl_fixed_from_int(e.value));
                        if(e.code==REL_WHEEL)org_kde_kwin_fake_input_axis(input,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_int(-15*e.value));
                        if(e.code==REL_HWHEEL)org_kde_kwin_fake_input_axis(input,WL_POINTER_AXIS_HORIZONTAL_SCROLL,wl_fixed_from_int(15*e.value));
                        ++events;
                    } else if(e.type==EV_SYN && e.code==SYN_REPORT){++frames;wl_display_flush(display);}
                }
                if(length==0 || (length<0 && errno==ENODEV))devices.erase(path);
            });
            devices.emplace(path,std::move(device));
            std::cout<<"Reading actual uinput evdev device "<<path.toStdString()<<" -> KWin fake-input transport\n"<<std::flush;
            if(const auto ready=qEnvironmentVariable("LOGI_WAYLAND_INPUT_READY");!ready.isEmpty()){QFile file(ready);if(file.open(QIODevice::WriteOnly))file.write("evdev-open\n");}
        }
    });scan.start();
    QSocketNotifier wayland(wl_display_get_fd(display),QSocketNotifier::Read);
    QObject::connect(&wayland,&QSocketNotifier::activated,&app,[&]{if(wl_display_dispatch(display)<0)app.exit(1);else wl_display_flush(display);});
    const int result=app.exec();devices.clear();
    std::cout<<"Actual kernel events forwarded: "<<events<<", SYN frames: "<<frames<<'\n';
    if(org_kde_kwin_fake_input_get_version(input)>=5)org_kde_kwin_fake_input_destroy(input);
    else wl_proxy_destroy(reinterpret_cast<wl_proxy*>(input));
    wl_registry_destroy(registry);wl_display_disconnect(display);return result;
}
