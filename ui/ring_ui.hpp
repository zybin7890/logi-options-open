#pragma once
#include <QDialog>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <functional>

class QPushButton;
class QLabel;
class QKeyEvent;
class QShowEvent;
namespace loo_ui {
struct RingEntry { QString label, action; };
using RingMenus = QMap<QString, QVector<RingEntry>>;
RingMenus readRing(const QString& text);
class RingDialog final : public QDialog {
public:
    explicit RingDialog(RingMenus menus, QWidget* parent = nullptr, const QString& folder = {});
    std::function<void(const QString&)> selected;
    QString error() const { return error_; }
protected:
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    void render();
    void choose(int index);
    void back();
    void highlight(int index);
    RingMenus menus_;
    QStringList stack_{QStringLiteral("ring")};
    QVector<QPushButton*> choices_;
    QLabel* heading_{};
    QPushButton* center_{};
    QString error_;
    int current_{};
};
}
