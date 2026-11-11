#include "desktop.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/XTest.h>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <set>
#include <unistd.h>

namespace loo_linux {
namespace {
using Clock=std::chrono::steady_clock;
thread_local int last_x_error{};
int capture_error(Display*,XErrorEvent* event) {last_x_error=event->error_code;return 0;}
struct ErrorScope {
    Display* display;
    XErrorHandler previous;
    explicit ErrorScope(Display* d):display(d) {XSync(d,False);last_x_error=0;previous=XSetErrorHandler(capture_error);}
    int finish(){XSync(display,False);return last_x_error;}
    ~ErrorScope(){XSync(display,False);XSetErrorHandler(previous);}
};
KeySym symbol(unsigned key) {
    static const std::map<unsigned,KeySym> names={
        {KEY_LEFTCTRL,XK_Control_L},{KEY_LEFTALT,XK_Alt_L},{KEY_LEFTSHIFT,XK_Shift_L},{KEY_LEFTMETA,XK_Super_L},
        {KEY_LEFT,XK_Left},{KEY_RIGHT,XK_Right},{KEY_UP,XK_Up},{KEY_DOWN,XK_Down},{KEY_TAB,XK_Tab},
        {KEY_ENTER,XK_Return},{KEY_ESC,XK_Escape},{KEY_SPACE,XK_space},{KEY_BACKSPACE,XK_BackSpace},
        {KEY_DELETE,XK_Delete},{KEY_HOME,XK_Home},{KEY_END,XK_End},{KEY_PAGEUP,XK_Page_Up},{KEY_PAGEDOWN,XK_Page_Down},
        {KEY_SYSRQ,XK_Print},{KEY_VOLUMEUP,XF86XK_AudioRaiseVolume},{KEY_VOLUMEDOWN,XF86XK_AudioLowerVolume},
        {KEY_MUTE,XF86XK_AudioMute},{KEY_PLAYPAUSE,XF86XK_AudioPlay},{KEY_NEXTSONG,XF86XK_AudioNext},
        {KEY_PREVIOUSSONG,XF86XK_AudioPrev}};
    if(auto it=names.find(key);it!=names.end())return it->second;
    constexpr unsigned letters[]={KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,
                                  KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
    for(unsigned i=0;i<26;++i)if(key==letters[i])return XK_a+i;
    if(key==KEY_0)return XK_0;
    if(key>=KEY_1 && key<=KEY_9)return XK_1+key-KEY_1;
    if(key>=KEY_F1 && key<=KEY_F10)return XK_F1+key-KEY_F1;
    if(key==KEY_F11)return XK_F11;if(key==KEY_F12)return XK_F12;
    throw std::runtime_error("Unsupported X11 keyboard key: "+std::to_string(key));
}
unsigned modifier(unsigned key) {
    if(key==KEY_LEFTCTRL)return ControlMask;
    if(key==KEY_LEFTALT)return Mod1Mask;
    if(key==KEY_LEFTSHIFT)return ShiftMask;
    if(key==KEY_LEFTMETA)return Mod4Mask;
    return 0;
}
bool down(const char* state,unsigned code){return code<256 && (static_cast<unsigned char>(state[code/8])&(1u<<(code%8)));}
bool text_actions(const std::vector<Action>& sequence){return std::ranges::any_of(sequence,[](const Action& a){return a.operation==Op::text;});}
}
struct Desktop::Impl {
    Display* display{};
    Window root{},clipboard_window{};
    Atom active{},pid{},clipboard{},targets{},utf8{};
    bool xtest{};
    std::string unavailable;
    unsigned locks{LockMask};
    struct Grab {unsigned code{},mask{};auto operator<=>(const Grab&)const=default;};
    std::map<Grab,std::vector<Action>> triggers;
    bool suspended{};
    std::map<unsigned,std::vector<Action>> pressed_triggers;
    std::vector<std::pair<unsigned,std::vector<Action>>> awaiting_release;
    std::vector<unsigned> modifiers;
    std::deque<std::string> texts;
    std::string clipboard_text;
    bool paste_pending{};
    Clock::time_point paste_deadline{};
    Impl() {
        const auto session=std::getenv("XDG_SESSION_TYPE"),wayland=std::getenv("WAYLAND_DISPLAY");
        if((session && normalized(session)=="wayland") || (wayland && *wayland)) {
            unavailable="Application profiles, global hotkeys and Text require an X11 session; native Wayland/Xwayland is not supported";return;
        }
        display=XOpenDisplay(nullptr);
        if(!display){unavailable="No accessible X11 DISPLAY; application profiles, global hotkeys and Text are unavailable";return;}
        root=DefaultRootWindow(display);
        active=XInternAtom(display,"_NET_ACTIVE_WINDOW",False);pid=XInternAtom(display,"_NET_WM_PID",False);
        clipboard=XInternAtom(display,"CLIPBOARD",False);targets=XInternAtom(display,"TARGETS",False);utf8=XInternAtom(display,"UTF8_STRING",False);
        clipboard_window=XCreateSimpleWindow(display,root,0,0,1,1,0,0,0);
        int event{},error{},major{},minor{};xtest=XTestQueryExtension(display,&event,&error,&major,&minor);
        Bool detectable{};XkbSetDetectableAutoRepeat(display,True,&detectable);
        for(auto sym:{XK_Control_L,XK_Control_R,XK_Shift_L,XK_Shift_R,XK_Alt_L,XK_Alt_R,XK_Super_L,XK_Super_R})
            if(auto code=XKeysymToKeycode(display,sym))modifiers.push_back(code);
        auto modmap=XGetModifierMapping(display);
        const auto num=XKeysymToKeycode(display,XK_Num_Lock),scroll=XKeysymToKeycode(display,XK_Scroll_Lock);
        if(modmap) {
            for(int bit=0;bit<8;++bit)for(int i=0;i<modmap->max_keypermod;++i) {
                const auto code=modmap->modifiermap[bit*modmap->max_keypermod+i];
                if(code && (code==num || code==scroll))locks|=1u<<bit;
            }
            XFreeModifiermap(modmap);
        }
    }
    ~Impl(){if(display){XDestroyWindow(display,clipboard_window);XCloseDisplay(display);}}
    bool released(const char* state)const{return std::ranges::none_of(modifiers,[&](unsigned code){return down(state,code);});}
    unsigned code(unsigned evdev)const {
        auto result=XKeysymToKeycode(display,symbol(evdev));
        if(!result)throw std::runtime_error("Configured key is not present in the X11 keyboard layout");return result;
    }
    unsigned long property(Window window,Atom name,Atom expected) {
        Atom actual{};int format{};unsigned long count{},remaining{};unsigned char* data{};
        unsigned long result{};
        if(XGetWindowProperty(display,window,name,0,1,False,expected,&actual,&format,&count,&remaining,&data)==Success && actual==expected && format==32 && count)
            result=*reinterpret_cast<unsigned long*>(data);
        if(data)XFree(data);return result;
    }
    std::vector<unsigned> variants(unsigned mask)const {
        std::vector<unsigned> result;
        for(unsigned bits=0;bits<256;++bits)if((bits&~locks)==0)result.push_back(mask|bits);
        return result;
    }
    void grab(Grab key,bool enable) {
        for(auto mask:variants(key.mask)) {
            if(enable)XGrabKey(display,key.code,mask,root,False,GrabModeAsync,GrabModeAsync);
            else XUngrabKey(display,key.code,mask,root);
        }
    }
    void send_chord(const std::vector<unsigned>& keys) {
        if(!xtest)throw std::runtime_error("XTEST extension is unavailable");
        std::vector<unsigned> codes;for(auto key:keys)codes.push_back(code(key));
        // Never release a modifier that was physically down before injection.
        char state[32]{};XQueryKeymap(display,state);std::vector<unsigned> pressed;
        for(auto keycode:codes)if(!down(state,keycode)) {
            if(!XTestFakeKeyEvent(display,keycode,True,CurrentTime))throw std::runtime_error("XTEST key press failed");
            pressed.push_back(keycode);
        }
        for(auto it=pressed.rbegin();it!=pressed.rend();++it)XTestFakeKeyEvent(display,*it,False,CurrentTime);
        XFlush(display);
    }
    void selection(const XSelectionRequestEvent& request) {
        XEvent event{};event.xselection.type=SelectionNotify;event.xselection.display=request.display;
        event.xselection.requestor=request.requestor;event.xselection.selection=request.selection;
        event.xselection.target=request.target;event.xselection.time=request.time;event.xselection.property=None;
        const auto property=request.property==None?request.target:request.property;
        if(request.selection==clipboard && request.target==targets) {
            Atom supported[]={targets,utf8,XA_STRING};
            XChangeProperty(display,request.requestor,property,XA_ATOM,32,PropModeReplace,reinterpret_cast<unsigned char*>(supported),3);
            event.xselection.property=property;
        } else if(request.selection==clipboard && (request.target==utf8 || request.target==XA_STRING)) {
            // STRING is only correct for ASCII. Non-ASCII clients must request UTF8_STRING.
            const bool ascii=std::ranges::all_of(clipboard_text,[](unsigned char c){return c<128;});
            if(request.target==utf8 || ascii) {
                XChangeProperty(display,request.requestor,property,request.target,8,PropModeReplace,
                    reinterpret_cast<const unsigned char*>(clipboard_text.data()),static_cast<int>(clipboard_text.size()));
                event.xselection.property=property;
                paste_pending=false;
            }
        }
        XSendEvent(display,request.requestor,False,0,&event);XFlush(display);
    }
};
Desktop::Desktop():impl_(std::make_unique<Impl>()){}
Desktop::~Desktop()=default;
bool Desktop::available()const{return impl_->display!=nullptr;}
const std::string& Desktop::reason()const{return impl_->unavailable;}
std::vector<std::string> Desktop::foreground() {
    if(!available())return {};
    auto& x=*impl_;ErrorScope errors(x.display);
    auto window=static_cast<Window>(x.property(x.root,x.active,XA_WINDOW));
    if(!window){int revert{};XGetInputFocus(x.display,&window,&revert);}
    std::vector<std::string> identities;
    auto add=[&](std::string value){value=normalized(value);if(!value.empty() && std::ranges::find(identities,value)==identities.end())identities.push_back(std::move(value));};
    // Focus may be a child widget without WM_CLASS: walk to its client parent.
    for(unsigned depth=0;window && window!=x.root && window!=PointerRoot && depth<32;++depth) {
        if(auto process=x.property(window,x.pid,XA_CARDINAL)) {
            std::error_code error;
            auto executable=std::filesystem::read_symlink("/proc/"+std::to_string(process)+"/exe",error);
            if(!error)add(executable.filename().string());
        }
        XClassHint hint{};
        if(XGetClassHint(x.display,window,&hint)) {
            if(hint.res_name){add(hint.res_name);XFree(hint.res_name);}
            if(hint.res_class){add(hint.res_class);XFree(hint.res_class);}
            if(!identities.empty())break;
        }
        Window root{},parent{},*children{};unsigned count{};
        if(!XQueryTree(x.display,window,&root,&parent,&children,&count))break;
        if(children)XFree(children);window=parent;
    }
    return errors.finish()?std::vector<std::string>{}:identities;
}
void Desktop::validate(const Config& config)const {
    bool text=false;
    for(const auto& [id,sequence]:config.bindings)text|=text_actions(sequence);
    for(const auto& [name,profile]:config.profiles)for(const auto& [id,sequence]:profile.bindings)text|=text_actions(sequence);
    for(const auto& trigger:config.hotkeys)text|=text_actions(trigger.actions);
    for(const auto& item:config.ring)text|=text_actions(item.actions);
    for(const auto& [name,items]:config.folders)for(const auto& item:items)text|=text_actions(item.actions);
    if((text || !config.profiles.empty() || !config.hotkeys.empty()) && !available())throw std::runtime_error(reason());
    if(text && !impl_->xtest)throw std::runtime_error("Text requires the XTEST extension");
    for(const auto& hotkey:config.hotkeys) {
        unsigned code{},mask{},consumed{};KeySym resolved{};
        for(auto key:hotkey.keys)if(auto mod=modifier(key))mask|=mod;else code=impl_->code(key);
        if(code && XkbLookupKeySym(impl_->display,code,mask,&consumed,&resolved) &&
           resolved>=XF86XK_Switch_VT_1 && resolved<=XF86XK_Switch_VT_12) {
            const auto name=XKeysymToString(resolved);
            throw std::runtime_error("Global hotkey "+hotkey.name+" resolves to reserved X11 keysym "+
                (name?std::string(name):std::to_string(resolved))+"; choose a non-reserved shortcut");
        }
    }
}
void Desktop::configure_hotkeys(const Config& config) {
    validate(config);if(!available())return;
    auto& x=*impl_;std::map<Impl::Grab,std::vector<Action>> fresh;
    for(const auto& hotkey:config.hotkeys) {
        Impl::Grab grab;
        for(auto key:hotkey.keys)if(auto mod=modifier(key))grab.mask|=mod;else grab.code=x.code(key);
        if(!grab.code || !fresh.emplace(grab,hotkey.actions).second)throw std::runtime_error("Duplicate or invalid X11 trigger");
    }
    std::vector<Impl::Grab> added;
    ErrorScope errors(x.display);
    if(x.suspended) {
        for(const auto& [grab,sequence]:fresh)x.grab(grab,true);
        const auto failed=errors.finish();
        for(const auto& [grab,sequence]:fresh)x.grab(grab,false);
        if(failed)throw std::runtime_error("A global hotkey is already registered by another application; previous configuration retained");
        x.triggers=std::move(fresh);x.awaiting_release.clear();x.pressed_triggers.clear();return;
    }
    for(const auto& [grab,sequence]:fresh)if(!x.triggers.contains(grab)){x.grab(grab,true);added.push_back(grab);}
    if(errors.finish()) {
        for(auto grab:added)x.grab(grab,false);
        throw std::runtime_error("A global hotkey is already registered by another application; previous hotkeys remain active");
    }
    for(const auto& [grab,sequence]:x.triggers)if(!fresh.contains(grab))x.grab(grab,false);
    x.triggers=std::move(fresh);x.awaiting_release.clear();x.pressed_triggers.clear();XFlush(x.display);
}
void Desktop::suspend_hotkeys(bool suspended) {
    if(!available() || impl_->suspended==suspended)return;
    auto& x=*impl_;ErrorScope errors(x.display);
    if(suspended) {
        for(const auto& [grab,sequence]:x.triggers)x.grab(grab,false);
        x.suspended=true;
    } else {
        // Drop key events already queued during suspension, never replay them as
        // triggers. SelectionRequest events are deliberately left for poll().
        XSync(x.display,False);XEvent event{};
        while(XCheckMaskEvent(x.display,KeyPressMask|KeyReleaseMask,&event)){}
        std::vector<Impl::Grab> restored;
        for(const auto& [grab,sequence]:x.triggers){x.grab(grab,true);restored.push_back(grab);}
        if(errors.finish()) {
            for(auto grab:restored)x.grab(grab,false);
            throw std::runtime_error("A hotkey was claimed by another application while actions were running");
        }
        x.suspended=false;
    }
    x.pressed_triggers.clear();x.awaiting_release.clear();XSync(x.display,False);
}
std::vector<std::vector<Action>> Desktop::poll_hotkeys() {
    std::vector<std::vector<Action>> result;if(!available())return result;
    auto& x=*impl_;ErrorScope errors(x.display);
    while(XPending(x.display)) {
        XEvent event{};XNextEvent(x.display,&event);
        if(event.type==MappingNotify){XRefreshKeyboardMapping(&event.xmapping);continue;}
        if(event.type==SelectionRequest){x.selection(event.xselectionrequest);continue;}
        if(event.type==SelectionClear){x.clipboard_text.clear();if(x.paste_pending){x.paste_pending=false;throw std::runtime_error("Clipboard ownership changed before Text could paste");}continue;}
        if(x.suspended)continue;
        if(event.type==KeyPress) {
            Impl::Grab grab{event.xkey.keycode,event.xkey.state&~x.locks&255u};
            if(auto it=x.triggers.find(grab);it!=x.triggers.end())x.pressed_triggers.try_emplace(grab.code,it->second);
            continue;
        }
        if(event.type!=KeyRelease)continue;
        const auto code=event.xkey.keycode;
        if(auto it=x.pressed_triggers.find(code);it!=x.pressed_triggers.end()) {
            if(std::ranges::none_of(x.awaiting_release,[&](const auto& pending){return pending.first==code;}))
                x.awaiting_release.emplace_back(code,std::move(it->second));
            x.pressed_triggers.erase(it);
        }
    }
    char state[32]{};XQueryKeymap(x.display,state);
    if(x.released(state)) {
        for(auto it=x.awaiting_release.begin();it!=x.awaiting_release.end();) {
            if(!down(state,it->first)){result.push_back(std::move(it->second));it=x.awaiting_release.erase(it);}else ++it;
        }
        if(!x.paste_pending && !x.texts.empty()) {
            x.clipboard_text=std::move(x.texts.front());x.texts.pop_front();
            XSetSelectionOwner(x.display,x.clipboard,x.clipboard_window,CurrentTime);
            if(XGetSelectionOwner(x.display,x.clipboard)!=x.clipboard_window)throw std::runtime_error("Could not own the X11 clipboard for Text");
            x.paste_pending=true;x.paste_deadline=Clock::now()+std::chrono::seconds(2);
            x.send_chord({KEY_LEFTCTRL,KEY_V});
        }
    }
    if(x.paste_pending && Clock::now()>x.paste_deadline){x.paste_pending=false;throw std::runtime_error("Text paste timed out: foreground application must support Ctrl+V / UTF8_STRING clipboard");}
    return result;
}
void Desktop::type_text(std::string_view text) {
    if(!available())throw std::runtime_error(reason());
    if(!impl_->xtest)throw std::runtime_error("Text requires the XTEST extension");
    if(text.empty() || text.size()>8192 || text.find('\0')!=text.npos)throw std::runtime_error("Invalid Text payload");
    if(impl_->texts.size()>=64)throw std::runtime_error("Text queue is full");
    impl_->texts.emplace_back(text);
}
bool Desktop::busy()const{return !impl_->texts.empty() || impl_->paste_pending;}
void Desktop::inject_chord(const std::vector<unsigned>& keys) {
    if(!available())throw std::runtime_error(reason());impl_->send_chord(keys);
}
}
