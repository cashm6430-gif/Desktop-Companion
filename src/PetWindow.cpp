#include "PetWindow.h"
#ifdef HAVE_CUBISM
#include "CubismCanvas.h"
#endif
#include "ScreenPlacement.h"
#include "StartupTrace.h"
#include "TrayMenuStyle.h"
#include "WorkReactionCoordinator.h"
#include "IntegrationStatusPanel.h"
#include "MemoStore.h"
#include "StickyNoteModel.h"
#include "overlay/FrameOverlay.h"

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QAbstractNativeEventFilter>
#include <QContextMenuEvent>
#include <QCursor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <numbers>
#include <QGuiApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QtMath>
#include <QUrl>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>

// System-wide hotkeys via Win32 RegisterHotKey. A desktop pet is almost never
// the foreground window, so a Qt shortcut can't fire; RegisterHotKey posts
// WM_HOTKEY to our HWND regardless of focus and the app-wide native event
// filter catches it. Failure (combo already taken) is non-fatal -- the caller
// simply runs without the hotkey.
class GlobalHotkeys : public QAbstractNativeEventFilter {
public:
    // Returns the number of hotkeys actually registered.
    int registerDefaults(HWND hwnd) {
        hwnd_ = hwnd;
        if (!hwnd_) return 0;
        // MOD_NOREPEAT so holding the key does not toggle repeatedly.
        registered_ = RegisterHotKey(hwnd_, kToggleId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
        return registered_ ? 1 : 0;
    }
    void unregisterDefaults() {
        if (hwnd_ && registered_) UnregisterHotKey(hwnd_, kToggleId);
        hwnd_ = nullptr;
        registered_ = false;
    }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        auto* msg = static_cast<MSG*>(message);
        if (msg && msg->message == WM_HOTKEY && msg->wParam == kToggleId && onToggle) {
            onToggle();
            return true;
        }
        return false;
    }
    std::function<void()> onToggle;
private:
    static constexpr int kToggleId = 1;
    HWND hwnd_ = nullptr;
    bool registered_ = false;
};
#endif

namespace {
QString workPostureKey(ParameterMotion::WorkPosturePreference preference) {
    switch (preference) {
    case ParameterMotion::WorkPosturePreference::Standing: return QStringLiteral("standing");
    case ParameterMotion::WorkPosturePreference::Laptop: return QStringLiteral("laptop");
    default: return QStringLiteral("automatic");
    }
}

ParameterMotion::WorkPosturePreference workPostureFromKey(const QString& key) {
    if (key == QStringLiteral("standing")) return ParameterMotion::WorkPosturePreference::Standing;
    if (key == QStringLiteral("laptop")) return ParameterMotion::WorkPosturePreference::Laptop;
    return ParameterMotion::WorkPosturePreference::Automatic;
}

QString imagePath(const char* name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/") + QLatin1String(name));
}

#ifdef Q_OS_WIN
// True when a window other than the shell or the pet covers the whole monitor
// and owns the foreground -- a game, a slideshow, a full-screen video. The pet
// suppresses new automatic reactions while that app owns the screen.
bool foregroundCoversScreen(HWND self) {
    const HWND fg = GetForegroundWindow();
    if (!fg || fg == self) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    const QString name = QString::fromWCharArray(cls);
    if (name == QStringLiteral("Progman") || name == QStringLiteral("WorkerW")
        || name == QStringLiteral("Shell_TrayWnd"))
        return false;
    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;
    const HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top
        && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
#endif

// ------------------------------------------------------- comic thought bubble
// The "?" bubble is window-layer art: no drawable in the MOC3 paints it, so it
// has to be composed into every frame the pet presents, live and captured.
// Geometry is expressed as a fraction of the widget because one renderer feeds
// both the 280 px desktop window and the 840 px approval captures.
//
// The anchor is fixed instead of following the head. A pose probe (yaw +-45,
// pitch/tilt +-30 at 840 px) shifted the head silhouette by only a few pixels,
// so a follow would be invisible in the desktop window while still coupling the
// bubble to parameters that have nothing to do with it. The pop and the fade
// carry the life instead.
constexpr double kBubbleCenterX = 0.765;
constexpr double kBubbleCenterY = 0.145;
constexpr double kBubbleRadius = 0.084;

// --------------------------------------------------------- sticky note paper
// Motion card 5: the note is window-layer art, same reasoning as the bubble.
// One painter for the full note and the corner badge so both read as the
// same object. Geometry is a fraction of the widget, like the bubble's.
void paintStickyPaper(QPainter& painter, const QRectF& rect, const QString& text) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    // Soft drop shadow lifts the paper off the scene.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 36));
    painter.drawRoundedRect(rect.translated(2, 3), 4, 4);
    // Paper with a folded top-right corner (a dog-ear).
    const qreal fold = qMin(rect.width(), rect.height()) * 0.30;
    painter.setPen(QPen(QColor(0xE8, 0xD4, 0x4D), 1.2));
    painter.setBrush(QColor(0xFF, 0xF6, 0xA8));
    painter.drawPolygon(QPolygonF{rect.topLeft(),
                                  QPointF(rect.right() - fold, rect.top()),
                                  QPointF(rect.right(), rect.top() + fold),
                                  rect.bottomRight(),
                                  rect.bottomLeft()});
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xEA, 0xD9, 0x7A));
    painter.drawPolygon(QPolygonF{QPointF(rect.right() - fold, rect.top()),
                                  QPointF(rect.right(), rect.top() + fold),
                                  QPointF(rect.right() - fold, rect.top() + fold)});
    if (!text.isEmpty()) {
        const QRectF textRect = rect.adjusted(rect.width() * 0.08, rect.height() * 0.10,
                                              -rect.width() * 0.08, -rect.height() * 0.08);
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPixelSize(std::clamp(int(rect.height() * 0.20), 10, 18));
        painter.setFont(font);
        painter.setPen(QColor(0x4A, 0x42, 0x34));
        painter.setClipRect(textRect);
        // The paper is a compact summary; the edit dialog keeps the full text.
        const QString summary = QFontMetrics(font).elidedText(
            text.simplified(), Qt::ElideRight, int(textRect.width()));
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, summary);
    }
    painter.restore();
}

// The comic-thought cloud shared by the "?" and the sleep "z Z Z" bubbles:
// trailing bubbles down towards the head, then a lobe-union cloud body (drawn
// as one path -- overlapping ellipses painted separately would show every
// internal edge). Returns the geometry so the caller paints its own content.
struct CloudGeometry {
    double cx, cy, radius;
};

CloudGeometry paintComicCloud(QPainter& painter, const QSize& size, double pulse) {
    // qMin/qMax, not std::min/std::max: windows.h defines min and max as
    // macros, so the std:: forms do not survive this translation unit.
    const double unit = qMin(size.width(), size.height());
    const double cx = size.width() * kBubbleCenterX;
    const double cy = size.height() * kBubbleCenterY;
    // The overshoot in the envelope briefly grows the bubble past its resting
    // size, which is what makes it read as popping in rather than fading up.
    const double scale = 0.72 + 0.28 * qMin(pulse, 1.3);
    const double radius = unit * kBubbleRadius * scale;
    const QColor ink(0x26, 0x35, 0x5a);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(qBound(0.0, pulse, 1.0));
    QPen outline(ink);
    outline.setWidthF(qMax(1.4, unit * 0.0045 * scale));
    outline.setJoinStyle(Qt::RoundJoin);
    outline.setCapStyle(Qt::RoundCap);
    painter.setPen(outline);
    painter.setBrush(Qt::white);

    // Two little bubbles trail back down towards the head, the way a comic
    // thought balloon is drawn, so the content is read as the pet's own.
    painter.drawEllipse(QPointF(cx - 1.26 * radius, cy + 0.64 * radius),
                        0.29 * radius, 0.29 * radius);
    painter.drawEllipse(QPointF(cx - 1.64 * radius, cy + 1.00 * radius),
                        0.16 * radius, 0.16 * radius);

    auto lobe = [&](double dx, double dy, double rx, double ry) {
        QPainterPath path;
        path.addEllipse(QPointF(cx + dx * radius, cy + dy * radius),
                        rx * radius, ry * radius);
        return path;
    };
    QPainterPath cloud = lobe(0.0, 0.0, 1.04, 0.86);
    cloud = cloud.united(lobe(-0.54, -0.30, 0.58, 0.52));
    cloud = cloud.united(lobe(0.56, -0.20, 0.52, 0.48));
    cloud = cloud.united(lobe(0.10, 0.42, 0.60, 0.46));
    painter.drawPath(cloud);
    painter.restore();
    return {cx, cy, radius};
}

void paintThoughtBubble(QPainter& painter, const QSize& size, double pulse) {
    if (pulse <= 0.001) return;
    const CloudGeometry g = paintComicCloud(painter, size, pulse);
    const QColor accent(0xd0, 0x94, 0x2a);
    painter.save();
    // "?" -- the one part of the bubble that carries the meaning.
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setBold(true);
    font.setPixelSize(qMax(9, qRound(g.radius * 1.15)));
    painter.setFont(font);
    painter.setPen(QPen(accent));
    painter.drawText(QRectF(g.cx - g.radius, g.cy - g.radius * 0.86,
                            2.0 * g.radius, 1.72 * g.radius),
                     Qt::AlignCenter, QStringLiteral("?"));
    painter.restore();
}

// The sleeping bubble: the same cloud, carrying three ascending z's instead
// of a question mark. Rides a slow bob so the doze reads as alive.
void paintSleepBubble(QPainter& painter, const QSize& size, double pulse, double bobPhase) {
    if (pulse <= 0.001) return;
    const double unit = qMin(size.width(), size.height());
    const double bob = std::sin(bobPhase) * unit * 0.006;
    painter.translate(0.0, bob);
    const CloudGeometry g = paintComicCloud(painter, size, pulse);
    const QColor accent(0xd0, 0x94, 0x2a);
    painter.save();
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setBold(true);
    painter.setPen(QPen(accent));
    // Three z's climbing to the upper right: small, medium, large.
    const struct { double dx, dy, scale; } zs[] = {
        {-0.58, +0.38, 0.62},
        {-0.02, -0.04, 0.95},
        { 0.52, -0.44, 1.30},
    };
    for (const auto& z : zs) {
        font.setPixelSize(qMax(7, qRound(g.radius * z.scale)));
        painter.setFont(font);
        const double half = g.radius * z.scale * 0.62;
        painter.drawText(QRectF(g.cx + z.dx * g.radius - half,
                                g.cy + z.dy * g.radius - half, 2.0 * half, 2.0 * half),
                         Qt::AlignCenter, QStringLiteral("Z"));
    }
    painter.restore();
}
} // namespace

// Flight of the deleted file's icon from its desktop position towards the
// pet's fist. A QWidget can only paint inside itself, and the deleted file
// usually sits outside the pet window, so the first leg of the eat
// choreography (up to drawFedProp's grip moment) rides on this small
// transparent, click-through overlay; there it hands the icon over to the
// in-window wrap sequence at the very same global position and size.
// The flight duration is `delete.grip` from delete.motion.json, passed in by the
// window that owns the motion: the handover has to land on the authored grip, so
// a second hard-coded copy here would drift away from the clip.
// Defined at global scope (not in an anonymous namespace) so it matches the
// forward declaration in PetWindow.h.
class DeleteOverlay final : public QWidget {
public:
    explicit DeleteOverlay(QWidget* parent)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                              | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus
                              | Qt::WindowTransparentForInput) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(96, 96);
        tick_.setInterval(16);
        connect(&tick_, &QTimer::timeout, this, [this] {
            const double t = clock_.elapsed() / 1000.0;
            if (t >= duration_) { hide(); tick_.stop(); return; }
            step();
            update();
        });
    }

    // `from`/`to` in logical global coordinates; the clock starts now, in the
    // same event-loop beat as the pet's fed clock so both stay in step.
    void launch(const QPixmap& icon, const QPointF& from, const QPointF& to, double duration) {
        if (icon.isNull()) return;
        icon_ = icon;
        from_ = from;
        to_ = to;
        duration_ = qMax(0.05, duration);
        clock_.restart();
        step();
        show();
        raise();
        tick_.start();
    }

    void stop() {
        tick_.stop();
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setOpacity(fadeAlpha_);  // brief fade-in
        const QRectF target((96.0 - iconSize_) / 2.0, (96.0 - iconSize_) / 2.0,
                            iconSize_, iconSize_);
        painter.drawPixmap(target, icon_, icon_.rect());
    }

private:
    void step() {
        const double t = clock_.elapsed() / 1000.0;
        const double k = qBound(0.0, t / duration_, 1.0);
        const double ease = k * k * (3.0 - 2.0 * k);
        const QPointF p = from_ + (to_ - from_) * ease
            - QPointF(0.0, 46.0 * qSin(M_PI * k));  // a slight arc over the desktop
        iconSize_ = 64.0 - 36.0 * ease;             // 64 px at the file, 28 px at the fist
        fadeAlpha_ = qBound(0.0, k * 6.0, 1.0);     // fade in over the first ~0.1 s
        move((p - QPointF(48.0, 48.0)).toPoint());
    }

    QPixmap icon_;
    QPointF from_;
    QPointF to_;
    double duration_ = 0.65; // replaced by delete.grip at launch
    double iconSize_ = 64.0;
    double fadeAlpha_ = 0.0;
    QElapsedTimer clock_;
    QTimer tick_;
};

PetWindow::PetWindow(PetController* controller, QWidget* parent)
    : QWidget(parent),
      idleImage_(imagePath("idle.png")),
      busyImage_(imagePath("busy.png")),
      deleteImage_(imagePath("delete.png")),
#ifdef HAVE_CUBISM
      cubismHitMask_(imagePath("live2d/whale-girl/hit-mask.png")),
#endif
      trayMenu_(this),
      controller_(controller) {
    const bool review = qApp->property("desktopCompanionReviewCapture").toBool();
    const QString reviewMemoPath = qApp->property("desktopCompanionReviewMemoPath").toString();
    if (!review || !reviewMemoPath.isEmpty()) {
        const QString path = review ? reviewMemoPath
            : QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                .filePath(QStringLiteral("DesktopCompanion/WhaleGirl/memo.json"));
        memoStore_ = std::make_unique<MemoStore>(path);
    }
    memo_ = std::make_unique<StickyNoteModel>(memoStore_.get());
    memoStoreOk_ = memo_->restore();
    memoReceiptClock_.start();
    setWindowTitle(QStringLiteral("DeepSeek 鲸鱼娘"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                   | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(280, 280);
    // The character area is the drop target for the feed gesture. Accepting
    // drops registers the OLE drop target for the whole window; the dynamic
    // WS_EX_TRANSPARENT click-through keeps only the character interactive,
    // so a file dropped beside the pet lands on the desktop as before.
    setAcceptDrops(true);
    setMouseTracking(true);

    // Window-layer overlay pipeline, in paint order. Guard and draw live in
    // one registration row, so adding a graphic cannot resurrect the old
    // frameWithBubble early-return trap (a layer without its guard being
    // permanently invisible).
    overlays_.add(std::make_shared<FunctionOverlay>(
        [](const FrameContext& ctx) { return ctx.bubblePulse > 0.001; },
        [](QPainter& painter, const QSize& size, const FrameContext& ctx) {
            paintThoughtBubble(painter, size, ctx.bubblePulse);
        }));
    // The sticky note rides the same composition path so the interaction mask
    // (built from the composed frame) makes exactly the paper clickable.
    overlays_.add(std::make_shared<FunctionOverlay>(
        [this](const FrameContext&) { return memo_->visual() != StickyNoteModel::Visual::None; },
        [this](QPainter& painter, const QSize& size, const FrameContext&) {
            drawMemoNote(painter, size);
        }));
    // Same for the sleep mask: in the mask, so touching the face wakes the nap.
    overlays_.add(std::make_shared<FunctionOverlay>(
        [this](const FrameContext&) { return motion_.sleepMaskEnvelope() > 0.001; },
        [this](QPainter& painter, const QSize& size, const FrameContext&) {
            drawSleepMask(painter, size);
        }));
    // The cardboard box hides the standing body (box hide-and-seek r1). It
    // paints above the character and below nothing else user-facing; the
    // per-pixel mask picks it up so clicks land exactly on its face.
    overlays_.add(std::make_shared<FunctionOverlay>(
        [this](const FrameContext&) { return motion_.boxPeekSlide() > 0.001; },
        [this](QPainter& painter, const QSize& size, const FrameContext&) {
            drawBoxProp(painter, size);
        }));
    // The rice bowl sits on the desk of the seated laptop loop (aroma break
    // r1); the delight floaters are painted by the same overlay, animated.
    overlays_.add(std::make_shared<FunctionOverlay>(
        [this](const FrameContext&) { return motion_.riceBowlSlide() > 0.001; },
        [this](QPainter& painter, const QSize& size, const FrameContext&) {
            drawRiceBowlProp(painter, size);
        }));

#ifdef HAVE_CUBISM
    // Keep OpenGL composition in a separate window. This translucent widget
    // remains raster-backed so Windows can compose its per-pixel alpha.
    cubismCanvas_ = new CubismCanvas(&motion_);
    cubismCanvas_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
                                  | Qt::WindowDoesNotAcceptFocus);
    cubismCanvas_->setFixedSize(size());
    cubismCanvas_->move(-10000, -10000);
    connect(cubismCanvas_, &CubismCanvas::readyChanged, this, [this] {
        setState(controller_->state());
    });
    cubismCanvas_->show();
#endif

    QString motionError;
    motion_.setDeskWorkMode(qApp->property("desktopCompanionDeskWorkMode").toBool());
    if (!motion_.loadMotionLibrary(imagePath("motions"), &motionError))
        qWarning() << "Motion library:" << motionError;
    // The state machine gets its durations from the same clips that drive the
    // motion, so a retimed asset can never desync the fallback timer.
    controller_->setActionDuration(PetController::State::Delete,
                                   motion_.actionDuration(PetController::State::Delete));
    controller_->setActionDuration(PetController::State::Grass,
                                   motion_.actionDuration(PetController::State::Grass));

    // Frame interval: user-selectable from the tray ("性能") and persisted.
    // Larger interval = fewer fps = less GPU. Clamp against a bad stored value.
    {
        QSettings perf(QSettings::defaultFormat(), QSettings::UserScope,
            QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
        frameIntervalMs_ = std::clamp(
            perf.value(QStringLiteral("frameIntervalMs"), 40).toInt(), 10, 200);
        quietPolicy_.setNightEnabled(
            perf.value(QStringLiteral("quietAtNight"), true).toBool());
        memo_->setAutomaticEnabled(perf.value(QStringLiteral("automaticWorkNotes"), true).toBool());
    }
    frameTimer_.setInterval(frameIntervalMs_);
    frameClock_.start();
    deleteOverlay_ = new DeleteOverlay(this);
    // Desktop deletions carry the file's snapshot (path, icon-grid position,
    // icon); drops arrive here too, through the shared triggerEat entry.
    connect(controller_, &PetController::eatTriggered,
            this, &PetWindow::onEatTriggered);
    // Share this ordering with the controller-to-player regression tests.
    // Stop/Interrupt do not prove success, so automatic work-end gets one
    // neutral return-gaze. The approved cheer remains an explicit preview.
    new WorkReactionCoordinator(controller_, &motion_, this,
        [this](PetController::State state) { setState(state); },
        [this](qint64 workedMs) { recordWorkDuration(workedMs); },
        [](const QString& id, bool started, const QString& reason) {
            startup::trace(QStringLiteral("work reaction %1: %2 (%3)")
                .arg(id, started ? QStringLiteral("started") : QStringLiteral("refused"), reason));
        }, [this] { return allowAutomaticReactions(); });
    connect(&frameTimer_, &QTimer::timeout, this, [this] {
        ++frame_;
        const double seconds = frameClock_.restart() / 1000.0;
        advanceLiveFrame(seconds);
    });
    frameTimer_.start();
    setState(controller_->state());

    // Spontaneous-scene scheduler: the same start code paths as the tray
    // menu, driven by the director's nominations every second.
    director_.setStartBox([this] { return motion_.playBoxPeek(); });
    director_.setStartWave([this] { return startWave(); });
    director_.setStartNap([this] { return startNap(); });
    director_.setStartStretch([this] {
        if (controller_->state() != PetController::State::Busy) return false;
        return motion_.playStretch();
    });
    director_.setStartRice([this] { return startRiceBowl(); });
    connect(&spontaneousTimer_, &QTimer::timeout, this, [this] {
        director_.tick(1.0, controller_->state() == PetController::State::Idle,
                       controller_->state() == PetController::State::Busy,
                       allowAutomaticReactions());
    });
    spontaneousTimer_.start(1000);

    // Poll once per second even while hidden; real turn state keeps updating.
    connect(&displayWatchTimer_, &QTimer::timeout, this, [this] { updateFullscreenPresence(); });
    displayWatchTimer_.start(1000);
    updateFullscreenPresence();

    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
    // Restore the saved scale first, so the position fallback below uses it.
    renderSize_ = std::clamp(settings.value(QStringLiteral("renderSize"), 280).toInt(), 180, 560);
    setFixedSize(renderSize_, renderSize_);
#ifdef HAVE_CUBISM
    if (cubismCanvas_) cubismCanvas_->setFixedSize(size());
#endif
    const QPoint saved = settings.value(QStringLiteral("position")).toPoint();
    if (!saved.isNull()) move(saved);
    else if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect r = screen->availableGeometry();
        move(r.right() - width() - 24, r.bottom() - height() - 24);
    }
    // A position saved before this contract existed can be off the right edge by
    // up to 184 px, which parks the thought bubble off-screen for good -- she
    // then looks broken from the first work turn of the session. Heal it here
    // rather than waiting for the user to drag her back. Resolve the screen from
    // the saved point itself: QWidget::screen() is only reliable once the window
    // is shown, and a pet parked on a second monitor must heal onto THAT monitor
    // instead of jumping to the primary one.
    const QScreen* target = QGuiApplication::screenAt(pos());
    if (!target) target = QGuiApplication::primaryScreen();
    if (target) {
        const QPoint healed = pet_placement::clampToScreen(target->availableGeometry(), size(), pos());
        if (healed != pos()) move(healed);
    }
    director_.setFrequency(static_cast<TriggerDirector::Frequency>(
        settings.value(QStringLiteral("spontaneousFrequency"),
                       int(TriggerDirector::Frequency::Normal)).toInt()));
    // Review commands use explicit poses or their own isolated preferences.
    // A saved live choice must not change legacy approval/capture trajectories.
    if (!review || qApp->property("desktopCompanionReviewSettingsIsolated").toBool())
        motion_.setWorkPosturePreference(workPostureFromKey(
            settings.value(QStringLiteral("workPosture"), QStringLiteral("automatic")).toString()));

    tray_.setIcon(QIcon(idleImage_.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    tray_.setToolTip(windowTitle());
    // The menu used to be a flat list of ~20 entries. Group by intent so it
    // reads at a glance; submenus are added in reading order. The review-only
    // previews live under 预览（开发用） so end users never see them mixed in.
    trayMenu_.setStyleSheet(tray_style::menuStyleSheet());
    toggleVisibilityAction_ = trayMenu_.addAction(QStringLiteral("显示 / 隐藏"), this, [this] { toggleVisibility(); });
    trayMenu_.addSeparator();
    auto* lookMenu = trayMenu_.addMenu(QStringLiteral("互动"));
    auto* workMenu = trayMenu_.addMenu(QStringLiteral("工作与休息"));
    workPostureMenu_ = workMenu->addMenu(QStringLiteral("工作姿态"));
    workPostureMenu_->setObjectName(QStringLiteral("workPostureMenu"));
    auto* postureGroup = new QActionGroup(workPostureMenu_);
    const std::array postureChoices{
        std::pair{ParameterMotion::WorkPosturePreference::Automatic, QStringLiteral("自动")},
        std::pair{ParameterMotion::WorkPosturePreference::Standing, QStringLiteral("站着陪伴")},
        std::pair{ParameterMotion::WorkPosturePreference::Laptop, QStringLiteral("抱电脑")},
    };
    for (const auto& [preference, label] : postureChoices) {
        auto* action = workPostureMenu_->addAction(label);
        const QString key = workPostureKey(preference);
        action->setObjectName(QStringLiteral("workPosture")
            + key.left(1).toUpper() + key.mid(1));
        action->setData(key);
        action->setCheckable(true);
        action->setChecked(motion_.workPosturePreference() == preference);
        postureGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, preference] {
            setWorkPosturePreference(preference);
        });
    }
    connect(workPostureMenu_, &QMenu::aboutToShow, this, [this] {
        const QString key = workPostureKey(motion_.workPosturePreference());
        for (auto* action : workPostureMenu_->actions())
            action->setChecked(action->data().toString() == key);
    });
    integrationStatusAction_ = workMenu->addAction(QStringLiteral("联动状态…"), this, [this] {
        if (!integrationStatusProvider_) return;
        if (!integrationStatusPanel_)
            integrationStatusPanel_ = new IntegrationStatusPanel(
                [this] { return integrationStatusProvider_(); },
                [this] { if (integrationResetBusy_) integrationResetBusy_(); }, this);
        integrationStatusPanel_->refresh();
        integrationStatusPanel_->show();
        integrationStatusPanel_->raise();
        integrationStatusPanel_->activateWindow();
    });
    integrationStatusAction_->setEnabled(false);
    auto* settingMenu = trayMenu_.addMenu(QStringLiteral("设置"));
    auto* previewMenu = trayMenu_.addMenu(QStringLiteral("预览（开发用）"));
    previewMenu->addAction(QStringLiteral("模拟文件删除"), this, [this] {
        controller_->desktopItemDeleted();
    });
    lookMenu->addAction(QStringLiteral("玩狗尾巴草"), this, &PetWindow::playGrass);
    interactionPreviewAction_ = previewMenu->addAction(QStringLiteral("新互动（待审批）"));
    interactionPreviewAction_->setCheckable(true);
    connect(interactionPreviewAction_, &QAction::toggled, this, &PetWindow::setInteractionPreviewEnabled);
    if (!motion_.grassTipDragApproved()) {
        grassDragPreviewAction_ = previewMenu->addAction(QStringLiteral("软草穗跟手（待审批）"));
        grassDragPreviewAction_->setObjectName(QStringLiteral("grassDragPreviewAction"));
        grassDragPreviewAction_->setCheckable(true);
        connect(grassDragPreviewAction_, &QAction::toggled, this, &PetWindow::setGrassDragPreviewEnabled);
    }
    if (!grassInteractionApproved()) {
        previewMenu->addAction(QStringLiteral("草穗接招"), this, [this] {
            setInteractionPreviewEnabled(true);
            startGrassInteraction();
        });
    }
    previewMenu->addAction(QStringLiteral("收工回望"), this, [this] {
        if (edgeDock_.active() || controller_->state() != PetController::State::Idle) return;
        setInteractionPreviewEnabled(true);
        motion_.playTurnEnded();
    });
    const auto* permissionClip = motion_.library().clip(QStringLiteral("permission-cue"));
    const bool permissionApproved = permissionClip && permissionClip->approval() == QStringLiteral("approved");
    auto* permissionCue = previewMenu->addAction(permissionApproved
        ? QStringLiteral("审批请求轻提示（仅忙碌）")
        : QStringLiteral("审批请求轻提示（待审批，仅忙碌）"), this, [this, permissionApproved] {
        if (edgeDock_.active() || controller_->state() != PetController::State::Busy) return;
        if (!permissionApproved) setInteractionPreviewEnabled(true);
        motion_.playPermissionCue(!permissionApproved);
    });
    permissionCue->setObjectName(QStringLiteral("permissionCuePreviewAction"));
    const auto* standingRestClip = motion_.library().clip(QStringLiteral("busy-rest"));
    if (!standingRestClip || standingRestClip->approval() != QStringLiteral("approved")) {
        previewMenu->addAction(QStringLiteral("站姿歇一下（待审批，仅站姿忙碌）"), this, [this] {
            if (edgeDock_.active() || controller_->state() != PetController::State::Busy || motion_.isLaptopBusy()) return;
            setInteractionPreviewEnabled(true);
            motion_.playStretch(true);
        });
    }
    workMenu->addAction(QStringLiteral("歇一下"), this, [this] {
        if (edgeDock_.active() || controller_->state() != PetController::State::Busy) return;
        motion_.playStretch();
    });
    lookMenu->addAction(QStringLiteral("眯一会儿（眼罩）"), this, [this] {
        startNap();
    });
    auto* memoMenu = lookMenu->addMenu(QStringLiteral("便笺"));
    memoStorageStatusAction_ = memoMenu->addAction(QStringLiteral("便笺操作未完成，原内容已保留"));
    memoStorageStatusAction_->setEnabled(false);
    memoStorageStatusAction_->setObjectName(QStringLiteral("memoStorageStatusAction"));
    memoCreateAction_ = memoMenu->addAction(QStringLiteral("放个便笺…"), this, &PetWindow::editStickyNote);
    memoCreateAction_->setObjectName(QStringLiteral("memoCreateAction"));
    memoReopenAction_ = memoMenu->addAction(QStringLiteral("展开便笺"), this, [this] {
        if (!reopenMemo()) reportMemoFailure();
    });
    memoReopenAction_->setObjectName(QStringLiteral("memoReopenAction"));
    memoFoldAction_ = memoMenu->addAction(QStringLiteral("先收起来"), this, [this] {
        if (!foldMemo()) reportMemoFailure();
    });
    memoFoldAction_->setObjectName(QStringLiteral("memoFoldAction"));
    memoCompleteAction_ = memoMenu->addAction(QStringLiteral("完成了"), this, [this] {
        if (!finishMemo()) reportMemoFailure();
    });
    memoCompleteAction_->setObjectName(QStringLiteral("memoCompleteAction"));
    memoDiscardAction_ = memoMenu->addAction(QStringLiteral("移除便笺"), this, [this] {
        if (!discardMemo()) reportMemoFailure();
    });
    memoDiscardAction_->setObjectName(QStringLiteral("memoDiscardAction"));
    connect(memoMenu, &QMenu::aboutToShow, this, &PetWindow::refreshMemoActions);
    automaticWorkNotesAction_ = workMenu->addAction(QStringLiteral("记录回合耗时"));
    automaticWorkNotesAction_->setObjectName(QStringLiteral("automaticWorkNotesAction"));
    automaticWorkNotesAction_->setCheckable(true);
    automaticWorkNotesAction_->setChecked(memo_->automaticEnabled());
    connect(automaticWorkNotesAction_, &QAction::toggled, this, &PetWindow::setAutomaticWorkNotesEnabled);
    refreshMemoActions();
    QAction* boxAction = lookMenu->addAction(QStringLiteral("躲一下"), this, [this] {
        // Toggle: the same entry starts the peek and calls her back out.
        if (motion_.boxPeekActive()) exitBoxPeek();
        else startBoxPeek();
    });
    connect(&trayMenu_, &QMenu::aboutToShow, this, [this, boxAction] {
        boxAction->setText(motion_.boxPeekActive() ? QStringLiteral("不躲啦")
                                                   : QStringLiteral("躲一下"));
    });
    QAction* riceAction = lookMenu->addAction(QStringLiteral("来碗饭香"), this, [this] {
        if (edgeDock_.active()) return;
        // Toggle: the same entry starts the break and sends the bowl away.
        if (motion_.riceBowlActive()) motion_.exitRiceBowl();
        else if (controller_->state() == PetController::State::Busy) motion_.playRiceBowl();
    });
    connect(&trayMenu_, &QMenu::aboutToShow, this, [this, riceAction] {
        riceAction->setText(motion_.riceBowlActive() ? QStringLiteral("吃完啦")
                                                     : QStringLiteral("来碗饭香"));
    });
    QAction* waveAction = lookMenu->addAction(QStringLiteral("打个招呼（挥手）"), this, [this] {
        if (!edgeDock_.active()) motion_.playWave();
    });
    connect(&trayMenu_, &QMenu::aboutToShow, this, [this, waveAction] {
        // Standing idle only: seated typing keeps the hands on the keys.
        waveAction->setEnabled(controller_->state() == PetController::State::Idle
                               && !motion_.waveActive());
    });
    // Manual un-stick: a work turn only ends on Stop/SessionEnd, so a client
    // that dies hard (crashed terminal, taskkill) can leave the pet "busy" and
    // seated until the stale-turn expiry. This clears every active turn at once.
    workMenu->addAction(QStringLiteral("结束忙碌（复位）"), this, [this] {
        if (integrationResetBusy_) integrationResetBusy_();
        else controller_->resetBusy();
    });
    // Spontaneous pace (TriggerDirector): the user outranks every watcher.
    auto* paceMenu = settingMenu->addMenu(QStringLiteral("自发行为"));
    const std::array levels{
        std::pair{TriggerDirector::Frequency::Off, QStringLiteral("关闭")},
        std::pair{TriggerDirector::Frequency::Sparse, QStringLiteral("少一些")},
        std::pair{TriggerDirector::Frequency::Normal, QStringLiteral("正常")},
        std::pair{TriggerDirector::Frequency::Often, QStringLiteral("多一些")},
    };
    auto* group = new QActionGroup(paceMenu);
    for (const auto& [level, label] : levels) {
        auto* action = paceMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(director_.frequency() == level);
        action->setData(int(level));
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, level] {
            director_.setFrequency(level);
            QSettings pace(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
            pace.setValue(QStringLiteral("spontaneousFrequency"), int(level));
        });
    }
    connect(paceMenu, &QMenu::aboutToShow, paceMenu, [this, paceMenu] {
        for (QAction* a : paceMenu->actions())
            a->setChecked(director_.frequency()
                          == TriggerDirector::Frequency(a->data().toInt()));
    });
    auto* quietMenu = settingMenu->addMenu(QStringLiteral("安静策略"));
    auto* quietStatus = quietMenu->addAction(QString());
    quietStatus->setEnabled(false);
    quietMenu->addSeparator();
    quietMenu->addAction(QStringLiteral("安静 30 分钟"), this, [this] {
        quietPolicy_.snoozeFor(30LL * 60 * 1000);
    });
    quietMenu->addAction(QStringLiteral("安静 1 小时"), this, [this] {
        quietPolicy_.snoozeFor(60LL * 60 * 1000);
    });
    auto* resumeAction = quietMenu->addAction(QStringLiteral("结束临时安静"), this, [this] {
        quietPolicy_.resume();
    });
    quietMenu->addSeparator();
    auto* nightAction = quietMenu->addAction(QStringLiteral("夜间安静（23:00–06:00）"));
    nightAction->setCheckable(true);
    nightAction->setChecked(quietPolicy_.nightEnabled());
    connect(nightAction, &QAction::toggled, this, [this](bool enabled) {
        quietPolicy_.setNightEnabled(enabled);
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
            QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
        settings.setValue(QStringLiteral("quietAtNight"), enabled);
    });
    auto* fullscreenHint = quietMenu->addAction(QStringLiteral("全屏时自动安静"));
    fullscreenHint->setEnabled(false);
    connect(quietMenu, &QMenu::aboutToShow, quietMenu,
            [this, quietStatus, resumeAction] {
        const QString reason = quietPolicy_.reason();
        if (reason == QStringLiteral("snoozed")) {
            const qint64 minutes = (quietPolicy_.snoozeRemainingMs() + 59999) / 60000;
            quietStatus->setText(QStringLiteral("临时安静：还剩 %1 分钟").arg(minutes));
        } else if (reason == QStringLiteral("night")) {
            quietStatus->setText(QStringLiteral("夜间安静中"));
        } else if (reason == QStringLiteral("fullscreen")) {
            quietStatus->setText(QStringLiteral("全屏安静中"));
        } else {
            quietStatus->setText(QStringLiteral("自动回应已开启"));
        }
        resumeAction->setEnabled(quietPolicy_.snoozeRemainingMs() > 0);
    });
    // Performance: frame interval (larger = fewer fps = less power).
    auto* perfMenu = settingMenu->addMenu(QStringLiteral("性能"));
    const std::array rates{
        std::pair{66, QStringLiteral("省电（~15 fps）")},
        std::pair{40, QStringLiteral("标准（~25 fps）")},
        std::pair{16, QStringLiteral("流畅（~60 fps）")},
    };
    auto* rateGroup = new QActionGroup(perfMenu);
    for (const auto& [ms, label] : rates) {
        auto* action = perfMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(frameIntervalMs_ == ms);
        action->setData(ms);
        rateGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, ms] { setFrameInterval(ms); });
    }
    connect(perfMenu, &QMenu::aboutToShow, perfMenu, [this, perfMenu] {
        for (QAction* a : perfMenu->actions())
            a->setChecked(frameIntervalMs_ == a->data().toInt());
    });
    patPreviewTimer_.setSingleShot(true);
    connect(&patPreviewTimer_, &QTimer::timeout, this, [this] { motion_.endHeadPat(); });
    // Long-press (hold on her without dragging) greets: a distinct, art-free
    // gesture, cancelled as soon as a drag begins or the button is released.
    longPressTimer_.setSingleShot(true);
    connect(&longPressTimer_, &QTimer::timeout, this, [this] {
        if (pointerGesture_.mode() != PetPointerGesture::Mode::Pending || dragging_) return;
        const bool mayWave = controller_->state() == PetController::State::Idle
            && !motion_.sleepMotionActive();
        releasePointerGesture();
        if (mayWave) startWave();
    });
    previewMenu->addAction(QStringLiteral("摸头"), this, [this] {
        if (edgeDock_.active() || motion_.sleepMotionActive()) return;
        setInteractionPreviewEnabled(true);
        if (motion_.beginHeadPat(1.0)) patPreviewTimer_.start(1600);
    });
    laptopPreviewTimer_.setSingleShot(true);
    connect(&laptopPreviewTimer_, &QTimer::timeout, this, [this] {
        controller_->turnStopped(QStringLiteral("local-preview"), QStringLiteral("laptop"));
        // Other real turns can keep Busy without emitting stateChanged.
        motion_.endWorkPosturePreview();
    });
    previewMenu->addAction(QStringLiteral("抱电脑工作"), this, [this] {
        if (edgeDock_.active() || controller_->state() == PetController::State::Delete
            || controller_->state() == PetController::State::Grass) return;
        // A preview turn participates in the same concurrent-turn accounting.
        controller_->turnStarted(QStringLiteral("local-preview"), QStringLiteral("laptop"));
        motion_.forceLaptopBusy();
        laptopPreviewTimer_.start(16000);
    });
    trayMenu_.addSeparator();
    trayMenu_.addAction(QStringLiteral("退出"), this, [this] {
        shutdown();
        qApp->quit();
    });
    tray_.setContextMenu(&trayMenu_);
    connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            if (isVisible()) hide(); else show();
        }
    });
    tray_.show();

#ifdef Q_OS_WIN
    // System-wide hotkey: Ctrl+Alt+H toggles show/hide even when the pet is not
    // focused (a pet is almost never the foreground window). Registered against
    // the window's HWND; failure (combo taken) is non-fatal.
    hotkeys_ = new GlobalHotkeys;
    hotkeys_->onToggle = [this] { toggleVisibility(); };
    const bool hotkeyOk = hotkeys_->registerDefaults(reinterpret_cast<HWND>(winId())) > 0;
    qApp->installNativeEventFilter(hotkeys_);
    // Only advertise the key if it actually registered (the combo may be taken).
    if (hotkeyOk && toggleVisibilityAction_)
        toggleVisibilityAction_->setText(QStringLiteral("显示 / 隐藏  (Ctrl+Alt+H)"));
#endif
}

PetWindow::~PetWindow() {
#ifdef Q_OS_WIN
    if (hotkeys_) {
        qApp->removeNativeEventFilter(hotkeys_);
        hotkeys_->unregisterDefaults();
        delete hotkeys_;
        hotkeys_ = nullptr;
    }
#endif
#ifdef HAVE_CUBISM
    delete cubismCanvas_;
#endif
}

void PetWindow::setFrameInterval(int ms) {
    frameIntervalMs_ = std::clamp(ms, 10, 200);
    frameTimer_.setInterval(frameIntervalMs_);
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
    settings.setValue(QStringLiteral("frameIntervalMs"), frameIntervalMs_);
}

void PetWindow::setWorkPosturePreference(ParameterMotion::WorkPosturePreference preference) {
    // Normalize unknown values, and store a stable key independent of enum order.
    const QString key = workPostureKey(preference);
    motion_.setWorkPosturePreference(workPostureFromKey(key));
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
    settings.setValue(QStringLiteral("workPosture"), key);
    if (workPostureMenu_)
        for (auto* action : workPostureMenu_->actions())
            action->setChecked(action->data().toString() == key);
}

QJsonObject PetWindow::workPostureReviewState() const {
    return {{QStringLiteral("preference"), workPostureKey(motion_.workPosturePreference())},
        {QStringLiteral("laptop_selected"), motion_.isLaptopBusy()},
        {QStringLiteral("busy_time"), motion_.busyPhaseTime()},
        {QStringLiteral("continuous_busy_time"), motion_.continuousBusyTime()},
        {QStringLiteral("next_choice_time"), motion_.nextBusyChoiceTime()},
        {QStringLiteral("desk_phase"), motion_.deskWorkPhase()},
        {QStringLiteral("desk_visible"), motion_.values().value(QStringLiteral("ParamDeskVisible"))},
        {QStringLiteral("controller_state"), int(controller_->state())},
        {QStringLiteral("active_turns"), controller_->activeTurnCount()}};
}

void PetWindow::toggleVisibility() {
    if (isVisible()) {
        hide();
    } else {
        show();
        raise();
    }
}

void PetWindow::setRenderSize(int px) {
    if (edgeDock_.active()) restoreFromEdgeDock(edgeDockNormalOrigin_);
    renderSize_ = std::clamp(px, 180, 560);
    setFixedSize(renderSize_, renderSize_);
#ifdef HAVE_CUBISM
    if (cubismCanvas_) cubismCanvas_->setFixedSize(size());
#endif
    constrainPositionToScreen();   // growing must not push her off-screen
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
    settings.setValue(QStringLiteral("renderSize"), renderSize_);
}

// On drag release, dock flush to a screen edge we were dropped near. Keeps the
// pet tidy without a hard dock; a drop away from every edge is left alone.
void PetWindow::snapToEdges() {
    const QScreen* screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect a = screen->availableGeometry();
    constexpr int kSnap = 24;
    int x = pos().x(), y = pos().y();
    if (qAbs(x - a.left()) <= kSnap) x = a.left();
    else if (qAbs(x + width() - (a.right() + 1)) <= kSnap) x = a.right() + 1 - width();
    if (qAbs(y - a.top()) <= kSnap) y = a.top();
    else if (qAbs(y + height() - (a.bottom() + 1)) <= kSnap) y = a.bottom() + 1 - height();
    if (QPoint(x, y) != pos()) move(x, y);
}

bool PetWindow::beginEdgeDock(edge_dock::Edge edge, const QRect& screen, QScreen* owner) {
    if (edgeDock_.active() || !edgeDockPreviewEnabled_ || renderBackend() != QStringLiteral("cubism_native")
        || controller_->state() == PetController::State::Delete
        || controller_->state() == PetController::State::Grass || width() != height()) return false;
    const int modelSize = width();
    const QPoint normal = pet_placement::clampToScreen(screen, QSize(modelSize, modelSize), pos());
    if (!screen.contains(QRect(normal, QSize(modelSize, modelSize)))) return false;
    if (memo_->visual() == StickyNoteModel::Visual::Bubble && !foldMemo()) return false;
    if (!edgeDock_.begin(edge, QRectF(screen), modelSize, QPointF(normal))) return false;
    edgeDockModelSize_ = modelSize;
    edgeDockNormalOrigin_ = QPointF(normal);
    edgeDockScreen_ = owner;
    edgeDockScreenGeometry_ = owner ? owner->geometry() : QRect();
    controller_->setDesktopDeletionSuppressed(true);
    setAcceptDrops(false);
    motion_.cancelInteraction();
    motion_.cancelDragMotion();
    patPreviewTimer_.stop();
    motion_.cancelSleepMotion();
    motion_.cancelMemoMotion();
    motion_.cancelBoxPeek();
    motion_.cancelRiceBowl();
    motion_.cancelWave();
    motion_.setState(PetController::State::Idle);
    lungeMirrored_ = false;
    if (deleteOverlay_) deleteOverlay_->stop();
    updateEdgeDockWindow();
    updateInputTransparency();
    return true;
}

void PetWindow::updateEdgeDockWindow() {
    if (!edgeDock_.active()) return;
    const QRect area = edgeDock_.windowGeometry();
    if (!area.isValid() || area.isEmpty()) { restoreFromEdgeDock(edgeDockNormalOrigin_); return; }
    if (size() != area.size()) setFixedSize(area.size());
    if (pos() != area.topLeft()) move(area.topLeft());
    update();
}

void PetWindow::restoreFromEdgeDock(const QPointF& origin) {
    if (!edgeDock_.active()) return;
    QRect available = edgeDock_.screen().toAlignedRect();
    if (!edgeDockFixtureScreen_.isValid() && (!edgeDockScreen_ || edgeDockScreen_->availableGeometry() != available
                              || edgeDockScreen_->geometry() != edgeDockScreenGeometry_)) {
        const QScreen* replacement = QGuiApplication::screenAt(origin.toPoint());
        if (!replacement) replacement = QGuiApplication::primaryScreen();
        if (replacement) available = replacement->availableGeometry();
    }
    const int modelSize = edgeDockModelSize_;
    edgeDock_.reset();
    edgeDockScreen_.clear();
    controller_->setDesktopDeletionSuppressed(false);
    setAcceptDrops(true);
    unsetCursor();
#ifdef HAVE_CUBISM
    if (cubismCanvas_) cubismCanvas_->setEdgeReviewBodyOffsets(0, 0);
#endif
    setFixedSize(modelSize, modelSize);
    move(pet_placement::clampToScreen(available, QSize(modelSize, modelSize), origin.toPoint()));
    setState(controller_->state()); // Restore current truth without replaying a greeting.
    if (!liveInteractionReview_) {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
            QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
        settings.setValue(QStringLiteral("position"), pos());
    }
}

void PetWindow::cancelEdgeDockDrag() {
    if (edgeDock_.phase() != EdgeDockSession::Phase::Dragging) return;
    edgeDock_.cancelDrag();
    unsetCursor();
    updateEdgeDockWindow();
}

QImage PetWindow::edgeDockFrame() const {
#ifdef HAVE_CUBISM
    if (!edgeDock_.active() || cubismFrame_.isNull() || size().isEmpty()) return {};
    const auto pose = edgeDock_.layout();
    if (!pose.valid) return {};
    QImage result(size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto& transform = pose.modelToScreen;
    painter.setWorldTransform(QTransform(transform.m11(), transform.m12(), transform.m21(), transform.m22(),
        transform.dx() - pos().x(), transform.dy() - pos().y()));
    painter.drawImage(QRectF(0, 0, edgeDockModelSize_, edgeDockModelSize_), cubismFrame_);
    return result;
#else
    return {};
#endif
}

bool PetWindow::setEdgeDockScreenForReview(const QRect& screen) {
    if (!liveInteractionReview_
        || !qApp->property("desktopCompanionReviewSettingsIsolated").toBool()) return false;
    // An empty fixture screen turns off the isolated replay. The normal
    // application has no entry point while the feature is on hold.
    if (edgeDock_.active()) {
        if (screen.isValid()) return false;
        restoreFromEdgeDock(edgeDockNormalOrigin_);
    }
    edgeDockFixtureScreen_ = screen;
    edgeDockPreviewEnabled_ = screen.isValid();
    return true;
}

QJsonObject PetWindow::edgeDockReviewState() const {
    const double size = edgeDock_.modelSize();
    const QPointF face = edgeDock_.layout().modelToScreen.map(QPointF(size * 0.5, size * 0.4));
    return {{QStringLiteral("enabled"), edgeDockPreviewEnabled_},
        {QStringLiteral("active"), edgeDock_.active()},
        {QStringLiteral("phase"), int(edgeDock_.phase())},
        {QStringLiteral("progress"), edgeDock_.progress()},
        {QStringLiteral("window_x"), x()}, {QStringLiteral("window_y"), y()},
        {QStringLiteral("window_width"), width()}, {QStringLiteral("window_height"), height()},
        {QStringLiteral("face_x"), face.x()}, {QStringLiteral("face_y"), face.y()},
        {QStringLiteral("drop_enabled"), acceptDrops()},
        {QStringLiteral("motion_state"), int(motion_.state())},
        {QStringLiteral("active_turns"), controller_->activeTurnCount()},
        {QStringLiteral("pending_permissions"), controller_->pendingPermissionCount()}};
}

void PetWindow::wheelEvent(QWheelEvent* event) {
    if (edgeDock_.active()) { event->accept(); return; }
    const int notches = event->angleDelta().y() / 120;   // 120 = one wheel step
    if (notches == 0) { QWidget::wheelEvent(event); return; }
    setRenderSize(renderSize_ + notches * 24);
    event->accept();
}

void PetWindow::updateFullscreenPresence() {
#ifdef Q_OS_WIN
    quietPolicy_.setFullscreenPresent(foregroundCoversScreen(reinterpret_cast<HWND>(winId())));
#endif
}

bool PetWindow::allowAutomaticReactions() const {
    if (edgeReviewSuppressed_ || edgeDock_.active()) return false;
    // Scripted captures replay their explicit events independently of the
    // user's night setting and current foreground app. Their director is off.
    if (qApp->property("desktopCompanionReviewCapture").toBool())
        return !qApp->property("desktopCompanionReviewSuppressAutomatic").toBool();
    return !quietPolicy_.suppressAutomatic();
}

void PetWindow::setState(PetController::State state) {
    if (edgeReviewSuppressed_ || edgeDock_.active()) return;
    if (state == PetController::State::Delete || state == PetController::State::Grass) {
        releasePointerGesture();
        if (memo_->visual() == StickyNoteModel::Visual::Bubble) {
            if (!memo_->interrupt()) memoStoreOk_ = false;
            motion_.cancelMemoMotion();
        }
    }
    motion_.setState(state);
    frame_ = 0;
    // Leaving the delete state cancels the sideways lunge: the next delete
    // re-decides the direction from its own file position.
    if (state != PetController::State::Delete) {
        lungeMirrored_ = false;
        if (deleteOverlay_) deleteOverlay_->stop();
    }
    const QPixmap* hitArtwork = nullptr;
    switch (state) {
    case PetController::State::Idle: hitArtwork = &idleImage_; break;
    case PetController::State::Busy: hitArtwork = &busyImage_; break;
    case PetController::State::Grass: hitArtwork = &idleImage_; break;
    case PetController::State::Delete: hitArtwork = &deleteImage_; break;
    }
#ifdef HAVE_CUBISM
    const bool useCubism = cubismCanvas_ && cubismCanvas_->isReady();
    if (useCubism) hitArtwork = &cubismHitMask_;
#endif
    if (hitArtwork && !hitArtwork->isNull()) setInteractionMask(*hitArtwork);
    else hitCoverage_ = QImage();
    clearMask();
    updateInputTransparency();
    update();
}

void PetWindow::setInteractionMask(const QPixmap& artwork) {
    // Keep the padded hit area separate from the visible window. A native
    // region clips soft edges and exposed a black silhouette on Windows.
    const QPixmap scaled = artwork.scaled(size(), Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
    QImage coverage(size(), QImage::Format_ARGB32_Premultiplied);
    coverage.fill(Qt::transparent);
    {
        QPainter painter(&coverage);
        constexpr int padding = 12;
        for (int y = -padding; y <= padding; y += padding / 2)
            for (int x = -padding; x <= padding; x += padding / 2)
                painter.drawPixmap(x, y, scaled);
    }
    hitCoverage_ = coverage.convertToFormat(QImage::Format_Alpha8);
}

void PetWindow::updateInputTransparency() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND window = reinterpret_cast<HWND>(winId());
    POINT cursor{};
    RECT bounds{};
    const BOOL cursorOk = GetCursorPos(&cursor);
    const BOOL boundsOk = GetWindowRect(window, &bounds);
    if (!boundsOk) return;
    const int windowWidth = bounds.right - bounds.left;
    const int windowHeight = bounds.bottom - bounds.top;
    if (windowWidth <= 0 || windowHeight <= 0) return;
    // Presence gaze: the same cursor read that drives click-through also drives
    // the pet's eyes. Normalise against the window centre. Tracking is ALWAYS
    // on (the eyes follow the cursor everywhere, saturating when far); "near"
    // now only adds the look-at-you smile, and cursor MOVEMENT -- not distance
    // -- drives the idle timer, so she stops watching only once the mouse has
    // gone quiet (see GazeBehavior).
    if (cursorOk && edgeDock_.active()) {
        const QPoint logical = QCursor::pos();
        const QPointF gaze = edgeDock_.layout().gaze(QPointF(logical));
        const bool cursorActive = !cursorSeen_
            || (qAbs(logical.x() - lastCursor_.x()) + qAbs(logical.y() - lastCursor_.y())) > 2;
        lastCursor_ = logical;
        cursorSeen_ = true;
        motion_.setGazeTarget(gaze.x(), gaze.y(), false, cursorActive);
    } else if (cursorOk) {
        const double cx = 0.5 * (bounds.left + bounds.right);
        const double cy = 0.5 * (bounds.top + bounds.bottom);
        const double nx = (cursor.x - cx) / (0.5 * windowWidth);
        const double ny = (cursor.y - cy) / (0.5 * windowHeight);
        const bool nearCursor = qAbs(nx) <= 1.2 && qAbs(ny) <= 1.2;
        const bool cursorActive = !cursorSeen_
            || (qAbs(cursor.x - lastCursor_.x()) + qAbs(cursor.y - lastCursor_.y())) > 2;
        lastCursor_ = QPoint(cursor.x, cursor.y);
        cursorSeen_ = true;
        motion_.setGazeTarget(nx, ny, nearCursor, cursorActive);
    }
    const int x = (cursor.x - bounds.left) * hitCoverage_.width() / windowWidth;
    const int y = (cursor.y - bounds.top) * hitCoverage_.height() / windowHeight;
    const bool buttonHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool interactive = ((pointerGesture_.mode() != PetPointerGesture::Mode::None || grassTouchPressed_) && buttonHeld)
        || (cursorOk && !hitCoverage_.isNull()
        && x >= 0 && y >= 0 && x < hitCoverage_.width() && y < hitCoverage_.height()
        && hitCoverage_.constScanLine(y)[x] >= 16);
#ifdef HAVE_CUBISM
    if (edgeDock_.active()) {
        interactive = edgeDock_.phase() == EdgeDockSession::Phase::Dragging
            || (cursorOk && edgeDock_.phase() == EdgeDockSession::Phase::Hidden
                && edgeDock_.layout().headHit(QCursor::pos(), cubismFrame_));
    }
#endif
    const LONG_PTR style = GetWindowLongPtr(window, GWL_EXSTYLE);
    const LONG_PTR desired = interactive ? (style & ~WS_EX_TRANSPARENT)
                                         : (style | WS_EX_TRANSPARENT);
    if (desired != style) SetWindowLongPtr(window, GWL_EXSTYLE, desired);
#endif
}

void PetWindow::shutdown() {
    if (integrationStatusPanel_) integrationStatusPanel_->hide();
    hide();
#ifdef HAVE_CUBISM
    if (cubismCanvas_) cubismCanvas_->hide();
#endif
    frameTimer_.stop();
    patPreviewTimer_.stop();
    laptopPreviewTimer_.stop();
    motion_.cancelInteraction();
    tray_.hide();
}

void PetWindow::setIntegrationStatusProvider(std::function<QJsonObject()> provider,
                                             std::function<void()> resetBusy) {
    integrationStatusProvider_ = std::move(provider);
    integrationResetBusy_ = std::move(resetBusy);
    integrationStatusAction_->setEnabled(bool(integrationStatusProvider_));
}

void PetWindow::paintEvent(QPaintEvent*) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        static bool firstPaintTraced = false;
        if (!firstPaintTraced) {
            firstPaintTraced = true;
            startup::trace(QStringLiteral("first composed frame painted"));
        }
        QPainter painter(this);
        // One composition path for the live window and the review captures, so
        // the approval frames can never show a bubble the desktop pet does not
        // draw. In Source mode the composed frame replaces the window content
        // outright, which is what the translucent widget wants.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        if (edgeDock_.active()) {
            painter.drawImage(QPoint(0, 0), edgeDockFrame());
            return;
        }
        const QImage frame = composedFrame();
        if (!frame.isNull()) painter.drawImage(rect(), frame);
        // The prop and the bubble are ordinary overlays: back to SourceOver or
        // their transparent pixels would erase the frame underneath.
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        drawFedProp(painter);
        return;
    }
#endif
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (controller_->state() == PetController::State::Delete && !deleteImage_.isNull()) {
        // Motion on a transparent key pose until a frame sequence is drawn.
        const qreal phase = qMin(frame_ / 35.0, 1.0);
        const qreal angle = 8.0 * qSin(phase * 4.0 * M_PI) * (1.0 - phase);
        painter.translate(width() / 2.0, height() * 0.8);
        painter.rotate(angle);
        painter.translate(-width() / 2.0, -height() * 0.8);
        painter.drawPixmap(rect(), deleteImage_);
    } else {
        const QPixmap& image = controller_->state() == PetController::State::Busy ? busyImage_ : idleImage_;
        const qreal bob = controller_->state() == PetController::State::Busy
            ? qSin(frame_ * 0.28) * 1.5 : qSin(frame_ * 0.11) * 2.0;
        painter.drawPixmap(QRectF(0, bob, width(), height()), image, image.rect());
    }
    drawFedProp(painter);
}

QString PetWindow::renderBackend() const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady())
        return QStringLiteral("cubism_native");
#endif
    return QStringLiteral("image_preview");
}

QString PetWindow::renderError() const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_) return cubismCanvas_->error();
#endif
    return {};
}

int PetWindow::renderSampleCount() const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_) return cubismCanvas_->sampleCount();
#endif
    return 0;
}

QJsonObject PetWindow::modelParameterRanges() const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) return cubismCanvas_->parameterRanges();
#endif
    return {};
}

QImage PetWindow::frameWithBubble(const QImage& frame, double pulse) const {
    const FrameContext ctx{pulse};
    return overlays_.compose(frame, ctx);
}

QImage PetWindow::composedFrame() const {
#ifdef HAVE_CUBISM
    // Mirroring happens here rather than on the canvas: the authored delete
    // choreography reaches with the right hand, so when the deleted file sat
    // on the pet's left the whole frame flips and the reach reads left-handed.
    const QImage base = lungeMirrored_ ? cubismFrame_.mirrored(true, false) : cubismFrame_;
    return frameWithBubble(base, motion_.bubblePulse());
#else
    return {};
#endif
}

// --- Sticky note (motion card 5) --------------------------------------------

double PetWindow::memoEnvelope() const {
    if (motion_.memoMotionActive()) return motion_.memoBubbleRise();
    // The gaze overlay ends by itself; the note stays up until the user acts
    // or the unattended timeout folds it into a corner badge.
    return memo_->riseSeen() ? 1.0 : 0.0;
}

QRectF PetWindow::memoBubbleRect(const QSizeF& s, double rise) const {
    const bool desk = motion_.values().value(QStringLiteral("ParamDeskVisible")) > 0.5;
    const double w = s.width() * 0.42;
    const double h = s.height() * 0.15;
    const double cx = s.width() * (desk ? 0.68 : 0.76);
    // Rises from the desk edge to about half a head high; without the desk it
    // floats up from the side of the body -- no special hand prop, per the card.
    const double cy = s.height() * ((desk ? 0.52 : 0.55) - rise * (desk ? 0.22 : 0.21));
    return QRectF(cx - w / 2.0, cy - h / 2.0, w, h);
}

QRectF PetWindow::memoBadgeRect(const QSizeF& s) const {
    const bool desk = motion_.values().value(QStringLiteral("ParamDeskVisible")) > 0.5;
    const double w = s.width() * 0.10;
    return QRectF(s.width() * (desk ? 0.82 : 0.84), s.height() * (desk ? 0.50 : 0.56), w, w * 0.9);
}

void PetWindow::drawMemoNote(QPainter& painter, const QSize& size) const {
    const QSizeF s(size);
    if (memo_->visual() == StickyNoteModel::Visual::Bubble) {
        const double rise = memoEnvelope();
        if (rise <= 0.001) return;
        const QRectF rect = memoBubbleRect(s, rise);
        painter.save();
        painter.setOpacity(std::clamp(rise * 3.0, 0.0, 1.0));
        // The paper scales in with the rise; a slight tilt keeps it handmade.
        painter.translate(rect.center());
        painter.rotate(-3.0);
        painter.scale(0.7 + 0.3 * rise, 0.7 + 0.3 * rise);
        painter.translate(-rect.center());
        paintStickyPaper(painter, rect, memo_->text());
        painter.restore();
    } else if (memo_->visual() == StickyNoteModel::Visual::Badge) {
        paintStickyPaper(painter, memoBadgeRect(s),
            memo_->hasUserNote() ? QStringLiteral("…") : QStringLiteral("时"));
    } else if (memo_->visual() == StickyNoteModel::Visual::Done) {
        const double t = memo_->visualAge();
        if (t >= 0.8) return;
        painter.save();
        painter.setOpacity(1.0 - t / 0.8);
        paintStickyPaper(painter, memoBadgeRect(s), QString());
        painter.restore();
    }
}

bool PetWindow::canPresentMemo() const {
    return canShowAutomaticMemo() && !motion_.interactionActive()
        && pointerGesture_.mode() == PetPointerGesture::Mode::None;
}

bool PetWindow::canShowAutomaticMemo() const {
    return !edgeDock_.active() && (controller_->state() == PetController::State::Idle
            || controller_->state() == PetController::State::Busy)
        && !motion_.sleepMotionActive() && !motion_.dragMotionActive()
        && !motion_.boxPeekActive() && !motion_.riceBowlActive()
        && !motion_.waveActive() && !motion_.workPauseActive();
}

void PetWindow::reportMemoFailure() {
    QMessageBox::warning(this, QStringLiteral("便笺未能保存"),
        memo_->error().isEmpty() ? QStringLiteral("操作未完成，原便笺已保留。") : memo_->error());
}

void PetWindow::editStickyNote() {
    bool accepted = false;
    const QString text = QInputDialog::getMultiLineText(this, QStringLiteral("便笺"),
        QStringLiteral("这件小事，先替你放这里（最多 2000 字符）："),
        memo_->hasUserNote() ? memo_->text() : QString{}, &accepted);
    if (!accepted || text.trimmed().isEmpty()) return;
    if (!setUserMemo(text)) reportMemoFailure();
}

bool PetWindow::setUserMemo(const QString& text) {
    const bool animate = canPresentMemo();
    if (!memo_->createUser(text, animate)) { memoStoreOk_ = false; return false; }
    memoStoreOk_ = true;
    motion_.cancelMemoMotion();
    if (animate) motion_.beginMemoMotion();
    refreshMemoActions();
    update();
    return true;
}

bool PetWindow::reopenMemo() {
    const bool animate = memo_->visual() == StickyNoteModel::Visual::Badge && canPresentMemo();
    if (!memo_->reopen(animate)) { memoStoreOk_ = false; refreshMemoActions(); return false; }
    if (animate && memo_->hasUserNote()) memoStoreOk_ = true;
    if (animate && memo_->visual() == StickyNoteModel::Visual::Bubble) motion_.beginMemoMotion();
    refreshMemoActions();
    update();
    return true;
}

bool PetWindow::foldMemo() {
    const bool saved = memo_->fold();
    if (!saved) memoStoreOk_ = false;
    else if (memo_->hasUserNote()) memoStoreOk_ = true;
    motion_.cancelMemoMotion();
    refreshMemoActions();
    update();
    return saved;
}

bool PetWindow::finishMemo() {
    const bool celebrate = memo_->hasUserNote() && canPresentMemo();
    if (!memo_->complete(celebrate)) {
        if (!memo_->error().isEmpty()) memoStoreOk_ = false;
        refreshMemoActions();
        return false;
    }
    memoStoreOk_ = true;
    motion_.cancelMemoMotion();
    if (celebrate) motion_.beginMemoCelebrate();
    refreshMemoActions();
    update();
    return true;
}

bool PetWindow::discardMemo() {
    const bool wasUser = memo_->hasUserNote();
    if (!memo_->discard()) { memoStoreOk_ = false; return false; }
    if (wasUser) memoStoreOk_ = true;
    motion_.cancelMemoMotion();
    refreshMemoActions();
    update();
    return true;
}

void PetWindow::setAutomaticWorkNotesEnabled(bool enabled) {
    const bool wasAutomatic = memo_->origin() == StickyNoteModel::Origin::WorkDuration;
    memo_->setAutomaticEnabled(enabled);
    if (!enabled && wasAutomatic) motion_.cancelMemoMotion();
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
        QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
    settings.setValue(QStringLiteral("automaticWorkNotes"), enabled);
    if (automaticWorkNotesAction_ && automaticWorkNotesAction_->isChecked() != enabled) {
        const QSignalBlocker blocker(automaticWorkNotesAction_);
        automaticWorkNotesAction_->setChecked(enabled);
    }
    refreshMemoActions();
    update();
}

void PetWindow::recordWorkDuration(qint64 workedMs) {
    memo_->recordDuration(workedMs, memoReceiptClock_.elapsed(),
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")), canShowAutomaticMemo());
    refreshMemoActions();
    update();
}

void PetWindow::refreshMemoActions() {
    if (!memoCreateAction_) return;
    memoStorageStatusAction_->setVisible(!memoStoreOk_);
    memoCreateAction_->setText(memo_->hasUserNote() ? QStringLiteral("查看 / 修改便笺…") : QStringLiteral("放个便笺…"));
    memoReopenAction_->setEnabled(memo_->visual() == StickyNoteModel::Visual::Badge && canPresentMemo());
    memoFoldAction_->setEnabled(memo_->visual() == StickyNoteModel::Visual::Bubble);
    memoCompleteAction_->setEnabled(memo_->hasUserNote());
    memoDiscardAction_->setEnabled(memo_->origin() != StickyNoteModel::Origin::None);
}

QJsonObject PetWindow::memoReviewState() const {
    const auto origin = memo_->origin();
    const auto visual = memo_->visual();
    const QRectF badge = memoBadgeRect(size());
    return {{QStringLiteral("origin"), origin == StickyNoteModel::Origin::User ? QStringLiteral("user")
                : origin == StickyNoteModel::Origin::WorkDuration ? QStringLiteral("work_duration") : QStringLiteral("none")},
        {QStringLiteral("visual"), visual == StickyNoteModel::Visual::Bubble ? QStringLiteral("bubble")
            : visual == StickyNoteModel::Visual::Badge ? QStringLiteral("badge")
            : visual == StickyNoteModel::Visual::Done ? QStringLiteral("done") : QStringLiteral("none")},
        {QStringLiteral("text_length"), memo_->text().size()},
        {QStringLiteral("text_sha256"), QString::fromLatin1(QCryptographicHash::hash(
            memo_->text().toUtf8(), QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("rise_seen"), memo_->riseSeen()},
        {QStringLiteral("motion_active"), motion_.memoMotionActive()},
        {QStringLiteral("work_count"), memo_->workCount()},
        {QStringLiteral("worked_ms"), memo_->workedMs()},
        {QStringLiteral("automatic_enabled"), memo_->automaticEnabled()},
        {QStringLiteral("age_seconds"), memo_->visualAge()},
        {QStringLiteral("store_ok"), memoStoreOk_},
        {QStringLiteral("badge_rect"), QJsonObject{{QStringLiteral("x"), badge.x()}, {QStringLiteral("y"), badge.y()},
            {QStringLiteral("width"), badge.width()}, {QStringLiteral("height"), badge.height()}}}};
}

bool PetWindow::handleMemoPress(const QPointF& localPos, const QPoint& globalPos) {
    const QSizeF s(size());
    if (memo_->visual() == StickyNoteModel::Visual::Bubble && memoEnvelope() > 0.9
        && memoBubbleRect(s, 1.0).contains(localPos)) {
        QMenu menu(this);
        QAction* edit = memo_->hasUserNote() ? menu.addAction(QStringLiteral("查看 / 修改便笺…")) : nullptr;
        QAction* view = memo_->hasUserNote() ? nullptr : menu.addAction(QStringLiteral("查看回合耗时…"));
        QAction* done = memo_->hasUserNote() ? menu.addAction(QStringLiteral("完成了")) : nullptr;
        QAction* later = menu.addAction(QStringLiteral("先收起来"));
        QAction* drop = menu.addAction(QStringLiteral("移除便笺"));
        QAction* picked = menu.exec(globalPos);
        if (picked && picked == edit) editStickyNote();
        else if (picked && picked == view) QMessageBox::information(this, QStringLiteral("回合耗时"), memo_->text());
        else if (picked && picked == done) { if (!finishMemo()) reportMemoFailure(); }
        else if (picked == later) { if (!foldMemo()) reportMemoFailure(); }
        else if (picked == drop) { if (!discardMemo()) reportMemoFailure(); }
        return true;
    }
    if (memo_->visual() == StickyNoteModel::Visual::Badge && memoBadgeRect(s).contains(localPos)) {
        // Re-open: the note rises again with the same glance beat.
        if (!reopenMemo()) reportMemoFailure();
        return true;
    }
    return false;
}

// --- Box hide-and-seek (optional fun scene r1) --------------------------------

bool PetWindow::startBoxPeek() {
    if (edgeDock_.active()) return false;
    if (renderBackend() != QStringLiteral("cubism_native")) return false;
    if (!motion_.playBoxPeek()) return false;
    updateInputTransparency();
    return true;
}

bool PetWindow::startRiceBowl() {
    if (edgeDock_.active()) return false;
    if (renderBackend() != QStringLiteral("cubism_native")) return false;
    if (!motion_.playRiceBowl()) return false;
    riceDelightClock_.restart();
    updateInputTransparency();
    return true;
}

bool PetWindow::startWave() {
    if (edgeDock_.active()) return false;
    if (renderBackend() != QStringLiteral("cubism_native")) return false;
    if (!motion_.playWave()) return false;
    updateInputTransparency();
    return true;
}

void PetWindow::exitRiceBowl() {
    motion_.exitRiceBowl();
    updateInputTransparency();
}

void PetWindow::exitBoxPeek() {
    motion_.exitBoxPeek();
    updateInputTransparency();
}

QRectF PetWindow::boxPeekRect() const {
    // Geometry in 840-reference fractions, exactly the concept card's
    // composite: the box top sits at neck height while she stands and the
    // bottom runs past the window bottom so the sunk feet stay covered.
    const double w = width(), h = height();
    constexpr double kBoxWidth = 500.0 / 840.0;
    constexpr double kBoxTop = 430.0 / 840.0;
    constexpr double kBoxAspect = 618.0 / 715.0;  // prop png content aspect
    const double boxW = kBoxWidth * w;
    const double boxH = boxW * kBoxAspect;
    const double slide = motion_.boxPeekSlide();
    const double x = w * 0.5 - boxW * 0.5 + (1.0 - slide) * w;
    return QRectF(x, kBoxTop * h, boxW, boxH);
}

bool PetWindow::handleBoxPress(const QPointF& localPos) {
    if (motion_.boxPeekSlide() < 0.95) return false;  // still sliding in
    if (!boxPeekRect().contains(localPos)) return false;
    motion_.boxPeekClicked();
    updateInputTransparency();
    return true;
}

void PetWindow::drawBoxProp(QPainter& painter, const QSize& size) {
    Q_UNUSED(size);  // boxPeekRect() reads width()/height(), always the same surface
    if (boxProp_.isNull()) boxProp_.load(imagePath("props/box.png"));
    if (boxProp_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawPixmap(boxPeekRect(), boxProp_, QRectF(boxProp_.rect()));
}


// --- Rice-bowl aroma break (optional fun scene r1) ----------------------------

QRectF PetWindow::riceBowlRect() const {
    // Geometry in 840-reference fractions, exactly the concept card's
    // composite: the bowl rests on the desktop surface right of the laptop.
    // The seated desk's wooden top only reaches x~771 (rounded corner pulls
    // in to ~742 at the surface rows), so the bowl's right edge sits at 755
    // and its foot contact (x620..695) stays on flat wood -- further right
    // it floats past the desk corner (user feedback r2).
    const double w = width(), h = height();
    constexpr double kBowlWidth = 175.0 / 840.0;
    constexpr double kBowlBottom = 606.0 / 840.0;
    constexpr double kBowlRightInset = 85.0 / 840.0;
    constexpr double kBowlAspect = 745.0 / 826.0;  // prop png content aspect
    const double bowlW = kBowlWidth * w;
    const double bowlH = bowlW * kBowlAspect;
    const double slide = motion_.riceBowlSlide();
    const double x = w - bowlW - kBowlRightInset * w
        + (1.0 - slide) * (bowlW + (20.0 / 840.0) * w);  // waits just off the right edge
    return QRectF(x, kBowlBottom * h - bowlH, bowlW, bowlH);
}

QRectF PetWindow::riceAromaRect() const {
    // The aroma zone rises from the rice mound; it extends ~150/840 above
    // the bowl so a tap near the bowl/aroma is "too close".
    QRectF r = riceBowlRect();
    const double lift = (150.0 / 840.0) * width();
    r.setTop(r.top() - lift);
    return r;
}

bool PetWindow::handleRiceBowlPress(const QPointF& localPos) {
    if (motion_.riceBowlSlide() < 0.95) return false;  // still sliding in
    if (!riceAromaRect().contains(localPos)) return false;
    motion_.riceBowlPoke();
    updateInputTransparency();
    return true;
}

void PetWindow::drawRiceBowlProp(QPainter& painter, const QSize& size) {
    Q_UNUSED(size);  // riceBowlRect() reads width()/height(), always the same surface
    if (riceBowlProp_.isNull()) riceBowlProp_.load(imagePath("props/rice-bowl.png"));
    if (riceBowlProp_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawPixmap(riceBowlRect(), riceBowlProp_, QRectF(riceBowlProp_.rect()));
    drawRiceDelight(painter);
}

void PetWindow::drawRiceDelight(QPainter& painter) {
    // No physical gas: the aroma reads through cartoon symbols -- small
    // hearts and music notes float up beside her head while she sniffs and
    // smiles. Three staggered slots share one loop; even slots bloom hearts,
    // odd slots notes, each fading in, rising, and fading out again.
    const double density = motion_.riceBowlDelight();
    if (density <= 0.01) return;
#ifdef HAVE_CUBISM
    const double headRide = cubismCanvas_ ? cubismCanvas_->headRideY() : 0.0;
#else
    const double headRide = 0.0;
#endif
    const double w = width(), h = height();
    const double t = riceDelightClock_.elapsed() / 1000.0;
    // Anchored just right of her face (face center 0.507w) so the symbols
    // read as her delight, not the bowl's emission; they ride with headRideY
    // like every face-hugging window layer.
    const double anchorX = w * 0.595;
    const double anchorY = h * 0.335 + headRide;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    constexpr double kPeriod = 1.9;
    const auto ease = [](double x) { return x * x * (3.0 - 2.0 * x); };
    for (int k = 0; k < 3; ++k) {
        const double p = std::fmod(t / kPeriod + k / 3.0, 1.0);
        const double alpha = std::sin(p * 3.14159265358979) * density;
        if (alpha <= 0.01) continue;
        const double sway = std::sin((t + k * 0.9) * 1.7) * w * 0.012;
        const double x = anchorX + w * 0.042 * k + sway;
        const double y = anchorY - ease(p) * h * 0.155;
        const double s = w * (k == 1 ? 0.034 : 0.028);
        const double tilt = std::sin((t + k * 1.3) * 1.3) * 8.0;

        painter.save();
        painter.setOpacity(std::clamp(alpha, 0.0, 1.0));
        painter.translate(x, y);
        painter.rotate(tilt);
        if (k % 2 == 0) {
            // Heart: two cubics into a bottom point.
            QPainterPath heart;
            heart.moveTo(0.0, s * 0.5);
            heart.cubicTo(-s * 0.62, s * 0.02, -s * 0.55, -s * 0.62, 0.0, -s * 0.18);
            heart.cubicTo(s * 0.55, -s * 0.62, s * 0.62, s * 0.02, 0.0, s * 0.5);
            painter.setPen(QPen(QColor(214, 84, 118), (std::max)(1.5, s * 0.09)));
            painter.setBrush(QColor(255, 148, 172));
            painter.drawPath(heart);
        } else {
            // Music note: slanted head, stem, and a small flag.
            QPainterPath note;
            note.moveTo(s * 0.26, s * 0.32);
            note.lineTo(s * 0.26, -s * 0.62);
            note.cubicTo(s * 0.62, -s * 0.52, s * 0.6, -s * 0.26, s * 0.34, -s * 0.16);
            painter.setPen(QPen(QColor(196, 132, 60), (std::max)(1.8, s * 0.12),
                                Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(note);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 200, 122));
            painter.drawEllipse(QRectF(-s * 0.34, s * 0.14, s * 0.62, s * 0.44));
        }
        painter.restore();
    }
    painter.restore();
}


// --- Eye-mask nap (motion card 8) ---------------------------------------------

void PetWindow::drawSleepMask(QPainter& painter, const QSize& size) const {
    const double envelope = motion_.sleepMaskEnvelope();
    if (envelope <= 0.001) return;
    const QSizeF s(size);
    painter.save();
    painter.setOpacity(std::clamp(envelope * 2.0, 0.0, 1.0));
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The mask settles onto the face during the enter phase (slides the last
    // stretch down from the forehead) and tips with the head -- pivot on the
    // eye line itself so the cloth hugs the face instead of swinging. The nap
    // is a seated scene: the canvas reports how far the head rode down with
    // the collar this frame, and the mask sinks with it.
#ifdef HAVE_CUBISM
    const double headRide = cubismCanvas_ ? cubismCanvas_->headRideY() : 0.0;
#else
    const double headRide = 0.0;
#endif
    const QPointF faceCenter(s.width() * 0.507, s.height() * 0.418 + headRide);
    painter.translate(faceCenter);
    painter.rotate(2.5 * envelope);
    painter.translate(-faceCenter.x(), -faceCenter.y() - (1.0 - envelope) * s.height() * 0.07);

    const double eyeY = s.height() * 0.418 + headRide;
    const double maskW = s.width() * 0.29;
    const double maskH = s.height() * 0.088;
    const double cx = s.width() * 0.507;

    // Strap first: a soft band running into the hair on both sides, behind
    // the cloth.
    QPen strapPen(QColor(74, 100, 164), s.height() * 0.016);
    strapPen.setCapStyle(Qt::RoundCap);
    painter.setPen(strapPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(s.width() * 0.322, eyeY - s.height() * 0.003),
                     QPointF(s.width() * 0.697, eyeY - s.height() * 0.003));

    // One piece of soft cloth across both eyes -- the classic sleep-mask
    // silhouette: a plump lozenge with a small nose notch at the bottom
    // center. Soft blue fabric (lighter than the hair so it reads as cloth),
    // a barely darker rim, one gentle sheen.
    const QRectF cloth(cx - maskW / 2.0, eyeY - maskH / 2.0, maskW, maskH);
    QPainterPath maskShape;
    maskShape.addRoundedRect(cloth, maskH * 0.48, maskH * 0.48);
    QPainterPath nose;
    nose.moveTo(cx - maskH * 0.22, cloth.bottom());
    nose.lineTo(cx, cloth.bottom() - maskH * 0.30);
    nose.lineTo(cx + maskH * 0.22, cloth.bottom());
    nose.closeSubpath();
    maskShape = maskShape.subtracted(nose);
    QLinearGradient fill(cloth.topLeft(), cloth.bottomLeft());
    fill.setColorAt(0.0, QColor(126, 154, 210));
    fill.setColorAt(1.0, QColor(88, 116, 180));
    painter.setPen(QPen(QColor(58, 82, 142), 1.0));
    painter.setBrush(fill);
    painter.drawPath(maskShape);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 50));
    painter.drawEllipse(cloth.adjusted(maskW * 0.10, maskH * 0.18, -maskW * 0.58, -maskH * 0.48));

    // DeepSeek whale mark on the right of the cloth: a small white silhouette
    // (rounded head, tapering body, twin tail flukes, one eye), echoing the
    // whale emblem on the desk front. Drawn in a unit box then scaled, so the
    // mark rides the mask's enter slide and head tip for free.
    painter.save();
    painter.translate(cx + maskW * 0.27, eyeY + maskH * 0.02);
    const double markH = maskH * 0.52;
    painter.scale(markH, markH); // unit box: roughly [-1.1, 1.1] x [-0.6, 0.6]
    QPainterPath whale;
    whale.moveTo(-1.05, 0.10);                                  // nose tip
    whale.cubicTo(-1.05, -0.42, -0.30, -0.58, 0.30, -0.34);     // over the back
    whale.cubicTo(0.58, -0.24, 0.70, -0.16, 0.82, -0.10);       // into the tail joint
    whale.lineTo(1.10, -0.44);                                  // upper fluke tip
    whale.cubicTo(1.02, -0.10, 1.02, 0.02, 1.06, 0.14);         // fluke notch
    whale.lineTo(0.72, 0.10);                                   // back to the joint
    whale.cubicTo(0.40, 0.34, -0.20, 0.52, -0.62, 0.38);        // along the belly
    whale.cubicTo(-0.90, 0.28, -1.05, 0.22, -1.05, 0.10);       // back to the nose
    whale.closeSubpath();
    painter.setBrush(QColor(245, 249, 255, 215));
    painter.setPen(Qt::NoPen);
    painter.drawPath(whale);
    painter.setBrush(QColor(74, 100, 164));                     // eye, cloth-navy
    painter.drawEllipse(QPointF(-0.60, -0.02), 0.085, 0.085);
    painter.restore();
    painter.restore();

    // Fully asleep: the zzz cloud pops in where the "?" bubble lives, gently
    // bobbing on its own slow clock. Enter/wake keep the face clean.
    if (envelope > 0.98)
        paintSleepBubble(painter, size, envelope,
                         sleepClock_.elapsed() / 1000.0 * std::numbers::pi_v<double> * 2.0 / 2.6);
}

bool PetWindow::startNap() {
    if (edgeDock_.active()) return false;
    if (motion_.sleepMotionActive()
        || controller_->state() == PetController::State::Delete
        || controller_->state() == PetController::State::Grass) return false;
    // Napping IS stopping work: an ongoing busy turn resets first, so the
    // entry point works straight from typing instead of silently doing
    // nothing while a turn is active.
    controller_->resetBusy();
    motion_.beginSleepMotion();
    sleepClock_.start();
    return true;
}

void PetWindow::onEatTriggered(const QString& file, const QPointF& sourcePos, const QIcon& icon) {
    // The drop path primes the prop itself before emitting filesDropped: the
    // shell moves the file to the Recycle Bin right after, and the real icon
    // has to be picked up while the file still exists. That happens before
    // this signal, so anything under 300 ms old is that primed prop.
    if (fedClock_.isValid() && fedClock_.elapsed() < 300 && !fedIcon_.isNull()) return;

    QPixmap pixmap;
    if (!icon.isNull()) {
        pixmap = icon.pixmap(64, 64);
    } else if (!file.isEmpty()) {
        // Snapshot missed the file (added and deleted within one snapshot
        // tick): fall back to whatever the provider still resolves, then to
        // the generic file icon.
        QIcon resolved = QFileIconProvider().icon(QFileInfo(file));
        if (resolved.isNull()) resolved = QFileIconProvider().icon(QFileIconProvider::File);
        pixmap = resolved.pixmap(64, 64);
    }
    lungeMirrored_ = false;

    // Only a deletion away from the pet gets a sideways lunge: within half a
    // window width the authored centred choreography already reads right.
    const bool sideways = !sourcePos.isNull()
        && qAbs(sourcePos.x() - (pos().x() + width() / 2.0)) > width() / 2.0;
    if (sideways) {
        lungeMirrored_ = sourcePos.x() < pos().x() + width() / 2.0;
        if (!pixmap.isNull()) {
            const double w = width();
            const double h = height();
            const QPointF fist(lungeMirrored_ ? 0.25 * w : 0.75 * w, 0.50 * h);
            deleteOverlay_->launch(pixmap, sourcePos, pos() + fist,
                                   motion_.clipEventTime(QStringLiteral("delete"),
                                                         QStringLiteral("delete.grip"), 0.65));
        }
    }

    if (!pixmap.isNull()) {
        fedIcon_ = pixmap;
        fedClock_.restart();
    }
}

bool PetWindow::saveRenderFrame(const QString& path) {
#ifdef HAVE_CUBISM
    if (edgeDock_.active()) return edgeDockFrame().save(path);
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        if (motion_.frozenPhysics()) {
            // A pose capture must read this pose, even when the normal frame
            // timer has not painted it yet. Cached frames can lag one keyframe
            // under load and invalidate otherwise identical model reviews.
            cubismCanvas_->advance(0.0);
            cubismFrame_ = cubismCanvas_->grabFramebuffer();
        }
        const QImage frame = cubismFrame_.isNull() ? cubismCanvas_->grabFramebuffer() : cubismFrame_;
        return frameWithBubble(frame, motion_.bubblePulse()).save(path);
    }
#endif
    return grab().save(path);
}

void PetWindow::setPreviewPose(const ParameterMotion::Parameters& parameters, int pixels) {
    motion_.setPreviewPose(parameters);
    setFixedSize(pixels, pixels);
#ifdef HAVE_CUBISM
    if (cubismCanvas_) cubismCanvas_->setFixedSize(size());
#endif
}

bool PetWindow::renderSequenceFrame(const ParameterMotion::Parameters& parameters, double seconds,
                                    const QString& path, double bubblePulse) {
    frameTimer_.stop();
    motion_.setSequencePose(parameters);
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        cubismCanvas_->advance(seconds);
        cubismFrame_ = cubismCanvas_->grabFramebuffer();
        return frameWithBubble(cubismFrame_, bubblePulse).save(path);
    }
#endif
    return false;
}

void PetWindow::advanceLiveFrame(double seconds) {
    if (edgeDock_.active() && !edgeDockFixtureScreen_.isValid()
        && (!edgeDockScreen_ || edgeDockScreen_->geometry() != edgeDockScreenGeometry_
            || edgeDockScreen_->availableGeometry() != edgeDock_.screen().toAlignedRect())) {
        restoreFromEdgeDock(edgeDockNormalOrigin_);
    }
    if (edgeDock_.active()) {
        edgeDock_.advance(seconds);
#ifdef HAVE_CUBISM
        if (cubismCanvas_) {
            const double p = edgeDock_.phase() == EdgeDockSession::Phase::Entering ? edgeDock_.progress() : 0.0;
            const double envelope = std::sin(3.141592653589793 * p);
            const auto edge = edgeDock_.edge();
            const double side = edge == edge_dock::Edge::Left ? 1.0 : edge == edge_dock::Edge::Right ? -1.0 : 0.0;
            cubismCanvas_->setEdgeReviewBodyOffsets(1.5 * envelope, side * envelope);
        }
#endif
        updateEdgeDockWindow();
    }
    if (pointerGesture_.mode() != PetPointerGesture::Mode::None) {
        pointerGesture_.advance(seconds);
        updatePointerGesture();
    }
    motion_.advance(seconds);
    // The bounded Hold deadline keeps running even with a captured pointer.
    // Consume this press until release so a timeout never turns into a window
    // drag or a head pat; only its force is removed as the phase exits.
    if (grassTouchPressed_ && motion_.grassInteractionPhase() != QStringLiteral("hold"))
        motion_.cancelGrassTipDrag();
    if (motion_.consumeActionFinished()) controller_->actionFinished();
    // Sticky note lifecycle: hold the risen note, fold it into a corner badge
    // when the user ignores it for a while, fade the badge once completed.
    const auto previousMemo = memo_->visual();
    if (previousMemo == StickyNoteModel::Visual::Bubble && !canShowAutomaticMemo()) {
        if (!memo_->interrupt()) memoStoreOk_ = false;
        motion_.cancelMemoMotion();
    }
    memo_->advance(seconds, motion_.memoMotionActive(), motion_.memoBubbleRise());
    if (!memo_->error().isEmpty()) memoStoreOk_ = false;
    if (memo_->visual() != previousMemo) refreshMemoActions();
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        cubismCanvas_->advance(seconds);
        cubismFrame_ = cubismCanvas_->grabFramebuffer();
        if (!edgeDock_.active() && !cubismFrame_.isNull())
            setInteractionMask(QPixmap::fromImage(composedFrame()));
    }
#endif
    updateInputTransparency();
    update();
}

void PetWindow::prepareLiveInteractionReview(int pixels) {
    liveInteractionReview_ = true;
    frameTimer_.stop();
    spontaneousTimer_.stop();  // a nomination must never race a scripted capture
    // Keep the busy branch after interruption reproducible in review captures.
    motion_.setBusyRandomSeed(20261002);
    controller_->setActionFallbackEnabled(false);
    setFixedSize(pixels, pixels);
#ifdef HAVE_CUBISM
    if (cubismCanvas_) cubismCanvas_->setFixedSize(size());
#endif
}

bool PetWindow::renderLiveInteractionFrame(double seconds, const QString& path) {
    advanceLiveFrame(seconds);
    return renderBackend() == QStringLiteral("cubism_native") && saveRenderFrame(path);
}

bool PetWindow::setEdgeReviewSuppressed(bool enabled) {
    if (!liveInteractionReview_ || !qApp->property("desktopCompanionReviewSettingsIsolated").toBool()) return false;
    if (edgeReviewSuppressed_ == enabled) return true;
    if (enabled) {
        if (memo_->visual() == StickyNoteModel::Visual::Bubble && !foldMemo()) return false;
        motion_.cancelInteraction();
        motion_.cancelDragMotion();
        motion_.cancelSleepMotion();
        motion_.setState(PetController::State::Idle);
        edgeReviewSuppressed_ = true;
    } else {
        edgeReviewSuppressed_ = false;
#ifdef HAVE_CUBISM
        if (cubismCanvas_) cubismCanvas_->setEdgeReviewBodyOffsets(0.0, 0.0);
#endif
        setState(controller_->state());
    }
    return true;
}

void PetWindow::setEdgeReviewGaze(double x, double y) {
    if (liveInteractionReview_ && edgeReviewSuppressed_)
        motion_.setGazeTarget(x, y, false, true);
}

void PetWindow::setEdgeReviewBodyOffsets(double x, double z) {
#ifdef HAVE_CUBISM
    if (liveInteractionReview_ && edgeReviewSuppressed_ && cubismCanvas_
        && qApp->property("desktopCompanionReviewSettingsIsolated").toBool()
        && std::isfinite(x) && std::isfinite(z))
        cubismCanvas_->setEdgeReviewBodyOffsets(std::clamp(x, -1.5, 1.5), std::clamp(z, -1.0, 1.0));
#else
    Q_UNUSED(x); Q_UNUSED(z);
#endif
}

QJsonObject PetWindow::edgeReviewState() const {
    QJsonObject state{{QStringLiteral("suppressed"), edgeReviewSuppressed_},
        {QStringLiteral("permission_cue"), motion_.permissionCueActive()},
        {QStringLiteral("interaction"), motion_.interactionActive()},
        {QStringLiteral("controller_state"), int(controller_->state())},
        {QStringLiteral("active_turns"), controller_->activeTurnCount()},
        {QStringLiteral("pending_permissions"), controller_->pendingPermissionCount()}};
#ifdef HAVE_CUBISM
    if (cubismCanvas_) {
        const auto applied = cubismCanvas_->edgeReviewAppliedBodyOffsets();
        state.insert(QStringLiteral("applied_body_x"), applied.x());
        state.insert(QStringLiteral("applied_body_z"), applied.y());
    }
#endif
    return state;
}

QImage PetWindow::advanceEdgeReviewFrame(double seconds) {
    if (!liveInteractionReview_ || renderBackend() != QStringLiteral("cubism_native")) return {};
    advanceLiveFrame(seconds);
#ifdef HAVE_CUBISM
    if (edgeReviewSuppressed_) return cubismFrame_;
#endif
    return composedFrame();
}

bool PetWindow::startGrassInteraction() {
    if (edgeDock_.active()) return false;
    if ((!grassInteractionApproved() && !interactionPreviewEnabled_) || renderBackend() != QStringLiteral("cubism_native")
        || controller_->state() == PetController::State::Delete) return false;
    controller_->playInteractiveGrass(motion_.grassInteractionMaxDuration() + 1.0);
    if (motion_.beginGrassInteraction()) return true;
    controller_->actionFinished();
    return false;
}

void PetWindow::playGrass() {
    if (edgeDock_.active()) return;
    if ((grassInteractionApproved() || interactionPreviewEnabled_) && startGrassInteraction()) return;
    controller_->playGrass();
}

bool PetWindow::grassInteractionApproved() const {
    const auto* clip = motion_.library().clip(QStringLiteral("grass-touch"));
    return clip && clip->approval() == QStringLiteral("approved");
}

bool PetWindow::headPatApproved() const {
    const auto* clip = motion_.library().clip(QStringLiteral("head-pat"));
    return clip && clip->approval() == QStringLiteral("approved");
}

QPainterPath PetWindow::grassTipHitPath() const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        const auto& path = cubismCanvas_->grassTipHitPath();
        if (!lungeMirrored_) return path;
        QTransform mirror;
        mirror.translate(width(), 0);
        mirror.scale(-1, 1);
        return mirror.map(path);
    }
#endif
    return {};
}

bool PetWindow::isGrassTipAt(const QPointF& position) const {
    return (grassInteractionApproved() || interactionPreviewEnabled_)
        && motion_.grassInteractionPhase() == QStringLiteral("hold")
        && grassTipHitPath().contains(position);
}

QJsonObject PetWindow::grassGeometry() const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        auto geometry = cubismCanvas_->grassGeometry();
        if (lungeMirrored_ && geometry.value(QStringLiteral("valid")).toBool()) {
            const auto mirror = [this](QJsonArray point) {
                point[0] = width() - point[0].toDouble();
                return point;
            };
            for (const auto* name : {"root", "mid", "tip"})
                geometry[QString::fromLatin1(name)] = mirror(geometry.value(QString::fromLatin1(name)).toArray());
            QJsonArray pinned;
            for (const auto& point : geometry.value(QStringLiteral("pinned_vertices")).toArray())
                pinned.append(mirror(point.toArray()));
            geometry[QStringLiteral("pinned_vertices")] = pinned;
        }
        return geometry;
    }
#endif
    return {{QStringLiteral("valid"), false}};
}

bool PetWindow::grassTipDragEnabled() const {
    return motion_.grassTipDragApproved() || grassDragPreviewEnabled_;
}

void PetWindow::setGrassDragPreviewEnabled(bool enabled) {
    grassDragPreviewEnabled_ = enabled;
    if (grassDragPreviewAction_ && grassDragPreviewAction_->isChecked() != enabled) {
        const QSignalBlocker block(grassDragPreviewAction_);
        grassDragPreviewAction_->setChecked(enabled);
    }
    if (!enabled && !motion_.grassTipDragApproved()) {
        motion_.cancelGrassTipDrag();
        if (grassTouchPressed_) releasePointerGesture();
    }
    updateInputTransparency();
}

QJsonObject PetWindow::grassDragReviewState() const {
    return {{QStringLiteral("enabled"), grassTipDragEnabled()},
        {QStringLiteral("approved"), motion_.grassTipDragApproved()},
        {QStringLiteral("preview_enabled"), grassDragPreviewEnabled_},
        {QStringLiteral("pressed"), grassTouchPressed_},
        {QStringLiteral("moved"), grassDragMoved_},
        {QStringLiteral("active"), motion_.grassTipDragActive()},
        {QStringLiteral("settling"), motion_.grassTipDragSettling()},
        {QStringLiteral("offset"), motion_.grassTipDragOffset()},
        {QStringLiteral("normal"), QJsonArray{grassDragNormal_.x(), grassDragNormal_.y()}},
        {QStringLiteral("span"), grassDragSpan_},
        {QStringLiteral("geometry"), grassGeometry()}};
}

void PetWindow::setInteractionPreviewEnabled(bool enabled) {
    interactionPreviewEnabled_ = enabled;
    if (interactionPreviewAction_ && interactionPreviewAction_->isChecked() != enabled)
        interactionPreviewAction_->setChecked(enabled);
    if (!enabled) {
        const auto* activeClip = motion_.library().clip(motion_.interactionId());
        const bool approvedReaction = activeClip && activeClip->approval() == QStringLiteral("approved");
        const bool approvedPat = approvedReaction && motion_.interactionId() == QStringLiteral("head-pat");
        const bool approvedGrassPress = grassTouchPressed_ && motion_.grassTipDragApproved();
        if (!approvedPat && !approvedGrassPress) {
            patPreviewTimer_.stop();
            releasePointerGesture();
        }
        motion_.cancelPendingInteraction();
        if (motion_.grassInteractionActive() && !grassInteractionApproved()) controller_->actionFinished();
    }
}

QJsonObject PetWindow::pointerReviewState() const {
    QString gesture;
    switch (pointerGesture_.mode()) {
    case PetPointerGesture::Mode::None: gesture = QStringLiteral("none"); break;
    case PetPointerGesture::Mode::Pending: gesture = QStringLiteral("pending"); break;
    case PetPointerGesture::Mode::Pat: gesture = QStringLiteral("pat"); break;
    case PetPointerGesture::Mode::Drag: gesture = QStringLiteral("drag"); break;
    }
    return {{QStringLiteral("gesture"), gesture},
        {QStringLiteral("reaction"), motion_.interactionId()},
        {QStringLiteral("reaction_time"), motion_.interactionTime()},
        {QStringLiteral("head_pat_approved"), headPatApproved()},
        {QStringLiteral("long_press_pending"), longPressTimer_.isActive()},
        {QStringLiteral("dragging"), dragging_},
        {QStringLiteral("drag_motion_active"), motion_.dragMotionActive()},
        {QStringLiteral("wave_active"), motion_.waveActive()},
        {QStringLiteral("preview_enabled"), interactionPreviewEnabled_}};
}

bool PetWindow::isHeadAt(const QPointF& position) const {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        const QPointF native = lungeMirrored_ ? QPointF(width() - position.x(), position.y()) : position;
        return cubismCanvas_->headHitPath().contains(native);
    }
#endif
    return false;
}

QJsonObject PetWindow::dragReviewState() const {
    auto state = pointerReviewState();
    state[QStringLiteral("following")] = motion_.dragFollowing();
    state[QStringLiteral("input_age")] = motion_.dragInputAge();
    state[QStringLiteral("travelled")] = motion_.dragTravelledDistance();
    state[QStringLiteral("curious_triggered")] = motion_.dragCuriousTriggered();
    return state;
}

bool PetWindow::event(QEvent* event) {
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate) {
        if (edgeDock_.active()) cancelEdgeDockDrag();
        else releasePointerGesture();
    }
    if (event->type() == QEvent::Hide) {
        if (edgeDock_.active()) restoreFromEdgeDock(edgeDockNormalOrigin_);
        releasePointerGesture();
        patPreviewTimer_.stop();
        motion_.cancelInteraction();
        if (motion_.grassInteractionActive()) controller_->actionFinished();
        // Low power: nothing needs rendering while the pet is hidden. Only
        // record the pause if WE stopped it, so a review-driven stop is never
        // restarted behind the reviewer's back.
        if (frameTimer_.isActive()) {
            frameTimer_.stop();
            framePausedForHide_ = true;
        }
    } else if (event->type() == QEvent::Show) {
        if (framePausedForHide_) {
            frameClock_.restart();   // avoid one huge dt after the pause
            frameTimer_.start();
            framePausedForHide_ = false;
        }
    }
    return QWidget::event(event);
}

void PetWindow::updatePointerGesture() {
    if (pointerGesture_.mode() == PetPointerGesture::Mode::Pat) {
        if (!patAttempted_) {
            patAttempted_ = true;
            patPreviewTimer_.stop();
            if (!motion_.beginHeadPat(pointerGesture_.normalizedPatDirection())) {
                patAttempted_ = false;
                pointerGesture_.rejectPat();
            } else {
                longPressTimer_.stop();
            }
        }
        if (pointerGesture_.mode() == PetPointerGesture::Mode::Pat)
            motion_.updateHeadPat(pointerGesture_.normalizedPatDirection());
    }
    // A rejected pat may already exceed the body's drag threshold. It needs
    // the same drag initialization as a gesture that began on the body.
    if (pointerGesture_.mode() == PetPointerGesture::Mode::Drag) {
        longPressTimer_.stop();
        if (!dragging_) {
            dragging_ = true;
            dragHasSample_ = false;
            dragLastGlobal_ = QPointF();
            dragVelocity_ = QPointF();
            dragClock_.start();
            motion_.beginDragMotion();
        }
    }
}

void PetWindow::releasePointerGesture() {
    longPressTimer_.stop();
    if (patAttempted_) motion_.endHeadPat();
    pointerGesture_.release();
    patAttempted_ = false;
    if (dragging_) {
        motion_.endDragMotion();
        if (!liveInteractionReview_) {
            constrainPositionToScreen();
            snapToEdges();   // dock flush to a nearby screen edge
            QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
            settings.setValue(QStringLiteral("position"), pos());
        }
    }
    dragging_ = false;
    dragHasSample_ = false;
    if (motion_.grassTipDragActive()) motion_.lookAtGrassTip(0.0);
    motion_.endGrassTipDrag();
    grassTouchPressed_ = false;
    grassDragMoved_ = false;
}

// Motion card 4: after a scene move the window must stay on the current screen.
// This used to keep a visible *strip* of the pet on screen, which is enough to
// grab her but not enough to keep her art intact: the thought bubble and the
// sticky note are painted in the right third of the widget, so an overhang of a
// few tens of pixels cut the bubble and a larger one hid it outright while she
// still looked fine (see pet_placement::clampToScreen). Docking flush to an
// edge is snapToEdges' job; this only guarantees the whole window is on screen.
void PetWindow::constrainPositionToScreen() {
    const QScreen* screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    if (!screen) return;
    const QPoint clamped = pet_placement::clampToScreen(screen->availableGeometry(), size(), pos());
    if (clamped != pos()) move(clamped);
}

void PetWindow::mousePressEvent(QMouseEvent* event) {
    if (edgeDock_.active()) {
#ifdef HAVE_CUBISM
        if (event->button() == Qt::LeftButton && edgeDock_.press(event->globalPosition(), cubismFrame_)) {
            setCursor(Qt::ClosedHandCursor);
            updateInputTransparency();
        }
#endif
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        // Waking is deliberate: only a touch on the head -- the mask sits on
        // the face, so touching it is touching her head -- wakes the nap.
        // Clicking the body or starting a window drag leaves her asleep.
        if (motion_.sleepMotionActive() && isHeadAt(event->position())) {
            motion_.wakeFromSleep(true);
            updateInputTransparency();
            event->accept();
            return;
        }
        if (motion_.sleepMotionActive()) {
            // A press elsewhere on the pet must not start a drag that fights
            // the sleeping face: swallow it quietly.
            event->accept();
            return;
        }
        // The sticky note owns its paper: clicking it never starts a drag.
        if (handleMemoPress(event->position(), event->globalPosition().toPoint())) {
            event->accept();
            return;
        }
        // The cardboard box owns its face: clicking it is "found you",
        // never a drag or a head pat.
        if (handleBoxPress(event->position())) {
            event->accept();
            return;
        }
        // The rice bowl and its aroma zone own their corner: a click is
        // "too close", never a drag or a head pat.
        if (handleRiceBowlPress(event->position())) {
            event->accept();
            return;
        }
        // Peeking mid-stretch: a click is "caught you" -- skip to the recovery
        // beat (smile fades, she sits back up) instead of a head pat or drag.
        if (motion_.stretchActive()) {
            motion_.pokeStretch();
            updateInputTransparency();
            event->accept();
            return;
        }
        releasePointerGesture();
        if (grassTipDragEnabled() && isGrassTipAt(event->position())) {
            const auto geometry = grassGeometry();
            const auto root = geometry.value(QStringLiteral("root")).toArray();
            const auto tip = geometry.value(QStringLiteral("tip")).toArray();
            if (geometry.value(QStringLiteral("valid")).toBool() && root.size() == 2 && tip.size() == 2) {
                const QPointF stem(tip[0].toDouble() - root[0].toDouble(),
                                   tip[1].toDouble() - root[1].toDouble());
                const double length = std::hypot(stem.x(), stem.y());
                if (length > 1.0 && motion_.beginGrassTipDrag(grassDragPreviewEnabled_)) {
                    grassDragPress_ = event->position();
                    grassDragNormal_ = QPointF(-stem.y(), stem.x()) / length;
                    if (grassDragNormal_.x() < 0.0) grassDragNormal_ = -grassDragNormal_;
                    grassDragSpan_ = (std::max)(12.0, 0.4 * length);
                    grassTouchPressed_ = true;
                    updateInputTransparency();
                    event->accept();
                    return;
                }
            }
        }
        if (isGrassTipAt(event->position()) && motion_.respondToGrass()) {
            grassTouchPressed_ = true;
            updateInputTransparency();
            event->accept();
            return;
        }
        gestureWindowOrigin_ = pos();
        gesturePressGlobal_ = event->globalPosition();
        dragOffset_ = event->globalPosition().toPoint() - pos();
        const bool allowPat = (headPatApproved() || interactionPreviewEnabled_) && motion_.canBeginHeadPat()
            && renderBackend() == QStringLiteral("cubism_native")
            && controller_->state() != PetController::State::Delete
            && controller_->state() != PetController::State::Grass;
        pointerGesture_.press(event->position(), allowPat && isHeadAt(event->position()));
        // A plain hold on her (not a head pat) becomes a greeting if she is
        // still holding after the delay -- see longPressTimer_.
        longPressTimer_.start(600);
        event->accept();
    }
}

void PetWindow::mouseMoveEvent(QMouseEvent* event) {
    if (edgeDock_.active()) {
        if (edgeDock_.phase() == EdgeDockSession::Phase::Dragging) {
            if (event->buttons() & Qt::LeftButton) {
                edgeDock_.drag(event->globalPosition());
                updateEdgeDockWindow();
            } else cancelEdgeDockDrag();
        }
        event->accept();
        return;
    }
    if (grassTouchPressed_ && motion_.grassTipDragActive()) {
        if (!(event->buttons() & Qt::LeftButton)) {
            releasePointerGesture();
            event->accept();
            return;
        }
        const QPointF delta = event->position() - grassDragPress_;
        if (std::hypot(delta.x(), delta.y()) >= (std::max)(2.0, width() * 0.012))
            grassDragMoved_ = true;
        if (grassDragMoved_) {
            const double bend = QPointF::dotProduct(delta, grassDragNormal_) / grassDragSpan_;
            motion_.updateGrassTipDrag(bend);
            motion_.lookAtGrassTip(std::clamp(delta.x() / grassDragSpan_, -1.0, 1.0));
        }
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (motion_.grassInteractionPhase() == QStringLiteral("hold")) {
        const QRectF tip = grassTipHitPath().boundingRect();
        const bool nearby = !tip.isEmpty() && tip.adjusted(-width() * 0.12, -height() * 0.12,
            width() * 0.12, height() * 0.12).contains(event->position());
        motion_.lookAtGrassTip(nearby ? std::clamp((event->position().x() - tip.center().x())
            / (width() * 0.15), -1.0, 1.0) : 0.0);
        setCursor(isGrassTipAt(event->position()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    } else setCursor(Qt::ArrowCursor);
    if (grassTouchPressed_) { event->accept(); return; }
    if (pointerGesture_.mode() != PetPointerGesture::Mode::None && (event->buttons() & Qt::LeftButton)) {
        // Relative to the press-time window, never to a window moved mid-drag.
        pointerGesture_.move(event->globalPosition() - QPointF(gestureWindowOrigin_));
        updatePointerGesture();
        if (dragging_) {
            // Low-passed window velocity feeds the drag reaction; the motion
            // layer turns it into eye/hair/body lag and the settle nod.
            // A scene move carries the box along; the peek loses its point,
            // so the box exits (repeated calls are safe while dragging).
            if (motion_.boxPeekActive()) motion_.exitBoxPeek();
            // The bowl slides off too: a dragged desk pet cannot enjoy dinner.
            if (motion_.riceBowlActive()) motion_.exitRiceBowl();
            const QPointF global = event->globalPosition();
            const QPointF delta = global - (dragHasSample_ ? dragLastGlobal_ : gesturePressGlobal_);
            if (dragHasSample_) {
                const double dt = (std::max)(dragClock_.restart() / 1000.0, 1.0 / 240.0);
                // After a pause the old low-pass value is stale as well; do
                // not reintroduce it on the next real sample.
                if (dt >= 0.10 || motion_.dragInputAge() >= 0.10) dragVelocity_ = QPointF();
                const QPointF inst = delta / dt;
                dragVelocity_ += (inst - dragVelocity_) * 0.45;
            }
            motion_.updateDragMotion(dragVelocity_.x(), dragVelocity_.y(), std::hypot(delta.x(), delta.y()));
            dragLastGlobal_ = global;
            dragHasSample_ = true;
            move(event->globalPosition().toPoint() - dragOffset_);
        }
        event->accept();
    }
}

void PetWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (edgeDock_.active()) {
        if (event->button() == Qt::LeftButton && edgeDock_.phase() == EdgeDockSession::Phase::Dragging) {
            edgeDock_.drag(event->globalPosition());
            QPointF origin;
            if (edgeDock_.release(&origin)) restoreFromEdgeDock(origin);
            else { unsetCursor(); updateEdgeDockWindow(); }
            updateInputTransparency();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        longPressTimer_.stop();
        const bool tap = grassTipDragEnabled() && grassTouchPressed_
            && motion_.grassTipDragActive() && !grassDragMoved_;
        auto edge = edge_dock::Edge::None;
        QScreen* owner = nullptr;
        QRect available;
        if (dragging_ && edgeDockPreviewEnabled_) {
            const QPoint cursor = event->globalPosition().toPoint();
            if (edgeDockFixtureScreen_.isValid()) {
                available = edgeDockFixtureScreen_;
                edge = edge_dock::outerEdgeAtRelease(geometry(), cursor, available, available, {available}, 24);
            } else if (!liveInteractionReview_ && (owner = QGuiApplication::screenAt(cursor))) {
                QList<QRect> screens;
                for (const auto* monitor : QGuiApplication::screens()) screens.append(monitor->geometry());
                available = owner->availableGeometry();
                edge = edge_dock::outerEdgeAtRelease(geometry(), cursor, owner->geometry(),
                    available, screens, 24);
            }
        }
        releasePointerGesture();
        if (edge != edge_dock::Edge::None) beginEdgeDock(edge, available, owner);
        if (tap) motion_.respondToGrass();
        updateInputTransparency();
        event->accept();
    }
}

void PetWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (edgeDock_.active()) { event->accept(); return; }
    if (event->button() == Qt::LeftButton) {
        if (motion_.grassInteractionActive()) {
            event->accept();
            return;
        }
        if (motion_.sleepMotionActive()) {
            // The press already woke the nap; the double-click must not
            // start the grass play through it.
            motion_.wakeFromSleep(true);
            event->accept();
            return;
        }
        releasePointerGesture();
        playGrass();
    }
}

void PetWindow::contextMenuEvent(QContextMenuEvent* event) {
    if (edgeDock_.active()) { event->accept(); return; }
    showMenu(event->globalPos());
}

void PetWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (edgeDock_.active()) { event->ignore(); return; }
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void PetWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (edgeDock_.active()) { event->ignore(); return; }
    // Kept in sync with dragEnterEvent so the accepted cursor follows the
    // pointer across the character while the drag is in flight.
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void PetWindow::dropEvent(QDropEvent* event) {
    if (edgeDock_.active()) { event->ignore(); return; }
    QStringList paths;
    const auto urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (!QFileInfo::exists(path)) continue;
        paths.append(path);
    }
    if (paths.isEmpty()) return;
    event->acceptProposedAction();
    // The prop shows the real icon of what was fed, so pick it up before the
    // shell removes the file from its old location.
    QIcon fedIcon = QFileIconProvider().icon(QFileInfo(paths.first()));
    if (fedIcon.isNull()) fedIcon = QFileIconProvider().icon(QFileIconProvider::File);
    fedIcon_ = fedIcon.pixmap(64, 64);
    fedClock_.restart();
    emit filesDropped(paths);
    // The eat is the gesture for "this file is gone": move it to the Recycle
    // Bin right away. FOF_ALLOWUNDO keeps it recoverable, so a misdrop is a
    // restore away -- the pet is playful, not destructive.
    recyclePaths(paths);
}

void PetWindow::recyclePaths(const QStringList& paths) {
#ifdef Q_OS_WIN
    if (paths.isEmpty()) return;
    // SHFileOperation's file list is double-NUL terminated. QString may hold
    // embedded NULs, so build the raw buffer by hand instead of join().
    QString raw;
    for (const QString& path : paths) raw += path + QChar(u'\0');
    raw += QChar(u'\0');
    SHFILEOPSTRUCTW operation{};
    operation.hwnd = reinterpret_cast<HWND>(winId());
    operation.wFunc = FO_DELETE;
    operation.pFrom = reinterpret_cast<LPCWSTR>(raw.utf16());
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&operation);
#else
    Q_UNUSED(paths)
#endif
}

namespace {
// The rolled-up "wrap" the file becomes mid-motion: a white paper roll with a
// spiral end and the red delete cross, matching delete-concept-v6 panel 2.
QPixmap makeWrapProp() {
    QPixmap pm(144, 52);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen outline(QColor(0xA9, 0xA4, 0x9B), 4);
    // body capsule
    p.setPen(outline);
    p.setBrush(QColor(0xFB, 0xFA, 0xF6));
    p.drawRoundedRect(QRectF(5, 7, 134, 38), 19, 19);
    // paper seam curves along the body
    QPen seam(QColor(0xDD, 0xD8, 0xCF), 2);
    p.setPen(seam);
    p.drawLine(QPointF(52, 12), QPointF(48, 40));
    p.drawLine(QPointF(84, 12), QPointF(80, 40));
    // spiral at the left end (the rolled edge)
    p.setPen(QPen(QColor(0xA9, 0xA4, 0x9B), 2.5));
    p.drawArc(QRectF(14, 16, 20, 20), 90 * 16, 270 * 16);
    p.drawArc(QRectF(19, 21, 10, 10), 200 * 16, 300 * 16);
    // red delete cross at the right tip
    QPen cross(QColor(0xD9, 0x53, 0x4F), 4, Qt::SolidLine, Qt::RoundCap);
    p.setPen(cross);
    p.drawLine(QPointF(114, 20), QPointF(126, 32));
    p.drawLine(QPointF(126, 20), QPointF(114, 32));
    return pm;
}
} // namespace

void PetWindow::drawFedProp(QPainter& painter) {
    if (fedIcon_.isNull() || !fedClock_.isValid()) return;
    if (wrapProp_.isNull()) wrapProp_ = makeWrapProp();
    // The moments of this prop choreography are authored markers on
    // delete.motion.json, so the wrap and the pose share one timeline instead of
    // two copies of the same numbers (previously 0.62/0.80/0.95/1.30/1.45/1.62
    // lived here and disagreed with the clip: the roll started at 0.80 in code
    // but at 1.05 in the authored contract). Fallbacks keep the drawn sequence
    // usable when the clip is missing.
    const auto marker = [this](const char* name, double fallback) {
        return motion_.clipEventTime(QStringLiteral("delete"), QLatin1String(name), fallback);
    };
    // Phases read straight off the authored markers. The old code used 0.62/0.80/
    // 0.95 for the first three, which rolled the file flat at 0.80 s while the
    // authored contract rolls it together with the arm rise up to 1.05 s: the prop
    // finished forming a quarter of a second before the pose it belongs to.
    const double gripT = marker("delete.grip", 0.65);
    const double rollT = marker("delete.roll_formed", 1.05);
    const double wrapT = rollT + 0.15; // strip unrolls into the drawn wrap
    const double raiseT = marker("delete.lunge", 1.30);
    const double biteT = marker("delete.bite", 1.45);
    const double goneT = marker("delete.consumed", 1.60);
    const double t = fedClock_.elapsed() / 1000.0;
    if (t < gripT || t >= goneT) return; // hand still reaching / already swallowed
    const double w = width();
    const double h = height();
    // Mirrored lunge: the whole authored choreography reflects horizontally,
    // anchors included, so the reach happens on the side the file sat on.
    const auto mx = [&](double x) { return lungeMirrored_ ? w - x : x; };
    const QPointF fistGrip(mx(0.75 * w), 0.50 * h);
    const QPointF fistUp(mx(0.77 * w), 0.44 * h);
    const QPointF mouth(mx(0.545 * w), 0.44 * h);
    const double side = qMax(12.0, 0.10 * w);
    QPointF pos;
    double angle = 0.0;      // degrees, clockwise
    double widthScale = 1.0;
    double heightScale = 1.0;
    double iconAlpha = 0.0;
    double wrapAlpha = 0.0;
    if (t < rollT) {
        // Rolling: the file spins flat around its centre while shrinking tall.
        const double k = qBound(0.0, (t - gripT) / (rollT - gripT), 1.0);
        const double ease = k * k * (3.0 - 2.0 * k);
        pos = fistGrip + (fistUp - fistGrip) * ease * 0.35;
        angle = 360.0 * ease;
        heightScale = 1.0 - 0.65 * ease;
        widthScale = 1.0 - 0.2 * ease;
        iconAlpha = 1.0;
    } else if (t < wrapT) {
        // The flat strip unrolls into the drawn wrap.
        const double k = qBound(0.0, (t - rollT) / (wrapT - rollT), 1.0);
        const double ease = k * k * (3.0 - 2.0 * k);
        pos = fistGrip + (fistUp - fistGrip) * (0.35 + 0.65 * ease);
        angle = -25.0 * ease;
        heightScale = 0.35 + 0.65 * ease;
        widthScale = 0.8 + 0.2 * ease;
        iconAlpha = 1.0 - k;
        wrapAlpha = k;
    } else if (t < raiseT) {
        // Held phase: the wrap rides the fist at its raised anchor.
        pos = fistUp;
        angle = -25.0;
        heightScale = 1.0;
        widthScale = 1.0;
        wrapAlpha = 1.0;
    } else if (t < biteT) {
        // Lunge phase: the wrap levels out as the mouth snaps forward to it.
        const double k = qBound(0.0, (t - raiseT) / (biteT - raiseT), 1.0);
        const double ease = k * k * (3.0 - 2.0 * k);
        pos = fistUp + (mouth - fistUp) * ease;
        angle = -25.0 * (1.0 - ease);
        widthScale = 1.0 - 0.1 * ease;
        heightScale = 1.0 - 0.1 * ease;
        wrapAlpha = 1.0;
    } else {
        // Bite lands: swallowed in a couple of frames, no slow drift.
        const double k = qBound(0.0, (t - biteT) / (goneT - biteT), 1.0);
        pos = mouth;
        angle = 0.0;
        widthScale = 0.9 * (1.0 - 0.75 * k);
        heightScale = 0.9 * (1.0 - 0.75 * k);
        wrapAlpha = 1.0 - k;
    }
    if (lungeMirrored_) angle = -angle;  // mirroring flips rotation handedness
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(pos);
    painter.rotate(angle);
    const QRectF source = fedIcon_.isNull() ? QRect() : fedIcon_.rect();
    if (iconAlpha > 0.0) {
        painter.setOpacity(iconAlpha);
        painter.drawPixmap(QRectF(-side * widthScale / 2.0, -side * heightScale / 2.0,
                                  side * widthScale, side * heightScale),
                           fedIcon_, source);
    }
    if (wrapAlpha > 0.0) {
        const double wrapW = side * 1.8 * widthScale;
        const double wrapH = side * 0.62 * heightScale;
        painter.setOpacity(wrapAlpha);
        painter.drawPixmap(QRectF(-wrapW / 2.0, -wrapH / 2.0, wrapW, wrapH),
                           wrapProp_, QRectF(wrapProp_.rect()));
    }
    painter.restore();
}

void PetWindow::showMenu(const QPoint& globalPos) {
    trayMenu_.exec(globalPos);
}
