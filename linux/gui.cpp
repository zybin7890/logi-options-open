#include "../first_run.hpp"
#include "ring_ui.hpp"
#include "device_canvas.hpp"

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
#include <QLineEdit>
#include <QKeySequenceEdit>
#include <QTableWidget>
#include <QHeaderView>
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
#include <QTabBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QPainter>
#include <QVariantAnimation>
#include <QButtonGroup>
#include <QToolButton>
#ifdef LOGI_GUI_TEST
#include <QtTest/QTest>
#include <QElapsedTimer>
#include <QDebug>
#include <QScreen>
#include <QWindow>
#endif

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr auto title = "Logi Options Open";
constexpr auto fallback_config =
    "# Linux mappings. Named application sections override the global profile on X11.\n"
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

QMap<QString, QString> globalBindings(const QString& text, const QString& section = QStringLiteral("global")) {
    QMap<QString, QString> result;
    bool global = false;
    for (const auto& line : text.split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith('[')) {
            global = trimmed.compare('[' + section + ']', Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!global || trimmed.startsWith('#') || trimmed.startsWith(';')) continue;
        const auto equal = line.indexOf('=');
        if (equal > 0) result.insert(line.left(equal).trimmed().toLower(), line.mid(equal + 1).trimmed());
    }
    return result;
}

QString replaceGlobalBinding(const QString& source, const QString& key, const QString& value,
                             const QString& section = QStringLiteral("global")) {
    auto lines = source.split('\n');
    int begin = -1, end = static_cast<int>(lines.size());
    for (int i = 0; i < lines.size(); ++i) {
        const auto trimmed = lines[i].trimmed();
        if (!trimmed.startsWith('[')) continue;
        if (begin >= 0) { end = i; break; }
        if (trimmed.compare('[' + section + ']', Qt::CaseInsensitive) == 0) begin = i;
    }
    const auto replacement = key + QStringLiteral(" = ") + value;
    if (begin < 0) {
        if (!lines.isEmpty() && lines.back().isEmpty()) lines.removeLast();
        lines << ('[' + section + ']') << replacement << QString{};
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

QStringList splitActions(const QString& sequence) {
    QStringList result; int depth = 0, begin = 0;
    for (int i = 0; i < sequence.size(); ++i) {
        if (sequence[i] == '(') ++depth;
        else if (sequence[i] == ')') --depth;
        else if (sequence[i] == ';' && depth == 0) { result << sequence.mid(begin, i-begin); begin = i+1; }
    }
    if (begin < sequence.size()) result << sequence.mid(begin);
    return result;
}

struct Task {
    QString key;
    QStringList arguments;
    std::function<void(bool, const QString&, const QString&)> done;
};

class Toggle final : public QCheckBox {
public:
    explicit Toggle(const QString& text,QWidget* parent=nullptr):QCheckBox(text,parent) {
        setMinimumHeight(42);setCursor(Qt::PointingHandCursor);
        animation_.setDuration(140);
        connect(&animation_,&QVariantAnimation::valueChanged,this,[this](const QVariant& v){position_=v.toReal();update();});
        connect(this,&QCheckBox::toggled,this,[this](bool checked){
            animation_.stop();animation_.setStartValue(position_);animation_.setEndValue(checked?1.:0.);animation_.start();
        });
    }
protected:
    bool hitButton(const QPoint& p)const override{return rect().contains(p);}
    void paintEvent(QPaintEvent*)override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(isEnabled()?1.:.4);p.setPen(QColor("#eeeeee"));
        p.drawText(rect().adjusted(0,0,-60,0),Qt::AlignVCenter|Qt::AlignLeft,text());
        const QRectF track(width()-48,(height()-24)/2.,44,24);
        p.setPen(Qt::NoPen);p.setBrush(isChecked()?QColor("#00d8c6"):QColor("#555555"));p.drawRoundedRect(track,12,12);
        p.setBrush(isChecked()?QColor("#101010"):QColor("#ffffff"));p.drawEllipse(QRectF(track.x()+3+position_*20,track.y()+3,18,18));
        if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(QColor("#00d8c6"),1));p.drawRoundedRect(track.adjusted(-3,-3,3,3),15,15);}
    }
private:QVariantAnimation animation_;qreal position_{};
};

class Window : public QMainWindow {
public:
    explicit Window(QString config, bool runtime = true, bool refreshOnOpen = true)
        : config_(std::move(config)), runtime_(runtime) {
        setWindowTitle(QString::fromLatin1(title));
        resize(1240, 800);
        setMinimumSize(1080, 700);
        cli_ = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logi-cli"));
        if (!QFileInfo::exists(cli_)) cli_ = QStandardPaths::findExecutable(QStringLiteral("logi-cli"));
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(32, 22, 24, 16);
        auto* header = new QHBoxLayout;
        auto* home = new QPushButton(QStringLiteral("‹"),central);home->setFixedSize(44,44);
        home->setAccessibleName(QStringLiteral("返回首页"));header->addWidget(home);
        auto* heading = new QLabel(QStringLiteral("MX Master 3S"), central);
        heading->setStyleSheet(QStringLiteral("font-size:26px;font-weight:600"));
        header->addWidget(heading);
        header->addStretch();
        auto* smart = new QPushButton(QStringLiteral("Smart Actions"),central);smart->setObjectName("navSmart");header->addWidget(smart);
        auto* ring = new QPushButton(QStringLiteral("Actions Ring"),central);ring->setObjectName("navRing");header->addWidget(ring);
        auto* refresh = new QPushButton(QStringLiteral("刷新"), central);
        header->addWidget(refresh);
        layout->addLayout(header);
        device_ = new QLabel(QStringLiteral("设备未连接"), central);
        profileBar_=new QHBoxLayout;profileBar_->setSpacing(8);profileBar_->addStretch();
        layout->addLayout(profileBar_);
        profile_ = new QComboBox(central); profile_->setObjectName(QStringLiteral("profile"));
        profile_->addItem(QStringLiteral("全局设置"), QStringLiteral("global"));profile_->hide();
        profileEditor_=new QWidget(central);auto* profiles=new QHBoxLayout(profileEditor_);
        profiles->addStretch();
        profileName_ = new QLineEdit(profileEditor_); profileName_->setObjectName(QStringLiteral("profileName"));
        profileName_->setPlaceholderText(QStringLiteral("应用名称，例如 firefox"));profileName_->setMaximumWidth(340);
        profiles->addWidget(profileName_);
        auto* addProfile = new QPushButton(QStringLiteral("添加应用"), profileEditor_);
        addProfile->setObjectName(QStringLiteral("addProfile")); profiles->addWidget(addProfile);
        layout->addWidget(profileEditor_);profileEditor_->hide();
        auto* body=new QHBoxLayout;body->setSpacing(24);layout->addLayout(body,1);
        auto* navigationWidget=new QWidget;auto* navigation=new QVBoxLayout(navigationWidget);navigation->setSpacing(12);navigation->addStretch();body->addWidget(navigationWidget);
        auto* tabs = new QTabWidget(central);
        tabs->setObjectName(QStringLiteral("pages"));
        tabs->tabBar()->hide();pages_=tabs;
        body->addWidget(tabs, 1);
        tabs->addTab(settingsPage(), QStringLiteral("Point & scroll"));
        tabs->addTab(mappingsPage(), QStringLiteral("Buttons & gestures"));
        tabs->addTab(smartPage(), QStringLiteral("Smart Actions"));
        tabs->addTab(ringPage(), QStringLiteral("Actions Ring"));
        tabs->addTab(advancedPage(), QStringLiteral("Configuration"));
        tabs->addTab(informationPage(QStringLiteral("Easy-Switch"),QStringLiteral("按下鼠标底部的 Easy-Switch 按钮，在已配对的电脑之间切换。")),QStringLiteral("Easy-Switch"));
        tabs->addTab(informationPage(QStringLiteral("Flow"),QStringLiteral("当前版本尚未实现跨电脑跟随和剪贴板。")),QStringLiteral("Flow"));
        auto* settings=new QWidget;auto* settingsLayout=new QVBoxLayout(settings);settingsLayout->setContentsMargins(60,60,60,60);
        settingsLayout->addWidget(new QLabel(QStringLiteral("设置")));settingsLayout->addWidget(device_);
        settingsLayout->addWidget(new QLabel(QStringLiteral("独立第三方实验软件 · 无需安装原版 Options+")));
        agent_ = new QLabel(QStringLiteral("后台状态未读取"), settings);settingsLayout->addWidget(agent_);
        auto* controls=new QHBoxLayout;start_=new QPushButton(QStringLiteral("启动后台"));stop_=new QPushButton(QStringLiteral("停止后台"));
        controls->addWidget(start_);controls->addWidget(stop_);controls->addStretch();settingsLayout->addLayout(controls);
        auto* advanced=new QPushButton(QStringLiteral("高级配置"));settingsLayout->addWidget(advanced);settingsLayout->addStretch();
        tabs->addTab(settings,QStringLiteral("设置"));
        auto* homePage=new QWidget;auto* homeLayout=new QVBoxLayout(homePage);homeLayout->addStretch();
        auto* deviceCard=new QToolButton;deviceCard->setObjectName("openDevice");deviceCard->setText(QStringLiteral("MX Master 3S"));
        deviceCard->setIcon(QIcon(QStringLiteral(":/assets/front.png")));deviceCard->setIconSize(QSize(200,300));
        deviceCard->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);deviceCard->setFixedSize(280,390);
        deviceCard->setStyleSheet("QToolButton{background:#202020;border:1px solid #303030;border-radius:16px;padding:24px;font-size:20px;}QToolButton:hover{border-color:#00d8c6;}");
        homeLayout->addWidget(deviceCard,0,Qt::AlignHCenter);homeLayout->addStretch();tabs->addTab(homePage,QStringLiteral("首页"));
        connect(deviceCard,&QToolButton::clicked,this,[tabs]{tabs->setCurrentIndex(1);});
        const QList<QPair<QString,int>> navigationPages{{QStringLiteral("按钮"),1},{QStringLiteral("指向和滚动"),0},
            {QStringLiteral("EASY-SWITCH"),5},{QStringLiteral("FLOW"),6},{QStringLiteral("设置"),7}};
        auto* navGroup=new QButtonGroup(this);navGroup->setExclusive(true);
        for(const auto& [caption,index]:navigationPages){
            auto* button=new QPushButton(caption);button->setObjectName(QStringLiteral("nav%1").arg(index));
            button->setProperty("navigation",true);button->setCheckable(true);button->setFixedWidth(160);button->setMinimumHeight(44);
            navGroup->addButton(button,index);navigation->addWidget(button);
            connect(button,&QPushButton::clicked,this,[this,index]{pages_->setCurrentIndex(index);});
        }
        navigation->addStretch();
        connect(tabs,&QTabWidget::currentChanged,this,[this,navGroup,navigationWidget,heading](int index){
            if(auto* button=navGroup->button(index))button->setChecked(true);
            navigationWidget->setVisible(index!=8 && index!=2 && index!=3);
            heading->setText(index==8?QString::fromLatin1(title):index==2?QStringLiteral("Smart Actions"):index==3?QStringLiteral("Actions Ring"):QStringLiteral("MX Master 3S"));
            profileEditor_->hide();rebuildProfiles();
        });
        connect(smart,&QPushButton::clicked,this,[tabs]{tabs->setCurrentIndex(2);});
        connect(ring,&QPushButton::clicked,this,[tabs]{tabs->setCurrentIndex(3);});
        connect(advanced,&QPushButton::clicked,this,[tabs]{tabs->setCurrentIndex(4);});
        connect(home,&QPushButton::clicked,this,[tabs]{tabs->setCurrentIndex(8);});
        tabs->setCurrentIndex(8);
        auto* footer = new QHBoxLayout;
        battery_=new QLabel(QStringLiteral("电量 —"));battery_->setObjectName("battery");footer->addWidget(battery_);footer->addStretch();
        layout->addLayout(footer);
        status_ = new QLabel(central);
        status_->setObjectName(QStringLiteral("statusMessage"));
        status_->setWordWrap(true);
        status_->setMinimumHeight(24);
        layout->addWidget(status_);
        setCentralWidget(central);
        connect(profile_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) setEditors(draft_); });
        connect(addProfile, &QPushButton::clicked, this, [this] {
            const auto name = profileName_->text().trimmed().toLower();
            if (name.isEmpty() || name.contains('[') || name.contains(']') || name.contains('\n') ||
                name == QStringLiteral("hotkeys") || name == QStringLiteral("ring") || name.startsWith(QStringLiteral("ring/"))) {
                message(QStringLiteral("Enter an application executable name or WM_CLASS."), true); return;
            }
            int index = profile_->findData(name);
            if (index < 0) {
                draft_ += QStringLiteral("\n[") + name + QStringLiteral("]\n");
                const QSignalBlocker block(profile_); profile_->addItem(name, name); index = profile_->count() - 1;
            }
            profile_->setCurrentIndex(index); profileName_->clear();profileEditor_->hide(); setEditors(draft_); scheduleSave();
        });
        setStyleSheet(QStringLiteral(
            "QMainWindow,QWidget{background:#1a1a1a;color:#eeeeee;font-size:15px;}"
            "QTabWidget::pane{border:0;}QPushButton[navigation=true]{text-align:left;border:0;background:transparent;padding:12px;}"
            "QPushButton[navigation=true]:checked{color:#00d8c6;background:#222222;}"
            "QWidget#drawer{background:#101010;}QLabel#battery{background:#101010;color:#8fe34a;border-radius:5px;padding:12px;}"
            "QGroupBox{border:1px solid #3b4040;border-radius:8px;}"
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
        if (runtime_ && refreshOnOpen) QTimer::singleShot(0, this, [this] { refreshAll(); });
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
        battery_->setText(object.value("battery").isDouble()?QStringLiteral("%1%  ▰").arg(object.value("battery").toInt()):QStringLiteral("电量 —"));
        updatePointerSummary();
        loading_ = false;
    }

    bool fixtureStateCorrect() const {
        return dpi_->value() == 1600 && smartShift_->value() == 25 &&
            checks_.value(QStringLiteral("hires"))->isChecked() &&
            !checks_.value(QStringLiteral("thumb-invert"))->isEnabled();
    }

private:
    void updatePointerSummary(){
        pointerCanvas_->setLabels({QStringLiteral("SmartShift: %1\n滚动方向：%2")
            .arg(smartShift_->value()?QStringLiteral("开启"):QStringLiteral("关闭"))
            .arg(checks_.value("invert")->isChecked()?QStringLiteral("反向"):QStringLiteral("标准")),
            QStringLiteral("%1 DPI").arg(dpi_->value()),
            checks_.value("thumb-invert")->isChecked()?QStringLiteral("滚动方向：反向"):QStringLiteral("滚动方向：默认")});
    }
    QWidget* informationPage(const QString& title,const QString& detail) {
        auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(80,80,80,80);layout->addStretch();
        auto* heading=new QLabel(title);heading->setStyleSheet("font-size:30px;font-weight:600;");layout->addWidget(heading);
        auto* description=new QLabel(detail);description->setWordWrap(true);layout->addWidget(description);layout->addStretch();return page;
    }
    void rebuildProfiles() {
        if(!profileBar_)return;
        while(auto* item=profileBar_->takeAt(0)){if(auto* widget=item->widget())widget->deleteLater();delete item;}
        profileBar_->addStretch();
        if(!pages_ || (pages_->currentIndex()!=0 && pages_->currentIndex()!=1 && pages_->currentIndex()!=2))return;
        const int count=pages_->currentIndex()==0?1:profile_->count();
        for(int i=0;i<count;++i){
            const auto name=profile_->itemData(i).toString();
            auto* button=new QPushButton(name=="global"?QStringLiteral("⊞"):name.left(1).toUpper());
            button->setObjectName(QStringLiteral("profile_%1").arg(name));button->setFixedSize(48,48);
            button->setToolTip(name=="global"?QStringLiteral("全局设置"):name);button->setAccessibleName(button->toolTip());
            if(name.contains("firefox")||name.contains("chrome")||name.contains("wps")) {
                const auto asset=name.contains("firefox")?"firefox":name.contains("chrome")?"chrome":"wps";
                button->setIcon(QIcon(QStringLiteral(":/assets/%1.png").arg(asset)));button->setIconSize(QSize(30,30));button->setText({});
            }
            const bool selected=pages_->currentIndex()==0?i==0:i==profile_->currentIndex();
            button->setStyleSheet(QStringLiteral("QPushButton{border:0;border-radius:0;background:transparent;font-size:25px;border-bottom:3px solid %1;}QPushButton:hover{background:#242424;}").arg(selected?"#00d8c6":"transparent"));
            connect(button,&QPushButton::clicked,this,[this,i]{profile_->setCurrentIndex(i);rebuildProfiles();});profileBar_->addWidget(button);
        }
        if(pages_->currentIndex()==1 || pages_->currentIndex()==2){
            auto* add=new QPushButton(QStringLiteral("＋"));add->setObjectName("openAddProfile");add->setAccessibleName(QStringLiteral("添加应用程序"));add->setFixedSize(48,48);
            connect(add,&QPushButton::clicked,this,[this]{profileEditor_->setVisible(!profileEditor_->isVisible());profileName_->setFocus();});profileBar_->addWidget(add);
        }
    }
    void syncCanvases() {
        if(buttonCanvas_){
            auto values=globalBindings(draft_);const auto overrides=globalBindings(draft_,profile_->currentData().toString());
            for(auto it=overrides.cbegin();it!=overrides.cend();++it)values[it.key()]=it.value();
            auto caption=[](QString value){
                static const QMap<QString,QString> names{{"MouseMiddle",QStringLiteral("中键")},{"MouseBack",QStringLiteral("后退")},
                    {"MouseForward",QStringLiteral("前进")},{"WheelMode",QStringLiteral("切换滚轮模式")},{"Super+Tab",QStringLiteral("任务视图")},
                    {"VolumeDown",QStringLiteral("音量降低")},{"VolumeUp",QStringLiteral("音量升高")}};
                return names.value(value,value);
            };
            buttonCanvas_->setLabels({caption(values.value("0052")),caption(values.value("00c4")),
                caption(values.value("thumb.left"))+QStringLiteral(" / ")+caption(values.value("thumb.right")),
                caption(values.value("0056")),caption(values.value("0053")),caption(values.value("00c3"))});
        }
        rebuildProfiles();
    }
    QWidget* settingsPage() {
        auto* page = new QWidget;
        auto* body=new QHBoxLayout(page);body->setContentsMargins(0,0,0,0);body->setSpacing(0);
        pointerCanvas_=new loo_ui::DeviceCanvas;pointerCanvas_->setMode(loo_ui::DeviceMode::Pointer);body->addWidget(pointerCanvas_,1);
        auto* drawer=new QWidget;drawer->setObjectName("drawer");drawer->setFixedWidth(352);body->addWidget(drawer);
        auto* layout = new QVBoxLayout(drawer);layout->setContentsMargins(24,28,24,28);
        auto* top=new QHBoxLayout;auto* title=new QLabel(QStringLiteral("滚轮"));title->setStyleSheet("font-size:24px;font-weight:600;");top->addWidget(title);top->addStretch();
        auto* close=new QPushButton(QStringLiteral("×"));close->setAccessibleName(QStringLiteral("关闭侧栏"));close->setFixedSize(36,36);top->addWidget(close);layout->addLayout(top);
        auto* group = new QGroupBox(drawer);group->setStyleSheet("QGroupBox{border:0;margin:0;padding:8px 0;}");
        auto* form = new QFormLayout(group);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapAllRows);form->setVerticalSpacing(20);
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
        form->addRow(QStringLiteral("指针速度"), dpiRow);
        smartShift_ = new QSpinBox(group);
        smartShift_->setRange(0, 50);
        smartShift_->setKeyboardTracking(false);
        smartShift_->setSpecialValueText(QStringLiteral("Disabled"));
        smartShift_->setEnabled(false);
        form->addRow(QStringLiteral("SmartShift 灵敏度"), smartShift_);
        for (const auto& pair : QList<QPair<QString, QString>>{
                 {QStringLiteral("ratchet"), QStringLiteral("逐行模式")},
                 {QStringLiteral("hires"), QStringLiteral("高分辨率滚动")},
                 {QStringLiteral("invert"), QStringLiteral("反转滚动方向")},
                 {QStringLiteral("thumb-invert"), QStringLiteral("反转拇指滚轮")}}) {
            auto* check = new Toggle(pair.second, group);
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
        auto* description = new QLabel(QStringLiteral("更改立即生效"), drawer);
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addStretch();
        pointerCanvas_->selected=[this,drawer,title,form,dpiRow](int index){
            pointerCanvas_->setSelected(index);title->setText(QStringList{QStringLiteral("滚轮"),QStringLiteral("指针速度"),QStringLiteral("拇指滚轮")}[index]);
            form->setRowVisible(dpiRow,index==1);form->setRowVisible(smartShift_,index==0);
            for(auto it=checks_.cbegin();it!=checks_.cend();++it)form->setRowVisible(it.value(),it.key()=="thumb-invert"?index==2:index==0);
            drawer->show();
        };
        connect(close,&QPushButton::clicked,this,[this,drawer]{drawer->hide();pointerCanvas_->setSelected(-1);});drawer->hide();
        return page;
    }

    QWidget* mappingsPage() {
        auto* outer=new QWidget;auto* body=new QHBoxLayout(outer);body->setContentsMargins(0,0,0,0);body->setSpacing(0);
        buttonCanvas_=new loo_ui::DeviceCanvas;body->addWidget(buttonCanvas_,1);
        auto* drawer=new QWidget;drawer->setObjectName("drawer");drawer->setFixedWidth(352);body->addWidget(drawer);
        auto* drawerLayout=new QVBoxLayout(drawer);drawerLayout->setContentsMargins(24,28,24,28);
        auto* top=new QHBoxLayout;auto* title=new QLabel;title->setStyleSheet("font-size:24px;font-weight:600;");top->addWidget(title);top->addStretch();
        auto* close=new QPushButton(QStringLiteral("×"));close->setAccessibleName(QStringLiteral("关闭侧栏"));close->setFixedSize(36,36);top->addWidget(close);drawerLayout->addLayout(top);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);drawerLayout->addWidget(scroll);
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);form->setVerticalSpacing(20);
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
            choice->setObjectName(QStringLiteral("binding_") + pair.first.toLower());
            choice->addItems(suggestions);
            bindings_.insert(pair.first.toLower(), choice);
            form->addRow(pair.second, choice);
            connect(choice, &QComboBox::currentTextChanged, this, [this, key = pair.first](const QString& value) {
                if (loading_) return;
                draft_ = replaceGlobalBinding(draft_, key, value, profile_->currentData().toString());
                const QSignalBlocker block(advanced_);
                advanced_->setPlainText(draft_);
                scheduleSave();
            });
        }
        auto* help = new QLabel(QStringLiteral("更改立即生效。应用专属设置会继承未修改的全局设置。"), page);
        help->setWordWrap(true);
        form->addRow(help);
        scroll->setWidget(page);
        buttonCanvas_->selected=[this,drawer,title,form](int index){
            buttonCanvas_->setSelected(index);title->setText(QStringList{QStringLiteral("中键"),QStringLiteral("顶部按钮"),QStringLiteral("拇指滚轮"),QStringLiteral("前进"),QStringLiteral("后退"),QStringLiteral("手势")}[index]);
            const QStringList prefixes{"0052","00c4","thumb.","0056","0053","00c3"};
            for(auto it=bindings_.cbegin();it!=bindings_.cend();++it)form->setRowVisible(it.value(),it.key().startsWith(prefixes[index]));
            drawer->show();
        };
        connect(close,&QPushButton::clicked,this,[this,drawer]{drawer->hide();buttonCanvas_->setSelected(-1);});drawer->hide();
        return outer;
    }

    QString smartSection() const {
        return trigger_->currentData().toString() == QStringLiteral("hotkey")
            ? QStringLiteral("hotkeys") : profile_->currentData().toString();
    }

    QString smartKey() const {
        if (trigger_->currentData().toString() != QStringLiteral("hotkey")) return trigger_->currentData().toString();
        auto key = hotkey_->keySequence().toString(QKeySequence::PortableText);
        return key.replace(QStringLiteral("Meta"), QStringLiteral("Super"));
    }

    QWidget* smartPage() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(64,24,64,24);layout->setSpacing(20);
        auto* title=new QLabel(QStringLiteral("创建 Smart Actions"));title->setStyleSheet("font-size:30px;font-weight:600;");layout->addWidget(title);
        auto* when=new QLabel(QStringLiteral("如果…"));when->setStyleSheet("font-size:22px;");layout->addWidget(when);
        auto* form = new QFormLayout;
        smartForm_ = form;
        trigger_ = new QComboBox(page); trigger_->setObjectName(QStringLiteral("smartTrigger"));
        const QList<QPair<QString, QString>> triggers{
            {QStringLiteral("Middle button"), QStringLiteral("0052")}, {QStringLiteral("Back button"), QStringLiteral("0053")},
            {QStringLiteral("Forward button"), QStringLiteral("0056")}, {QStringLiteral("Top button"), QStringLiteral("00c4")},
            {QStringLiteral("Gesture click"), QStringLiteral("00c3")}, {QStringLiteral("Gesture left"), QStringLiteral("00c3.left")},
            {QStringLiteral("Gesture right"), QStringLiteral("00c3.right")}, {QStringLiteral("Global shortcut"), QStringLiteral("hotkey")}};
        for (const auto& pair : triggers) trigger_->addItem(pair.first, pair.second);
        form->addRow(QStringLiteral("触发器"), trigger_);
        hotkey_ = new QKeySequenceEdit(page); hotkey_->setObjectName(QStringLiteral("smartHotkey"));
        hotkey_->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Shift+F10"))); hotkey_->setEnabled(false);
        form->addRow(QStringLiteral("Shortcut trigger"), hotkey_);
        form->setRowVisible(hotkey_, false);
        layout->addLayout(form);
        auto* then=new QLabel(QStringLiteral("那么…"));then->setStyleSheet("font-size:22px;");layout->addWidget(then);
        steps_ = new QTableWidget(0, 2, page); steps_->setObjectName(QStringLiteral("smartSteps"));
        steps_->setShowGrid(false);steps_->setFrameShape(QFrame::NoFrame);steps_->verticalHeader()->hide();steps_->horizontalHeader()->hide();
        steps_->setStyleSheet("QTableWidget{background:#1a1a1a;border:0;}QTableWidget QLineEdit,QTableWidget QComboBox{background:#101010;border:0;border-radius:8px;padding:14px;}");
        steps_->setHorizontalHeaderLabels({QStringLiteral("Then"), QStringLiteral("Value")});
        steps_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        steps_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        steps_->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(steps_, 1);
        auto* buttons = new QHBoxLayout;
        auto* add = new QPushButton(QStringLiteral("＋ 添加操作"), page); add->setObjectName(QStringLiteral("addStep"));
        auto* remove = new QPushButton(QStringLiteral("移除"), page); remove->setObjectName(QStringLiteral("removeStep"));
        auto* up = new QPushButton(QStringLiteral("上移"), page); up->setObjectName(QStringLiteral("stepUp"));
        auto* down = new QPushButton(QStringLiteral("下移"), page); down->setObjectName(QStringLiteral("stepDown"));
        auto* play = new QPushButton(QStringLiteral("试试看"), page); play->setObjectName(QStringLiteral("trySequence"));
        for (auto* button : {add, remove, up, down, play}) buttons->addWidget(button);
        layout->addLayout(buttons);
        auto* help = new QLabel(QStringLiteral("更改自动保存并生效。"), page);
        help->setWordWrap(true); layout->addWidget(help);
        connect(trigger_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) loadSmart(); });
        connect(hotkey_, &QKeySequenceEdit::keySequenceChanged, this, [this] { if (!loading_) loadSmart(); });
        connect(add, &QPushButton::clicked, this, [this] {
            addStep(QStringLiteral("Wait"), QStringLiteral("100")); steps_->selectRow(steps_->rowCount()-1); updateSmart();
        });
        connect(remove, &QPushButton::clicked, this, [this] {
            if (steps_->currentRow() >= 0) { steps_->removeRow(steps_->currentRow()); updateSmart(); }
        });
        connect(up, &QPushButton::clicked, this, [this] { moveStep(-1); });
        connect(down, &QPushButton::clicked, this, [this] { moveStep(1); });
        connect(play, &QPushButton::clicked, this, [this] { playSequence(smartSequence()); });
        return page;
    }

    void addStep(const QString& type, const QString& value) {
        const int row = steps_->rowCount(); steps_->insertRow(row);
        auto* choice = new QComboBox(steps_);
        choice->addItems({QStringLiteral("Shortcut"), QStringLiteral("Wait"), QStringLiteral("Text"),
            QStringLiteral("Open"), QStringLiteral("Built-in")});
        choice->setCurrentText(type); steps_->setCellWidget(row, 0, choice);
        auto* input = new QLineEdit(value, steps_); steps_->setCellWidget(row, 1, input);
        input->setPlaceholderText(type == QStringLiteral("Wait") ? QStringLiteral("Milliseconds, e.g. 400") : QStringLiteral("Action value"));
        steps_->setRowHeight(row, 66);
        connect(input, &QLineEdit::textEdited, this, [this] { updateSmart(); });
        connect(choice, &QComboBox::currentTextChanged, this, [this, input](const QString& type) {
            input->setPlaceholderText(type == QStringLiteral("Wait") ? QStringLiteral("Milliseconds, e.g. 400") :
                type == QStringLiteral("Text") ? QStringLiteral("Text to paste") :
                type == QStringLiteral("Open") ? QStringLiteral("File path or URL") :
                type == QStringLiteral("Shortcut") ? QStringLiteral("Ctrl+C, Super+Tab…") : QStringLiteral("VolumeUp, Mute, Ring…"));
            updateSmart();
        });
    }

    QString smartSequence() const {
        QStringList result;
        for (int row = 0; row < steps_->rowCount(); ++row) {
            const auto type = static_cast<QComboBox*>(steps_->cellWidget(row, 0))->currentText();
            auto value = static_cast<QLineEdit*>(steps_->cellWidget(row, 1))->text();
            if (type == QStringLiteral("Text")) {
                QString encoded;
                for (const auto unit : value) encoded += QStringLiteral("%1").arg(unit.unicode(), 4, 16, QLatin1Char('0'));
                value = QStringLiteral("TextHex(") + encoded + ')';
            } else if (type == QStringLiteral("Wait") || type == QStringLiteral("Open"))
                value = type + '(' + value + ')';
            result.push_back(value);
        }
        return result.isEmpty() ? QStringLiteral("None") : result.join(';');
    }

    void loadSmart() {
        const bool previous = loading_; loading_ = true;
        hotkey_->setEnabled(trigger_->currentData().toString() == QStringLiteral("hotkey"));
        smartForm_->setRowVisible(hotkey_, hotkey_->isEnabled());
        auto entries = globalBindings(draft_, smartSection());
        auto sequence = entries.value(smartKey().toLower());
        if (sequence.isEmpty() && smartSection() != QStringLiteral("hotkeys"))
            sequence = globalBindings(draft_).value(smartKey().toLower());
        steps_->setRowCount(0);
        for (const auto& action : splitActions(sequence)) {
            const auto trimmed = action.trimmed(); if (trimmed == QStringLiteral("None")) continue;
            const auto open = trimmed.indexOf('(');
            if (trimmed.startsWith(QStringLiteral("TextHex(")) && trimmed.endsWith(')')) {
                const auto hex = trimmed.mid(8, trimmed.size()-9); QString decoded;
                for (int i = 0; i+3 < hex.size(); i += 4) decoded += QChar(hex.mid(i, 4).toUShort(nullptr, 16));
                addStep(QStringLiteral("Text"), decoded);
            } else if (open > 0 && trimmed.endsWith(')') &&
                (trimmed.startsWith(QStringLiteral("Wait(")) || trimmed.startsWith(QStringLiteral("Text(")) || trimmed.startsWith(QStringLiteral("Open("))))
                addStep(trimmed.left(open), trimmed.mid(open+1, trimmed.size()-open-2));
            else addStep(trimmed.contains('+') ? QStringLiteral("Shortcut") : QStringLiteral("Built-in"), trimmed);
        }
        loading_ = previous;
    }

    void updateSmart() {
        if (loading_ || smartKey().isEmpty()) return;
        draft_ = replaceGlobalBinding(draft_, smartKey(), smartSequence(), smartSection());
        const QSignalBlocker block(advanced_); advanced_->setPlainText(draft_); scheduleSave();
    }

    void moveStep(int offset) {
        const int row = steps_->currentRow(), target = row + offset;
        if (row < 0 || target < 0 || target >= steps_->rowCount()) return;
        auto* type = static_cast<QComboBox*>(steps_->cellWidget(row, 0));
        auto* value = static_cast<QLineEdit*>(steps_->cellWidget(row, 1));
        auto* otherType = static_cast<QComboBox*>(steps_->cellWidget(target, 0));
        auto* otherValue = static_cast<QLineEdit*>(steps_->cellWidget(target, 1));
        const auto savedType = type->currentText(), savedValue = value->text();
        loading_ = true; type->setCurrentText(otherType->currentText()); value->setText(otherValue->text());
        otherType->setCurrentText(savedType); otherValue->setText(savedValue); loading_ = false;
        steps_->selectRow(target); updateSmart();
    }

    void playSequence(const QString& sequence) {
        auto file = std::make_shared<QTemporaryFile>();
        if (!file->open()) { message(QStringLiteral("Could not create action preview file."), true); return; }
        const auto bytes = (QStringLiteral("[hotkeys]\nCtrl+Alt+F10 = ") + sequence + '\n').toUtf8();
        if (file->write(bytes) != bytes.size() || !file->flush()) { message(QStringLiteral("Could not write action preview."), true); return; }
        const auto path = file->fileName(); file->close();
        submit({QStringLiteral("check-actions"), {QStringLiteral("check-actions"), path},
            [this, file, path](bool ok, const QString&, const QString& error) {
                if (!ok) { message(error, true); return; }
                submit({QStringLiteral("play-actions"), {QStringLiteral("play-actions"), path},
                    [this, file](bool ok, const QString& output, const QString& error) {
                        if (!ok) { message(error, true); return; }
                        const auto result = QJsonDocument::fromJson(output.toUtf8()).object();
                        if (!result.value(QStringLiteral("accepted")).toBool()) {
                            message(QStringLiteral("The backend did not confirm accepting this action sequence."), true); return;
                        }
                        if (result.value(QStringLiteral("completed")).toBool()) message(QStringLiteral("Action sequence completed."));
                        else message(QStringLiteral("Action #%1 queued. Execution has not been confirmed; check backend action status.")
                            .arg(result.value(QStringLiteral("action_id")).toInt()));
                    }});
            }});
    }

    QWidget* ringPage() {
        auto* page = new QWidget; auto* layout = new QVBoxLayout(page);
        auto* text = new QLabel(QStringLiteral("Actions Ring opens a native radial menu. Select with the mouse, number keys, or arrows and Enter.\n"
            "Folders open a submenu; Escape returns or closes. Assign Ring to any button or Smart Action."), page);
        text->setWordWrap(true); layout->addWidget(text);
        auto* preview = new QPushButton(QStringLiteral("Open Actions Ring"), page); preview->setObjectName(QStringLiteral("previewRing"));
        layout->addWidget(preview);
        auto* defaults = new QPushButton(QStringLiteral("Create default ring"), page); defaults->setObjectName(QStringLiteral("defaultRing")); layout->addWidget(defaults);
        connect(defaults, &QPushButton::clicked, this, [this] {
            if (!loo_ui::readRing(draft_).isEmpty()) { message(QStringLiteral("A ring already exists; its items were kept.")); return; }
            draft_ += QStringLiteral("\n[ring]\nPlay / Pause = PlayPause\nMedia = Folder(Media)\nMute = Mute\nShow Desktop = Super+D\n"
                "\n[ring/Media]\nPrevious Track = PreviousTrack\nNext Track = NextTrack\n");
            setEditors(draft_); scheduleSave();
        });
        connect(preview, &QPushButton::clicked, this, [this] {
            auto* ring = new loo_ui::RingDialog(loo_ui::readRing(draft_), this);
            ring->setAttribute(Qt::WA_DeleteOnClose);
            ring->selected = [this](const QString& action) { playSequence(action); };
            ring->show(); ring->activateWindow();
        });
        layout->addStretch(); return page;
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

    void setEditors(const QString& text, bool updateSteps = true) {
        loading_ = true;
        if (advanced_->toPlainText() != text) advanced_->setPlainText(text);
        for (const auto& raw : text.split('\n')) {
            const auto line = raw.trimmed();
            if (!line.startsWith('[') || !line.endsWith(']')) continue;
            const auto name = line.mid(1, line.size()-2).toLower();
            if (name == QStringLiteral("hotkeys") || name == QStringLiteral("ring") || name.startsWith(QStringLiteral("ring/"))) continue;
            if (profile_->findData(name) < 0) profile_->addItem(name, name);
        }
        auto entries = globalBindings(text);
        const auto overrides = globalBindings(text, profile_->currentData().toString());
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) entries[it.key()] = it.value();
        for (auto it = bindings_.cbegin(); it != bindings_.cend(); ++it)
            it.value()->setCurrentText(entries.value(it.key()));
        if (updateSteps) loadSmart();
        loading_ = false;
        syncCanvases();
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
                message(ok ? QStringLiteral("已应用并核对设备设置。") : error, !ok);
                if(ok)updatePointerSummary();
                if (!ok) refreshAll();
            }});
    }

    void updateThumbInvert() {
        auto* check = checks_.value(QStringLiteral("thumb-invert"));
        check->setEnabled(thumbInvertSupported_);
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
                setEditors(QString::fromUtf8(bytes), false);
                if (start) { startBackend(); return; }
                if (!backendRunning_) {
                    message(QStringLiteral("Saved. Start the backend to apply mappings."));
                    return;
                }
                submit({QStringLiteral("reload"), {QStringLiteral("reload"), config_},
                    [this](bool ok, const QString& output, const QString& error) {
                        const bool notified = QJsonDocument::fromJson(output.toUtf8()).object().value(QStringLiteral("reloaded")).toBool();
                        message(ok ? (notified ? QStringLiteral("Saved and applied by the running backend.")
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
    QLabel *device_{}, *agent_{}, *status_{}, *battery_{};
    QHBoxLayout* profileBar_{};
    QWidget* profileEditor_{};
    QTabWidget* pages_{};
    loo_ui::DeviceCanvas *buttonCanvas_{},*pointerCanvas_{};
    QSpinBox *dpi_{}, *smartShift_{};
    QSlider* slider_{};
    QPlainTextEdit* advanced_{};
    QPushButton *start_{}, *stop_{};
    QComboBox *profile_{}, *trigger_{};
    QLineEdit* profileName_{};
    QKeySequenceEdit* hotkey_{};
    QFormLayout* smartForm_{};
    QTableWidget* steps_{};
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

#ifdef LOGI_GUI_TEST
int interactionTests() {
    if (qEnvironmentVariableIsSet("LOGI_TEST_REQUIRE_NATIVE_WAYLAND") &&
        !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        qCritical("GUI interaction test requires the actual native Wayland platform"); return 29;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) return 10;
    const auto path = directory.filePath(QStringLiteral("mappings.ini"));
    const auto initial = QString::fromLatin1(fallback_config) + QStringLiteral(
        "\n[ring]\nMedia = Folder(Media)\nMute = Mute\n[ring/Media]\nNext = NextTrack\nPrevious = PreviousTrack\n");
    if (!writeFile(path, initial.toUtf8())) return 11;
    Window window(path, true, false); // Real asynchronous validator, no HID queries or backend start.
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
        const auto* handle = window.windowHandle();
        auto* screen = handle ? handle->screen() : QGuiApplication::primaryScreen();
        qCritical() << "GUI exposure failure: platform=" << QGuiApplication::platformName()
            << "visible=" << window.isVisible() << "exposed=" << (handle && handle->isExposed())
            << "visibility=" << (handle ? int(handle->visibility()) : -1)
            << "geometry=" << window.geometry() << "screen=" << (screen ? screen->geometry() : QRect{})
            << "active=" << QApplication::activeWindow() << "modal=" << QApplication::activeModalWidget()
            << "popup=" << QApplication::activePopupWidget();
        const auto evidence = qEnvironmentVariable("LOGI_TEST_EVIDENCE");
        if (!evidence.isEmpty() && screen && QDir().mkpath(evidence)) {
            const auto root = screen->grabWindow(0);
            if (root.isNull() || !root.save(QDir(evidence).filePath(QStringLiteral("gui-exposure-failure-root.png"))))
                qWarning("The platform could not capture the root window after exposure failure.");
        }
        return 12;
    }
    const auto evidence = qEnvironmentVariable("LOGI_TEST_EVIDENCE");
    auto screenshot = [&](QWidget& widget, const QString& name) {
        if (evidence.isEmpty()) return true;
        return QDir().mkpath(evidence) && widget.grab().save(QDir(evidence).filePath(name));
    };
    if (!screenshot(window, QStringLiteral("linux-home.png"))) return 26;
    QTest::mouseClick(window.findChild<QToolButton*>(QStringLiteral("openDevice")),Qt::LeftButton);
    if (!screenshot(window, QStringLiteral("linux-main-settings.png"))) return 26;
    auto await = [](const std::function<bool()>& condition) {
        QElapsedTimer elapsed; elapsed.start();
        while (!condition() && elapsed.elapsed() < 10000) QTest::qWait(20);
        return condition();
    };
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("pages"));
    QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("openAddProfile")),Qt::LeftButton);
    auto* profileName = window.findChild<QLineEdit*>(QStringLiteral("profileName"));
    QTest::mouseClick(profileName, Qt::LeftButton); QTest::keyClicks(profileName, "firefox");
    QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("addProfile")), Qt::LeftButton);
    if (!await([&] { return readFile(path).contains(QStringLiteral("[firefox]")); })) return 13;
    QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav1")),Qt::LeftButton);
    QTest::mouseClick(tabs->widget(1)->findChild<QPushButton*>(QStringLiteral("hotspot_4")),Qt::LeftButton);
    auto* mapping = window.findChild<QComboBox*>(QStringLiteral("binding_0053"));
    QTest::mouseClick(mapping->lineEdit(), Qt::LeftButton);
    QTest::keyClick(mapping->lineEdit(), Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(mapping->lineEdit(), "Ctrl+Tab");
    if (!await([&] { return globalBindings(readFile(path), QStringLiteral("firefox")).value(QStringLiteral("0053")) == QStringLiteral("Ctrl+Tab"); })) return 14;
    if (globalBindings(readFile(path)).value(QStringLiteral("0053")) != QStringLiteral("MouseBack")) return 15;
    if (!screenshot(window,QStringLiteral("linux-button-sidebar.png")))return 30;
    QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav0")),Qt::LeftButton);
    QTest::mouseClick(tabs->widget(0)->findChild<QPushButton*>(QStringLiteral("hotspot_1")),Qt::LeftButton);
    if (!screenshot(window,QStringLiteral("linux-pointer-sidebar.png")))return 31;
    QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("navSmart")),Qt::LeftButton);
    QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("addStep")), Qt::LeftButton);
    auto* steps = window.findChild<QTableWidget*>(QStringLiteral("smartSteps"));
    auto* value = static_cast<QLineEdit*>(steps->cellWidget(steps->rowCount()-1, 1));
    QTest::mouseClick(value, Qt::LeftButton); QTest::keyClick(value, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(value, "240");
    if (!await([&] { return globalBindings(readFile(path), QStringLiteral("firefox")).value(QStringLiteral("0052")).endsWith(QStringLiteral("Wait(240)")); })) return 16;
    if (!screenshot(window, QStringLiteral("linux-smart-actions.png"))) return 27;
    const auto valid = readFile(path);
    QTest::keyClick(value, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(value, "not-a-number");
    auto* status = window.findChild<QLabel*>(QStringLiteral("statusMessage"));
    if (!await([&] { return status->text().contains(QStringLiteral("Invalid"), Qt::CaseInsensitive); })) return 17;
    if (readFile(path) != valid) return 18;
    QTest::keyClick(value, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(value, "300");
    if (!await([&] { return readFile(path).contains(QStringLiteral("Wait(300)")); })) return 19;
    QString selected;
    loo_ui::RingDialog ring(loo_ui::readRing(initial)); ring.selected = [&](const QString& action) { selected = action; };
    ring.show();ring.activateWindow(); if (!QTest::qWaitForWindowExposed(&ring)||!QTest::qWaitForWindowActive(&ring)) return 20;
    QTest::mouseClick(ring.findChild<QPushButton*>(QStringLiteral("ringItem0")), Qt::LeftButton);
    QTest::qWait(10);
    if (ring.findChild<QPushButton*>(QStringLiteral("ringBack"))->text() != QStringLiteral("Back")) return 21;
    if (!screenshot(ring, QStringLiteral("linux-ring-submenu.png"))) return 28;
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    QTest::qWait(10);
    if (ring.findChild<QPushButton*>(QStringLiteral("ringBack"))->text() != QStringLiteral("Close")) return 22;
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_1); QTest::qWait(10);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Right);
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    if (selected != QStringLiteral("PreviousTrack") || ring.isVisible()) return 23;
    loo_ui::RingDialog cancel(loo_ui::readRing(initial)); cancel.show();cancel.activateWindow();
    if (!QTest::qWaitForWindowExposed(&cancel)||!QTest::qWaitForWindowActive(&cancel)) return 24;
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    if (cancel.isVisible()) return 25;
    window.close();
    qInfo("Qt interaction tests passed: profile isolation, edits, durable save, rejected invalid config, ring mouse/keyboard/navigation/cancel.");
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("logi-options-open"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0-alpha.2"));
#ifdef LOGI_GUI_TEST
    const int result = interactionTests();
    if (result) qCritical("Qt interaction test failed at checkpoint %d", result);
    return result;
#endif
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
    const auto arguments = application.arguments();
    const auto ringIndex = arguments.indexOf(QStringLiteral("--ring"));
    if (ringIndex >= 0) {
        if (ringIndex+1 >= arguments.size()) { qCritical("--ring requires a configuration path"); return 2; }
        const auto folderIndex = arguments.indexOf(QStringLiteral("--folder"));
        const auto folder = folderIndex >= 0 && folderIndex+1 < arguments.size() ? arguments[folderIndex+1] : QString{};
        loo_ui::RingDialog ring(loo_ui::readRing(readFile(arguments[ringIndex+1])), nullptr, folder);
        if (!ring.error().isEmpty()) { qCritical("%s", qPrintable(ring.error())); return 2; }
        application.setQuitOnLastWindowClosed(false);
        bool executing = false;
        QObject::connect(&ring, &QDialog::finished, &application, [&](int) { if (!executing) application.quit(); });
        ring.selected = [&](const QString& action) {
            executing = true;
            auto file = std::make_shared<QTemporaryFile>();
            if (!file->open()) { application.exit(1); return; }
            const auto content = (QStringLiteral("[hotkeys]\nCtrl+Alt+F10 = ") + action + '\n').toUtf8();
            if (file->write(content) != content.size() || !file->flush()) { application.exit(1); return; }
            const auto path = file->fileName(); file->close();
            auto* command = new QProcess(&application);
            auto cli = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logi-cli"));
            if (!QFileInfo::exists(cli)) cli = QStandardPaths::findExecutable(QStringLiteral("logi-cli"));
            command->setProgram(cli); command->setArguments({QStringLiteral("play-actions"), path});
            QObject::connect(command, &QProcess::finished, &application, [&, command, file](int code, QProcess::ExitStatus status) {
                if (status != QProcess::NormalExit || code != 0) {
                    qCritical("%s", command->readAllStandardError().constData());
                    application.exit(status == QProcess::NormalExit ? code : 1); return;
                }
                const auto result = QJsonDocument::fromJson(command->readAllStandardOutput()).object();
                if (!result.value(QStringLiteral("accepted")).toBool()) {
                    qCritical("The backend did not confirm accepting the selected ring action."); application.exit(1); return;
                }
                if (result.value(QStringLiteral("completed")).toBool()) qInfo("Ring action completed.");
                else qInfo("Ring action #%d queued; execution has not been confirmed.", result.value(QStringLiteral("action_id")).toInt());
                application.exit(0);
            });
            QObject::connect(command, &QProcess::errorOccurred, &application, [&, file](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) application.exit(1);
            });
            QTimer::singleShot(150, command, [command] { command->start(); });
        };
        ring.show(); ring.activateWindow();
        return application.exec();
    }
    Window window(configPath());
    window.show();
    return application.exec();
}
