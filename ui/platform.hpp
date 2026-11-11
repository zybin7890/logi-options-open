#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QProcess;

namespace loo_ui::platform {
QString configPath();
QString cliPath();
QStringList defaultConfigCandidates();
QString fallbackConfig();
QStringList startArguments(const QString& config, bool takeOverOfficial = true);
// Only pass a shortcut chord, never arbitrary Text/Open payloads or whole INIs.
QString actionShortcut(const QString& portableChord);
QString desktopNavigation(bool right);
QString profileName(const QString& name);
bool canSetThumbInvert(bool backendRunning);
bool stopConfirmsRestoration();
void prepareProcess(QProcess& process);

// Device status JSON already uses the same schema on both platforms. These
// command outcomes intentionally retain the different confirmation guarantees.
enum class ReloadResult { Rejected, Notified, Applied };
ReloadResult reloadResult(bool success, const QJsonObject& reply);
enum class PlaybackResult { Failed, Attempted, Queued, Completed };
PlaybackResult playbackResult(bool success, const QJsonObject& reply);
}
