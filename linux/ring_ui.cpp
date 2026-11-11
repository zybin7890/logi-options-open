#include "ring_ui.hpp"
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <cmath>

namespace loo_ui {
RingMenus readRing(const QString& text) {
    RingMenus menus;
    QString section;
    for (const auto& raw : text.split('\n')) {
        const auto line = raw.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        if (section != QStringLiteral("ring") && !section.startsWith(QStringLiteral("ring/"))) continue;
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';')) continue;
        const auto equals = line.indexOf('=');
        if (equals > 0) menus[section].push_back({line.left(equals).trimmed(), line.mid(equals + 1).trimmed()});
    }
    return menus;
}

RingDialog::RingDialog(RingMenus menus, QWidget* parent, const QString& folder) : QDialog(parent), menus_(std::move(menus)) {
    setObjectName(QStringLiteral("actionsRing"));
    setWindowTitle(QStringLiteral("Actions Ring"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setFixedSize(600, 600);
    setStyleSheet(QStringLiteral("QDialog{background:#191d1d;border:1px solid #52635f;border-radius:24px;}"
        "QLabel{color:#dce7e3;font-size:14px;}QPushButton{background:#303b38;color:white;border:2px solid #52635f;"
        "border-radius:24px;padding:8px;font-size:14px;}QPushButton:hover,QPushButton:focus{border-color:#00d0b6;}"
        "QPushButton[selected=true]{background:#006e61;border-color:#00d0b6;}"));
    heading_ = new QLabel(this);
    heading_->setGeometry(140, 18, 320, 40);
    heading_->setAlignment(Qt::AlignCenter);
    center_ = new QPushButton(QStringLiteral("Close"), this);
    center_->setObjectName(QStringLiteral("ringBack"));
    center_->setGeometry(242, 270, 116, 60);
    connect(center_, &QPushButton::clicked, this, [this] { back(); });
    center_->installEventFilter(this);
    auto* help = new QLabel(QStringLiteral("1–8 select · arrows move · Enter run · Esc back"), this);
    help->setGeometry(40, 552, 520, 28); help->setAlignment(Qt::AlignCenter);
    for (auto it = menus_.cbegin(); it != menus_.cend(); ++it) {
        if (it.value().size() > 8) { error_ = QStringLiteral("A ring supports at most eight items."); break; }
        for (const auto& entry : it.value()) {
            if (entry.action.startsWith(QStringLiteral("Folder(")) && entry.action.endsWith(')')) {
                const auto target = QStringLiteral("ring/") + entry.action.mid(7, entry.action.size() - 8);
                if (!menus_.contains(target) || menus_.value(target).isEmpty()) error_ = QStringLiteral("Missing ring folder: ") + target;
            }
        }
    }
    if (menus_.value(QStringLiteral("ring")).isEmpty()) error_ = QStringLiteral("No Actions Ring items configured.");
    if (!folder.isEmpty()) {
        const auto key = QStringLiteral("ring/") + folder;
        if (!menus_.contains(key)) error_ = QStringLiteral("Missing ring folder: ") + folder;
        else stack_.push_back(key);
    }
    render();
    if (auto* screen = QGuiApplication::screenAt(QCursor::pos())) {
        const auto area = screen->availableGeometry();
        move(qBound(area.left(), QCursor::pos().x() - width()/2, qMax(area.left(), area.right() - width() + 1)),
             qBound(area.top(), QCursor::pos().y() - height()/2, qMax(area.top(), area.bottom() - height() + 1)));
    }
}

void RingDialog::render() {
    for (auto* button : choices_) { button->hide(); button->deleteLater(); }
    choices_.clear();
    heading_->setText(error_.isEmpty() ? (stack_.size() > 1 ? stack_.back().mid(5) : QStringLiteral("Actions Ring")) : error_);
    heading_->setWordWrap(true);
    center_->setText(stack_.size() > 1 ? QStringLiteral("Back") : QStringLiteral("Close"));
    if (!error_.isEmpty()) return;
    const auto& entries = menus_[stack_.back()];
    for (int i = 0; i < entries.size(); ++i) {
        const double angle = -1.5707963267948966 + i * 6.283185307179586 / entries.size();
        auto* button = new QPushButton(QString::number(i+1) + QStringLiteral("  ") + entries[i].label, this);
        button->setObjectName(QStringLiteral("ringItem%1").arg(i));
        button->setGeometry(qRound(300 + 204 * std::cos(angle) - 74), qRound(300 + 204 * std::sin(angle) - 34), 148, 68);
        connect(button, &QPushButton::clicked, this, [this, i] { choose(i); });
        button->installEventFilter(this);
        choices_.push_back(button); button->show();
    }
    highlight(0);
}

void RingDialog::highlight(int index) {
    if (choices_.isEmpty()) return;
    current_ = (index + choices_.size()) % choices_.size();
    for (int i = 0; i < choices_.size(); ++i) {
        choices_[i]->setProperty("selected", i == current_);
        choices_[i]->setStyleSheet(i == current_ ? QStringLiteral("background:#006e61;border-color:#00d0b6;") : QString{});
    }
    choices_[current_]->setFocus();
}

void RingDialog::choose(int index) {
    if (!error_.isEmpty() || index < 0 || index >= menus_.value(stack_.back()).size()) return;
    const auto action = menus_.value(stack_.back())[index].action;
    if (action.startsWith(QStringLiteral("Folder(")) && action.endsWith(')')) {
        const auto target = QStringLiteral("ring/") + action.mid(7, action.size() - 8);
        if (stack_.contains(target)) { heading_->setText(QStringLiteral("Folder cycle; use Back to return.")); return; }
        stack_.push_back(target); render();
    } else {
        hide(); // Return focus before injecting a selected keyboard action.
        if (selected) selected(action);
        accept();
    }
}

void RingDialog::back() { if (stack_.size() > 1) { stack_.removeLast(); render(); } else reject(); }
bool RingDialog::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() >= Qt::Key_1 && key->key() <= Qt::Key_8) || key->key() == Qt::Key_Escape ||
            key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
            key->key() == Qt::Key_Up || key->key() == Qt::Key_Down || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            keyPressEvent(key); return true;
        }
    }
    return QDialog::eventFilter(object, event);
}
void RingDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Backspace) { back(); return; }
    if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_8) { choose(event->key()-Qt::Key_1); return; }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up) { highlight(current_-1); return; }
    if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Down) { highlight(current_+1); return; }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { choose(current_); return; }
    QDialog::keyPressEvent(event);
}
}
