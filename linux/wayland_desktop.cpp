#include "wayland_desktop.hpp"
#include <gio/gio.h>
#include <chrono>
#include <deque>
#include <set>

namespace loo_linux {
namespace {
using Clock=std::chrono::steady_clock;
constexpr char service[]="org.logi.OptionsOpen.Desktop1";
constexpr char object[]="/org/logi/OptionsOpen/Desktop1";
using Variant=std::unique_ptr<GVariant,decltype(&g_variant_unref)>;
std::string error_text(GError* error) {
    const std::string result=error?error->message:"Unknown D-Bus failure";
    if(error)g_error_free(error);return result;
}
std::string quote(std::string_view value) {
    std::string out="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=c;}return out+'"';
}
bool needs_text(const Config& config) {
    auto text=[](const auto& actions){return std::ranges::any_of(actions,[](const Action& action){return action.operation==Op::text;});};
    for(const auto& [id,actions]:config.bindings)if(text(actions))return true;
    for(const auto& [name,profile]:config.profiles)for(const auto& [id,actions]:profile.bindings)if(text(actions))return true;
    for(const auto& hotkey:config.hotkeys)if(text(hotkey.actions))return true;
    for(const auto& item:config.ring)if(text(item.actions))return true;
    for(const auto& [name,items]:config.folders)for(const auto& item:items)if(text(item.actions))return true;
    return false;
}
std::string accelerator(const std::vector<unsigned>& keys) {
    std::string modifiers,base;
    static const std::map<unsigned,std::string> names={
        {KEY_LEFT,"Left"},{KEY_RIGHT,"Right"},{KEY_UP,"Up"},{KEY_DOWN,"Down"},{KEY_TAB,"Tab"},
        {KEY_ENTER,"Return"},{KEY_ESC,"Escape"},{KEY_SPACE,"space"},{KEY_BACKSPACE,"BackSpace"},
        {KEY_DELETE,"Delete"},{KEY_HOME,"Home"},{KEY_END,"End"},{KEY_PAGEUP,"Page_Up"},{KEY_PAGEDOWN,"Page_Down"},
        {KEY_SYSRQ,"Print"},{KEY_VOLUMEUP,"XF86AudioRaiseVolume"},{KEY_VOLUMEDOWN,"XF86AudioLowerVolume"},
        {KEY_MUTE,"XF86AudioMute"},{KEY_PLAYPAUSE,"XF86AudioPlay"},{KEY_NEXTSONG,"XF86AudioNext"},{KEY_PREVIOUSSONG,"XF86AudioPrev"}};
    constexpr unsigned letters[]={KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,
                                  KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
    for(auto key:keys) {
        if(key==KEY_LEFTCTRL){modifiers+="<Control>";continue;}
        if(key==KEY_LEFTSHIFT){modifiers+="<Shift>";continue;}
        if(key==KEY_LEFTALT){modifiers+="<Alt>";continue;}
        if(key==KEY_LEFTMETA){modifiers+="<Super>";continue;}
        if(!base.empty())throw std::runtime_error("Global shortcut requires exactly one non-modifier key");
        if(auto found=names.find(key);found!=names.end())base=found->second;
        for(unsigned n=0;n<26;++n)if(key==letters[n])base=std::string(1,static_cast<char>('a'+n));
        if(key>=KEY_1 && key<=KEY_9)base=std::string(1,static_cast<char>('1'+key-KEY_1));
        if(key==KEY_0)base="0";
        if(key>=KEY_F1 && key<=KEY_F10)base="F"+std::to_string(key-KEY_F1+1);
        if(key==KEY_F11)base="F11";if(key==KEY_F12)base="F12";
        if(base.empty())throw std::runtime_error("Unsupported native Wayland shortcut key: "+std::to_string(key));
    }
    if(base.empty())throw std::runtime_error("Global shortcut has no main key");
    return modifiers+base;
}
}
struct WaylandDesktop::Impl {
    GMainContext* context{g_main_context_new()};
    GDBusConnection* connection{};
    GDBusProxy* proxy{};
    KeyEmitter emit;
    std::string unavailable,owner,desktop{"Wayland"},capability_reason;
    bool native{},profiles{},shortcuts{},clipboard{},clipboard_ack{},suspended{},registered{},profiles_required{};
    unsigned generation{},paste_serial{};
    std::map<std::string,std::vector<Action>> triggers;
    std::deque<std::string> triggered,texts;
    std::set<unsigned> consumed;
    Clock::time_point paste_deadline{};
    std::string pending_error;
    explicit Impl(KeyEmitter callback):emit(std::move(callback)) {
        g_main_context_push_thread_default(context);
        GError* error{};char* address=g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION,nullptr,&error);
        if(!address){unavailable="Native Wayland session bus unavailable: "+error_text(error);g_main_context_pop_thread_default(context);return;}
        connection=g_dbus_connection_new_for_address_sync(address,
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),nullptr,nullptr,&error);
        g_free(address);
        if(!connection){unavailable="Native Wayland session bus unavailable: "+error_text(error);g_main_context_pop_thread_default(context);return;}
        g_dbus_connection_set_exit_on_close(connection,FALSE);
        proxy=g_dbus_proxy_new_sync(connection,static_cast<GDBusProxyFlags>(G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START|G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES),
            nullptr,service,object,service,nullptr,&error);
        if(!proxy){unavailable="Native compositor bridge unavailable: "+error_text(error);g_main_context_pop_thread_default(context);return;}
        g_signal_connect(proxy,"g-signal",G_CALLBACK(signal),this);
        g_signal_connect(proxy,"notify::g-name-owner",G_CALLBACK(owner_changed),this);
        g_main_context_pop_thread_default(context);
        try{refresh();}catch(const std::exception& e){unavailable=e.what();}
    }
    ~Impl() {
        if(proxy) {
            if(!owner.empty()){GError* error{};auto reply=g_dbus_proxy_call_sync(proxy,"Release",nullptr,G_DBUS_CALL_FLAGS_NO_AUTO_START,500,nullptr,&error);if(reply)g_variant_unref(reply);if(error)g_error_free(error);}
            g_signal_handlers_disconnect_by_data(proxy,this);g_object_unref(proxy);
        }
        if(connection){g_dbus_connection_close_sync(connection,nullptr,nullptr);g_object_unref(connection);}
        g_main_context_unref(context);
    }
    Variant call(const char* method,GVariant* parameters=nullptr,const GVariantType* expected=nullptr)const {
        if(!proxy)throw std::runtime_error(unavailable);
        GError* error{};Variant reply(g_dbus_proxy_call_sync(proxy,method,parameters,G_DBUS_CALL_FLAGS_NO_AUTO_START,2000,nullptr,&error),g_variant_unref);
        if(!reply)throw std::runtime_error(std::string("Native Wayland ")+method+": "+error_text(error));
        if(expected && !g_variant_is_of_type(reply.get(),expected))throw std::runtime_error(std::string("Invalid compositor bridge reply for ")+method);
        return reply;
    }
    void refresh() {
        native=profiles=shortcuts=clipboard=clipboard_ack=false;
        capability_reason.clear();
        if(!proxy)return;
        gchar* current=g_dbus_proxy_get_name_owner(proxy);owner=current?current:"";g_free(current);
        if(owner.empty()){unavailable="Install and enable the native GNOME or KDE Logi Options Open desktop bridge";return;}
        auto reply=call("GetCapabilities",nullptr,G_VARIANT_TYPE("(a{sv})"));
        GVariant* raw{};g_variant_get(reply.get(),"(@a{sv})",&raw);Variant caps(raw,g_variant_unref);
        guint32 version{};gboolean value{};
        if(!g_variant_lookup(caps.get(),"protocol","u",&version) || version!=1)throw std::runtime_error("Native desktop bridge protocol mismatch");
        if(g_variant_lookup(caps.get(),"native_wayland","b",&value))native=value;
        if(!native)throw std::runtime_error("Desktop bridge is not serving a native Wayland compositor");
        if(g_variant_lookup(caps.get(),"foreground","b",&value))profiles=value;
        if(g_variant_lookup(caps.get(),"shortcuts","b",&value))shortcuts=value;
        if(g_variant_lookup(caps.get(),"clipboard","b",&value))clipboard=value;
        if(g_variant_lookup(caps.get(),"clipboard_ack","b",&value))clipboard_ack=value;
        const char* text{};
        if(g_variant_lookup(caps.get(),"desktop","&s",&text))desktop=text;
        if(g_variant_lookup(caps.get(),"reason","&s",&text))capability_reason=text;
        unavailable.clear();
    }
    static void owner_changed(GObject*,GParamSpec*,gpointer user)noexcept {
        auto& self=*static_cast<Impl*>(user);
        try {
            const bool in_use=self.registered || self.paste_serial || !self.texts.empty();
            self.triggered.clear();self.consumed.clear();self.paste_serial=0;self.texts.clear();
            self.refresh();
            if(in_use)self.pending_error="Native compositor bridge restarted/disconnected; reload configuration to restore desktop integration";
            self.registered=false;
        }catch(const std::exception& e){self.native=false;self.unavailable=e.what();self.pending_error=e.what();}
    }
    static void signal(GDBusProxy*,gchar*,gchar* name,GVariant* values,gpointer user)noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(std::string_view(name)=="ShortcutReleased" && g_variant_is_of_type(values,G_VARIANT_TYPE("(s)"))) {
            const char* token{};g_variant_get(values,"(&s)",&token);
            if(!self.suspended && self.triggers.contains(token) && self.triggered.size()<128)self.triggered.emplace_back(token);
        }else if(std::string_view(name)=="ClipboardConsumed" && g_variant_is_of_type(values,G_VARIANT_TYPE("(u)"))) {
            guint32 serial{};g_variant_get(values,"(u)",&serial);if(self.consumed.size()<128)self.consumed.insert(serial);
        }
    }
    void pump(){for(unsigned n=0;n<128 && g_main_context_pending(context);++n)g_main_context_iteration(context,FALSE);}
};
WaylandDesktop::WaylandDesktop(KeyEmitter emitter):impl_(std::make_unique<Impl>(std::move(emitter))){}
WaylandDesktop::~WaylandDesktop()=default;
bool WaylandDesktop::available()const{impl_->pump();return impl_->native && !impl_->owner.empty();}
const std::string& WaylandDesktop::reason()const{return impl_->unavailable;}
std::string WaylandDesktop::status_json()const {
    impl_->pump();
    // Read current permissions/capabilities: keyboard devices may have appeared
    // after the provider started, so constructor-time flags are not authoritative.
    try{impl_->refresh();}catch(const std::exception& e){impl_->native=false;impl_->unavailable=e.what();}
    const auto& w=*impl_;const auto yes=[](bool value){return value?"true":"false";};
    return "{\"available\":"+std::string(yes(available()))+",\"desktop\":"+quote(w.desktop)+",\"native_wayland\":"+yes(w.native)+
        ",\"foreground\":"+yes(w.profiles)+",\"shortcuts\":"+yes(w.shortcuts)+",\"clipboard\":"+yes(w.clipboard)+
        ",\"clipboard_ack\":"+yes(w.clipboard_ack)+",\"reason\":"+quote(w.unavailable.empty()?w.capability_reason:w.unavailable)+'}';
}
std::vector<std::string> WaylandDesktop::foreground() {
    auto& w=*impl_;w.pump();
    if(!available() || !w.profiles){if(w.profiles_required)throw std::runtime_error("Native desktop bridge no longer provides required foreground identities");return {};}
    auto reply=w.call("GetForeground",nullptr,G_VARIANT_TYPE("(as)"));gchar** values{};g_variant_get(reply.get(),"(^as)",&values);
    std::vector<std::string> result;
    for(auto item=values;item && *item;++item){auto identity=normalized(*item);if(!identity.empty() && std::ranges::find(result,identity)==result.end())result.push_back(std::move(identity));}
    g_strfreev(values);return result;
}
void WaylandDesktop::validate(const Config& config)const {
    const auto& w=*impl_;const bool text=needs_text(config);
    if((text || !config.profiles.empty() || !config.hotkeys.empty()) && !available())throw std::runtime_error(reason());
    if(!config.profiles.empty() && !w.profiles)throw std::runtime_error("Native bridge does not support application profiles");
    if(!config.hotkeys.empty() && !w.shortcuts)throw std::runtime_error("Native bridge cannot observe complete shortcut release; global shortcuts are unavailable");
    if(text && (!w.clipboard || !w.clipboard_ack))throw std::runtime_error("Native bridge lacks confirmed clipboard data delivery");
    if(text && !w.emit)throw std::runtime_error("Native Wayland Text requires an attached uinput key emitter");
    for(const auto& hotkey:config.hotkeys)static_cast<void>(accelerator(hotkey.keys));
}
void WaylandDesktop::configure_hotkeys(const Config& config) {
    validate(config);auto& w=*impl_;
    if(!available()){if(config.hotkeys.empty()){w.profiles_required=!config.profiles.empty();return;}throw std::runtime_error(reason());}
    if(config.hotkeys.empty()) {
        if(w.registered)w.call("UnregisterShortcuts",nullptr,G_VARIANT_TYPE("()"));
        w.registered=false;w.triggers.clear();w.triggered.clear();w.profiles_required=!config.profiles.empty();return;
    }
    std::map<std::string,std::vector<Action>> fresh;GVariantBuilder builder;g_variant_builder_init(&builder,G_VARIANT_TYPE("a{ss}"));
    ++w.generation;unsigned index{};
    for(const auto& hotkey:config.hotkeys) {
        auto token="g"+std::to_string(w.generation)+"_"+std::to_string(index++);const auto accel=accelerator(hotkey.keys);
        g_variant_builder_add(&builder,"{ss}",token.c_str(),accel.c_str());fresh.emplace(std::move(token),hotkey.actions);
    }
    auto reply=w.call("RegisterShortcuts",g_variant_new("(a{ss})",&builder),G_VARIANT_TYPE("(b)"));gboolean accepted{};g_variant_get(reply.get(),"(b)",&accepted);
    if(!accepted)throw std::runtime_error("Native shortcut registration rejected; previous configuration retained");
    // A client can suspend before its first registration (for example while a
    // running action is cancelled by reload). Keep that state on the provider.
    if(w.suspended)w.call("SetSuspended",g_variant_new("(b)",TRUE),G_VARIANT_TYPE("()"));
    w.triggers=std::move(fresh);w.triggered.clear();w.registered=true;w.profiles_required=!config.profiles.empty();w.pending_error.clear();
}
void WaylandDesktop::suspend_hotkeys(bool suspended) {
    auto& w=*impl_;if(w.suspended==suspended)return;
    if(w.registered){w.call("SetSuspended",g_variant_new("(b)",suspended),G_VARIANT_TYPE("()"));w.pump();}
    w.suspended=suspended;w.triggered.clear();
}
std::vector<std::vector<Action>> WaylandDesktop::poll_hotkeys() {
    auto& w=*impl_;w.pump();
    if(!w.pending_error.empty()){auto error=std::move(w.pending_error);w.pending_error.clear();throw std::runtime_error(error);}
    std::vector<std::vector<Action>> result;
    while(!w.triggered.empty()) {
        auto token=std::move(w.triggered.front());w.triggered.pop_front();
        if(!w.suspended)if(auto found=w.triggers.find(token);found!=w.triggers.end())result.push_back(found->second);
    }
    if(w.paste_serial && w.consumed.erase(w.paste_serial))w.paste_serial=0;
    if(w.paste_serial && Clock::now()>w.paste_deadline) {
        w.paste_serial=0;w.texts.clear();w.consumed.clear();
        throw std::runtime_error("Native clipboard paste timed out: no data-read acknowledgement; target must support Ctrl+V");
    }
    if(!w.paste_serial && !w.texts.empty()) {
        auto text=std::move(w.texts.front());w.texts.pop_front();w.consumed.clear();
        auto reply=w.call("SetClipboard",g_variant_new("(s)",text.c_str()),G_VARIANT_TYPE("(u)"));guint32 serial{};g_variant_get(reply.get(),"(u)",&serial);
        if(!serial)throw std::runtime_error("Native compositor did not acknowledge clipboard ownership");
        w.paste_serial=serial;w.paste_deadline=Clock::now()+std::chrono::seconds(3);
        try{inject_chord({KEY_LEFTCTRL,KEY_V});}catch(...){w.paste_serial=0;w.texts.clear();throw;}
    }
    return result;
}
void WaylandDesktop::type_text(std::string_view text) {
    if(!available())throw std::runtime_error(reason());
    if(!impl_->clipboard || !impl_->clipboard_ack || !impl_->emit)throw std::runtime_error("Native clipboard/uinput integration is unavailable");
    if(text.empty() || text.size()>8192 || text.find('\0')!=text.npos || !g_utf8_validate(text.data(),static_cast<gssize>(text.size()),nullptr))throw std::runtime_error("Invalid UTF-8 Text payload");
    if(impl_->texts.size()>=64)throw std::runtime_error("Text queue is full");impl_->texts.emplace_back(text);
}
bool WaylandDesktop::busy()const{return !impl_->texts.empty() || impl_->paste_serial!=0;}
void WaylandDesktop::inject_chord(const std::vector<unsigned>& keys) {
    if(!impl_->emit)throw std::runtime_error("Native Wayland input requires a production uinput key emitter");impl_->emit(keys);
}
}
