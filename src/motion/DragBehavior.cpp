#include "DragBehavior.h"

#include "MotionMath.h"

#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace {
constexpr double kInputFreshSeconds = 0.10;
}

void DragBehavior::begin()
{
    phase_ = Phase::Follow;
    lookX_ = lookY_ = lookTX_ = lookTY_ = 0.0;
    hairX_ = hairTarget_ = bodyX_ = bodyTarget_ = 0.0;
    inputAge_ = distance_ = 0.0;
    appliedEyeX_ = appliedEyeY_ = releaseEyeX_ = releaseEyeY_ = 0.0;
    eyeApplied_ = false;
    curiousDone_ = false;
    curiousTime_ = nodTime_ = settleTime_ = 0.0;
}

void DragBehavior::update(double vx, double vy, double travelledPixels)
{
    if (phase_ != Phase::Follow || !qIsFinite(vx) || !qIsFinite(vy)
        || !qIsFinite(travelledPixels) || travelledPixels < 0.0) return;
    inputAge_ = 0.0;
    lookTX_ = std::clamp(vx / 600.0, -1.0, 1.0);
    lookTY_ = std::clamp(vy / 900.0, -0.6, 0.6);
    // Hair streams opposite to the motion: it lags behind the body.
    hairTarget_ = std::clamp(-vx / 500.0, -1.0, 1.0);
    bodyTarget_ = std::clamp(-vx / 110.0, -10.0, 10.0);
    const double limit = std::numeric_limits<double>::max();
    distance_ = travelledPixels > limit - distance_ ? limit : distance_ + travelledPixels;
    // Only a newly measured path can earn this once-per-drag look-back.
    // A held but stationary pointer must not accumulate imaginary distance.
    if (!curiousDone_ && distance_ > 400.0) {
        curiousDone_ = true;
        curiousTime_ = 0.6;
    }
}

void DragBehavior::end()
{
    if (phase_ != Phase::Follow) return;
    phase_ = Phase::Settle;
    // Follow may currently be looking back (absolute eyes 0/0.1), rather than
    // applying the lagged lookX/Y. Carry the actual last contribution so
    // release cannot resurrect the old direction for one frame.
    releaseEyeX_ = eyeApplied_ ? appliedEyeX_ : 0.6 * lookX_;
    releaseEyeY_ = eyeApplied_ ? appliedEyeY_ : 0.4 * lookY_;
    lookTX_ = lookTY_ = hairTarget_ = bodyTarget_ = 0.0;
    // Release: residual sway decays while the eyes come back to the user,
    // plus one light nod on arrival.
    curiousTime_ = 0.5;
    nodTime_ = 0.7;
    settleTime_ = 2.5;
}

void DragBehavior::cancel()
{
    phase_ = Phase::None;
    lookX_ = lookY_ = lookTX_ = lookTY_ = 0.0;
    hairX_ = hairTarget_ = bodyX_ = bodyTarget_ = 0.0;
    inputAge_ = distance_ = 0.0;
    appliedEyeX_ = appliedEyeY_ = releaseEyeX_ = releaseEyeY_ = 0.0;
    eyeApplied_ = false;
    curiousDone_ = false;
    curiousTime_ = nodTime_ = settleTime_ = 0.0;
}

void DragBehavior::advance(double seconds)
{
    if (phase_ == Phase::None || !qIsFinite(seconds) || seconds <= 0.0) return;
    if (phase_ == Phase::Follow) {
        const auto followStep = [this](double dt) {
            if (dt <= 0.0) return;
            if (curiousTime_ > 0.0) {
                curiousTime_ = std::max(0.0, curiousTime_ - dt);
                lookTX_ = lookTY_ = 0.0;
            }
            lookX_ += (lookTX_ - lookX_) * (1.0 - qExp(-dt / 0.12));
            lookY_ += (lookTY_ - lookY_) * (1.0 - qExp(-dt / 0.14));
            // Wider time constants than the eyes: hair and body read as mass.
            hairX_ += (hairTarget_ - hairX_) * (1.0 - qExp(-dt / 0.24));
            bodyX_ += (bodyTarget_ - bodyX_) * (1.0 - qExp(-dt / 0.18));
        };
        // Split a frame at expiry rather than rounding the timeout to a whole
        // frame. The same input trace therefore releases at 30/60/120 Hz.
        const double fresh = std::clamp(kInputFreshSeconds - inputAge_, 0.0, seconds);
        followStep(fresh);
        inputAge_ = std::min(60.0, inputAge_ + seconds);
        if (inputAge_ >= kInputFreshSeconds - 1e-9)
            lookTX_ = lookTY_ = hairTarget_ = bodyTarget_ = 0.0;
        followStep(seconds - fresh);
    } else {
        // Settle: exponential decay only -- the card explicitly forbids a
        // persistent sine sway after the drag stops. The slow 0.45s constant
        // keeps the lean/hair visibly swinging for a second or so, and a hard
        // cap guarantees the phase always ends.
        if (settleTime_ > 0.0) {
            settleTime_ -= seconds;
            if (settleTime_ <= 0.0) {
                cancel();
                return;
            }
        }
        const double decay = qExp(-seconds / 0.45);
        lookX_ *= decay; lookY_ *= decay;
        releaseEyeX_ *= decay; releaseEyeY_ *= decay;
        hairX_ *= decay; bodyX_ *= decay;
        if (curiousTime_ > 0.0) curiousTime_ -= seconds;
        if (nodTime_ > 0.0) nodTime_ -= seconds;
        if (qAbs(lookX_) < 0.02 && qAbs(lookY_) < 0.02
            && qAbs(releaseEyeX_) < 0.012 && qAbs(releaseEyeY_) < 0.008
            && qAbs(hairX_) < 0.02 && qAbs(bodyX_) < 0.05
            && nodTime_ <= 0.0 && curiousTime_ <= 0.0)
            cancel();
    }
}

void DragBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
    const double baseEyeX = desired.value(QStringLiteral("ParamEyeBallX"));
    const double baseEyeY = desired.value(QStringLiteral("ParamEyeBallY"));
    if (phase_ == Phase::Follow) {
        if (curiousTime_ > 0.0) {
            // Looking at the user instead of the drag direction, slightly pleased.
            desired[QStringLiteral("ParamEyeBallX")] = 0.0;
            desired[QStringLiteral("ParamEyeBallY")] = 0.1;
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
        } else {
            auto eyeball = desired.contains(QStringLiteral("ParamEyeBallX"))
                ? desired.value(QStringLiteral("ParamEyeBallX")) : 0.0;
            desired[QStringLiteral("ParamEyeBallX")] = std::clamp(eyeball + 0.6 * lookX_, -1.0, 1.0);
            auto eyeballY = desired.contains(QStringLiteral("ParamEyeBallY"))
                ? desired.value(QStringLiteral("ParamEyeBallY")) : 0.0;
            desired[QStringLiteral("ParamEyeBallY")] = std::clamp(eyeballY + 0.4 * lookY_, -1.0, 1.0);
        }
    } else {
        desired[QStringLiteral("ParamEyeBallX")] = std::clamp(baseEyeX + releaseEyeX_, -1.0, 1.0);
        desired[QStringLiteral("ParamEyeBallY")] = std::clamp(baseEyeY + releaseEyeY_, -1.0, 1.0);
        if (curiousTime_ > 0.0)
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
    }
    appliedEyeX_ = desired.value(QStringLiteral("ParamEyeBallX")) - baseEyeX;
    appliedEyeY_ = desired.value(QStringLiteral("ParamEyeBallY")) - baseEyeY;
    eyeApplied_ = true;
    auto hairFront = desired.contains(QStringLiteral("ParamHairFront"))
        ? desired.value(QStringLiteral("ParamHairFront")) : 0.0;
    desired[QStringLiteral("ParamHairFront")] = std::clamp(hairFront + 0.65 * hairX_, -1.0, 1.0);
    auto hairBack = desired.contains(QStringLiteral("ParamHairBack"))
        ? desired.value(QStringLiteral("ParamHairBack")) : 0.0;
    desired[QStringLiteral("ParamHairBack")] = std::clamp(hairBack + 0.45 * hairX_, -1.0, 1.0);
    auto bodyAngleX = desired.contains(QStringLiteral("ParamBodyAngleX"))
        ? desired.value(QStringLiteral("ParamBodyAngleX")) : 0.0;
    desired[QStringLiteral("ParamBodyAngleX")] = std::clamp(bodyAngleX + bodyX_, -12.0, 12.0);
    if (nodTime_ > 0.0) {
        // One light nod on release: a single sine hump, down and back.
        const double p = 1.0 - nodTime_ / 0.7;
        auto angleY = desired.contains(QStringLiteral("ParamAngleY"))
            ? desired.value(QStringLiteral("ParamAngleY")) : 0.0;
        desired[QStringLiteral("ParamAngleY")] = angleY - 10.0 * std::sin(std::numbers::pi_v<double> * p);
    }
}
