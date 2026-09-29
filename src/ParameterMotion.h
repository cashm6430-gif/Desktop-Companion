#pragma once

#include "PetController.h"

#include <QHash>
#include <QString>

// Values are Cubism parameter units, not screen pixels or pre-rendered frames.
// The model artist must rig these IDs (or provide an explicit mapping).
class ParameterMotion final {
public:
    using Parameters = QHash<QString, double>;

    void setState(PetController::State state);
    void advance(double seconds);
    const Parameters& values() const { return values_; }

private:
    static double smooth(double t);
    static double pulse(double t, double start, double peak, double end);
    static double target(const Parameters& values, const QString& id);

    PetController::State state_ = PetController::State::Idle;
    Parameters values_;
    double clock_ = 0.0;
    double actionTime_ = 0.0;
    double blinkClock_ = 0.0;
};
