#include "clipboard.hpp"
#include "../mapping.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>
#include <array>
#include <csignal>
#include <iostream>
#include <linux/input.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

using ShortcutMap=QMap<QString,QString>;
Q_DECLARE_METATYPE(ShortcutMap)
namespace {
constexpr auto service="org.logi.OptionsOpen.Desktop1";
constexpr auto object_path="/org/logi/OptionsOpen/Desktop1";
constexpr auto callback_path="/org/logi/OptionsOpen/KWinCallback1";
constexpr auto callback_interface="org.logi.OptionsOpen.KWinCallback1";
volatile std::sig_atomic_t stopped{};
void signalStop(int){stopped=1;}
bool bit(const unsigned long* values,unsigned code){constexpr unsigned width=sizeof(unsigned long)*8;return values[code/width]&(1ul<<(code%width));}

// Reading EVIOCGKEY does not consume or record keyboard input. Unlike the KF5
// globalShortcutReleased signal, this observes the main key AND all modifiers.
class KeyboardState {
    std::vector<int> descriptors;
    bool complete{};
    QString failure;
    static bool sysKeyboard(const QString& event) {
        QFile file("/sys/class/input/"+event+"/device/capabilities/key");
        if(!file.open(QIODevice::ReadOnly))return true;
        const auto words=QString::fromLatin1(file.readAll()).simplified().split(' ');
        auto has=[&](unsigned code){const auto index=words.size()-1-code/(sizeof(unsigned long)*8);return index>=0 && index<words.size() && (words[index].toULongLong(nullptr,16)&(1ull<<(code%(sizeof(unsigned long)*8))));};
        return has(KEY_A) || has(KEY_LEFTCTRL) || has(KEY_F1);
    }
public:
    ~KeyboardState(){for(auto fd:descriptors)::close(fd);}
    void refresh() {
        for(auto fd:descriptors)::close(fd);descriptors.clear();complete=true;failure.clear();
        for(const auto& event:QDir("/dev/input").entryList({"event*"},QDir::System|QDir::Files)) {
            if(!sysKeyboard(event))continue;
            const auto path="/dev/input/"+event;
            const int fd=::open(path.toLocal8Bit().constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(fd<0){complete=false;failure="Read access required for keyboard state: "+path;continue;}
            std::array<unsigned long,(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))> keys{};
            if(ioctl(fd,EVIOCGKEY(sizeof(keys)),keys.data())<0){::close(fd);complete=false;failure="Cannot query keyboard state: "+path;continue;}
            descriptors.push_back(fd);
        }
        if(descriptors.empty()){complete=false;if(failure.isEmpty())failure="No readable keyboard event device; shortcuts require input/logind session access";}
    }
    bool available()const{return complete && !descriptors.empty();}
    QString reason()const{return failure;}
    bool released(unsigned key) {
        if(!available())return false;
        for(auto fd:descriptors) {
            std::array<unsigned long,(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))> keys{};
            if(ioctl(fd,EVIOCGKEY(sizeof(keys)),keys.data())<0){complete=false;return false;}
            for(auto modifier:{KEY_LEFTCTRL,KEY_RIGHTCTRL,KEY_LEFTSHIFT,KEY_RIGHTSHIFT,KEY_LEFTALT,KEY_RIGHTALT,KEY_LEFTMETA,KEY_RIGHTMETA})
                if(bit(keys.data(),modifier))return false;
            if(key<=KEY_MAX && bit(keys.data(),key))return false;
        }
        return true;
    }
};
struct Shortcut {QString token,action,accelerator;int combined{};unsigned evdev{};};
Shortcut decode(const QString& token,QString accelerator) {
    if(token.isEmpty() || token.size()>128)throw std::runtime_error("Shortcut token must contain 1..128 characters");
    const QString original=accelerator;
    QString qt;
    while(accelerator.startsWith('<')) {
        const auto end=accelerator.indexOf('>');if(end<0)throw std::runtime_error("Invalid GTK accelerator");
        const auto name=accelerator.mid(1,end-1).toLower();
        if(name=="control" || name=="ctrl" || name=="primary")qt+="Ctrl+";
        else if(name=="shift")qt+="Shift+";
        else if(name=="alt")qt+="Alt+";
        else if(name=="super" || name=="meta")qt+="Meta+";
        else throw std::runtime_error("Unsupported accelerator modifier");
        accelerator.remove(0,end+1);
    }
    QString evdev=accelerator;
    const QMap<QString,QString> names{{"Return","Enter"},{"Escape","Esc"},{"space","Space"},{"Page_Up","PageUp"},{"Page_Down","PageDown"},
        {"XF86AudioRaiseVolume","VolumeUp"},{"XF86AudioLowerVolume","VolumeDown"},{"XF86AudioMute","Mute"},{"XF86AudioPlay","PlayPause"},
        {"XF86AudioNext","NextTrack"},{"XF86AudioPrev","PreviousTrack"},{"Print","PrintScreen"}};
    evdev=names.value(evdev,evdev);if(evdev.size()==1)evdev=evdev.toUpper();
    const auto code=loo_linux::key(evdev.toStdString());
    if(code>=BTN_MISC)throw std::runtime_error("Global shortcuts require keyboard keys");
    const QMap<QString,QString> qtNames{{"Page_Up","PgUp"},{"Page_Down","PgDown"},{"XF86AudioRaiseVolume","Volume Up"},{"XF86AudioLowerVolume","Volume Down"},
        {"XF86AudioMute","Volume Mute"},{"XF86AudioPlay","Media Play"},{"XF86AudioNext","Media Next"},{"XF86AudioPrev","Media Previous"}};
    qt+=qtNames.value(accelerator,accelerator);
    const auto sequence=QKeySequence::fromString(qt,QKeySequence::PortableText);
    if(sequence.count()!=1 || sequence[0].key()==Qt::Key_unknown || sequence[0].key()==0)throw std::runtime_error("Invalid single-chord accelerator");
    return {token,QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(),QCryptographicHash::Sha256).toHex()),original,sequence[0].toCombined(),code};
}
QDBusMessage call(const char* service,const char* path,const char* interface,const QString& method,const QVariantList& args={}) {
    auto message=QDBusMessage::createMethodCall(QString::fromLatin1(service),QString::fromLatin1(path),QString::fromLatin1(interface),method);message.setArguments(args);
    auto reply=QDBusConnection::sessionBus().call(message,QDBus::Block,3000);
    if(reply.type()==QDBusMessage::ErrorMessage)throw std::runtime_error((method+": "+reply.errorMessage()).toStdString());
    return reply;
}
QDBusMessage accel(const QString& method,const QVariantList& args={}){return call("org.kde.kglobalaccel","/kglobalaccel","org.kde.KGlobalAccel",method,args);}
}

class ForegroundCallback final : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.logi.OptionsOpen.KWinCallback1")
public slots:
    void UpdateForeground(const QString& app,const QString& instance,const QString& desktop,const QString& process) {
        const auto owner=QDBusConnection::sessionBus().interface()->serviceOwner("org.kde.KWin");
        if(!calledFromDBus() || !owner.isValid() || owner.value()!=message().service())return;
        QStringList identities;
        auto add=[&](QString value){value=value.toLower();if(!value.isEmpty() && !identities.contains(value))identities.append(value);};
        add(app);add(instance);add(desktop.endsWith(".desktop")?desktop.chopped(8):desktop);
        bool ok{};const auto pid=process.toULongLong(&ok);
        if(ok && pid)add(QFileInfo(QFileInfo("/proc/"+QString::number(pid)+"/exe").symLinkTarget()).fileName());
        emit updated(identities);
    }
signals:void updated(const QStringList& identities);
};

class Provider final : public QObject,protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.logi.OptionsOpen.Desktop1")
    struct Client {QString component;QMap<QString,Shortcut> shortcuts;bool suspended{};QSet<QString> pending;};
    QMap<QString,Client> clients;
    QString instance=QUuid::createUuid().toString(QUuid::Id128);
    QString scriptName="logi-options-open-"+instance;
    QTemporaryDir scriptDirectory;
    QStringList foreground;
    bool foregroundReady{},scriptLoaded{};
    KeyboardState keyboard;
    WaylandClipboard clipboard;
    ForegroundCallback callback;
    QDBusServiceWatcher watcher;
    QTimer refreshTimer,releaseTimer,signalTimer;
    QHash<quint32,QString> clipboardOwners;
    QString activeClipboardOwner;
    QString error;
    QString sender()const {return calledFromDBus()?message().service():QString{};}
    Client& client(const QString& owner) {
        if(owner.isEmpty())throw std::runtime_error("D-Bus caller identity is required");
        if(!clients.contains(owner)) {
            Client entry;entry.component="logi-options-open-"+instance+"-"+QString::fromLatin1(QCryptographicHash::hash(owner.toUtf8(),QCryptographicHash::Sha256).toHex().left(12));
            clients.insert(owner,entry);watcher.addWatchedService(owner);
        }
        return clients[owner];
    }
    QStringList actionId(const Client& client,const Shortcut& shortcut)const{return {client.component,shortcut.action,"Logi Options Open",shortcut.token};}
    void remove(Client& entry) {
        for(const auto& shortcut:entry.shortcuts)accel("unregister",{entry.component,shortcut.action});
        entry.shortcuts.clear();entry.pending.clear();
    }
    void install(Client& entry,const QMap<QString,Shortcut>& desired) {
        for(const auto& shortcut:desired) {
            const auto id=actionId(entry,shortcut);
            accel("doRegister",{id});
            entry.shortcuts.insert(shortcut.token,shortcut);
            const auto reply=accel("setShortcut",{id,QVariant::fromValue(QList<int>{shortcut.combined}),quint32(6)});
            const auto actual=qdbus_cast<QList<int>>(reply.arguments().value(0));
            if(actual!=QList<int>{shortcut.combined})throw std::runtime_error(("Shortcut conflict: "+shortcut.accelerator).toStdString());
            if(entry.suspended)accel("setInactive",{id});
        }
    }
    void report(const std::exception& e){sendErrorReply("org.logi.OptionsOpen.Error",QString::fromUtf8(e.what()));}
    void emitTo(const QString& destination,const char* signal,const QVariantList& args) {
        auto event=QDBusMessage::createTargetedSignal(destination,object_path,service,signal);event.setArguments(args);QDBusConnection::sessionBus().send(event);
    }
    void releaseOwner(const QString& owner) {
        if(activeClipboardOwner==owner){clipboard.clear();activeClipboardOwner.clear();}
        if(auto it=clients.find(owner);it!=clients.end()) {
            try{remove(it.value());}catch(const std::exception& e){qWarning("Cleanup: %s",e.what());}
            clients.erase(it);watcher.removeWatchedService(owner);
        }
        for(auto it=clipboardOwners.begin();it!=clipboardOwners.end();)if(it.value()==owner)it=clipboardOwners.erase(it);else ++it;
    }
    void loadScript() {
        QFile script(scriptDirectory.filePath("foreground.js"));
        if(!script.open(QIODevice::WriteOnly))throw std::runtime_error("Cannot create temporary KWin script");
        script.write(R"JS((function () {
    function current() { return workspace.activeWindow !== undefined ? workspace.activeWindow : workspace.activeClient; }
    function update(window) {
        var w = window === undefined ? current() : window;
        callDBus("org.logi.OptionsOpen.Desktop1", "/org/logi/OptionsOpen/KWinCallback1", "org.logi.OptionsOpen.KWinCallback1", "UpdateForeground",
                 w ? String(w.resourceClass || "") : "", w ? String(w.resourceName || "") : "",
                 w ? String(w.desktopFileName || "") : "", w ? String(w.pid || "") : "");
    }
    var activated = workspace.windowActivated !== undefined ? workspace.windowActivated : workspace.clientActivated;
    activated.connect(update); update(current());
})();
)JS");script.close();
        const auto reply=call("org.kde.KWin","/Scripting","org.kde.kwin.Scripting","loadScript",{script.fileName(),scriptName});
        const auto id=reply.arguments().value(0).toInt();if(id<0)throw std::runtime_error("KWin refused the foreground script");
        scriptLoaded=true;
        const auto path="/"+QString::number(id);
        QDBusInterface legacy("org.kde.KWin",path,"org.kde.kwin.Script",QDBusConnection::sessionBus());
        const auto actualPath=legacy.isValid()?path:"/Scripting/Script"+QString::number(id);
        const auto utf8=actualPath.toUtf8();call("org.kde.KWin",utf8.constData(),"org.kde.kwin.Script","run");
    }
public:
    Provider():clipboard(this),watcher(this) {
        watcher.setConnection(QDBusConnection::sessionBus());watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
        connect(&watcher,&QDBusServiceWatcher::serviceUnregistered,this,[this](const QString& owner){releaseOwner(owner);});
        connect(&callback,&ForegroundCallback::updated,this,[this](const QStringList& ids){foregroundReady=true;if(foreground!=ids){foreground=ids;emit ForegroundChanged(ids);}});
        connect(&clipboard,&WaylandClipboard::consumed,this,[this](quint32 serial){if(auto it=clipboardOwners.find(serial);it!=clipboardOwners.end()){emitTo(it.value(),"ClipboardConsumed",{serial});clipboardOwners.erase(it);}});
        auto bus=QDBusConnection::sessionBus();
        if(!bus.registerObject(callback_path,&callback,QDBusConnection::ExportAllSlots))throw std::runtime_error("Cannot register KWin callback object");
        if(!bus.connect("org.kde.kglobalaccel",QString{},"org.kde.kglobalaccel.Component","globalShortcutPressed",this,SLOT(shortcutPressed(QString,QString,qlonglong))))
            throw std::runtime_error("Cannot subscribe to KGlobalAccel");
        keyboard.refresh();refreshTimer.setInterval(500);
        connect(&refreshTimer,&QTimer::timeout,this,[this]{keyboard.refresh();});refreshTimer.start();
        releaseTimer.setInterval(5);
        connect(&releaseTimer,&QTimer::timeout,this,[this]{
            bool pending=false;
            for(auto it=clients.begin();it!=clients.end();++it) {
                auto& entry=it.value();
                for(const auto& token:QSet<QString>(entry.pending)) {
                    const auto shortcut=entry.shortcuts.find(token);
                    if(entry.suspended || shortcut==entry.shortcuts.end()){entry.pending.remove(token);continue;}
                    if(keyboard.released(shortcut->evdev)){entry.pending.remove(token);emitTo(it.key(),"ShortcutReleased",{token});}
                }
                pending|=!entry.pending.isEmpty();
            }
            if(!pending)releaseTimer.stop();
        });
        signalTimer.setInterval(100);connect(&signalTimer,&QTimer::timeout,this,[]{if(stopped)QCoreApplication::quit();});signalTimer.start();
        loadScript();
    }
    ~Provider()override {
        for(const auto& owner:clients.keys())releaseOwner(owner);
        if(scriptLoaded)try{call("org.kde.KWin","/Scripting","org.kde.kwin.Scripting","unloadScript",{scriptName});}catch(...){}
        QDBusConnection::sessionBus().unregisterObject(callback_path);
    }
public slots:
    QVariantMap GetCapabilities() {
        keyboard.refresh();
        const bool global=QDBusConnection::sessionBus().interface()->isServiceRegistered("org.kde.kglobalaccel").value();
        return {{"protocol",quint32(1)},{"desktop","kde"},{"native_wayland",true},{"foreground",foregroundReady},
            {"shortcuts",foregroundReady && global && keyboard.available()},{"clipboard",clipboard.available()},{"clipboard_ack",clipboard.available()},
            {"shortcut_reason",keyboard.reason()},{"clipboard_reason",clipboard.reason()}};
    }
    QStringList GetForeground(){return foreground;}
    bool RegisterShortcuts(const ShortcutMap& desired) {
        try {
            if(desired.size()>64)throw std::runtime_error("At most 64 global shortcuts are supported");
            keyboard.refresh();if(!desired.isEmpty() && !keyboard.available())throw std::runtime_error(keyboard.reason().toStdString());
            auto& entry=client(sender());QMap<QString,Shortcut> parsed;QSet<int> keys;
            for(auto it=desired.begin();it!=desired.end();++it) {
                const auto shortcut=decode(it.key(),it.value());
                if(keys.contains(shortcut.combined))throw std::runtime_error("Duplicate shortcut chord");keys.insert(shortcut.combined);
                bool ours=false;for(const auto& old:entry.shortcuts)ours|=old.combined==shortcut.combined;
                if(!ours && !accel("isGlobalShortcutAvailable",{shortcut.combined,QString{}}).arguments().value(0).toBool())
                    throw std::runtime_error(("Shortcut already used by another application: "+shortcut.accelerator).toStdString());
                parsed.insert(it.key(),shortcut);
            }
            const auto previous=entry.shortcuts;remove(entry);
            try{install(entry,parsed);}catch(const std::exception& failure){const auto message=std::string(failure.what());remove(entry);try{install(entry,previous);}catch(const std::exception& restore){throw std::runtime_error(message+"; rollback failed: "+restore.what());}throw std::runtime_error(message);}
            return true;
        }catch(const std::exception& e){report(e);return false;}
    }
    void UnregisterShortcuts(){try{if(clients.contains(sender()))remove(clients[sender()]);}catch(const std::exception& e){report(e);}}
    void SetSuspended(bool suspended) {
        try {
            auto& entry=client(sender());if(entry.suspended==suspended)return;
            if(suspended){for(const auto& shortcut:entry.shortcuts)accel("setInactive",{actionId(entry,shortcut)});entry.suspended=true;entry.pending.clear();}
            else {
                for(const auto& shortcut:entry.shortcuts) {
                    const auto reply=accel("setShortcut",{actionId(entry,shortcut),QVariant::fromValue(QList<int>{shortcut.combined}),quint32(6)});
                    if(qdbus_cast<QList<int>>(reply.arguments().value(0))!=QList<int>{shortcut.combined})throw std::runtime_error("A shortcut was claimed while suspended");
                }
                entry.suspended=false;
            }
        }catch(const std::exception& e){report(e);}
    }
    quint32 SetClipboard(const QString& text) {
        try{client(sender());const auto serial=clipboard.setText(text);clipboardOwners.insert(serial,sender());activeClipboardOwner=sender();return serial;}
        catch(const std::exception& e){report(e);return 0;}
    }
    void Release(){releaseOwner(sender());}
private slots:
    void shortcutPressed(const QString& component,const QString& action,qlonglong) {
        for(auto& entry:clients)if(entry.component==component && !entry.suspended)
            for(const auto& shortcut:entry.shortcuts)if(shortcut.action==action){entry.pending.insert(shortcut.token);releaseTimer.start();return;}
    }
signals:
    void ForegroundChanged(const QStringList& identities);
    void ShortcutReleased(const QString& token);
    void ClipboardConsumed(quint32 serial);
};

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("logi-kde-provider");
    std::signal(SIGTERM,signalStop);std::signal(SIGINT,signalStop);std::signal(SIGPIPE,SIG_IGN);
    qDBusRegisterMetaType<ShortcutMap>();qDBusRegisterMetaType<QList<int>>();
    try {
        if(qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland" || qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
            throw std::runtime_error("The KDE provider requires a native Wayland session");
        auto bus=QDBusConnection::sessionBus();
        if(!bus.isConnected() || !bus.registerService(service))throw std::runtime_error("D-Bus provider service unavailable or already owned");
        Provider provider;
        if(!bus.registerObject(object_path,&provider,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals))throw std::runtime_error("Cannot export desktop provider");
        std::cout<<"KDE native Wayland provider ready; foreground=KWin, clipboard=data-control, shortcut release=evdev bitmap\n"<<std::flush;
        const auto result=app.exec();bus.unregisterObject(object_path);return result;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#include "provider.moc"
