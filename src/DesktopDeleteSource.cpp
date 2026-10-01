#include "DesktopDeleteSource.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace {
constexpr UINT kNotificationMessage = WM_APP + 88;
constexpr int kSnapshotIntervalMs = 2000;

QString knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) return {};
    const QString path = QDir::cleanPath(QString::fromWCharArray(value));
    CoTaskMemFree(value);
    return path;
}

// The desktop icons live in an Explorer-owned SysListView32. The desktop
// Progman window normally hosts SHELLDLL_DefView directly, but after wallpaper
// changes Explorer sometimes re-parents it under a WorkerW, so both are tried.
HWND findDesktopListView() {
    HWND defView = nullptr;
    if (const HWND progman = FindWindowW(L"Progman", nullptr))
        defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defView) {
        HWND worker = nullptr;
        while (!defView && (worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)))
            defView = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
    }
    if (!defView) return nullptr;
    return FindWindowExW(defView, nullptr, L"SysListView32", L"FolderView");
}

// The list view belongs to Explorer, so LVM_GETITEMPOSITION's output pointer
// must live in Explorer's address space: allocate remotely, let the message
// fill it, read it back. SendMessage marshals only the message parameters,
// never the buffer they point at.
struct RemoteReader {
    DWORD pid = 0;
    HANDLE process = nullptr;
    void* point = nullptr;
    void* item = nullptr;
    void* text = nullptr;

    bool open(HWND listview) {
        GetWindowThreadProcessId(listview, &pid);
        process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
                                  | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
        if (!process) return false;
        point = VirtualAllocEx(process, nullptr, sizeof(POINT), MEM_COMMIT, PAGE_READWRITE);
        item = VirtualAllocEx(process, nullptr, sizeof(LVITEMW), MEM_COMMIT, PAGE_READWRITE);
        text = VirtualAllocEx(process, nullptr, 520, MEM_COMMIT, PAGE_READWRITE);
        return point && item && text;
    }
    ~RemoteReader() {
        for (void* block : {point, item, text})
            if (block) VirtualFreeEx(process, block, 0, MEM_RELEASE);
        if (process) CloseHandle(process);
    }
};

// Shell coordinates are physical pixels; Qt global coordinates are logical.
// Convert with the DPI of the screen the point ends up on (best effort: the
// first guess uses the primary screen's ratio, then the screen lookup refines
// it -- good enough for a lunge direction that only needs coarse precision).
QPointF toLogical(const POINT& pt) {
    double ratio = 1.0;
    if (const QScreen* primary = QGuiApplication::primaryScreen())
        ratio = primary->devicePixelRatio();
    if (ratio <= 0.0) ratio = 1.0;
    QPointF logical(pt.x / ratio, pt.y / ratio);
    if (const QScreen* screen = QGuiApplication::screenAt(logical.toPoint())) {
        const double screenRatio = screen->devicePixelRatio();
        if (screenRatio > 0.0 && !qFuzzyCompare(screenRatio, ratio))
            logical = QPointF(pt.x / screenRatio, pt.y / screenRatio);
    }
    return logical;
}
} // namespace

DesktopDeleteSource::DesktopDeleteSource(QObject* parent) : QObject(parent) {
    connect(&snapshotTimer_, &QTimer::timeout, this, &DesktopDeleteSource::refreshSnapshot);
    snapshotTimer_.setInterval(kSnapshotIntervalMs);
}

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
    // Prime the snapshot immediately: a deletion can arrive before the first
    // timer tick, and without a snapshot the pet would miss icon and position.
    refreshSnapshot();
    snapshotTimer_.start();
    qInfo() << "Watching desktop folders:" << desktopPaths_;
    return true;
}

void DesktopDeleteSource::refreshSnapshot() {
    const HWND listview = findDesktopListView();
    if (!listview) return;
    const int count = ListView_GetItemCount(listview);
    if (count <= 0) return;

    RemoteReader reader;
    if (!reader.open(listview)) return;

    // Map what the list view shows (display names, extensions hidden by
    // default) onto real desktop paths. Both the plain name and the name
    // without its final suffix are registered, matching either Explorer
    // setting "hide known extensions".
    QHash<QString, QString> byName;  // lowercase display name -> absolute path
    for (const QString& dirPath : desktopPaths_) {
        QDirIterator it(dirPath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            byName.insert(info.fileName().toLower(), info.absoluteFilePath());
            if (!info.completeBaseName().isEmpty() && !info.suffix().isEmpty())
                byName.insert(info.completeBaseName().toLower(), info.absoluteFilePath());
        }
    }

    QHash<QString, DesktopItem> fresh;
    for (int i = 0; i < count; ++i) {
        LVITEMW request{};
        request.mask = LVIF_TEXT;
        request.iSubItem = 0;
        request.pszText = reinterpret_cast<LPWSTR>(reader.text);
        request.cchTextMax = 260;
        WriteProcessMemory(reader.process, reader.item, &request, sizeof(request), nullptr);
        SendMessageW(listview, LVM_GETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(reader.item));
        wchar_t text[260] = {};
        ReadProcessMemory(reader.process, reader.text, text, sizeof(text), nullptr);

        SendMessageW(listview, LVM_GETITEMPOSITION, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(reader.point));
        POINT pt{};
        ReadProcessMemory(reader.process, reader.point, &pt, sizeof(pt), nullptr);

        const QString name = QString::fromWCharArray(text).trimmed();
        if (name.isEmpty()) continue;
        const QString path = byName.value(name.toLower());
        if (path.isEmpty()) continue;  // special icons (This PC, Recycle Bin, ...)

        const QString key = path.toLower();
        DesktopItem entry{toLogical(pt), QIcon()};
        // Icons are expensive to fetch, so carry over the cached one whenever
        // the previous snapshot saw the same file.
        const auto cached = snapshot_.constFind(key);
        if (cached != snapshot_.constEnd() && !cached->icon.isNull()) entry.icon = cached->icon;
        else entry.icon = QFileIconProvider().icon(QFileInfo(path));
        fresh.insert(key, entry);
    }
    snapshot_ = fresh;
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
            const QString deleted = QDir::cleanPath(QString::fromWCharArray(path));
            const QString parent = QDir::cleanPath(QFileInfo(deleted).absolutePath());
            if (desktopPaths_.contains(parent, Qt::CaseInsensitive)) {
                const auto entry = snapshot_.constFind(deleted.toLower());
                qInfo() << "Desktop item deleted:" << deleted
                        << (entry == snapshot_.constEnd() ? QStringLiteral("no snapshot entry")
                                                          : QStringLiteral("snapshot hit"))
                        << entry->pos;
                emit desktopItemDeleted(deleted,
                    entry == snapshot_.constEnd() ? QPointF() : entry->pos,
                    entry == snapshot_.constEnd() ? QIcon() : entry->icon);
            }
        }
    }
    SHChangeNotification_Unlock(lock);
    return false;
}
