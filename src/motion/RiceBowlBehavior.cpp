#include "RiceBowlBehavior.h"

#include "MotionMath.h"

#include <algorithm>
#include <QCoreApplication>
#include <cstdio>

namespace {
// Choreography constants (seconds), matching the approved concept card.
constexpr double kSlideDuration = 0.5;  // bowl glides onto the desk
constexpr double kSniffDuration = 0.9;  // eyes close, head tips, steam thickens
constexpr double kSmileDuration = 0.7;  // eyes reopen into the omega smile + nod
constexpr double kHoldSeconds = 2.2;    // happy pause before the bowl leaves
constexpr double kLeanDuration = 0.35;  // steam click: lean away
constexpr double kPeekDuration = 0.5;   // steam click: curious peek back
constexpr double kRelaxDuration = 0.35; // exit leg 1: face relaxes
constexpr double kLeaveDuration = 0.55; // exit leg 2: bowl slides off

// Gaze targets. The bowl sits right of the laptop and below the eye line.
// X is capped at 0.3: at >=0.4 the right iris reaches the eyewhite edge and
// shows a blue rim (user feedback on the concept card).
constexpr double kGazeX = 0.3;
constexpr double kGazeY = -0.35;
constexpr double kLeanGazeX = 0.35;  // brief glance up at the steam, allowed

// Face targets per channel (the rest of each phase glides toward zeros).
constexpr double kSniffTilt = 6.0;    // AngleY degrees toward the bowl
constexpr double kSmileTilt = 3.0;    // the nod settles here
constexpr double kLeanTilt = -5.0;    // leaning away from the steam
constexpr double kPeekTilt = 4.0;
}  // namespace

void RiceBowlBehavior::begin()
{
    phase_ = Phase::SlideIn;
    time_ = 0.0;
    phaseEnd_ = kSlideDuration;
    ch_ = from_ = to_ = Channels{};
    to_.slide = 1.0;
    to_.gazeX = kGazeX;
    to_.gazeY = kGazeY;
    to_.steam = 0.7;
}

void RiceBowlBehavior::exit()
{
    if (phase_ == Phase::None || phase_ == Phase::SlideOut) return;
    // Leg 1 relaxes the face (and finishes any slide-in inside the same
    // glide so the prop never jumps), leg 2 slides the bowl off.
    glideTo(Phase::SlideOut, Channels{}, kRelaxDuration);
    to_.slide = 1.0;
    to_.steam = 0.0;
}

void RiceBowlBehavior::cancel()
{
    phase_ = Phase::None;
    time_ = 0.0;
    phaseEnd_ = 0.0;
    ch_ = from_ = to_ = Channels{};
}

void RiceBowlBehavior::poke()
{
    if (phase_ != Phase::SlideIn && phase_ != Phase::Sniff && phase_ != Phase::Smile
        && phase_ != Phase::Hold)
        return;
    Channels target;
    target.gazeX = kLeanGazeX;
    target.gazeY = -0.5;
    target.headTilt = kLeanTilt;
    target.steam = ch_.steam;
    target.slide = 1.0;
    glideTo(Phase::LeanAway, target, kLeanDuration);
}

void RiceBowlBehavior::glideTo(Phase phase, const Channels& target, double seconds)
{
    phase_ = phase;
    time_ = 0.0;
    phaseEnd_ = seconds;
    from_ = ch_;
    to_ = target;
}

double RiceBowlBehavior::eased() const
{
    // Hold phases set phaseEnd_ = 0 (no glide): treat that as "settled"
    // instead of dividing by zero and freezing the channels on from_.
    if (phaseEnd_ <= 0.0) return 1.0;
    // One smoothstep over the phase duration drives every channel alike.
    return motion::smooth(std::clamp(time_ / phaseEnd_, 0.0, 1.0));
}

void RiceBowlBehavior::advance(double seconds)
{
    if (phase_ == Phase::None) return;
    time_ += seconds;
    const double e = eased();
    ch_.slide = from_.slide + (to_.slide - from_.slide) * e;
    ch_.gazeX = from_.gazeX + (to_.gazeX - from_.gazeX) * e;
    ch_.gazeY = from_.gazeY + (to_.gazeY - from_.gazeY) * e;
    ch_.lids = from_.lids + (to_.lids - from_.lids) * e;
    ch_.eyeSmile = from_.eyeSmile + (to_.eyeSmile - from_.eyeSmile) * e;
    ch_.headTilt = from_.headTilt + (to_.headTilt - from_.headTilt) * e;
    ch_.smile = from_.smile + (to_.smile - from_.smile) * e;
    ch_.steam = from_.steam + (to_.steam - from_.steam) * e;

    switch (phase_) {
    case Phase::SlideIn:
        if (time_ >= kSlideDuration) {
            Channels target;
            target.slide = 1.0;
            target.gazeX = kGazeX;
            target.gazeY = kGazeY;
            target.lids = 1.0;          // eyes close for the sniff
            target.eyeSmile = 1.0;
            target.headTilt = kSniffTilt;
            target.steam = 1.0;         // thickest while she breathes in
            glideTo(Phase::Sniff, target, kSniffDuration);
        }
        break;
    case Phase::Sniff:
        if (time_ >= kSniffDuration) {
            Channels target;
            target.slide = 1.0;
            target.gazeX = kGazeX;
            target.gazeY = kGazeY;
            target.lids = 0.0;          // eyes reopen
            target.eyeSmile = 0.35;
            target.headTilt = kSmileTilt;
            target.smile = 1.0;         // the omega mouth
            target.steam = 0.55;
            glideTo(Phase::Smile, target, kSmileDuration);
        }
        break;
    case Phase::Smile:
        if (time_ >= kSmileDuration) {
            phase_ = Phase::Hold;
            time_ = 0.0;
            phaseEnd_ = 0.0;
        }
        break;
    case Phase::Hold:
        if (time_ >= kHoldSeconds) exit();
        break;
    case Phase::LeanAway:
        if (time_ >= kLeanDuration) {
            Channels target;
            target.slide = 1.0;
            target.gazeX = kGazeX;
            target.gazeY = kGazeY;
            target.eyeSmile = 0.3;
            target.headTilt = kPeekTilt;
            target.smile = 0.7;         // curious half smile
            target.steam = 0.55;
            glideTo(Phase::PeekBack, target, kPeekDuration);
        }
        break;
    case Phase::PeekBack:
        if (time_ >= kPeekDuration) {
            // The happy pause resumes a little shorter than the first one.
            phase_ = Phase::Hold;
            time_ = -0.7;               // 2.2 - 0.7: the peek counted toward it
            phaseEnd_ = 0.0;
        }
        break;
    case Phase::SlideOut: {
        // Leg 1 glides the face back to rest while the bowl finishes any
        // remaining slide-in; leg 2 slides the bowl off the desk.
        if (time_ <= kRelaxDuration) break;  // channels already gliding
        const double leaveT = std::clamp((time_ - kRelaxDuration) / kLeaveDuration, 0.0, 1.0);
        ch_.slide = 1.0 - motion::smooth(leaveT);
        ch_.steam = 0.0;
        if (time_ >= kRelaxDuration + kLeaveDuration) cancel();
        break;
    }
    case Phase::None:
        break;
    }
}

void RiceBowlBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
    desired[QStringLiteral("ParamEyeBallX")] = std::clamp(ch_.gazeX, -1.0, 1.0);
    desired[QStringLiteral("ParamEyeBallY")] = std::clamp(ch_.gazeY, -1.0, 1.0);
    if (ch_.lids > 0.001) {
        // The sniff closes the eyes; during LeanAway lids=0 and the eyes
        // simply inherit the loop's open value (wide awake is fine there).
        desired[QStringLiteral("ParamEyeLOpen")] = std::max(
            0.0, 1.0 - ch_.lids);
        desired[QStringLiteral("ParamEyeROpen")] = std::max(
            0.0, 1.0 - ch_.lids);
    }
    if (ch_.eyeSmile > 0.001)
        desired[QStringLiteral("ParamEyeSmile")] = ch_.eyeSmile;
    if (std::abs(ch_.headTilt) > 0.01)
        desired[QStringLiteral("ParamAngleY")] = ch_.headTilt;
    if (ch_.smile > 0.001)
        desired[QStringLiteral("ParamSmileOpen")] = ch_.smile;
}
