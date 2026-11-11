#include "platform.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace loo_ui::platform {
QString configPath() {
    const auto override=qEnvironmentVariable("LOGI_OPTIONS_OPEN_CONFIG");
    if(!override.isEmpty())return QFileInfo(override).absoluteFilePath();
#ifdef Q_OS_WIN
    // Preserve existing portable Windows installations and their reload-event
    // path. Do not silently copy a live configuration to another directory.
    const auto portable=QCoreApplication::applicationDirPath()+QStringLiteral("/mappings.ini");
    if(QFileInfo::exists(portable))return portable;
    auto base=qEnvironmentVariable("LOCALAPPDATA");
    if(base.isEmpty())base=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#else
    const auto base=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
    return QDir(base).filePath(QStringLiteral("logi-options-open/mappings.ini"));
}
QString cliPath() {
#ifdef Q_OS_WIN
    const auto name=QStringLiteral("logi-cli.exe");
#else
    const auto name=QStringLiteral("logi-cli");
#endif
    const auto sibling=QDir(QCoreApplication::applicationDirPath()).filePath(name);
    return QFileInfo::exists(sibling)?sibling:QStandardPaths::findExecutable(name);
}
QStringList defaultConfigCandidates() {
#ifdef Q_OS_WIN
    const auto name=QStringLiteral("default-windows.ini");
#else
    const auto name=QStringLiteral("default-linux.ini");
#endif
    const auto directory=QCoreApplication::applicationDirPath();
    return {directory+'/'+name,directory+QStringLiteral("/../share/logi-options-open/")+name};
}
QString fallbackConfig() {
    QString text=QStringLiteral(
        "# Third-party experimental mouse mappings.\n"
        "[global]\n0052 = MouseMiddle\n0053 = MouseBack\n0056 = MouseForward\n"
        "00C4 = WheelMode\n");
#ifdef Q_OS_WIN
    text+=QStringLiteral("00C3 = Win+Tab\n00C3.left = Win+Ctrl+Left\n00C3.right = Win+Ctrl+Right\n00C3.up = Win\n00C3.down = Win+D\n");
#else
    text+=QStringLiteral("00C3 = Super+Tab\n00C3.left = Ctrl+Alt+Left\n00C3.right = Ctrl+Alt+Right\n00C3.up = Super\n00C3.down = Super+D\n");
#endif
    return text+QStringLiteral("thumb.left = VolumeDown\nthumb.right = VolumeUp\nthumb.speed = 100\n");
}
QStringList startArguments(const QString& config,bool takeOverOfficial) {
#ifdef Q_OS_WIN
    return {takeOverOfficial?QStringLiteral("run-preview"):QStringLiteral("run"),config};
#else
    (void)takeOverOfficial;return {QStringLiteral("run"),config};
#endif
}
QString actionShortcut(const QString& portableChord) {
    auto tokens=portableChord.split('+');
    for(auto& token:tokens){
        token=token.trimmed();
        if(token!=QStringLiteral("Meta") && token!=QStringLiteral("Super") && token!=QStringLiteral("Win"))continue;
#ifdef Q_OS_WIN
        token=QStringLiteral("Win");
#else
        token=QStringLiteral("Super");
#endif
    }
    return tokens.join('+');
}
QString profileName(const QString& name) {
    auto normalized=name.trimmed().toLower();
#ifdef Q_OS_WIN
    if(!normalized.isEmpty() && normalized!=QStringLiteral("global") &&
       !normalized.contains('/') && !normalized.contains('\\') && !normalized.contains('.'))
        normalized+=QStringLiteral(".exe");
#endif
    return normalized;
}
QString desktopNavigation(bool right) {
#ifdef Q_OS_WIN
    return right?QStringLiteral("Win+Ctrl+Right"):QStringLiteral("Win+Ctrl+Left");
#else
    return right?QStringLiteral("Ctrl+Alt+Right"):QStringLiteral("Ctrl+Alt+Left");
#endif
}
bool canSetThumbInvert(bool backendRunning) {
#ifdef Q_OS_WIN
    return !backendRunning; // Windows CLI explicitly rejects live thumb-invert.
#else
    (void)backendRunning;return true; // Linux serializes restore/set/reconfigure.
#endif
}
bool stopConfirmsRestoration() {
#ifdef Q_OS_WIN
    return false; // Named event only; query agent-status to observe actual exit.
#else
    return true; // IPC response follows successful hardware restoration.
#endif
}
void prepareProcess(QProcess& process) {
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments){
        arguments->flags|=0x08000000; // CREATE_NO_WINDOW; CLI has no interactive UI.
    });
#else
    (void)process;
#endif
}
ReloadResult reloadResult(bool success,const QJsonObject& reply) {
    if(!success)return ReloadResult::Rejected;
    if(reply.value(QStringLiteral("reloaded")).toBool())return ReloadResult::Applied;
#ifdef Q_OS_WIN
    if(reply.value(QStringLiteral("reload_requested")).toBool())return ReloadResult::Notified;
#endif
    return ReloadResult::Rejected;
}
PlaybackResult playbackResult(bool success,const QJsonObject& reply) {
    if(!success)return PlaybackResult::Failed;
    if(reply.value(QStringLiteral("accepted")).toBool())
        return reply.value(QStringLiteral("completed")).toBool()?PlaybackResult::Completed:PlaybackResult::Queued;
#ifdef Q_OS_WIN
    // Existing Windows playback runs synchronously, but execute() only logs
    // rejected SendInput calls. Exit 0 does not prove target input delivery.
    if(reply.isEmpty())return PlaybackResult::Attempted;
#endif
    return PlaybackResult::Failed;
}
}
