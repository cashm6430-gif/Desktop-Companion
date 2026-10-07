#include "MemoBehavior.h"

#include "MotionMath.h"
#include "MotionParameterIds.h"

#include <QtMath>
#include <algorithm>
#include <cmath>
#include <numbers>

void MemoBehavior::begin()
{
    phase_ = Phase::Desk;
    time_ = 0.0;
    userEnd_ = kUserEnd;
    lookX_ = lookY_ = 0.0;
    nodTime_ = 0.0;
    laugh_ = false;
}

void MemoBehavior::beginCelebrate()
{
    phase_ = Phase::User;
    time_ = 0.0;
    userEnd_ = 0.7;
    nodTime_ = 0.6;
    laugh_ = true;
}

void MemoBehavior::cancel()
{
    phase_ = Phase::None;
    time_ = 0.0;
    userEnd_ = kUserEnd;
    lookX_ = lookY_ = 0.0;
    nodTime_ = 0.0;
    laugh_ = false;
}

double MemoBehavior::bubbleRise() const
{
    switch (phase_) {
    case Phase::Desk:
        return 0.0;
    case Phase::Rise:
        return motion::smooth(std::clamp((time_ - kDeskEnd) / (kRiseEnd - kDeskEnd), 0.0, 1.0));
    case Phase::User:
        return 1.0;
    default:
        return 0.0;
    }
}

void MemoBehavior::advance(double seconds)
{
    if (phase_ == Phase::None) return;
    time_ += seconds;
    if (nodTime_ > 0.0) nodTime_ -= seconds;
    double targetX = 0.0, targetY = 0.0;
    if (phase_ == Phase::Desk) {
        // First beat: glance down at the desk edge where the note appears.
        targetX = 0.15;
        targetY = -0.55;
        if (time_ >= kDeskEnd) phase_ = Phase::Rise;
    } else if (phase_ == Phase::Rise) {
        // The gaze rides the note as it floats up to half a head high.
        const double p = motion::smooth(std::clamp(
            (time_ - kDeskEnd) / (kRiseEnd - kDeskEnd), 0.0, 1.0));
        targetY = -0.55 + 1.0 * p;
        targetX = 0.15 * (1.0 - p);
        if (time_ >= kRiseEnd) {
            phase_ = Phase::User;
            nodTime_ = 0.6;
        }
    }
    if (phase_ == Phase::User) {
        // Look back at the user, pleased to have been trusted with this.
        targetX = 0.0;
        targetY = 0.1;
        if (nodTime_ <= 0.0 && time_ >= userEnd_) {
            cancel();
            return;
        }
    }
    const double alpha = 1.0 - qExp(-seconds / 0.09);
    lookX_ += (targetX - lookX_) * alpha;
    lookY_ += (targetY - lookY_) * alpha;
}

void MemoBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
    desired[QStringLiteral("ParamEyeBallX")] = std::clamp(
        desired.value(QStringLiteral("ParamEyeBallX")) + lookX_, -1.0, 1.0);
    desired[QStringLiteral("ParamEyeBallY")] = std::clamp(
        desired.value(QStringLiteral("ParamEyeBallY")) + lookY_, -1.0, 1.0);
    if (phase_ == Phase::User) {
        if (laugh_) {
            // Completion beat only: delighted laugh -- smiling CLOSED eyes
            // and a round O-shaped mouth (the wide grin art read as creepy).
            // The gape is the approved neutral "ah" art; the canvas gate
            // holds MouthOpenY fully open while it is selected, so the O
            // reads clean. The nod rides on top.
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 1.0);
            desired[motion::leftEye] = 0.0;
            desired[motion::rightEye] = 0.0;
            desired[QStringLiteral("ParamMouthGape")] = 1.0;
        } else {
            // Creating a note: hand the face back with a soft smile, the
            // eyes staying open -- the laugh is the completion reward.
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
        }
    }
    if (nodTime_ > 0.0) {
        const double p = 1.0 - nodTime_ / 0.6;
        auto angleY = desired.contains(QStringLiteral("ParamAngleY"))
            ? desired.value(QStringLiteral("ParamAngleY")) : 0.0;
        desired[QStringLiteral("ParamAngleY")] = angleY - 6.0 * std::sin(std::numbers::pi_v<double> * p);
    }
}
