#include "PetWindow.h"
#ifdef HAVE_CUBISM
#include "CubismCanvas.h"
#endif

#include <QApplication>
#include <QBitmap>
#include <QContextMenuEvent>
#include <QDir>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QtMath>

namespace {
QString imagePath(const char* name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/") + QLatin1String(name));
}
}

PetWindow::PetWindow(PetController* controller, QWidget* parent)
    : QWidget(parent),
      grassMovie_(imagePath("grass.gif")),
      idleImage_(imagePath("idle.png")),
      busyImage_(imagePath("busy.png")),
      deleteImage_(imagePath("delete.png")),
#ifdef HAVE_CUBISM
      cubismHitMask_(imagePath("live2d/whale-girl/hit-mask.png")),
#endif
      trayMenu_(this),
      controller_(controller) {
    setWindowTitle(QStringLiteral("DeepSeek 鲸鱼娘"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(280, 280);

#ifdef HAVE_CUBISM
    cubismCanvas_ = new CubismCanvas(&motion_, this);
    cubismCanvas_->setGeometry(rect());
    connect(cubismCanvas_, &CubismCanvas::readyChanged, this, [this] {
        setState(controller_->state());
    });
#endif

    connect(&grassMovie_, &QMovie::frameChanged, this, [this] { update(); });
    grassMovie_.setScaledSize(QSize(280, 280));
    grassMovie_.start();
    grassMovie_.setPaused(true);

    frameTimer_.setInterval(40);
    frameClock_.start();
    connect(&frameTimer_, &QTimer::timeout, this, [this] {
        ++frame_;
        const double seconds = frameClock_.restart() / 1000.0;
        motion_.advance(seconds);
#ifdef HAVE_CUBISM
        if (cubismCanvas_ && cubismCanvas_->isReady()) cubismCanvas_->advance(seconds);
#endif
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
    trayMenu_.addAction(QStringLiteral("重置忙碌状态"), controller_, &PetController::resetBusy);
    trayMenu_.addSeparator();
    trayMenu_.addAction(QStringLiteral("退出"), qApp, &QApplication::quit);
    tray_.setContextMenu(&trayMenu_);
    connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            if (isVisible()) hide(); else show();
        }
    });
    tray_.show();
}

void PetWindow::setState(PetController::State state) {
    motion_.setState(state);
    grassMovie_.setPaused(true);
    currentMovie_ = nullptr;
    frame_ = 0;
    const QPixmap* image = nullptr;
    switch (state) {
    case PetController::State::Idle: image = &idleImage_; break;
    case PetController::State::Busy: image = &busyImage_; break;
    case PetController::State::Grass: currentMovie_ = &grassMovie_; grassMovie_.jumpToFrame(0); break;
    case PetController::State::Delete: image = &deleteImage_; break;
    }
    if (currentMovie_) currentMovie_->setPaused(false);
#ifdef HAVE_CUBISM
    const bool useCubism = cubismCanvas_ && cubismCanvas_->isReady()
        && state != PetController::State::Grass;
    if (cubismCanvas_) cubismCanvas_->setVisible(useCubism);
    if (useCubism && !cubismHitMask_.isNull()) {
        const QImage alpha = cubismHitMask_.scaled(size(), Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation).toImage().createAlphaMask();
        setMask(QBitmap::fromImage(alpha));
        update();
        return;
    }
#endif
    if (image && !image->isNull()) {
        const QImage alpha = image->scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).toImage().createAlphaMask();
        setMask(QBitmap::fromImage(alpha));
    } else clearMask();
    update();
}

void PetWindow::paintEvent(QPaintEvent*) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()
        && controller_->state() != PetController::State::Grass) return;
#endif
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (currentMovie_) {
        const QPixmap image = currentMovie_->currentPixmap();
        if (!image.isNull()) painter.drawPixmap(rect(), image);
    } else if (controller_->state() == PetController::State::Delete && !deleteImage_.isNull()) {
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
    if (cubismCanvas_ && cubismCanvas_->isReady()
        && controller_->state() != PetController::State::Grass)
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

bool PetWindow::saveRenderFrame(const QString& path) {
#ifdef HAVE_CUBISM
    if (cubismCanvas_ && cubismCanvas_->isReady()
        && controller_->state() != PetController::State::Grass)
        return cubismCanvas_->grabFramebuffer().save(path);
#endif
    return grab().save(path);
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
