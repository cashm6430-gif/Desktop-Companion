#pragma once

#include "ParameterMotion.h"

#include <QJsonObject>
#include <QOpenGLWidget>
#include <QPainterPath>
#include <QString>
#include <memory>

class CubismCanvas final : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit CubismCanvas(const ParameterMotion* motion, QWidget* parent = nullptr);
    ~CubismCanvas() override;

    bool isReady() const { return ready_; }
    QString error() const { return error_; }
    int sampleCount() const { return sampleCount_; }
    void advance(double seconds);
    // Parameter id -> {min, max, default} straight out of the MOC3. Name lists
    // (the CDI3, the PSD2Live metadata) do not carry limits, and an authored
    // curve can only be judged against the range the rig actually accepts.
    // Empty before the canvas has loaded the model.
    QJsonObject parameterRanges() const;
    // Logical widget coordinates of the latest rendered head mesh. Uses the
    // same grounded transform as the picture, so input follows head/seat motion.
    const QPainterPath& headHitPath() const { return headHitPath_; }
    const QPainterPath& grassTipHitPath() const { return grassTipHitPath_; }
    // How far the face moved down this frame (widget pixels) because the head
    // rides the seated collar target. Window-layer art that is glued to the
    // face -- the nap eye mask -- adds this so it sinks with the head.
    double headRideY() const { return headRideY_; }

signals:
    void readyChanged(bool ready);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    const ParameterMotion* motion_;
    bool ready_ = false;
    QString error_;
    int sampleCount_ = -1;
    double frameSeconds_ = 0.0;
    QPainterPath headHitPath_;
    QPainterPath grassTipHitPath_;
    double headRideY_ = 0.0;
};
