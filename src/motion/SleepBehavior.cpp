#include "SleepBehavior.h"

#include "MotionMath.h"
#include "MotionParameterIds.h"

#include <QtMath>
#include <algorithm>

void SleepBehavior::begin()
{
    phase_ = Phase::Enter;
    time_ = 0.0;
    breathClock_ = 0.0;
    wakeInteractive_ = false;
}

bool SleepBehavior::wake(bool interactive)
{
    if (phase_ == Phase::None || phase_ == Phase::Wake) return false;
    phase_ = Phase::Wake;
    time_ = 0.0;
    wakeInteractive_ = interactive;
    return true;
}

void SleepBehavior::cancel()
{
    phase_ = Phase::None;
    time_ = 0.0;
    wakeInteractive_ = false;
}

double SleepBehavior::envelope() const
{
    switch (phase_) {
    case Phase::Enter:
        return motion::smooth(std::clamp(time_ / kEnterEnd, 0.0, 1.0));
    case Phase::Asleep:
        return 1.0;
    case Phase::Wake:
        return 1.0 - motion::smooth(std::clamp(time_ / kWakeEnd, 0.0, 1.0));
    default:
        return 0.0;
    }
}

void SleepBehavior::advance(double seconds)
{
    if (phase_ == Phase::None) return;
    time_ += seconds;
    if (phase_ == Phase::Asleep) breathClock_ += seconds;
    if (phase_ == Phase::Enter && time_ >= kEnterEnd) {
        phase_ = Phase::Asleep;
        time_ = 0.0;
    } else if (phase_ == Phase::Wake && time_ >= kWakeEnd) {
        cancel();
    }
}

void SleepBehavior::apply(Parameters& desired) const
{
    if (phase_ == Phase::None) return;
    // The nap happens at the small desk: sit down, no laptop. The desk
    // manager owns ParamDeskVisible and the cover/recover ordering; this only
    // asks for the seated pose. No new art per the card.
    desired[QStringLiteral("ParamSitPose")] = 1.0;
    desired[QStringLiteral("ParamLaptopVisible")] = 0.0;
    desired[QStringLiteral("ParamBusyLaptop")] = 0.0;
    const double tip = 6.0; // head tip toward the desk edge, Cubism AngleZ units
    if (phase_ == Phase::Enter) {
        // Eyes half-close first; full closure lands one beat later in Asleep.
        const double p = motion::smooth(std::clamp(time_ / kEnterEnd, 0.0, 1.0));
        desired[motion::leftEye] = std::min(desired.value(motion::leftEye), 1.0 - 0.45 * p);
        desired[motion::rightEye] = std::min(desired.value(motion::rightEye), 1.0 - 0.45 * p);
        desired[motion::angleZ] += tip * p;
    } else if (phase_ == Phase::Asleep) {
        // Sleeping face: closed eyes under the window-layer sleep mask, a
        // faint smile, head tipped onto the desk edge. The breath gets its
        // own slow clock (~7s cycle) with a small
        // body rise on the same phase -- much slower than the idle base.
        desired[motion::leftEye] = 0.0;
        desired[motion::rightEye] = 0.0;
        desired[QStringLiteral("ParamEyeSmile")] = std::max(
            desired.value(QStringLiteral("ParamEyeSmile")), 0.25);
        desired[motion::angleZ] += tip;
        desired[motion::breath] = 0.5 + 0.3 * qSin(breathClock_ * 0.9);
        desired[motion::bodyY] += 0.6 * qSin(breathClock_ * 0.9);
    } else { // Wake
        const double p = std::clamp(time_ / kWakeEnd, 0.0, 1.0);
        desired[motion::angleZ] += tip * (1.0 - motion::smooth(p));
        if (wakeInteractive_) {
            // Clicked awake: one eye first, then the other, per the card.
            // Assigned, not maxed: the idle base has the eyes fully open and
            // the staged targets must own both channels while waking.
            const double leftOpen = motion::smooth(std::clamp(time_ / 0.25, 0.0, 1.0));
            const double rightOpen = motion::smooth(std::clamp((time_ - 0.45) / 0.35, 0.0, 1.0));
            desired[motion::leftEye] = leftOpen;
            desired[motion::rightEye] = rightOpen;
        }
        // Non-interactive wake (a new turn or an event): the generic 0.12s
        // blend already reopens the eyes quickly; only the head tip and the
        // mask envelope need the authored 0.8s handback.
    }
}
