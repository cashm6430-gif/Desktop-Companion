#pragma once

#include "PetController.h"

#include <QHash>
#include <QString>
#include <QVector>

// Values are Cubism parameter units, not screen pixels or pre-rendered frames.
// The model artist must rig these IDs (or provide an explicit mapping).
class ParameterMotion final {
public:
    using Parameters = QHash<QString, double>;

    void setState(PetController::State state);
    void advance(double seconds);
    bool loadGrassMotion(const QString& path, QString* error = nullptr);
    Parameters grassPose(double seconds) const;
    void setPreviewPose(const Parameters& parameters);
    bool isPreview() const { return preview_; }
    const Parameters& values() const { return values_; }

private:
    static double smooth(double t);
    static double pulse(double t, double start, double peak, double end);
    static double target(const Parameters& values, const QString& id);
    void updateGrassFlex(double seconds, double target);

    PetController::State state_ = PetController::State::Idle;
    Parameters values_;
    double clock_ = 0.0;
    double actionTime_ = 0.0;
    double blinkClock_ = 0.0;
    double leftEyeExpression_ = 1.0;
    double rightEyeExpression_ = 1.0;
    struct Keyframe { double time; Parameters parameters; };
    QVector<Keyframe> grassKeys_;
    bool preview_ = false;
    // Two damped modes: the stem follows the grip and the softer tip trails it.
    double grassBend_ = 0.0;
    double grassBendVelocity_ = 0.0;
    double grassTip_ = 0.0;
    double grassTipVelocity_ = 0.0;
    double previousGripAngle_ = 0.0;
    double previousReach_ = 0.0;
};
