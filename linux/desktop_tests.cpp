// Run under an actual X server (CI: xvfb-run with openbox). No HID device,
// uinput access or root is used. Test windows receive real XTEST input and paste.
#include "desktop.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace loo_linux;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<class F>void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Expected rejection");}
struct Receiver {
    Display* display{XOpenDisplay(nullptr)};
    Window first{},second{};
    Atom active{},clipboard{},utf8{},paste_property{};
    unsigned press{},release{},focus{};
    std::vector<KeySym> symbols;
    std::vector<std::string> pasted;
    Receiver() {
        require(display,"No X11 test display");
        active=XInternAtom(display,"_NET_ACTIVE_WINDOW",False);clipboard=XInternAtom(display,"CLIPBOARD",False);
        utf8=XInternAtom(display,"UTF8_STRING",False);paste_property=XInternAtom(display,"LOO_TEST_PASTE",False);
        // Map only the client currently under test. Mapping both lets a delayed
        // WM focus-on-map for Browser race with Editor's first profile lookup.
        first=window("loo-editor","LooEditor",40);
        wait_mapped(first);
    }
    void wait_mapped(Window window) {
        XSync(display,False);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        for(;;) {
            XWindowAttributes state{};XGetWindowAttributes(display,window,&state);
            if(state.map_state==IsViewable)break;
            require(std::chrono::steady_clock::now()<deadline,"Window manager did not map test clients");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void create_browser(){second=window("loo-browser","LooBrowser",300);wait_mapped(second);}
    ~Receiver(){if(display){if(first)XDestroyWindow(display,first);if(second)XDestroyWindow(display,second);XCloseDisplay(display);}}
    Window window(const char* instance,const char* type,int x) {
        const auto w=XCreateSimpleWindow(display,DefaultRootWindow(display),x,60,240,160,0,0,0x223344);
        XClassHint hint{const_cast<char*>(instance),const_cast<char*>(type)};XSetClassHint(display,w,&hint);
        const unsigned long pid=getpid();
        XChangeProperty(display,w,XInternAtom(display,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,reinterpret_cast<const unsigned char*>(&pid),1);
        XStoreName(display,w,type);XSelectInput(display,w,KeyPressMask|KeyReleaseMask|FocusChangeMask|PropertyChangeMask);
        XMapWindow(display,w);return w;
    }
    void activate(Window window) {
        XEvent message{};message.xclient.type=ClientMessage;message.xclient.window=window;message.xclient.message_type=active;
        message.xclient.format=32;message.xclient.data.l[0]=2;message.xclient.data.l[1]=CurrentTime;
        XSendEvent(display,DefaultRootWindow(display),False,SubstructureRedirectMask|SubstructureNotifyMask,&message);
        XMapRaised(display,window);XSetInputFocus(display,window,RevertToParent,CurrentTime);XSync(display,False);
    }
    void key(KeySym sym,bool pressed) {
        auto code=XKeysymToKeycode(display,sym);require(code,"Test key missing");
        require(XTestFakeKeyEvent(display,code,pressed,CurrentTime),"XTest input failed");XSync(display,False);
    }
    void pump() {
        while(XPending(display)) {
            XEvent event{};XNextEvent(display,&event);
            if(event.type==FocusIn){++focus;continue;}
            if(event.type==MappingNotify){XRefreshKeyboardMapping(&event.xmapping);continue;}
            if(event.type==KeyPress) {
                ++press;auto sym=XLookupKeysym(&event.xkey,0);symbols.push_back(sym);
                if(sym==XK_v && (event.xkey.state&ControlMask))
                    XConvertSelection(display,clipboard,utf8,paste_property,event.xkey.window,CurrentTime);
            }
            if(event.type==KeyRelease)++release;
            if(event.type==SelectionNotify) {
                require(event.xselection.property!=None,"Text paste rejected");
                Atom actual{};int format{};unsigned long count{},after{};unsigned char* value{};
                require(XGetWindowProperty(display,event.xselection.requestor,paste_property,0,16384,True,utf8,&actual,&format,&count,&after,&value)==Success,"Cannot read paste");
                require(actual==utf8 && format==8 && !after,"Invalid UTF8 paste property");
                pasted.emplace_back(reinterpret_cast<char*>(value),count);if(value)XFree(value);
            }
        }
        XFlush(display);
    }
    bool modifiers_up() {
        char keys[32]{};XQueryKeymap(display,keys);
        for(auto sym:{XK_Control_L,XK_Control_R,XK_Shift_L,XK_Shift_R,XK_Alt_L,XK_Alt_R,XK_Super_L,XK_Super_R}) {
            auto code=XKeysymToKeycode(display,sym);if(code && (static_cast<unsigned char>(keys[code/8])&(1u<<(code%8))))return false;
        }
        return true;
    }
};
template<class F>void until(const char* checkpoint,Desktop& desktop,Receiver& receiver,F condition,std::vector<std::vector<Action>>& fired) {
    std::cout<<"Waiting for "<<checkpoint<<std::endl;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do {
        receiver.pump();auto actions=desktop.poll_hotkeys();
        for(auto& action:actions)fired.push_back(std::move(action));
        receiver.pump();if(condition()){std::cout<<"Passed "<<checkpoint<<std::endl;return;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }while(std::chrono::steady_clock::now()<end);
    Window focused{};int revert{};XGetInputFocus(receiver.display,&focused,&revert);
    std::cerr<<"Checkpoint "<<checkpoint<<": focus-window="<<focused<<", expected-clients="<<receiver.first<<','<<receiver.second
             <<", focus-events="<<receiver.focus<<", presses="<<receiver.press<<", releases="<<receiver.release
             <<", hotkeys="<<fired.size()<<", pasted="<<receiver.pasted.size()<<", text-busy="<<desktop.busy()<<", modifiers-up="<<receiver.modifiers_up()<<"\nidentities:";
    for(const auto& id:desktop.foreground())std::cerr<<' '<<id;
    std::cerr<<"\nreceived symbols:";
    for(auto symbol:receiver.symbols)std::cerr<<' '<<(XKeysymToString(symbol)?XKeysymToString(symbol):"<unknown>");
    std::cerr<<'\n';
    throw std::runtime_error(std::string("Timed out waiting for actual X11 events: ")+checkpoint);
}
}
int main() {
    try {
        Receiver receiver;Desktop desktop;require(desktop.available(),desktop.reason().c_str());
        std::vector<std::vector<Action>> fired;
        // The CI XKB layout resolves Ctrl+Alt+Fn to a server VT action. A
        // successful XGrabKey alone did not mean that the trigger could fire.
        auto reserved=parse("[hotkeys]\nCtrl+Alt+F10=Text(Reserved)\n");
        bool reserved_rejected=false;
        try{desktop.configure_hotkeys(reserved);}
        catch(const std::exception& error) {
            require(std::string(error.what()).find("XF86Switch_VT_10")!=std::string::npos,"Expected the actual reserved XKB keysym");
            std::cout<<"Reserved trigger rejected: "<<error.what()<<std::endl;reserved_rejected=true;
        }
        require(reserved_rejected,"Server-reserved VT shortcut was silently accepted");
        auto config=parse("[global]\n0053=Ctrl+A\n0056=Ctrl+C\n[loo-editor]\n0053=Ctrl+E\n[loobrowser]\n0053=Ctrl+B\n[hotkeys]\nCtrl+Shift+F10=Text(Triggered)\n");
        desktop.configure_hotkeys(config);
        receiver.activate(receiver.first);
        until("editor foreground",desktop,receiver,[&]{const auto ids=desktop.foreground();return std::ranges::find(ids,"loo-editor")!=ids.end();},fired);
        auto identity=desktop.foreground();
        require(!identity.empty() && identity.front()==normalized(std::filesystem::read_symlink("/proc/self/exe").filename().string()),"PID executable identity priority");
        desktop.inject_chord(lookup(config,{0x53,Direction::tap},identity)->front().keys);
        until("editor Ctrl+E input",desktop,receiver,[&]{return std::ranges::find(receiver.symbols,XK_e)!=receiver.symbols.end();},fired);
        receiver.create_browser();
        receiver.activate(receiver.second);
        until("browser foreground",desktop,receiver,[&]{const auto ids=desktop.foreground();return std::ranges::find(ids,"loobrowser")!=ids.end();},fired);
        desktop.inject_chord(lookup(config,{0x53,Direction::tap},desktop.foreground())->front().keys);
        desktop.inject_chord(lookup(config,{0x56,Direction::tap},desktop.foreground())->front().keys);
        until("browser Ctrl+B and inherited Ctrl+C input",desktop,receiver,[&]{return std::ranges::find(receiver.symbols,XK_b)!=receiver.symbols.end() && std::ranges::find(receiver.symbols,XK_c)!=receiver.symbols.end();},fired);

        // Hold trigger modifiers after releasing the main key: no action fires
        // until the user's modifiers are up. Also exercise modifier-first release.
        receiver.key(XK_Control_L,true);receiver.key(XK_Shift_L,true);receiver.key(XK_F10,true);
        desktop.poll_hotkeys();receiver.key(XK_F10,false);
        require(desktop.poll_hotkeys().empty(),"Hotkey fired with held modifiers");
        receiver.key(XK_Shift_L,false);receiver.key(XK_Control_L,false);
        until("global trigger normal release",desktop,receiver,[&]{return fired.size()==1;},fired);
        require(fired[0][0].text=="Triggered","Wrong hotkey action");
        receiver.key(XK_Control_L,true);receiver.key(XK_Shift_L,true);receiver.key(XK_F10,true);
        desktop.poll_hotkeys();receiver.key(XK_Control_L,false);receiver.key(XK_Shift_L,false);receiver.key(XK_F10,false);
        until("global trigger modifiers-first release",desktop,receiver,[&]{return fired.size()==2;},fired);
        require(receiver.modifiers_up(),"Hotkey left modifier down");

        // A different client owns F9. Failed replacement must retain F10.
        const auto f9=XKeysymToKeycode(receiver.display,XK_F9);
        XGrabKey(receiver.display,f9,ControlMask|ShiftMask,DefaultRootWindow(receiver.display),False,GrabModeAsync,GrabModeAsync);
        XSync(receiver.display,False);
        auto conflict=parse("[hotkeys]\nCtrl+Shift+F9=Text(Conflict)\n");
        rejects([&]{desktop.configure_hotkeys(conflict);});
        desktop.inject_chord({KEY_LEFTCTRL,KEY_LEFTSHIFT,KEY_F10});
        until("old trigger after conflicting reload",desktop,receiver,[&]{return fired.size()==3;},fired);
        XUngrabKey(receiver.display,f9,ControlMask|ShiftMask,DefaultRootWindow(receiver.display));XSync(receiver.display,False);

        // The same chord emitted by an action must reach the focused client,
        // without recursively activating our global trigger.
        desktop.suspend_hotkeys(true);desktop.inject_chord({KEY_LEFTCTRL,KEY_LEFTSHIFT,KEY_F10});
        until("suspended shortcut reaches client",desktop,receiver,[&]{return std::ranges::find(receiver.symbols,XK_F10)!=receiver.symbols.end();},fired);
        desktop.suspend_hotkeys(false);require(fired.size()==3,"Synthetic action recursively fired a trigger");

        const std::string text="Hello; (X11) \xe4\xbd\xa0\xe5\xa5\xbd\nsecond line";
        desktop.type_text(text);
        until("Unicode paste received",desktop,receiver,[&]{return !desktop.busy() && !receiver.pasted.empty();},fired);
        require(receiver.pasted.back()==text,"UTF-8 Text did not reach focused window");
        desktop.type_text("one");desktop.type_text("two");
        until("ordered text queue received",desktop,receiver,[&]{return !desktop.busy() && receiver.pasted.size()==3;},fired);
        require(receiver.pasted[1]=="one" && receiver.pasted[2]=="two","Text queue reordered payloads");
        desktop.type_text("discard-before-dispatch");desktop.cancel_actions();
        require(!desktop.busy(),"Cancelled unsent Text remained busy");
        desktop.type_text("fresh-after-cancel");
        until("new text after unsent cancellation",desktop,receiver,[&]{return !desktop.busy()&&receiver.pasted.size()==4;},fired);
        require(receiver.pasted.back()=="fresh-after-cancel","Cancelled unsent Text was injected");
        // The first chord is already sent and cannot be recalled. Cancel its
        // acknowledgement wait plus the second, not-yet-dispatched text.
        desktop.type_text("already-dispatched");desktop.poll_hotkeys();
        require(desktop.busy(),"Expected an in-flight clipboard request");
        desktop.type_text("discard-following");desktop.cancel_actions();
        require(!desktop.busy(),"Cancelled clipboard wait remained busy");
        until("already dispatched text may finish",desktop,receiver,[&]{return receiver.pasted.size()==5;},fired);
        require(receiver.pasted.back()=="already-dispatched","Cancellation altered already dispatched clipboard contents");
        desktop.type_text("fresh-after-late-read");
        until("new text after late cancelled read",desktop,receiver,[&]{return !desktop.busy()&&receiver.pasted.size()==6;},fired);
        require(receiver.pasted.back()=="fresh-after-late-read","Cancelled queued Text survived a late read");
        require(receiver.modifiers_up(),"Text left modifiers down");
        require(receiver.focus>=2 && receiver.release>0,"Focus/key release events not observed");

        // A Wayland session without its native bridge must not silently fall
        // back to the otherwise accessible X11 DISPLAY used by this test.
        const char* previous_bus=std::getenv("DBUS_SESSION_BUS_ADDRESS");
        const std::string saved_bus=previous_bus?previous_bus:"";
        setenv("DBUS_SESSION_BUS_ADDRESS",("unix:path=/tmp/loo-no-native-bridge-"+std::to_string(getpid())).c_str(),1);
        setenv("XDG_SESSION_TYPE","wayland",1);
        {Desktop wayland;require(!wayland.available(),"Missing native bridge incorrectly fell back to Xwayland");
         wayland.validate(parse("[global]\n0053=MouseBack"));
         rejects([&]{wayland.validate(config);});
         rejects([&]{wayland.type_text("missing native bridge");});}
        if(previous_bus)setenv("DBUS_SESSION_BUS_ADDRESS",saved_bus.c_str(),1);else unsetenv("DBUS_SESSION_BUS_ADDRESS");
        std::cout<<"X11 real events: focus="<<receiver.focus<<", key presses="<<receiver.press<<", key releases="<<receiver.release
                 <<", global triggers="<<fired.size()<<", UTF-8 paste payloads="<<receiver.pasted.size()<<"\n"
                 <<"Application/PID matching, profile inheritance, grab rollback, modifier release, UTF-8 paste and missing-native-bridge rejection passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
