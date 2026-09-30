#include "PetWindow.h"
#ifdef HAVE_CUBISM
#include "CubismCanvas.h"
#endif

#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QtMath>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString imagePath(const char* name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/") + QLatin1String(name));
}
}

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
    if (!motion_.loadGrassMotion(imagePath("motions/grass.motion.json"), &motionError))
        qWarning() << "Grass motion:" << motionError;
    if (!motion_.loadBusyLaptopMotion(imagePath("motions/busy-laptop.motion.json"), &motionError))
        qWarning() << "Laptop motion:" << motionError;

    frameTimer_.setInterval(40);
    frameClock_.start();
    connect(&frameTimer_, &QTimer::timeout, this, [this] {
        ++frame_;
        const double seconds = frameClock_.restart() / 1000.0;
        motion_.advance(seconds);
#ifdef HAVE_CUBISM
        if (cubismCanvas_ && cubismCanvas_->isReady()) {
            cubismCanvas_->advance(seconds);
            cubismFrame_ = cubismCanvas_->grabFramebuffer();
            if (!cubismFrame_.isNull()) setInteractionMask(QPixmap::fromImage(cubismFrame_));
        }
#endif
        updateInputTransparency();
        update();
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
    trayMenu_.addAction(QStringLiteral("播放删除动作"), controller_, &PetController::desktopItemDeleted);
    trayMenu_.addAction(QStringLiteral("玩狗尾巴草"), controller_, &PetController::playGrass);
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
    motion_.setState(state);
    frame_ = 0;
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
    const bool interactive = (dragging_ && buttonHeld)
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
    tray_.hide();
}

void PetWindow::paintEvent(QPaintEvent*) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        if (!cubismFrame_.isNull()) painter.drawImage(rect(), cubismFrame_);
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

bool PetWindow::saveRenderFrame(const QString& path) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady())
        return (cubismFrame_.isNull() ? cubismCanvas_->grabFramebuffer() : cubismFrame_).save(path);
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

bool PetWindow::renderSequenceFrame(const ParameterMotion::Parameters& parameters, double seconds, const QString& path) {
    frameTimer_.stop();
    motion_.setSequencePose(parameters);
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()) {
        cubismCanvas_->advance(seconds);
        cubismFrame_ = cubismCanvas_->grabFramebuffer();
        return cubismFrame_.save(path);
    }
#endif
    return false;
}

void PetWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - pos();
        event->accept();
    }
}

void PetWindow::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
    }
}

void PetWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        QSettings settings(QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
        settings.setValue(QStringLiteral("position"), pos());
        updateInputTransparency();
        event->accept();
    }
}

void PetWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) controller_->playGrass();
}

void PetWindow::contextMenuEvent(QContextMenuEvent* event) {
    showMenu(event->globalPos());
}

void PetWindow::showMenu(const QPoint& globalPos) {
    trayMenu_.exec(globalPos);
}
