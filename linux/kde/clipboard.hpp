#pragma once
#include <QObject>
#include <QString>
#include <memory>

// Native Wayland data-control, not an X11 clipboard or a focused helper window.
class WaylandClipboard final : public QObject {
    Q_OBJECT
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit WaylandClipboard(QObject* parent=nullptr);
    ~WaylandClipboard() override;
    bool available() const;
    QString reason() const;
    quint32 setText(const QString& text,quint64 targetPid);
    void clear();
signals:
    // The native foreground process requested and received the MIME payload.
    // Clipboard-manager prefetch is deliberately excluded. This is not an assertion
    // that the destination application inserted the text into its document.
    void consumed(quint32 serial);
};
