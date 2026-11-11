#include "clipboard.hpp"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include <QSocketNotifier>
#include <QTimer>
#include <wayland-client.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

struct WaylandClipboard::Impl {
    WaylandClipboard* owner;
    wl_display* display{};
    wl_registry* registry{};
    wl_seat* seat{};
    zwlr_data_control_manager_v1* manager{};
    zwlr_data_control_device_v1* device{};
    std::unique_ptr<QSocketNotifier> notifier;
    QString error;
    quint32 serial{};
    struct Source {
        Impl* parent{};zwlr_data_control_source_v1* source{};QByteArray text;quint32 serial{};bool reported{};
        ~Source(){if(source)zwlr_data_control_source_v1_destroy(source);}
    };
    std::vector<std::shared_ptr<Source>> sources;
    Source* current{};
    struct Transfer final : QObject {
        int fd{-1};QByteArray text;qsizetype offset{};std::shared_ptr<Source> source;QSocketNotifier* notifier{};QTimer timeout;
        Transfer(int descriptor,std::shared_ptr<Source> from,WaylandClipboard* owner):QObject(owner),fd(descriptor),text(from->text),source(std::move(from)) {
            fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
            notifier=new QSocketNotifier(fd,QSocketNotifier::Write,this);
            connect(notifier,&QSocketNotifier::activated,this,[this]{writeReady();});
            timeout.setSingleShot(true);timeout.setInterval(5000);
            connect(&timeout,&QTimer::timeout,this,[this]{deleteLater();});timeout.start();writeReady();
        }
        ~Transfer()override{if(fd>=0)::close(fd);}
        void writeReady() {
            while(offset<text.size()) {
                const auto n=::write(fd,text.constData()+offset,static_cast<std::size_t>(text.size()-offset));
                if(n>0){offset+=n;continue;}
                if(n<0 && errno==EINTR)continue;
                if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return;
                deleteLater();return;
            }
            notifier->setEnabled(false);::close(fd);fd=-1;
            if(!source->reported){source->reported=true;emit source->parent->owner->consumed(source->serial);}
            deleteLater();
        }
    };
    explicit Impl(WaylandClipboard* parent):owner(parent) {
        display=wl_display_connect(nullptr);
        if(!display){error="Cannot connect to the native Wayland display";return;}
        registry=wl_display_get_registry(display);
        static const wl_registry_listener listener{
            [](void* data,wl_registry* registry,uint32_t name,const char* interface,uint32_t version){
                auto& self=*static_cast<Impl*>(data);
                if(std::strcmp(interface,zwlr_data_control_manager_v1_interface.name)==0)
                    self.manager=static_cast<zwlr_data_control_manager_v1*>(wl_registry_bind(registry,name,&zwlr_data_control_manager_v1_interface,std::min(version,2u)));
                if(std::strcmp(interface,wl_seat_interface.name)==0 && !self.seat)
                    self.seat=static_cast<wl_seat*>(wl_registry_bind(registry,name,&wl_seat_interface,std::min(version,5u)));
            },[](void*,wl_registry*,uint32_t){}
        };
        wl_registry_add_listener(registry,&listener,this);
        if(wl_display_roundtrip(display)<0 || !manager || !seat){error="KWin native data-control protocol or seat is unavailable";return;}
        device=zwlr_data_control_manager_v1_get_data_device(manager,seat);
        static const zwlr_data_control_offer_v1_listener offer_listener{
            [](void*,zwlr_data_control_offer_v1*,const char*){}
        };
        static const zwlr_data_control_device_v1_listener device_listener{
            [](void*,zwlr_data_control_device_v1*,zwlr_data_control_offer_v1* offer){
                zwlr_data_control_offer_v1_add_listener(offer,&offer_listener,nullptr);
            },
            [](void*,zwlr_data_control_device_v1*,zwlr_data_control_offer_v1* offer){if(offer)zwlr_data_control_offer_v1_destroy(offer);},
            [](void* data,zwlr_data_control_device_v1*){static_cast<Impl*>(data)->error="Wayland data-control device finished";},
            [](void*,zwlr_data_control_device_v1*,zwlr_data_control_offer_v1* offer){if(offer)zwlr_data_control_offer_v1_destroy(offer);}
        };
        zwlr_data_control_device_v1_add_listener(device,&device_listener,this);
        notifier=std::make_unique<QSocketNotifier>(wl_display_get_fd(display),QSocketNotifier::Read);
        QObject::connect(notifier.get(),&QSocketNotifier::activated,owner,[this]{
            if(wl_display_dispatch(display)<0){error="Wayland display disconnected";notifier->setEnabled(false);}else wl_display_flush(display);
        });
        wl_display_flush(display);
    }
    ~Impl(){notifier.reset();sources.clear();if(device)zwlr_data_control_device_v1_destroy(device);if(manager)zwlr_data_control_manager_v1_destroy(manager);if(seat)wl_seat_destroy(seat);if(registry)wl_registry_destroy(registry);if(display)wl_display_disconnect(display);}
};
WaylandClipboard::WaylandClipboard(QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(this)){}
WaylandClipboard::~WaylandClipboard(){qDeleteAll(children());}
bool WaylandClipboard::available()const{return impl_->error.isEmpty() && impl_->device;}
QString WaylandClipboard::reason()const{return impl_->error;}
quint32 WaylandClipboard::setText(const QString& text) {
    if(!available())throw std::runtime_error(reason().toStdString());
    const auto utf8=text.toUtf8();if(utf8.isEmpty() || utf8.size()>8192)throw std::runtime_error("Clipboard text must contain 1..8192 UTF-8 bytes");
    auto source=std::make_shared<Impl::Source>();source->parent=impl_.get();source->text=utf8;source->serial=++impl_->serial;
    source->source=zwlr_data_control_manager_v1_create_data_source(impl_->manager);
    static const zwlr_data_control_source_v1_listener listener{
        [](void* data,zwlr_data_control_source_v1*,const char* mime,int fd){
            auto* source=static_cast<Impl::Source*>(data);
            if(std::strcmp(mime,"text/plain;charset=utf-8") && std::strcmp(mime,"text/plain") && std::strcmp(mime,"UTF8_STRING")){::close(fd);return;}
            const auto found=std::ranges::find_if(source->parent->sources,[&](const auto& item){return item.get()==source;});
            if(found==source->parent->sources.end()){::close(fd);return;}
            new Impl::Transfer(fd,*found,source->parent->owner);
        },
        [](void* data,zwlr_data_control_source_v1*){
            auto* source=static_cast<Impl::Source*>(data);
            if(source->source){zwlr_data_control_source_v1_destroy(source->source);source->source=nullptr;}
            if(source->parent->current==source)source->parent->current=nullptr;
            auto& sources=source->parent->sources;
            std::erase_if(sources,[&](const auto& item){return item.get()==source;});
        }
    };
    zwlr_data_control_source_v1_add_listener(source->source,&listener,source.get());
    for(const char* mime:{"text/plain;charset=utf-8","text/plain","UTF8_STRING"})zwlr_data_control_source_v1_offer(source->source,mime);
    const auto serial=source->serial;
    zwlr_data_control_device_v1_set_selection(impl_->device,source->source);
    impl_->current=source.get();
    impl_->sources.push_back(std::move(source));wl_display_flush(impl_->display);return serial;
}
void WaylandClipboard::clear(){if(available() && impl_->current && impl_->current->source){zwlr_data_control_device_v1_set_selection(impl_->device,nullptr);impl_->current=nullptr;wl_display_flush(impl_->display);}}
