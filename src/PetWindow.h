#pragma once

#include "PetController.h"
#include "ParameterMotion.h"

#include <QMovie>
#include <QElapsedTimer>
#include <QMenu>
#include <QPoint>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

class PetWindow final : public QWidget {
    Q_OBJECT
public:
    explicit PetWindow(PetController* controller, QWidget* parent = nullptr);
    const ParameterMotion::Parameters& motionParameters() const { return motion_.values(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void setState(PetController::State state);
    void showMenu(const QPoint& globalPos);
    QMovie grassMovie_;
    QPixmap idleImage_;
    QPixmap busyImage_;
    QPixmap deleteImage_;
    QMovie* currentMovie_ = nullptr;
    QSystemTrayIcon tray_;
    QMenu trayMenu_;
    QTimer frameTimer_;
    QElapsedTimer frameClock_;
    ParameterMotion motion_;
    PetController* controller_;
    QPoint dragOffset_;
    bool dragging_ = false;
    int frame_ = 0;
};
