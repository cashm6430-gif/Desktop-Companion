#include "DesktopDeleteSource.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace {
constexpr UINT kNotificationMessage = WM_APP + 88;

QString knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) return {};
    const QString path = QDir::cleanPath(QString::fromWCharArray(value));
    CoTaskMemFree(value);
    return path;
}
}

DesktopDeleteSource::DesktopDeleteSource(QObject* parent) : QObject(parent) {}

DesktopDeleteSource::~DesktopDeleteSource() {
    if (registration_) SHChangeNotifyDeregister(registration_);
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool DesktopDeleteSource::start(HWND targetWindow) {
    targetWindow_ = targetWindow;
    QStringList paths;
    for (const QString& path : {knownFolder(FOLDERID_Desktop), knownFolder(FOLDERID_PublicDesktop)}) {
        if (!path.isEmpty() && !paths.contains(path, Qt::CaseInsensitive)) paths.append(path);
    }
    desktopPaths_ = paths;
    if (paths.isEmpty()) { error_ = QStringLiteral("known folders unavailable"); return false; }

    PIDLIST_ABSOLUTE pidls[2] = {nullptr, nullptr};
    SHChangeNotifyEntry entries[2] = {};
    for (qsizetype i = 0; i < paths.size(); ++i) {
        const QString nativePath = QDir::toNativeSeparators(paths[i]);
        pidls[i] = ILCreateFromPathW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
        if (!pidls[i]) {
            error_ = QStringLiteral("PIDL unavailable: ") + paths[i];
            for (PIDLIST_ABSOLUTE pidl : pidls) if (pidl) ILFree(pidl);
            return false;
        }
        entries[i].pidl = pidls[i];
        entries[i].fRecursive = FALSE;
    }
    registration_ = SHChangeNotifyRegister(targetWindow_,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_DELETE | SHCNE_RMDIR, kNotificationMessage,
        static_cast<int>(paths.size()), entries);
    for (PIDLIST_ABSOLUTE pidl : pidls) if (pidl) ILFree(pidl);
    if (!registration_) {
        error_ = QStringLiteral("SHChangeNotifyRegister failed: ") + QString::number(GetLastError());
        qWarning() << "Failed to register desktop Shell notifications";
        return false;
    }
    QCoreApplication::instance()->installNativeEventFilter(this);
    qInfo() << "Watching desktop folders:" << desktopPaths_;
    return true;
}

bool DesktopDeleteSource::nativeEventFilter(const QByteArray&, void* message, qintptr*) {
    const auto* msg = static_cast<MSG*>(message);
    if (msg->hwnd != targetWindow_ || msg->message != kNotificationMessage) return false;
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(msg->wParam),
        static_cast<DWORD>(msg->lParam), &pidls, &event);
    if (!lock) return false;
    if ((event & (SHCNE_DELETE | SHCNE_RMDIR)) && pidls && pidls[0]) {
        wchar_t path[32768] = {};
        if (SHGetPathFromIDListEx(pidls[0], path, 32768, GPFIDL_DEFAULT)) {
            const QString parent = QDir::cleanPath(QFileInfo(QString::fromWCharArray(path)).absolutePath());
            if (desktopPaths_.contains(parent, Qt::CaseInsensitive)) emit desktopItemDeleted();
        }
    }
    SHChangeNotification_Unlock(lock);
    return false;
}
