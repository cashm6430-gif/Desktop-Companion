#include "DragBehavior.h"

#include "MotionMath.h"

#include <QtMath>
#include <algorithm>
#include <cmath>
#include <numbers>

void DragBehavior::begin()
{
    phase_ = Phase::Follow;
    lookX_ = lookY_ = lookTX_ = lookTY_ = 0.0;
    hairX_ = hairTarget_ = bodyX_ = bodyTarget_ = 0.0;
    speed_ = distance_ = 0.0;
    curiousDone_ = false;
    curiousTime_ = nodTime_ = settleTime_ = 0.0;
}

void DragBehavior::update(double vx, double vy)
{
    if (phase_ != Phase::Follow || !qIsFinite(vx) || !qIsFinite(vy)) return;
    lookTX_ = std::clamp(vx / 600.0, -1.0, 1.0);
    lookTY_ = std::clamp(vy / 900.0, -0.6, 0.6);
    // Hair streams opposite to the motion: it lags behind the body.
    hairTarget_ = std::clamp(-vx / 500.0, -1.0, 1.0);
    bodyTarget_ = std::clamp(-vx / 110.0, -10.0, 10.0);
    speed_ = std::sqrt(vx * vx + vy * vy);
}

void DragBehavior::end()
{
    if (phase_ != Phase::Follow) return;
    phase_ = Phase::Settle;
    lookTX_ = lookTY_ = hairTarget_ = bodyTarget_ = 0.0;
    speed_ = 0.0;
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
    speed_ = distance_ = 0.0;
    curiousDone_ = false;
    curiousTime_ = nodTime_ = settleTime_ = 0.0;
}

void DragBehavior::advance(double seconds)
{
    if (phase_ == Phase::None) return;
    if (phase_ == Phase::Follow) {
        distance_ += speed_ * seconds;
        // One curious look back at the user per long drag: eyes leave the drag
        // direction briefly, then return to following the motion.
        if (!curiousDone_ && distance_ > 400.0) {
            curiousDone_ = true;
            curiousTime_ = 0.6;
        }
        if (curiousTime_ > 0.0) {
            curiousTime_ -= seconds;
            lookTX_ = lookTY_ = 0.0;
        }
        lookX_ += (lookTX_ - lookX_) * (1.0 - qExp(-seconds / 0.12));
        lookY_ += (lookTY_ - lookY_) * (1.0 - qExp(-seconds / 0.14));
        // Wider time constants than the eyes: hair and body read as mass.
        hairX_ += (hairTarget_ - hairX_) * (1.0 - qExp(-seconds / 0.24));
        bodyX_ += (bodyTarget_ - bodyX_) * (1.0 - qExp(-seconds / 0.18));
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
        hairX_ *= decay; bodyX_ *= decay;
        if (curiousTime_ > 0.0) curiousTime_ -= seconds;
        if (nodTime_ > 0.0) nodTime_ -= seconds;
        if (qAbs(lookX_) < 0.02 && qAbs(hairX_) < 0.02 && qAbs(bodyX_) < 0.05
            && nodTime_ <= 0.0 && curiousTime_ <= 0.0)
            cancel();
    }
}

void DragBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
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
    } else if (curiousTime_ > 0.0) {
        // Release look-back: keep the soft smile, but let the eyes glide home
        // through the decaying lookX_ -- snapping them to zero and back
        // read as a visible jump once the window expired.
        desired[QStringLiteral("ParamEyeSmile")] = std::max(
            desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
    }
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
