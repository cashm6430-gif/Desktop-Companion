#pragma once

#include "ParameterMotion.h"

#include <QOpenGLWidget>
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
    double frameSeconds_ = 0.04;
};
