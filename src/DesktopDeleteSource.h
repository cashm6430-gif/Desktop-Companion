#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QStringList>
#include <windows.h>

class DesktopDeleteSource final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit DesktopDeleteSource(QObject* parent = nullptr);
    ~DesktopDeleteSource() override;
    bool start(HWND targetWindow);
    bool isWatching() const { return registration_ != 0; }
    QStringList desktopPaths() const { return desktopPaths_; }
    QString error() const { return error_; }
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void desktopItemDeleted();

private:
    QStringList desktopPaths_;
    QString error_;
    HWND targetWindow_ = nullptr;
    ULONG registration_ = 0;
};
