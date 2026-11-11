#pragma once

#include <QPixmap>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>

class QPushButton;

namespace loo_ui {
enum class DeviceMode { Buttons, Pointer };

// A shared device scene: controls and nearby labels use the original native
// illustration's local coordinates, independent of the surrounding window.
class DeviceCanvas final : public QWidget {
public:
    explicit DeviceCanvas(QWidget* parent = nullptr);
    void setMode(DeviceMode mode);
    void setSelected(int index);
    void setLabels(const QStringList& labels);
    DeviceMode mode() const { return mode_; }
    int selectedIndex() const { return selected_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    std::function<void(int)> selected;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void rebuild();
    void arrange();
    void activate(int index);
    DeviceMode mode_{DeviceMode::Buttons};
    int selected_{-1};
    QStringList labels_;
    QPixmap mouse_;
    QRectF image_;
    QVector<QPushButton*> cards_, pins_;
    QVector<QPointF> anchors_;
};
}
