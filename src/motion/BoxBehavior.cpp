#include "BoxBehavior.h"

#include "MotionMath.h"

#include <algorithm>

namespace {
// Choreography constants live in the 840 reference frame the approval
// captures use, so the concept card and the runtime share one set of
// numbers. The canvas scales the duck to its own pixel height. Durations
// were stretched ~1.4x after the user feedback r2 ("节奏太快了").
constexpr double kSlideDuration = 0.7;   // box glides in from the right
constexpr double kSinkDuration = 0.65;   // body drops behind the box
constexpr double kEyesDuration = 0.5;    // eyes over the edge
constexpr double kPopDuration = 0.7;     // head pops up one beat later
constexpr double kRestoreDuration = 0.55; // exit leg 1: stand before the box leaves
constexpr double kLeaveDuration = 0.7;   // exit leg 2: box slides away
constexpr double kBlinkDuration = 0.25;  // caught: the surprised blink
constexpr double kSmileDuration = 0.65;  // caught: smile peek from the same side

// Duck targets. The box top sits at y=430 in the reference frame and the
// standing eye line is ~85px above it: +100 hides the eyes below the edge,
// +85 puts them exactly on it. The pop returns exactly to the standing
// height (duck 0): the old -80 overshoot lifted the model above its base,
// clipping the ahoge at the window top (user feedback r2) -- the stand
// already clears the box top, so overshoot bought nothing.
constexpr double kHidden = 100.0;
constexpr double kPeek = 85.0;
constexpr double kPopped = 0.0;
constexpr double kCaught = 118.0;

// Hold rotation (seconds): deterministic but never metronomic.
constexpr double kHolds[] = {2.4, 3.6, 2.8};
}  // namespace

void BoxBehavior::begin()
{
    phase_ = Phase::SlideIn;
    time_ = 0.0;
    phaseEnd_ = kSlideDuration;
    duck_ = duckFrom_ = duckTo_ = 0.0;
    slide_ = slideFrom_ = 0.0;
    hold_ = 0.0;
    cycle_ = 0;
}

void BoxBehavior::exit()
{
    if (phase_ == Phase::None || phase_ == Phase::SlideOut) return;
    // Stand first (whatever the current duck), then slide the box away.
    // If the box was still gliding in, leg 1 finishes that glide so the
    // prop never jumps position.
    slideFrom_ = slide_;
    glideTo(Phase::SlideOut, 0.0, kRestoreDuration);
}

void BoxBehavior::cancel()
{
    phase_ = Phase::None;
    time_ = 0.0;
    phaseEnd_ = 0.0;
    duck_ = duckFrom_ = duckTo_ = 0.0;
    slide_ = slideFrom_ = 0.0;
    hold_ = 0.0;
    cycle_ = 0;
}

void BoxBehavior::clicked()
{
    // Only a visible head can be "caught"; the slide legs ignore taps
    // (the box is still moving) and the full sink keeps the face hidden.
    if (phase_ != Phase::EyesPeek && phase_ != Phase::Pop && phase_ != Phase::Hold
        && phase_ != Phase::SinkBack)
        return;
    glideTo(Phase::Blink, kCaught, kBlinkDuration);
}

void BoxBehavior::glideTo(Phase phase, double target, double seconds)
{
    phase_ = phase;
    time_ = 0.0;
    phaseEnd_ = seconds;
    duckFrom_ = duck_;
    duckTo_ = target;
}

double BoxBehavior::eased() const
{
    // One smoothstep over the phase duration drives the duck glide.
    return motion::smooth(std::clamp(time_ / phaseEnd_, 0.0, 1.0));
}

void BoxBehavior::advance(double seconds)
{
    if (phase_ == Phase::None) return;
    time_ += seconds;
    switch (phase_) {
    case Phase::SlideIn:
        slide_ = motion::smooth(std::clamp(time_ / kSlideDuration, 0.0, 1.0));
        if (time_ >= kSlideDuration) { slide_ = 1.0; glideTo(Phase::Sink, kHidden, kSinkDuration); }
        break;
    case Phase::Sink:
        duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
        if (time_ >= kSinkDuration) glideTo(Phase::EyesPeek, kPeek, kEyesDuration);
        break;
    case Phase::EyesPeek:
        duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
        if (time_ >= kEyesDuration) glideTo(Phase::Pop, kPopped, kPopDuration);
        break;
    case Phase::Pop:
        duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
        if (time_ >= kPopDuration) {
            phase_ = Phase::Hold;
            time_ = 0.0;
            phaseEnd_ = 0.0;
            hold_ = kHolds[cycle_++ % 3];
        }
        break;
    case Phase::Hold:
        duck_ = duckTo_;
        hold_ -= seconds;
        if (hold_ <= 0.0) glideTo(Phase::SinkBack, kHidden, kPopDuration);
        break;
    case Phase::SinkBack:
        duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
        if (time_ >= kPopDuration) glideTo(Phase::EyesPeek, kPeek, kEyesDuration);
        break;
    case Phase::Blink:
        duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
        if (time_ >= kBlinkDuration) glideTo(Phase::SmilePeek, kPeek, kSmileDuration);
        break;
    case Phase::SmilePeek:
        duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
        if (time_ >= kSmileDuration) glideTo(Phase::Pop, kPopped, kPopDuration);
        break;
    case Phase::SlideOut:
        // Leg 1 glides the duck to 0 (stand restored) while the box finishes
        // any remaining slide-in; leg 2 slides the box off. The ordering is
        // the card's acceptance.
        if (time_ <= kRestoreDuration) {
            duck_ = duckFrom_ + (duckTo_ - duckFrom_) * eased();
            slide_ = slideFrom_ + (1.0 - slideFrom_) * eased();
        } else {
            duck_ = 0.0;
            slide_ = 1.0 - motion::smooth(
                std::clamp((time_ - kRestoreDuration) / kLeaveDuration, 0.0, 1.0));
        }
        if (time_ >= kRestoreDuration + kLeaveDuration) cancel();
        break;
    case Phase::None:
        break;
    }
}

void BoxBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
    desired[QStringLiteral("ParamBoxDuck")] = duck_;
    if (phase_ == Phase::EyesPeek || phase_ == Phase::SmilePeek) {
        // Looking over the edge before the head follows.
        desired[QStringLiteral("ParamEyeBallY")] = std::clamp(
            desired.value(QStringLiteral("ParamEyeBallY")) + 0.85, -1.0, 1.0);
    }
    if (phase_ == Phase::Blink) {
        // The surprised blink of being caught.
        desired[QStringLiteral("ParamEyeLOpen")] = 0.0;
        desired[QStringLiteral("ParamEyeROpen")] = 0.0;
    }
    if (phase_ == Phase::SmilePeek) {
        // The same-side smile re-peek: caught, but pleased about it.
        desired[QStringLiteral("ParamEyeSmile")] = std::max(
            desired.value(QStringLiteral("ParamEyeSmile")), 0.85);
    }
}
