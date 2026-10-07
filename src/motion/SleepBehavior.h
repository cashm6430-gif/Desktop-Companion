#pragma once

#include <QHash>
#include <QString>

// Eye-mask nap at the desk (motion card 8, reworked per feedback: the pillow
// prop is gone, a sleep mask covers the eyes instead). A sustained overlay:
// eyes half-close while the head tips, then full sleep with a slow breath on
// its own clock. Clicking the head wakes it interactively (one eye first,
// then the other); a new turn or a delete/grass event wakes it
// non-interactively over the same authored 0.8s while the event action starts
// underneath. No new art -- the mask itself is window-layer.
class SleepBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { None, Enter, Asleep, Wake };

    void begin();
    // Returns true only when a real transition happened, so the coordinator's
    // lifecycle trace stays silent on ignored repeat calls.
    bool wake(bool interactive);
    void cancel();
    void advance(double seconds);
    void apply(Parameters& desired) const;
    bool active() const { return phase_ != Phase::None; }
    // 0..1 mask visibility envelope, in step with the enter/wake phases.
    double envelope() const;

private:
    // Authored beats: 0.8s settle into the nap, sleep until woken, 0.8s
    // wake-up (a new start shortens waking to this per the card).
    static constexpr double kEnterEnd = 0.8;
    static constexpr double kWakeEnd = 0.8;

    Phase phase_ = Phase::None;
    double time_ = 0.0;                                 // phase-local clock
    double breathClock_ = 0.0;                          // own slow breath, ~7s cycle
    bool wakeInteractive_ = false;
};
