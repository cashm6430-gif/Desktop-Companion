#include "ParameterMotion.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
const QString angleX = QStringLiteral("ParamAngleX");
const QString angleY = QStringLiteral("ParamAngleY");
const QString angleZ = QStringLiteral("ParamAngleZ");
const QString bodyX = QStringLiteral("ParamBodyAngleX");
const QString bodyY = QStringLiteral("ParamBodyAngleY");
const QString bodyZ = QStringLiteral("ParamBodyAngleZ");
const QString breath = QStringLiteral("ParamBreath");
const QString leftEye = QStringLiteral("ParamEyeLOpen");
const QString rightEye = QStringLiteral("ParamEyeROpen");
const QString leftArm = QStringLiteral("ParamArmLA");
const QString rightArm = QStringLiteral("ParamArmRA");
const QString mouth = QStringLiteral("ParamMouthOpenY");
const QString cheek = QStringLiteral("ParamCheek");
constexpr double pi = 3.14159265358979323846;
}

double ParameterMotion::smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double ParameterMotion::pulse(double t, double start, double peak, double end) {
    if (t < start || t >= end) return 0.0;
    if (t < peak) return smooth((t - start) / (peak - start));
    return 1.0 - smooth((t - peak) / (end - peak));
}

double ParameterMotion::target(const Parameters& values, const QString& id) {
    return values.value(id, 0.0);
}

void ParameterMotion::setState(PetController::State state) {
    if (state_ == state && state != PetController::State::Delete
        && state != PetController::State::Grass) return;
    state_ = state;
    actionTime_ = 0.0;
    // Keep current parameter values. The next advance blends from the current pose.
}

void ParameterMotion::advance(double seconds) {
    if (!qIsFinite(seconds) || seconds <= 0.0) return;
    seconds = std::min(seconds, 0.1); // Avoid a leap after suspend or debugger pause.
    clock_ += seconds;
    actionTime_ += seconds;
    blinkClock_ += seconds;

    Parameters desired{
        {angleX, 2.0 * qSin(clock_ * 0.68)},
        {angleY, 1.5 * qSin(clock_ * 0.53)},
        {angleZ, 1.0 * qSin(clock_ * 0.91)},
        {bodyX, 0.8 * qSin(clock_ * 0.68 - 0.3)},
        {bodyY, 0.5 * qSin(clock_ * 1.7)},
        {bodyZ, 0.0},
        {breath, 0.5 + 0.5 * qSin(clock_ * 2.1)},
        {leftArm, 0.0}, {rightArm, 0.0},
        {mouth, 0.0}, {cheek, 0.0},
    };

    if (state_ == PetController::State::Busy) {
        desired[angleY] += -7.0 + 1.8 * qSin(clock_ * 4.3);
        desired[bodyX] += 2.0;
        desired[leftArm] = 9.0 + 5.0 * qSin(clock_ * 10.0);
        desired[rightArm] = -9.0 + 5.0 * qSin(clock_ * 10.0 + pi);
        desired[mouth] = 0.12;
    } else if (state_ == PetController::State::Delete) {
        // Anticipation -> swing -> recoil. A rigged arm/prop responds to these
        // parameters continuously; no pose swapping or GIF frame stepping.
        const double windup = pulse(actionTime_, 0.0, 0.28, 0.48);
        const double strike = pulse(actionTime_, 0.34, 0.59, 0.91);
        const double recoil = pulse(actionTime_, 0.82, 1.02, 1.34);
        desired[rightArm] = -22.0 * windup + 30.0 * strike - 8.0 * recoil;
        desired[leftArm] = 8.0 * windup - 5.0 * strike;
        desired[bodyZ] = -7.0 * windup + 10.0 * strike - 3.0 * recoil;
        desired[angleZ] += -6.0 * windup + 8.0 * strike;
        desired[angleY] += 4.0 * strike;
        desired[mouth] = 0.4 * strike;
    } else if (state_ == PetController::State::Grass) {
        const double reach = pulse(actionTime_, 0.15, 1.2, 5.8);
        desired[angleX] += 10.0 * reach;
        desired[angleY] += -4.0 * reach;
        desired[bodyX] += 5.0 * reach;
        desired[rightArm] = 25.0 * reach + 2.0 * qSin(clock_ * 3.2);
        desired[cheek] = 0.35 * reach;
    }

    // Short asymmetric blink. It stays procedural across action transitions.
    const double blinkPhase = std::fmod(blinkClock_, 4.3);
    const double eye = 1.0 - pulse(blinkPhase, 0.0, 0.075, 0.16);
    desired[leftEye] = eye;
    desired[rightEye] = eye;

    // Time-based exponential blending is stable at both 30 and 60 Hz.
    const double alpha = 1.0 - qExp(-seconds / 0.12);
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const double previous = values_.contains(it.key()) ? values_.value(it.key()) : it.value();
        values_[it.key()] = previous + (it.value() - previous) * alpha;
    }
}
