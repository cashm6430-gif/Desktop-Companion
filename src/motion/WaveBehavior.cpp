#include "WaveBehavior.h"

#include "MotionMath.h"

#include <algorithm>

namespace {
// Choreography constants (seconds), matching the approved concept card.
constexpr double kRaiseDuration = 0.5;   // arm lifts beside her head
constexpr double kSwingDuration = 0.3;   // one out (or in) swing leg
constexpr double kSwingRounds = 2;       // out+in, twice
constexpr double kSettleDuration = 0.5;  // arm glides home, smile blooms
constexpr double kReleaseDuration = 0.4; // the smile relaxes back to idle

// Pose targets (concept card keyframes). The gaze finds the user first;
// 0.2 stays under the 0.3 clamp (>=0.4 shows an iris rim on the eyewhite).
constexpr double kGazeX = 0.2;
}  // namespace

// --- Concept-card keyframe targets -------------------------------------------

WaveBehavior::Channels WaveBehavior::raiseTarget() {
    Channels c;
    c.arm = 50.0;
    c.elbow = 35.0;
    c.gazeX = kGazeX;
    return c;
}

WaveBehavior::Channels WaveBehavior::swingOutTarget() {
    Channels c;
    c.arm = 56.0;
    c.elbow = 24.0;
    c.wrist = 22.0;
    c.gazeX = kGazeX;
    return c;
}

WaveBehavior::Channels WaveBehavior::swingInTarget() {
    Channels c;
    c.arm = 44.0;
    c.elbow = 46.0;
    c.wrist = -25.0;
    c.gazeX = kGazeX;
    return c;
}

WaveBehavior::Channels WaveBehavior::settleTarget() {
    Channels c;
    c.eyeSmile = 0.85;
    c.smile = 1.0;
    return c;
}

void WaveBehavior::begin()
{
    phase_ = Phase::Raise;
    time_ = 0.0;
    phaseEnd_ = kRaiseDuration;
    ch_ = from_ = to_ = Channels{};
    to_ = raiseTarget();
    swings_ = 0;
}

void WaveBehavior::exit()
{
    if (phase_ == Phase::None || phase_ == Phase::Release) return;
    // Quick release: everything glides home together, no pop.
    glideTo(Phase::Release, Channels{}, 0.3);
}

void WaveBehavior::cancel()
{
    phase_ = Phase::None;
    time_ = 0.0;
    phaseEnd_ = 0.0;
    ch_ = from_ = to_ = Channels{};
    swings_ = 0;
}

void WaveBehavior::glideTo(Phase phase, const Channels& target, double seconds)
{
    phase_ = phase;
    time_ = 0.0;
    phaseEnd_ = seconds;
    from_ = ch_;
    to_ = target;
}

double WaveBehavior::eased() const
{
    // Release-only guard symmetry with the other behaviors: a zero-length
    // phase must read as settled, never divide by zero.
    if (phaseEnd_ <= 0.0) return 1.0;
    return motion::smooth(std::clamp(time_ / phaseEnd_, 0.0, 1.0));
}

void WaveBehavior::blend(const Channels& target, double e)
{
    ch_.arm = from_.arm + (target.arm - from_.arm) * e;
    ch_.elbow = from_.elbow + (target.elbow - from_.elbow) * e;
    ch_.wrist = from_.wrist + (target.wrist - from_.wrist) * e;
    ch_.gazeX = from_.gazeX + (target.gazeX - from_.gazeX) * e;
    ch_.eyeSmile = from_.eyeSmile + (target.eyeSmile - from_.eyeSmile) * e;
    ch_.smile = from_.smile + (target.smile - from_.smile) * e;
}

void WaveBehavior::advance(double seconds)
{
    if (phase_ == Phase::None) return;
    time_ += seconds;
    const double e = eased();
    blend(to_, e);

    switch (phase_) {
    case Phase::Raise:
        if (time_ >= kRaiseDuration) {
            glideTo(Phase::SwingOut, swingOutTarget(), kSwingDuration);
        }
        break;
    case Phase::SwingOut:
        if (time_ >= kSwingDuration) {
            glideTo(Phase::SwingIn, swingInTarget(), kSwingDuration);
        }
        break;
    case Phase::SwingIn:
        if (time_ >= kSwingDuration) {
            ++swings_;
            if (swings_ < kSwingRounds) {
                glideTo(Phase::SwingOut, swingOutTarget(), kSwingDuration);
            } else {
                glideTo(Phase::Settle, settleTarget(), kSettleDuration);
            }
        }
        break;
    case Phase::Settle:
        if (time_ >= kSettleDuration) {
            glideTo(Phase::Release, Channels{}, kReleaseDuration);
        }
        break;
    case Phase::Release:
        if (time_ >= kReleaseDuration) cancel();
        break;
    case Phase::None:
        break;
    }
}

void WaveBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
    desired[QStringLiteral("ParamArmRA")] = std::clamp(ch_.arm, -65.0, 65.0);
    desired[QStringLiteral("ParamElbowRA")] = std::clamp(ch_.elbow, -35.0, 55.0);
    desired[QStringLiteral("ParamWristRA")] = std::clamp(ch_.wrist, -25.0, 25.0);
    if (ch_.gazeX > 0.001)
        desired[QStringLiteral("ParamEyeBallX")] = std::clamp(ch_.gazeX, -1.0, 1.0);
    if (ch_.eyeSmile > 0.001)
        desired[QStringLiteral("ParamEyeSmile")] = ch_.eyeSmile;
    if (ch_.smile > 0.001)
        desired[QStringLiteral("ParamSmileOpen")] = ch_.smile;
}
