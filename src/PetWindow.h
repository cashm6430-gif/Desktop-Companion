#pragma once

#include "PetController.h"
#include "ParameterMotion.h"

#include <QMovie>
#include <QElapsedTimer>
#include <QImage>
#include <QMenu>
#include <QPoint>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#ifdef HAVE_CUBISM
class CubismCanvas;
#endif

class PetWindow final : public QWidget {
    Q_OBJECT
public:
    explicit PetWindow(PetController* controller, QWidget* parent = nullptr);
    ~PetWindow() override;
    const ParameterMotion::Parameters& motionParameters() const { return motion_.values(); }
    QString renderBackend() const;
    QString renderError() const;
    int renderSampleCount() const;
    bool saveRenderFrame(const QString& path);
    void shutdown();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void setState(PetController::State state);
    void setInteractionMask(const QPixmap& artwork);
    void updateInputTransparency();
    void showMenu(const QPoint& globalPos);
    QMovie grassMovie_;
    QPixmap idleImage_;
    QPixmap busyImage_;
    QPixmap deleteImage_;
#ifdef HAVE_CUBISM
    QPixmap cubismHitMask_;
    QImage cubismFrame_;
    CubismCanvas* cubismCanvas_ = nullptr;
#endif
    QMovie* currentMovie_ = nullptr;
    QSystemTrayIcon tray_;
    QMenu trayMenu_;
    QTimer frameTimer_;
    QImage hitCoverage_;
    QElapsedTimer frameClock_;
    ParameterMotion motion_;
    PetController* controller_;
    QPoint dragOffset_;
    bool dragging_ = false;
    int frame_ = 0;
};
