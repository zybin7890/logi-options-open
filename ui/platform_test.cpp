#include "platform.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <cstdio>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    using namespace loo_ui::platform;
    auto require=[](bool value,const char* why){if(!value)std::fprintf(stderr,"Platform adapter: %s\n",why);return value;};
    const auto override=qgetenv("LOGI_OPTIONS_OPEN_CONFIG");
    qputenv("LOGI_OPTIONS_OPEN_CONFIG","platform-test/mappings.ini");
    if(!require(configPath()==QFileInfo(QStringLiteral("platform-test/mappings.ini")).absoluteFilePath(),"explicit config path lost"))return 1;
    if(override.isNull())qunsetenv("LOGI_OPTIONS_OPEN_CONFIG");else qputenv("LOGI_OPTIONS_OPEN_CONFIG",override);
    const QJsonObject notified{{QStringLiteral("reload_requested"),true}};
    const QJsonObject applied{{QStringLiteral("reload_requested"),true},{QStringLiteral("reloaded"),true}};
    const QJsonObject queued{{QStringLiteral("accepted"),true},{QStringLiteral("action_id"),17}};
    const QJsonObject completed{{QStringLiteral("accepted"),true},{QStringLiteral("completed"),true}};
    if(!require(reloadResult(false,applied)==ReloadResult::Rejected,"failed reload became success") ||
       !require(reloadResult(true,applied)==ReloadResult::Applied,"applied acknowledgement lost") ||
       !require(playbackResult(false,completed)==PlaybackResult::Failed,"failed playback became success") ||
       !require(playbackResult(true,queued)==PlaybackResult::Queued,"queue receipt became execution") ||
       !require(playbackResult(true,completed)==PlaybackResult::Completed,"execution acknowledgement lost"))return 2;
#ifdef Q_OS_WIN
    if(!require(startArguments("config").front()=="run-preview","Windows takeover command") ||
       !require(startArguments("config",false).front()=="run","Windows standalone command") ||
       !require(actionShortcut("Meta+Ctrl+Left")=="Win+Ctrl+Left","Windows modifier") ||
       !require(profileName("Firefox")=="firefox.exe","Windows executable profile") ||
       !require(!canSetThumbInvert(true)&&canSetThumbInvert(false),"Windows live thumb restriction") ||
       !require(!stopConfirmsRestoration(),"Windows stop notification inflated") ||
       !require(reloadResult(true,notified)==ReloadResult::Notified,"Windows notification semantics") ||
       !require(playbackResult(true,{})==PlaybackResult::Attempted,"Windows unconfirmed playback inflated") ||
       !require(fallbackConfig().contains("Win+Ctrl+Left")&&!fallbackConfig().contains("Super"),"Windows defaults"))return 3;
#else
    if(!require(startArguments("config").front()=="run","Linux daemon command") ||
       !require(actionShortcut("Meta+Tab")=="Super+Tab","Linux modifier") ||
       !require(profileName("Firefox")=="firefox","Linux application profile") ||
       !require(canSetThumbInvert(true),"Linux serialized thumb settings") ||
       !require(stopConfirmsRestoration(),"Linux stop confirmation lost") ||
       !require(reloadResult(true,notified)==ReloadResult::Rejected,"Linux incomplete reload accepted") ||
       !require(playbackResult(true,{})==PlaybackResult::Failed,"Linux missing playback reply accepted") ||
       !require(fallbackConfig().contains("Super+Tab"),"Linux defaults"))return 3;
#endif
    QProcess process;prepareProcess(process); // Does not launch a process or access HID.
    std::puts("Shared UI platform adapter: paths, commands and confirmation semantics passed.");
    return 0;
}
