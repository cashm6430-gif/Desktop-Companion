#include "GazeBehavior.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
// Axis gains. The directional target is +1 = toward SCREEN-RIGHT, +1 down (the
// window frame of nx/ny).
//
// Sign probe (2026-10-11, native pose renders + red/cyan overlay, +/-14 and
// +/-30; see .local/authoring/gaze-sign-probe-20261011): all four axes are
// "positive = screen-right / screen-up" --
//   ParamEyeBallX +  -> iris moves RIGHT
//   ParamEyeBallY +  -> iris moves UP
//   ParamAngleX   +  -> head turns RIGHT
//   ParamAngleY   +  -> head pitches UP
// So looking right needs a POSITIVE EyeBallX and a positive AngleX; looking up
// (ny < 0) needs a POSITIVE EyeBallY and a positive AngleY. This is encoded
// below. (The class-comment SIGN NOTE used to claim the opposite for EyeBallX --
// that was wrong, and DragBehavior's += 0.6*lookX_ was right all along.)
constexpr double kEyeGainX = 0.42;    // ParamEyeBallX at a fully rightward look
constexpr double kEyeGainY = -0.30;   // ParamEyeBallY at a fully upward look (ny<0 -> +)
constexpr double kHeadGainX = 3.5;    // ParamAngleX degrees at a full look
constexpr double kHeadGainY = -2.5;   // ParamAngleY degrees at a full look (ny<0 -> +)
constexpr double kSmileLook = 0.30;   // soft "looking at you" eye smile

constexpr double kEyeTau = 0.13;      // eyes lead
constexpr double kHeadTau = 0.30;     // head follows, heavier
constexpr double kSmileTau = 0.35;
constexpr double kHeadDeadzone = 0.22; // small eye moves must not wag the head

// Idle glance cadence (only once the cursor has been still for a while). Kept
// slow so the pet reads as calm company, not a twitchy thing looking for
// attention.
constexpr double kGlanceGapMin = 3.5;
constexpr double kGlanceGapMax = 8.5;
constexpr double kGlanceHoldMin = 0.5;
constexpr double kGlanceHoldMax = 1.1;

// Idle trigger: seconds of a STILL cursor before she stops tracking and settles
// into idle glances. Mainstream desktop mascots key "alone" on the mouse going
// quiet, not on distance -- she keeps watching you wherever the cursor is.
constexpr double kIdleAfter = 6.0;

// Saccadic micro-drift: a tiny random eye offset re-aimed every ~0.5-1.5 s so a
// held gaze never freezes. Kept small (about a third of a pixel at 280 px).
constexpr double kMicroSaccadeAmp = 0.05;
constexpr double kSaccadeGapMin = 0.5;
constexpr double kSaccadeGapMax = 1.5;

double deadzone(double v)
{
    const double a = std::abs(v);
    if (a <= kHeadDeadzone) return 0.0;
    return std::copysign((a - kHeadDeadzone) / (1.0 - kHeadDeadzone), v);
}
} // namespace

void GazeBehavior::setTarget(double nx, double ny, bool nearCursor, bool cursorActive)
{
    targetX_ = std::clamp(nx, -1.0, 1.0);
    targetY_ = std::clamp(ny, -1.0, 1.0);
    near_ = nearCursor;
    cursorActive_ = cursorActive;
    hasTarget_ = true;
}

void GazeBehavior::reset()
{
    targetX_ = targetY_ = 0.0;
    near_ = false;
    cursorActive_ = true;
    hasTarget_ = false;
    stillSeconds_ = 0.0;
    suppressed_ = false;
    eyeX_ = eyeY_ = headX_ = headY_ = smile_ = 0.0;
    idle_ = 0.0;
    nextGlance_ = 3.0;
    glanceLeft_ = 0.0;
    glanceX_ = glanceY_ = 0.0;
    saccadeX_ = saccadeY_ = 0.0;
    saccadeClock_ = 0.0;
    nextSaccade_ = kSaccadeGapMin;
}

void GazeBehavior::scheduleGlance()
{
    glanceX_ = (rng_.generateDouble() * 2.0 - 1.0) * 0.75;
    glanceY_ = (rng_.generateDouble() * 2.0 - 1.0) * 0.45;
    glanceLeft_ = kGlanceHoldMin + rng_.generateDouble() * (kGlanceHoldMax - kGlanceHoldMin);
    nextGlance_ = kGlanceGapMin + rng_.generateDouble() * (kGlanceGapMax - kGlanceGapMin);
}

void GazeBehavior::advance(double seconds, bool suppressed)
{
    suppressed_ = suppressed;

    // Idle is triggered by a STILL cursor (the user left the mouse), not by the
    // cursor being far -- she keeps watching you wherever the mouse goes.
    if (cursorActive_) stillSeconds_ = 0.0;
    else stillSeconds_ += seconds;
    const bool idling = !suppressed && !near_ && stillSeconds_ >= kIdleAfter;

    double tx = 0.0, ty = 0.0;
    double smileTarget = 0.0;
    if (suppressed) {
        // Stand down completely and forget any in-flight glance, so re-entry
        // starts from a clean, centred eye.
        idle_ = 0.0;
        glanceLeft_ = 0.0;
        nextGlance_ = kGlanceGapMin;
    } else if (idling) {
        // The user left the mouse alone: settle to centre, and every few seconds
        // take one short glance off to the side before returning to the user.
        idle_ += seconds;
        if (glanceLeft_ > 0.0) {
            glanceLeft_ -= seconds;
            tx = glanceX_;
            ty = glanceY_;
        } else if (idle_ >= nextGlance_) {
            idle_ = 0.0;
            scheduleGlance();
        }
    } else {
        // FOLLOW: track the cursor wherever it is; "near" only adds the smile.
        tx = targetX_ + saccadeX_;
        ty = targetY_ + saccadeY_;
        smileTarget = near_ ? kSmileLook : 0.0;
        idle_ = 0.0;
        glanceLeft_ = 0.0;
    }

    // Re-aim the micro-saccade every ~1 s (once we have a cursor feed). Small,
    // and deadzoned out of the head, so it reads as "alive", not "twitchy".
    if (hasTarget_) {
        saccadeClock_ += seconds;
        if (saccadeClock_ >= nextSaccade_) {
            saccadeClock_ = 0.0;
            nextSaccade_ = kSaccadeGapMin + rng_.generateDouble() * (kSaccadeGapMax - kSaccadeGapMin);
            saccadeX_ = (rng_.generateDouble() * 2.0 - 1.0) * kMicroSaccadeAmp;
            saccadeY_ = (rng_.generateDouble() * 2.0 - 1.0) * kMicroSaccadeAmp;
        }
    }

    const double eEye = 1.0 - qExp(-seconds / kEyeTau);
    const double eHead = 1.0 - qExp(-seconds / kHeadTau);
    const double eSmile = 1.0 - qExp(-seconds / kSmileTau);
    eyeX_ += (std::clamp(tx, -1.0, 1.0) - eyeX_) * eEye;
    eyeY_ += (std::clamp(ty, -1.0, 1.0) - eyeY_) * eEye;
    smile_ += (smileTarget - smile_) * eSmile;
    headX_ += (deadzone(eyeX_) - headX_) * eHead;
    headY_ += (deadzone(eyeY_) - headY_) * eHead;
}

void GazeBehavior::apply(Parameters& desired) const
{
    if (suppressed_) return;

    const QString eyeBallX = QStringLiteral("ParamEyeBallX");
    const QString eyeBallY = QStringLiteral("ParamEyeBallY");
    // Additive: the background state and any authored clip keep their own gaze;
    // the cursor only nudges it. A centred look adds nothing.
    desired[eyeBallX] = std::clamp(
        desired.value(eyeBallX) + kEyeGainX * eyeX_, -1.0, 1.0);
    desired[eyeBallY] = std::clamp(
        desired.value(eyeBallY) + kEyeGainY * eyeY_, -1.0, 1.0);

    const QString angleX = QStringLiteral("ParamAngleX");
    const QString angleY = QStringLiteral("ParamAngleY");
    // Clamp to the model's real head range (+/-30), not a tighter feel limit:
    // clamping the sum to a smaller value would truncate a head angle an authored
    // clip already set (the class contract is "never removes what is already
    // there"). The gaze's own contribution is tiny (kHeadGainX/Y ~ a few degrees).
    desired[angleX] = std::clamp(
        desired.value(angleX) + kHeadGainX * headX_, -30.0, 30.0);
    desired[angleY] = std::clamp(
        desired.value(angleY) + kHeadGainY * headY_, -30.0, 30.0);

    if (smile_ > 0.0) {
        const QString eyeSmile = QStringLiteral("ParamEyeSmile");
        desired[eyeSmile] = std::max(desired.value(eyeSmile), smile_);
    }
}
