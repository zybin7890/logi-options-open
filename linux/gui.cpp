#include "../first_run.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr auto title = "Logi Options Open";
constexpr auto fallback_config =
    "# Global Linux mappings. Application profiles are not supported yet.\n"
    "[global]\n0052 = MouseMiddle\n0053 = MouseBack\n0056 = MouseForward\n"
    "00C4 = WheelMode\n00C3 = Super+Tab\n00C3.left = Ctrl+Alt+Left\n"
    "00C3.right = Ctrl+Alt+Right\n00C3.up = Super\n00C3.down = Super+D\n"
    "thumb.left = VolumeDown\nthumb.right = VolumeUp\nthumb.speed = 100\n";

QString configPath() {
    const auto override = qEnvironmentVariable("LOGI_OPTIONS_OPEN_CONFIG");
    if (!override.isEmpty()) return QFileInfo(override).absoluteFilePath();
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/logi-options-open/mappings.ini");
}

QString readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

QMap<QString, QString> globalBindings(const QString& text) {
    QMap<QString, QString> result;
    bool global = false;
    for (const auto& line : text.split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith('[')) {
            global = trimmed.compare(QStringLiteral("[global]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!global || trimmed.startsWith('#') || trimmed.startsWith(';')) continue;
        const auto equal = line.indexOf('=');
        if (equal > 0) result.insert(line.left(equal).trimmed().toLower(), line.mid(equal + 1).trimmed());
    }
    return result;
}

QString replaceGlobalBinding(const QString& source, const QString& key, const QString& value) {
    auto lines = source.split('\n');
    int begin = -1, end = static_cast<int>(lines.size());
    for (int i = 0; i < lines.size(); ++i) {
        const auto trimmed = lines[i].trimmed();
        if (!trimmed.startsWith('[')) continue;
        if (begin >= 0) { end = i; break; }
        if (trimmed.compare(QStringLiteral("[global]"), Qt::CaseInsensitive) == 0) begin = i;
    }
    const auto replacement = key + QStringLiteral(" = ") + value;
    if (begin < 0) {
        if (!lines.isEmpty() && lines.back().isEmpty()) lines.removeLast();
        lines << QStringLiteral("[global]") << replacement << QString{};
    } else {
        for (int i = begin + 1; i < end; ++i) {
            const auto equal = lines[i].indexOf('=');
            if (equal > 0 && lines[i].left(equal).trimmed().compare(key, Qt::CaseInsensitive) == 0) {
                lines[i] = replacement;
                return lines.join('\n');
            }
        }
        lines.insert(end, replacement);
    }
    return lines.join('\n');
}

bool writeFile(const QString& path, const QByteArray& data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(data) == data.size() && file.commit();
}

struct Task {
    QString key;
    QStringList arguments;
    std::function<void(bool, const QString&, const QString&)> done;
};

class Window : public QMainWindow {
public:
    explicit Window(QString config, bool runtime = true)
        : config_(std::move(config)), runtime_(runtime) {
        setWindowTitle(QString::fromLatin1(title));
        resize(960, 740);
        setMinimumSize(680, 540);
        cli_ = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logi-cli"));
        if (!QFileInfo::exists(cli_)) cli_ = QStandardPaths::findExecutable(QStringLiteral("logi-cli"));
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(28, 22, 28, 22);
        auto* header = new QHBoxLayout;
        auto* heading = new QLabel(QString::fromLatin1(title), central);
        heading->setStyleSheet(QStringLiteral("font-size:26px;font-weight:600"));
        header->addWidget(heading);
        header->addStretch();
        auto* refresh = new QPushButton(QStringLiteral("Refresh"), central);
        header->addWidget(refresh);
        layout->addLayout(header);
        device_ = new QLabel(QStringLiteral("No device read yet"), central);
        layout->addWidget(device_);
        auto* note = new QLabel(QStringLiteral("Third-party experimental software · Linux preview"), central);
        note->setStyleSheet(QStringLiteral("color:#b6bebd"));
        layout->addWidget(note);
        auto* tabs = new QTabWidget(central);
        layout->addWidget(tabs, 1);
        tabs->addTab(settingsPage(), QStringLiteral("Point & scroll"));
        tabs->addTab(mappingsPage(), QStringLiteral("Buttons & gestures"));
        tabs->addTab(advancedPage(), QStringLiteral("Configuration"));
        auto* footer = new QHBoxLayout;
        agent_ = new QLabel(QStringLiteral("Backend status not read"), central);
        footer->addWidget(agent_, 1);
        start_ = new QPushButton(QStringLiteral("Start backend"), central);
        stop_ = new QPushButton(QStringLiteral("Stop backend"), central);
        footer->addWidget(start_);
        footer->addWidget(stop_);
        layout->addLayout(footer);
        status_ = new QLabel(central);
        status_->setWordWrap(true);
        status_->setMinimumHeight(44);
        layout->addWidget(status_);
        setCentralWidget(central);
        setStyleSheet(QStringLiteral(
            "QMainWindow,QWidget{background:#1b1d1d;color:#eeeeee;font-size:14px;}"
            "QTabWidget::pane,QGroupBox{border:1px solid #3b4040;border-radius:8px;}"
            "QGroupBox{margin-top:14px;padding:18px;}QGroupBox::title{subcontrol-origin:margin;left:12px;}"
            "QPushButton,QComboBox,QSpinBox{background:#292d2c;border:1px solid #4c5553;border-radius:5px;padding:8px;}"
            "QPushButton:hover{border-color:#00c7b5;}QPushButton:disabled{color:#737c7a;}"
            "QComboBox QAbstractItemView{selection-background-color:#285b55;}"
            "QPlainTextEdit{background:#111414;border:1px solid #3b4040;font-family:monospace;}"
            "QTabBar::tab{padding:12px 20px;}QTabBar::tab:selected{color:#00d4c1;}"
            "QSlider::groove:horizontal{height:5px;background:#454d4b;}"
            "QSlider::handle:horizontal{background:#00c7b5;width:16px;margin:-6px 0;border-radius:8px;}"));

        process_ = new QProcess(this);
        processTimer_.setSingleShot(true);
        processTimer_.setInterval(25000);
        connect(&processTimer_, &QTimer::timeout, this, [this] {
            timedOut_ = true;
            process_->kill();
        });
        connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
            finishTask(code == 0 && exit == QProcess::NormalExit && !timedOut_);
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finishTask(false);
        });
        saveTimer_.setSingleShot(true);
        saveTimer_.setInterval(350);
        connect(&saveTimer_, &QTimer::timeout, this, [this] { validateDraft(false); });
        connect(refresh, &QPushButton::clicked, this, [this] { refreshAll(); });
        connect(start_, &QPushButton::clicked, this, [this] {
            saveTimer_.stop();
            validateDraft(true);
        });
        connect(stop_, &QPushButton::clicked, this, [this] {
            submit({QStringLiteral("stop"), {QStringLiteral("stop")}, [this](bool ok, const QString&, const QString& error) {
                message(ok ? QStringLiteral("Stop requested. The backend restores normal mouse reporting before exit.") : error, !ok);
                QTimer::singleShot(700, this, [this] { refreshAgent(); });
            }});
        });

        auto data = readFile(config_);
        if (data.isEmpty() && !QFileInfo::exists(config_)) {
            for (const auto& path : {QCoreApplication::applicationDirPath() + QStringLiteral("/default-linux.ini"),
                                    QCoreApplication::applicationDirPath() + QStringLiteral("/../share/logi-options-open/default-linux.ini")}) {
                data = readFile(path);
                if (!data.isEmpty()) break;
            }
            if (data.isEmpty()) data = QString::fromLatin1(fallback_config);
            if (runtime_) {
                if (!QDir().mkpath(QFileInfo(config_).absolutePath()) || !writeFile(config_, data.toUtf8()))
                    message(QStringLiteral("Cannot create configuration: ") + config_, true);
            }
        }
        draft_ = data;
        setEditors(data);
        if (runtime_) QTimer::singleShot(0, this, [this] { refreshAll(); });
    }

    void applyStatus(const QJsonObject& object) {
        loading_ = true;
        device_->setText(object.value(QStringLiteral("device")).toString(QStringLiteral("HID++ mouse"))
            + QStringLiteral("  ·  Battery ")
            + (object.value(QStringLiteral("battery")).isDouble()
                ? QString::number(object.value(QStringLiteral("battery")).toInt()) + '%' : QStringLiteral("unknown")));
        const auto settings = object.value(QStringLiteral("settings")).toObject();
        dpiChoices_.clear();
        for (const auto value : object.value(QStringLiteral("dpi_choices")).toArray()) dpiChoices_.push_back(value.toInt());
        std::sort(dpiChoices_.begin(), dpiChoices_.end());
        dpiChoices_.erase(std::unique(dpiChoices_.begin(), dpiChoices_.end()), dpiChoices_.end());
        if (!dpiChoices_.empty()) {
            dpi_->setRange(dpiChoices_.front(), dpiChoices_.back());
            slider_->setRange(0, static_cast<int>(dpiChoices_.size()) - 1);
        }
        const auto dpi = settings.value(QStringLiteral("dpi"));
        dpi_->setEnabled(dpi.isDouble());
        slider_->setEnabled(dpi.isDouble() && !dpiChoices_.empty());
        if (dpi.isDouble()) {
            dpi_->setValue(dpi.toInt());
            const auto found = std::find(dpiChoices_.begin(), dpiChoices_.end(), dpi.toInt());
            if (found != dpiChoices_.end()) slider_->setValue(static_cast<int>(found - dpiChoices_.begin()));
        }
        const auto shift = settings.value(QStringLiteral("smartshift"));
        smartShift_->setEnabled(shift.isDouble());
        if (shift.isDouble()) smartShift_->setValue(shift.toInt());
        for (auto it = checks_.cbegin(); it != checks_.cend(); ++it) {
            const auto value = settings.value(it.key());
            it.value()->setEnabled(value.isDouble());
            if (value.isDouble()) it.value()->setChecked(value.toInt() != 0);
        }
        thumbInvertSupported_ = settings.value(QStringLiteral("thumb-invert")).isDouble();
        updateThumbInvert();
        loading_ = false;
    }

    bool fixtureStateCorrect() const {
        return dpi_->value() == 1600 && smartShift_->value() == 25 &&
            checks_.value(QStringLiteral("hires"))->isChecked() &&
            !checks_.value(QStringLiteral("thumb-invert"))->isEnabled();
    }

private:
    QWidget* settingsPage() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        auto* group = new QGroupBox(QStringLiteral("Mouse settings"), page);
        auto* form = new QFormLayout(group);
        auto* dpiRow = new QWidget(group);
        auto* dpiLayout = new QHBoxLayout(dpiRow);
        dpiLayout->setContentsMargins(0, 0, 0, 0);
        slider_ = new QSlider(Qt::Horizontal, dpiRow);
        dpi_ = new QSpinBox(dpiRow);
        dpi_->setRange(100, 8000);
        dpi_->setSuffix(QStringLiteral(" DPI"));
        dpi_->setKeyboardTracking(false);
        dpi_->setEnabled(false); slider_->setEnabled(false);
        dpiLayout->addWidget(slider_, 1); dpiLayout->addWidget(dpi_);
        form->addRow(QStringLiteral("Pointer speed"), dpiRow);
        smartShift_ = new QSpinBox(group);
        smartShift_->setRange(0, 50);
        smartShift_->setKeyboardTracking(false);
        smartShift_->setSpecialValueText(QStringLiteral("Disabled"));
        smartShift_->setEnabled(false);
        form->addRow(QStringLiteral("SmartShift sensitivity"), smartShift_);
        for (const auto& pair : QList<QPair<QString, QString>>{
                 {QStringLiteral("ratchet"), QStringLiteral("Ratchet scroll mode")},
                 {QStringLiteral("hires"), QStringLiteral("High resolution scrolling")},
                 {QStringLiteral("invert"), QStringLiteral("Reverse main wheel")},
                 {QStringLiteral("thumb-invert"), QStringLiteral("Reverse thumb wheel")}}) {
            auto* check = new QCheckBox(pair.second, group);
            check->setEnabled(false);
            checks_.insert(pair.first, check);
            form->addRow(check);
            connect(check, &QCheckBox::toggled, this, [this, key = pair.first](bool value) { setSetting(key, value ? 1 : 0); });
        }
        connect(slider_, &QSlider::valueChanged, this, [this](int index) {
            if (loading_ || index < 0 || index >= dpiChoices_.size()) return;
            const QSignalBlocker block(dpi_);
            dpi_->setValue(dpiChoices_[index]);
            setSetting(QStringLiteral("dpi"), dpiChoices_[index]);
        });
        connect(dpi_, &QSpinBox::valueChanged, this, [this](int value) { setSetting(QStringLiteral("dpi"), value); });
        connect(smartShift_, &QSpinBox::valueChanged, this, [this](int value) { setSetting(QStringLiteral("smartshift"), value); });
        layout->addWidget(group);
        auto* description = new QLabel(QStringLiteral("Changes apply immediately. Unsupported settings remain disabled.\n"
            "The backend requires access to your Logitech hidraw device and /dev/uinput.\n"
            "Stop the backend before changing thumb wheel inversion.\n"
            "Stop other mouse remapping software before starting this backend."), page);
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addStretch();
        return page;
    }

    QWidget* mappingsPage() {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);
        const QStringList suggestions{QStringLiteral("MouseMiddle"), QStringLiteral("MouseBack"),
            QStringLiteral("MouseForward"), QStringLiteral("WheelMode"), QStringLiteral("Super+Tab"),
            QStringLiteral("Super+D"), QStringLiteral("Super"), QStringLiteral("Ctrl+Alt+Left"),
            QStringLiteral("Ctrl+Alt+Right"), QStringLiteral("VolumeDown"), QStringLiteral("VolumeUp"),
            QStringLiteral("Mute"), QStringLiteral("PlayPause"), QStringLiteral("Ctrl+Tab"), QStringLiteral("Ctrl+Shift+Tab")};
        const QList<QPair<QString, QString>> controls{
            {QStringLiteral("0052"), QStringLiteral("Middle button")},
            {QStringLiteral("0053"), QStringLiteral("Back button")},
            {QStringLiteral("0056"), QStringLiteral("Forward button")},
            {QStringLiteral("00C4"), QStringLiteral("Top button")},
            {QStringLiteral("00C3"), QStringLiteral("Gesture button · click")},
            {QStringLiteral("00C3.left"), QStringLiteral("Gesture · left")},
            {QStringLiteral("00C3.right"), QStringLiteral("Gesture · right")},
            {QStringLiteral("00C3.up"), QStringLiteral("Gesture · up")},
            {QStringLiteral("00C3.down"), QStringLiteral("Gesture · down")},
            {QStringLiteral("thumb.left"), QStringLiteral("Thumb wheel · left")},
            {QStringLiteral("thumb.right"), QStringLiteral("Thumb wheel · right")}};
        for (const auto& pair : controls) {
            auto* choice = new QComboBox(page);
            choice->setEditable(true);
            choice->addItems(suggestions);
            bindings_.insert(pair.first.toLower(), choice);
            form->addRow(pair.second, choice);
            connect(choice, &QComboBox::currentTextChanged, this, [this, key = pair.first](const QString& value) {
                if (loading_) return;
                draft_ = replaceGlobalBinding(draft_, key, value);
                const QSignalBlocker block(advanced_);
                advanced_->setPlainText(draft_);
                scheduleSave();
            });
        }
        auto* help = new QLabel(QStringLiteral("Mappings are validated and saved automatically.\n"
            "Linux preview currently uses global mappings. Desktop shortcuts depend on your compositor.\n"
            "Application profiles, global hotkey triggers, Actions Ring and Flow are not implemented on Linux."), page);
        help->setWordWrap(true);
        form->addRow(help);
        scroll->setWidget(page);
        return scroll;
    }

    QWidget* advancedPage() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        auto* path = new QLabel(QStringLiteral("Configuration: ") + config_, page);
        path->setWordWrap(true);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(path);
        advanced_ = new QPlainTextEdit(page);
        layout->addWidget(advanced_);
        connect(advanced_, &QPlainTextEdit::textChanged, this, [this] {
            if (loading_) return;
            draft_ = advanced_->toPlainText();
            scheduleSave();
        });
        layout->addWidget(new QLabel(QStringLiteral("Invalid edits stay here; the last valid file and active mappings are retained."), page));
        return page;
    }

    void setEditors(const QString& text) {
        loading_ = true;
        advanced_->setPlainText(text);
        const auto entries = globalBindings(text);
        for (auto it = bindings_.cbegin(); it != bindings_.cend(); ++it)
            it.value()->setCurrentText(entries.value(it.key()));
        loading_ = false;
    }

    void message(const QString& text, bool error = false) {
        status_->setText(text);
        status_->setStyleSheet(error ? QStringLiteral("color:#ffb4a9") : QStringLiteral("color:#a5e6dd"));
    }

    void submit(Task task) {
        if (!runtime_) return;
        for (auto& queued : tasks_) {
            if (queued.key == task.key) { queued = std::move(task); return; }
        }
        tasks_.push_back(std::move(task));
        nextTask();
    }

    void nextTask() {
        if (active_ || tasks_.empty()) return;
        active_ = std::move(tasks_.front()); tasks_.pop_front();
        timedOut_ = false;
        process_->setProgram(cli_);
        process_->setArguments(active_->arguments);
        process_->start();
        processTimer_.start();
    }

    void finishTask(bool ok) {
        if (!active_) return;
        processTimer_.stop();
        const auto output = QString::fromUtf8(process_->readAllStandardOutput()).trimmed();
        auto error = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!ok && error.isEmpty()) error = timedOut_ ? QStringLiteral("Mouse command timed out.") : process_->errorString();
        auto task = std::move(*active_); active_.reset();
        if (task.done) task.done(ok, output, error);
        QTimer::singleShot(0, this, [this] { nextTask(); });
    }

    void refreshAll() {
        submit({QStringLiteral("status"), {QStringLiteral("status-json")}, [this](bool ok, const QString& output, const QString& error) {
            QJsonParseError parse{};
            const auto document = QJsonDocument::fromJson(output.toUtf8(), &parse);
            if (ok && parse.error == QJsonParseError::NoError && document.isObject()) applyStatus(document.object());
            else {
                device_->setText(QStringLiteral("Device unavailable"));
                thumbInvertSupported_ = false;
                dpi_->setEnabled(false); slider_->setEnabled(false); smartShift_->setEnabled(false);
                for (auto* check : checks_) check->setEnabled(false);
                message(error.isEmpty() ? QStringLiteral("Invalid device status response.") : error, true);
            }
        }});
        refreshAgent();
    }

    void refreshAgent() {
        submit({QStringLiteral("agent"), {QStringLiteral("agent-status")}, [this](bool ok, const QString& output, const QString& error) {
            QJsonParseError parse{};
            const auto document = QJsonDocument::fromJson(output.toUtf8(), &parse);
            if (!ok || parse.error != QJsonParseError::NoError || !document.isObject()) {
                agent_->setText(QStringLiteral("Backend status unavailable"));
                message(error.isEmpty() ? QStringLiteral("Invalid backend status response.") : error, true);
                return;
            }
            const bool running = document.object().value(QStringLiteral("native")).toBool();
            backendRunning_ = running;
            updateThumbInvert();
            agent_->setText(running ? QStringLiteral("Backend running · device readiness shown separately") : QStringLiteral("Backend stopped"));
            start_->setEnabled(!running);
            stop_->setEnabled(running);
        }});
    }

    void setSetting(const QString& setting, int value) {
        if (loading_) return;
        submit({QStringLiteral("set:") + setting, {QStringLiteral("set"), setting, QString::number(value)},
            [this, setting](bool ok, const QString&, const QString& error) {
                message(ok ? setting + QStringLiteral(" applied and read back from device.") : error, !ok);
                if (!ok) refreshAll();
            }});
    }

    void updateThumbInvert() {
        auto* check = checks_.value(QStringLiteral("thumb-invert"));
        check->setEnabled(thumbInvertSupported_ && !backendRunning_);
        check->setToolTip(backendRunning_ ? QStringLiteral("Stop the backend before changing this device setting.") : QString{});
    }

    void scheduleSave() {
        ++revision_;
        if (!runtime_) return;
        message(QStringLiteral("Validating changes…"));
        saveTimer_.start();
    }

    void validateDraft(bool start) {
        if (!runtime_) return;
        auto candidate = std::make_shared<QTemporaryFile>(QFileInfo(config_).absolutePath() + QStringLiteral("/.mapping-XXXXXX.ini"));
        if (!candidate->open()) { message(QStringLiteral("Cannot create temporary configuration."), true); return; }
        const auto bytes = draft_.toUtf8();
        if (candidate->write(bytes) != bytes.size() || !candidate->flush()) {
            message(QStringLiteral("Cannot write temporary configuration."), true); return;
        }
        const auto path = candidate->fileName();
        candidate->close();
        const auto revision = revision_;
        submit({QStringLiteral("validate"), {QStringLiteral("check-config"), path},
            [this, candidate, bytes, revision, start](bool ok, const QString&, const QString& error) {
                if (revision != revision_) return;
                if (!ok) { message(error, true); return; }
                if (!writeFile(config_, bytes)) { message(QStringLiteral("Cannot save configuration."), true); return; }
                setEditors(QString::fromUtf8(bytes));
                if (start) { startBackend(); return; }
                if (!backendRunning_) {
                    message(QStringLiteral("Saved. Start the backend to apply mappings."));
                    return;
                }
                submit({QStringLiteral("reload"), {QStringLiteral("reload"), config_},
                    [this](bool ok, const QString& output, const QString& error) {
                        const bool notified = QJsonDocument::fromJson(output.toUtf8()).object().value(QStringLiteral("reload_requested")).toBool();
                        message(ok ? (notified ? QStringLiteral("Saved; notified the running backend to reload.")
                                               : QStringLiteral("Saved. Start the backend to apply mappings."))
                                   : QStringLiteral("Saved, but reload failed: ") + error, !ok);
                    }});
            }});
    }

    void startBackend() {
        QProcess daemon;
        daemon.setProgram(cli_);
        daemon.setArguments({QStringLiteral("run"), config_});
        daemon.setWorkingDirectory(QFileInfo(config_).absolutePath());
        daemon.setStandardInputFile(QProcess::nullDevice());
        daemon.setStandardOutputFile(QFileInfo(config_).absolutePath() + QStringLiteral("/agent.log"), QIODevice::Append);
        daemon.setStandardErrorFile(QFileInfo(config_).absolutePath() + QStringLiteral("/agent.log"), QIODevice::Append);
        if (!daemon.startDetached()) { message(QStringLiteral("Could not launch mouse backend."), true); return; }
        start_->setEnabled(false);
        message(QStringLiteral("Backend process launched. Check device status and agent.log if input mappings do not activate."));
        QTimer::singleShot(1000, this, [this] { refreshAgent(); });
    }

    QString config_, cli_, draft_;
    bool runtime_ = true, loading_ = false, timedOut_ = false;
    bool backendRunning_ = false, thumbInvertSupported_ = false;
    unsigned revision_ = 0;
    QLabel *device_{}, *agent_{}, *status_{};
    QSpinBox *dpi_{}, *smartShift_{};
    QSlider* slider_{};
    QPlainTextEdit* advanced_{};
    QPushButton *start_{}, *stop_{};
    QMap<QString, QCheckBox*> checks_;
    QMap<QString, QComboBox*> bindings_;
    QList<int> dpiChoices_;
    QProcess* process_{};
    QTimer saveTimer_, processTimer_;
    std::deque<Task> tasks_;
    std::optional<Task> active_;
};

int selfTest() {
    const auto input = QStringLiteral("# preserved\n[global]\n0052 = MouseMiddle\n[other]\nx = y\n");
    const auto replaced = replaceGlobalBinding(input, QStringLiteral("0052"), QStringLiteral("MouseBack"));
    if (globalBindings(replaced).value(QStringLiteral("0052")) != QStringLiteral("MouseBack")) return 1;
    const auto inserted = replaceGlobalBinding(replaced, QStringLiteral("thumb.left"), QStringLiteral("VolumeDown"));
    if (!inserted.contains(QStringLiteral("[other]\nx = y")) ||
        globalBindings(inserted).value(QStringLiteral("thumb.left")) != QStringLiteral("VolumeDown")) return 2;
    QTemporaryDir directory;
    if (!directory.isValid()) return 3;
    const auto path = directory.filePath(QStringLiteral("mappings.ini"));
    Window window(path, false);
    window.applyStatus(QJsonObject{{QStringLiteral("device"), QStringLiteral("Fixture mouse")},
        {QStringLiteral("battery"), 95}, {QStringLiteral("dpi_choices"), QJsonArray{800, 1600, 3200}},
        {QStringLiteral("settings"), QJsonObject{{QStringLiteral("dpi"), 1600},
            {QStringLiteral("smartshift"), 25}, {QStringLiteral("hires"), 1}, {QStringLiteral("invert"), 0}}}});
    if (!window.fixtureStateCorrect()) return 4;
    if (QFileInfo::exists(path)) return 5; // No config, acceptance marker, CLI, or HID access in this test.
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("logi-options-open"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    if (application.arguments().contains(QStringLiteral("--self-test"))) return selfTest();
    if (loo::notice_required()) {
        QMessageBox notice(QMessageBox::Warning, QStringLiteral("Third-party experimental software / 第三方实验软件"),
            QString::fromUtf8(loo::notice_text), QMessageBox::NoButton);
        notice.setInformativeText(QStringLiteral("Continue only if you understand these limitations.\n"
            "本软件为第三方非官方项目，与 Logitech 无隶属或认可关系。目前尚不成熟，可能存在错误、配置丢失或自定义按键失效。"));
        auto* accept = notice.addButton(QStringLiteral("I understand — continue / 我已了解并继续"), QMessageBox::AcceptRole);
        notice.addButton(QStringLiteral("Exit / 退出"), QMessageBox::RejectRole);
        notice.exec();
        if (notice.clickedButton() != accept) return 0;
        if (!loo::accept_notice()) {
            QMessageBox::critical(nullptr, QString::fromLatin1(title), QStringLiteral("Could not save acknowledgement. No mouse backend was started."));
            return 1;
        }
    }
    Window window(configPath());
    window.show();
    return application.exec();
}
