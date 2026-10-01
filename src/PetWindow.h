#pragma once

#include "PetController.h"
#include "ParameterMotion.h"

#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
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
    // Parameter id -> {min, max, default} from the loaded MOC3; empty when the
    // Native backend is not up, so callers can tell "no data" from "no range".
    QJsonObject modelParameterRanges() const;
    bool saveRenderFrame(const QString& path);
    void shutdown();
    void setPreviewPose(const ParameterMotion::Parameters& parameters, int pixels = 840);
    bool renderSequenceFrame(const ParameterMotion::Parameters& parameters, double seconds, const QString& path);

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
    QPixmap idleImage_;
    QPixmap busyImage_;
    QPixmap deleteImage_;
#ifdef HAVE_CUBISM
    QPixmap cubismHitMask_;
    QImage cubismFrame_;
    CubismCanvas* cubismCanvas_ = nullptr;
#endif
    QSystemTrayIcon tray_;
    QMenu trayMenu_;
    QTimer frameTimer_;
    QTimer laptopPreviewTimer_;
    QImage hitCoverage_;
    QElapsedTimer frameClock_;
    ParameterMotion motion_;
    PetController* controller_;
    QPoint dragOffset_;
    bool dragging_ = false;
    int frame_ = 0;
};
