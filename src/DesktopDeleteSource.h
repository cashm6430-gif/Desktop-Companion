#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPointF>
#include <QStringList>
#include <QTimer>
#include <windows.h>

// Watches the desktop folders for deletions (SHCNE_DELETE / SHCNE_RMDIR).
// Besides the event itself it keeps a snapshot of the icon grid: by the time
// SHCNE_DELETE arrives the file is already gone from disk, so the real icon
// and its grid position can only come from a snapshot taken while the file
// still existed. The pet uses both to lunge towards the deleted file and to
// show its icon before rolling it into a wrap.
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
    // `globalPos` is the icon centre in Qt logical global coordinates and
    // `icon` is the icon the file had while it existed. Both are empty/null
    // when the snapshot has no entry for the path (added and deleted between
    // snapshot ticks, or the desktop list view could not be read).
    void desktopItemDeleted(const QString& path, const QPointF& globalPos, const QIcon& icon);

private:
    struct DesktopItem {
        QPointF pos;  // logical (Qt) global coordinates
        QIcon icon;
    };
    void refreshSnapshot();

    QStringList desktopPaths_;
    QString error_;
    HWND targetWindow_ = nullptr;
    ULONG registration_ = 0;
    QTimer snapshotTimer_;
    // Key: cleaned path in lowercase. Desktop paths compare case-insensitively
    // on Windows, and SHCNE_DELETE hands back its own casing.
    QHash<QString, DesktopItem> snapshot_;
};
