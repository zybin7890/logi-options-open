// Actual Qt Wayland clients in GNOME/KWin, driven through production uinput.
// Nested CI desktops forward these kernel events using a compositor-native test
// transport. No Xwayland window or synthetic Qt key event is used here.
#include "agent.hpp"
#include "desktop.hpp"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <chrono>
#include <iostream>
#include <thread>

using namespace loo_linux;
namespace {
void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
struct Editor final:QTextEdit {
    QJsonArray presses,pastes,key_events;
    unsigned releases{};
    void keyPressEvent(QKeyEvent* event) override {
        presses.append(event->key());
        key_events.append(QJsonObject{{"key",event->key()},{"modifiers",int(event->modifiers())},{"pressed",true}});
        QTextEdit::keyPressEvent(event);
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        ++releases;
        key_events.append(QJsonObject{{"key",event->key()},{"modifiers",int(event->modifiers())},{"pressed",false}});
        QTextEdit::keyReleaseEvent(event);
    }
    void insertFromMimeData(const QMimeData* data) override {
        pastes.append(data->text());QTextEdit::insertFromMimeData(data);
    }
};
void write_json(const QString& path,const QJsonObject& object) {
    QSaveFile file(path);if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(object).toJson());file.commit();}
}
QJsonObject read_json(const QString& path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
int receiver(QApplication& app,const QString& id,const QString& status) {
    QGuiApplication::setDesktopFileName(id);
    Editor editor;editor.setWindowTitle(id);editor.resize(720,440);editor.show();editor.setFocus();
    QTimer timer;QObject::connect(&timer,&QTimer::timeout,[&]{
        const auto* clipboard = QApplication::clipboard()->mimeData();
        auto widget_state=[](QWidget* widget)->QJsonObject {
            if(!widget)return {};
            return {{"class",widget->metaObject()->className()},{"visible",widget->isVisible()},
                {"focus",widget->hasFocus()},{"title",widget->windowTitle()}};
        };
        QJsonArray windows;
        for(auto* widget:QApplication::topLevelWidgets())windows.append(widget_state(widget));
        write_json(status,{{"platform",QGuiApplication::platformName()},{"id",id},
            {"active",editor.isActiveWindow()},{"focus",editor.hasFocus()},
            {"popup",widget_state(QApplication::activePopupWidget())},
            {"modal",widget_state(QApplication::activeModalWidget())},{"windows",windows},
            {"key_events",editor.key_events},{"clipboard_formats",QJsonArray::fromStringList(clipboard ? clipboard->formats() : QStringList{})},
            {"presses",editor.presses},{"releases",int(editor.releases)},
            {"pastes",editor.pastes},{"text",editor.toPlainText()}});
        if(qEnvironmentVariableIsSet("LOGI_TEST_EVIDENCE")) {
            editor.grab().save(qEnvironmentVariable("LOGI_TEST_EVIDENCE")+"/"+id+".png");
            if(auto* popup=QApplication::activePopupWidget())
                popup->grab().save(qEnvironmentVariable("LOGI_TEST_EVIDENCE")+"/"+id+"-popup.png");
        }
    });timer.start(80);return app.exec();
}
struct Client {
    QProcess process;QString status,id;
    Client(const QString& app_id,const QString& dir):status(dir+"/"+app_id+".json"),id(app_id) {
        process.setProcessChannelMode(QProcess::ForwardedChannels);
        process.start(QCoreApplication::applicationFilePath(),{"--receiver",id,status});
        require(process.waitForStarted(),"Receiver failed to start");
    }
    ~Client(){
        if(auto evidence=qEnvironmentVariable("LOGI_TEST_EVIDENCE");!evidence.isEmpty())
            write_json(evidence+"/"+id+"-state.json",state());
        process.terminate();if(!process.waitForFinished(1000)){process.kill();process.waitForFinished();}
    }
    QJsonObject state()const{return read_json(status);}
    bool key(int key)const {for(const auto v:state()["presses"].toArray())if(v.toInt()==key)return true;return false;}
    int key_count(int key)const {int count{};for(const auto v:state()["presses"].toArray())if(v.toInt()==key)++count;return count;}
};
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        require(QGuiApplication::platformName().startsWith("wayland"),"Test receiver is not native Wayland");
        if(argc==4 && std::string_view(argv[1])=="--receiver")return receiver(app,argv[2],argv[3]);
        if(argc==3 && std::string_view(argv[1])=="--hold-shortcut") {
            Desktop owner;owner.configure_hotkeys(parse("[hotkeys]\nCtrl+Shift+F9=Ctrl+A\n"));
            write_json(argv[2],{{"ready",true}});QTimer timer;
            QObject::connect(&timer,&QTimer::timeout,[&]{owner.poll_hotkeys();});timer.start(10);
            return app.exec();
        }
        auto input=make_uinput_sink();
        // Event forwarding/hotplug is established by the isolated session runner.
        std::cout<<"Native Wayland test input device: "<<input->sysname()<<std::endl;
        if(auto ready=qEnvironmentVariable("LOGI_WAYLAND_INPUT_READY");!ready.isEmpty()) {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(!QFile::exists(ready)) {
                require(std::chrono::steady_clock::now()<end,"Nested compositor input bridge did not open production uinput device");
                app.processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        auto inject=[&](const std::vector<unsigned>& keys){
            for(auto k:keys){input->key(k,true);input->sync();}
            for(auto i=keys.rbegin();i!=keys.rend();++i){input->key(*i,false);input->sync();}
        };
        Desktop desktop(inject);
        require(desktop.available(),desktop.reason());
        QTemporaryDir temporary;require(temporary.isValid(),"Cannot create test directory");
        std::vector<std::vector<Action>> fired;
        auto pump=[&]{app.processEvents();for(auto& a:desktop.poll_hotkeys())fired.push_back(std::move(a));};
        auto until=[&](const char* label,auto condition){
            std::cout<<"Waiting for "<<label<<std::endl;
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            do {pump();if(condition()){std::cout<<"Passed "<<label<<std::endl;return;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }while(std::chrono::steady_clock::now()<end);
            std::cerr<<"Foreground:";for(const auto& id:desktop.foreground())std::cerr<<' '<<id;
            std::cerr<<"; triggers="<<fired.size()<<"; busy="<<desktop.busy()<<'\n';
            throw std::runtime_error(std::string("Native Wayland behavior timed out: ")+label);
        };
        auto observe=[&](unsigned ms){
            const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
            do {pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(std::chrono::steady_clock::now()<end);
        };
        auto identity=[&](const char* expected){const auto ids=desktop.foreground();return std::ranges::find(ids,expected)!=ids.end();};
        Client editor("org.logi.test.Editor",temporary.path());
        until("native Editor focus",[&]{return editor.state()["active"].toBool() && identity("org.logi.test.editor");});
        require(editor.state()["platform"].toString().startsWith("wayland"),"Editor used Xwayland");
        auto config=parse("[global]\n0053=Ctrl+A\n0056=Ctrl+C\n[org.logi.test.editor]\n0053=Ctrl+E\n[org.logi.test.browser]\n0053=Ctrl+B\n[hotkeys]\nCtrl+Shift+F10=Text(Triggered)\n");
        desktop.validate(config);desktop.configure_hotkeys(config);
        desktop.inject_chord(lookup(config,{0x53,Direction::tap},desktop.foreground())->front().keys);
        until("profile key received by Editor",[&]{return editor.key(Qt::Key_E);});
        Client browser("org.logi.test.Browser",temporary.path());
        until("native Browser focus",[&]{return browser.state()["active"].toBool() && identity("org.logi.test.browser");});
        require(browser.state()["platform"].toString().startsWith("wayland"),"Browser used Xwayland");
        desktop.inject_chord(lookup(config,{0x53,Direction::tap},desktop.foreground())->front().keys);
        desktop.inject_chord(lookup(config,{0x56,Direction::tap},desktop.foreground())->front().keys);
        until("profile and inherited keys received by Browser",[&]{return browser.key(Qt::Key_B)&&browser.key(Qt::Key_C);});
        desktop.inject_chord({KEY_LEFTALT,KEY_TAB});
        until("native Alt+Tab changes foreground profile",[&]{return editor.state()["active"].toBool() && identity("org.logi.test.editor");});
        auto key=[&](unsigned code,bool down){input->key(code,down);input->sync();observe(70);};
        key(KEY_LEFTCTRL,true);key(KEY_LEFTSHIFT,true);key(KEY_F10,true);key(KEY_F10,false);
        observe(150);require(fired.empty(),"Global shortcut fired while modifiers held");
        key(KEY_LEFTSHIFT,false);key(KEY_LEFTCTRL,false);
        until("global shortcut after full release",[&]{return fired.size()==1;});
        require(fired.front().front().text=="Triggered","Wrong global action");
        key(KEY_LEFTCTRL,true);key(KEY_LEFTSHIFT,true);key(KEY_F10,true);
        key(KEY_LEFTCTRL,false);key(KEY_LEFTSHIFT,false);
        require(fired.size()==1,"Global shortcut fired while main key held");
        key(KEY_F10,false);until("modifier-first release",[&]{return fired.size()==2;});
        QProcess competing;competing.setProcessChannelMode(QProcess::ForwardedChannels);
        const auto competing_ready=temporary.path()+"/competing.json";
        competing.start(QCoreApplication::applicationFilePath(),{"--hold-shortcut",competing_ready});
        require(competing.waitForStarted(),"Competing shortcut client failed to start");
        until("independent shortcut owner",[&]{return read_json(competing_ready)["ready"].toBool();});
        bool rejected=false;
        try{desktop.configure_hotkeys(parse("[hotkeys]\nCtrl+Shift+F9=Ctrl+A\n"));}
        catch(const std::exception& e){std::cout<<"Expected native shortcut conflict: "<<e.what()<<std::endl;rejected=true;}
        require(rejected,"Shortcut conflict silently stole another client's binding");
        inject({KEY_LEFTCTRL,KEY_LEFTSHIFT,KEY_F10});
        until("previous shortcut retained after rejected reload",[&]{return fired.size()==3;});
        // Abrupt death must remove that bus sender's bindings without Release().
        competing.kill();require(competing.waitForFinished(2000),"Competing client failed to exit");
        until("crashed sender's shortcut released",[&]{
            try{desktop.configure_hotkeys(parse("[hotkeys]\nCtrl+Shift+F9=Ctrl+A\n"));return true;}
            catch(const std::exception&){return false;}
        });
        desktop.configure_hotkeys(config);
        const auto previous_f10=editor.key_count(Qt::Key_F10);
        desktop.suspend_hotkeys(true);desktop.inject_chord({KEY_LEFTCTRL,KEY_LEFTSHIFT,KEY_F10});
        until("suspended shortcut reaches actual client",[&]{return editor.key_count(Qt::Key_F10)>previous_f10;});
        desktop.suspend_hotkeys(false);require(fired.size()==3,"Synthetic action recursively triggered");
        desktop.type_text("must-not-dispatch");desktop.cancel_actions();
        require(!desktop.busy(),"Cancelled native text remained pending");
        const std::string text="Wayland: \xe4\xbd\xa0\xe5\xa5\xbd; (native)\nsecond line";
        desktop.type_text(text);
        until("Unicode text reached real application",[&]{return !desktop.busy()&&editor.state()["pastes"].toArray().size()==1;});
        require(editor.state()["pastes"].toArray()[0].toString().toStdString()==text,"Native paste text corrupted");
        desktop.type_text("one");desktop.type_text("two");
        until("consecutive native pastes",[&]{return !desktop.busy()&&editor.state()["pastes"].toArray().size()==3;});
        auto pastes=editor.state()["pastes"].toArray();
        require(pastes[1].toString()=="one"&&pastes[2].toString()=="two","Consecutive clipboard payloads reordered/replaced");
        desktop.configure_hotkeys(parse("[global]\n0053=MouseBack\n"));
        inject({KEY_LEFTCTRL,KEY_LEFTSHIFT,KEY_F10});observe(250);
        require(fired.size()==3,"Unregistered hotkey still fired");
        require(editor.state()["releases"].toInt()>0,"No real Wayland key releases");
        auto evidence=qEnvironmentVariable("LOGI_TEST_EVIDENCE");
        if(!evidence.isEmpty())write_json(evidence+"/wayland-receivers.json",{{"editor",editor.state()},{"browser",browser.state()},
            {"native_wayland",true},{"production_uinput",true},{"global_triggers",int(fired.size())},
            {"physical_mouse",false}});
        std::cout<<"Native Wayland passed: app_id profiles, inherited mappings, focus switching, release-safe global shortcuts, suspension, Unicode and ordered text, unregister; physical HID untested\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
