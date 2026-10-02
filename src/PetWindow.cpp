#include "PetWindow.h"
#ifdef HAVE_CUBISM
#include "CubismCanvas.h"
#endif

#include <QApplication>
#include <QAction>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QtMath>
#include <QUrl>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString imagePath(const char* name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/") + QLatin1String(name));
}

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

void paintThoughtBubble(QPainter& painter, const QSize& size, double pulse) {
    if (pulse <= 0.001) return;
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
    const QColor accent(0xd0, 0x94, 0x2a);

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
    // thought balloon is drawn, so the "?" is read as the pet's own.
    painter.drawEllipse(QPointF(cx - 1.26 * radius, cy + 0.64 * radius),
                        0.29 * radius, 0.29 * radius);
    painter.drawEllipse(QPointF(cx - 1.64 * radius, cy + 1.00 * radius),
                        0.16 * radius, 0.16 * radius);

    // The body is a union of lobes: overlapping ellipses drawn separately would
    // show every internal edge, which reads as a diagram instead of a cloud.
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

    // "?" -- the one part of the bubble that carries the meaning.
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setBold(true);
    font.setPixelSize(qMax(9, qRound(radius * 1.15)));
    painter.setFont(font);
    painter.setPen(QPen(accent));
    painter.drawText(QRectF(cx - radius, cy - radius * 0.86, 2.0 * radius, 1.72 * radius),
                     Qt::AlignCenter, QStringLiteral("?"));
    painter.restore();
}
} // namespace

// Flight of the deleted file's icon from its desktop position towards the
// pet's fist. A QWidget can only paint inside itself, and the deleted file
// usually sits outside the pet window, so the first leg of the eat
// choreography (up to drawFedProp's grip moment, 0.62 s) rides on this small
// transparent, click-through overlay; there it hands the icon over to the
// in-window wrap sequence at the very same global position and size.
// The grip moment is duplicated here and in drawFedProp on purpose: both are
// annotations of delete.motion.json's authored grip frame, and a shared
// constant would not keep them in sync with the clip either.
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
            if (t >= 0.62) { hide(); tick_.stop(); return; }
            step();
            update();
        });
    }

    // `from`/`to` in logical global coordinates; the clock starts now, in the
    // same event-loop beat as the pet's fed clock so both stay in step.
    void launch(const QPixmap& icon, const QPointF& from, const QPointF& to) {
        if (icon.isNull()) return;
        icon_ = icon;
        from_ = from;
        to_ = to;
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
        const double k = qBound(0.0, t / 0.62, 1.0);
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
    if (!motion_.loadMotionLibrary(imagePath("motions"), &motionError))
        qWarning() << "Motion library:" << motionError;
    // The state machine gets its durations from the same clips that drive the
    // motion, so a retimed asset can never desync the fallback timer.
    controller_->setActionDuration(PetController::State::Delete,
                                   motion_.actionDuration(PetController::State::Delete));
    controller_->setActionDuration(PetController::State::Grass,
                                   motion_.actionDuration(PetController::State::Grass));

    frameTimer_.setInterval(40);
    frameClock_.start();
    deleteOverlay_ = new DeleteOverlay(this);
    // Desktop deletions carry the file's snapshot (path, icon-grid position,
    // icon); drops arrive here too, through the shared triggerEat entry.
    connect(controller_, &PetController::eatTriggered,
            this, &PetWindow::onEatTriggered);
    connect(controller_, &PetController::allTurnsStopped, this, [this] {
        if (interactionPreviewEnabled_ && controller_->state() == PetController::State::Idle)
            motion_.playTurnEnded();
    });
    connect(&frameTimer_, &QTimer::timeout, this, [this] {
        ++frame_;
        const double seconds = frameClock_.restart() / 1000.0;
        advanceLiveFrame(seconds);
    });
    frameTimer_.start();
    connect(controller_, &PetController::stateChanged, this, &PetWindow::setState);
    setState(PetController::State::Idle);

    QSettings settings(QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
    const QPoint saved = settings.value(QStringLiteral("position")).toPoint();
    if (!saved.isNull()) move(saved);
    else if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect r = screen->availableGeometry();
        move(r.right() - width() - 24, r.bottom() - height() - 24);
    }

    tray_.setIcon(QIcon(idleImage_.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    tray_.setToolTip(windowTitle());
    trayMenu_.addAction(QStringLiteral("显示 / 隐藏"), this, [this] { if (isVisible()) hide(); else show(); });
    trayMenu_.addAction(QStringLiteral("播放删除动作"), this, [this] {
        controller_->desktopItemDeleted();
    });
    trayMenu_.addAction(QStringLiteral("玩狗尾巴草"), this, &PetWindow::playGrass);
    interactionPreviewAction_ = trayMenu_.addAction(QStringLiteral("预览新互动（待审批）"));
    interactionPreviewAction_->setCheckable(true);
    connect(interactionPreviewAction_, &QAction::toggled, this, &PetWindow::setInteractionPreviewEnabled);
    if (!grassInteractionApproved()) {
        trayMenu_.addAction(QStringLiteral("预览草穗接招"), this, [this] {
            setInteractionPreviewEnabled(true);
            startGrassInteraction();
        });
    }
    trayMenu_.addAction(QStringLiteral("预览收工回望"), this, [this] {
        if (controller_->state() != PetController::State::Idle) return;
        setInteractionPreviewEnabled(true);
        motion_.playTurnEnded();
    });
    patPreviewTimer_.setSingleShot(true);
    connect(&patPreviewTimer_, &QTimer::timeout, this, [this] { motion_.endHeadPat(); });
    trayMenu_.addAction(QStringLiteral("预览摸头"), this, [this] {
        setInteractionPreviewEnabled(true);
        if (motion_.beginHeadPat(1.0)) patPreviewTimer_.start(1600);
    });
    laptopPreviewTimer_.setSingleShot(true);
    connect(&laptopPreviewTimer_, &QTimer::timeout, this, [this] {
        controller_->turnStopped(QStringLiteral("local-preview"), QStringLiteral("laptop"));
    });
    trayMenu_.addAction(QStringLiteral("预览抱电脑工作"), this, [this] {
        if (controller_->state() == PetController::State::Delete
            || controller_->state() == PetController::State::Grass) return;
        // A preview turn participates in the same concurrent-turn accounting.
        controller_->turnStarted(QStringLiteral("local-preview"), QStringLiteral("laptop"));
        motion_.forceLaptopBusy();
        laptopPreviewTimer_.start(16000);
    });
    trayMenu_.addAction(QStringLiteral("重置忙碌状态"), controller_, &PetController::resetBusy);
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
}

PetWindow::~PetWindow() {
#ifdef HAVE_CUBISM
    delete cubismCanvas_;
#endif
}

void PetWindow::setState(PetController::State state) {
    if (state == PetController::State::Delete || state == PetController::State::Grass)
        releasePointerGesture();
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
    const int x = (cursor.x - bounds.left) * hitCoverage_.width() / windowWidth;
    const int y = (cursor.y - bounds.top) * hitCoverage_.height() / windowHeight;
    const bool buttonHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool interactive = ((pointerGesture_.mode() != PetPointerGesture::Mode::None || grassTouchPressed_) && buttonHeld)
        || (cursorOk && !hitCoverage_.isNull()
        && x >= 0 && y >= 0 && x < hitCoverage_.width() && y < hitCoverage_.height()
        && hitCoverage_.constScanLine(y)[x] >= 16);
    const LONG_PTR style = GetWindowLongPtr(window, GWL_EXSTYLE);
    const LONG_PTR desired = interactive ? (style & ~WS_EX_TRANSPARENT)
                                         : (style | WS_EX_TRANSPARENT);
    if (desired != style) SetWindowLongPtr(window, GWL_EXSTYLE, desired);
#endif
}

void PetWindow::shutdown() {
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

void PetWindow::paintEvent(QPaintEvent*) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        QPainter painter(this);
        // One composition path for the live window and the review captures, so
        // the approval frames can never show a bubble the desktop pet does not
        // draw. In Source mode the composed frame replaces the window content
        // outright, which is what the translucent widget wants.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
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
    if (pulse <= 0.001 || frame.isNull()) return frame;
    // QImage is copy-on-write, so this copies only when a bubble is actually
    // being drawn over the frame.
    QImage composed = frame;
    QPainter painter(&composed);
    paintThoughtBubble(painter, composed.size(), pulse);
    return composed;
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
            deleteOverlay_->launch(pixmap, sourcePos, pos() + fist);
        }
    }

    if (!pixmap.isNull()) {
        fedIcon_ = pixmap;
        fedClock_.restart();
    }
}

bool PetWindow::saveRenderFrame(const QString& path) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
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
    if (pointerGesture_.mode() != PetPointerGesture::Mode::None) {
        pointerGesture_.advance(seconds);
        updatePointerGesture();
    }
    motion_.advance(seconds);
    if (motion_.consumeActionFinished()) controller_->actionFinished();
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        cubismCanvas_->advance(seconds);
        cubismFrame_ = cubismCanvas_->grabFramebuffer();
        if (!cubismFrame_.isNull())
            setInteractionMask(QPixmap::fromImage(composedFrame()));
    }
#endif
    updateInputTransparency();
    update();
}

void PetWindow::prepareLiveInteractionReview(int pixels) {
    frameTimer_.stop();
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

bool PetWindow::startGrassInteraction() {
    if ((!grassInteractionApproved() && !interactionPreviewEnabled_) || renderBackend() != QStringLiteral("cubism_native")
        || controller_->state() == PetController::State::Delete) return false;
    controller_->playInteractiveGrass(motion_.grassInteractionMaxDuration() + 1.0);
    if (motion_.beginGrassInteraction()) return true;
    controller_->actionFinished();
    return false;
}

void PetWindow::playGrass() {
    if ((grassInteractionApproved() || interactionPreviewEnabled_) && startGrassInteraction()) return;
    controller_->playGrass();
}

bool PetWindow::grassInteractionApproved() const {
    const auto* clip = motion_.library().clip(QStringLiteral("grass-touch"));
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

void PetWindow::setInteractionPreviewEnabled(bool enabled) {
    interactionPreviewEnabled_ = enabled;
    if (interactionPreviewAction_ && interactionPreviewAction_->isChecked() != enabled)
        interactionPreviewAction_->setChecked(enabled);
    if (!enabled) {
        patPreviewTimer_.stop();
        releasePointerGesture();
        motion_.cancelInteraction();
        if (motion_.grassInteractionActive() && !grassInteractionApproved()) controller_->actionFinished();
    }
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

bool PetWindow::event(QEvent* event) {
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)
        releasePointerGesture();
    if (event->type() == QEvent::Hide) {
        releasePointerGesture();
        patPreviewTimer_.stop();
        motion_.cancelInteraction();
        if (motion_.grassInteractionActive()) controller_->actionFinished();
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
                dragging_ = pointerGesture_.mode() == PetPointerGesture::Mode::Drag;
                return;
            }
        }
        motion_.updateHeadPat(pointerGesture_.normalizedPatDirection());
    } else if (pointerGesture_.mode() == PetPointerGesture::Mode::Drag) {
        dragging_ = true;
    }
}

void PetWindow::releasePointerGesture() {
    if (patAttempted_) motion_.endHeadPat();
    pointerGesture_.release();
    patAttempted_ = false;
    if (dragging_) {
        QSettings settings(QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
        settings.setValue(QStringLiteral("position"), pos());
    }
    dragging_ = false;
    grassTouchPressed_ = false;
}

void PetWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        releasePointerGesture();
        if (isGrassTipAt(event->position()) && motion_.respondToGrass()) {
            grassTouchPressed_ = true;
            updateInputTransparency();
            event->accept();
            return;
        }
        gestureWindowOrigin_ = pos();
        dragOffset_ = event->globalPosition().toPoint() - pos();
        const bool allowPat = interactionPreviewEnabled_ && motion_.canBeginHeadPat()
            && renderBackend() == QStringLiteral("cubism_native")
            && controller_->state() != PetController::State::Delete
            && controller_->state() != PetController::State::Grass;
        pointerGesture_.press(event->position(), allowPat && isHeadAt(event->position()));
        event->accept();
    }
}

void PetWindow::mouseMoveEvent(QMouseEvent* event) {
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
        if (dragging_) move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
    }
}

void PetWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        releasePointerGesture();
        updateInputTransparency();
        event->accept();
    }
}

void PetWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (motion_.grassInteractionActive()) {
            event->accept();
            return;
        }
        releasePointerGesture();
        playGrass();
    }
}

void PetWindow::contextMenuEvent(QContextMenuEvent* event) {
    showMenu(event->globalPos());
}

void PetWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void PetWindow::dragMoveEvent(QDragMoveEvent* event) {
    // Kept in sync with dragEnterEvent so the accepted cursor follows the
    // pointer across the character while the drag is in flight.
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void PetWindow::dropEvent(QDropEvent* event) {
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
    // Choreography mirrors delete.motion.json (3.2 s): the hand reaches out and
    // the grip closes at 0.62 s, the file is rolled into a wrap while the arm
    // settles, the mouth lunges forward between 1.30-1.45 s, and the bite
    // consumes the wrap right after. Anchors were measured on the 840 px probe
    // renders (fist at grip 0.75w/0.50h, fist raised 0.77w/0.44h, mouth once
    // the ShiftX=80 lunge lands 0.545w/0.44h).
    const double t = fedClock_.elapsed() / 1000.0;
    const double gripT = 0.62;
    const double rollT = 0.80;   // file has spun flat
    const double wrapT = 0.95;   // wrap fully formed
    const double raiseT = 1.30;
    const double biteT = 1.45;
    const double goneT = 1.62;
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
