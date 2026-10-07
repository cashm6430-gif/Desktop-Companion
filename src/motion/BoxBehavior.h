#pragma once

#include <QHash>
#include <QString>

// Box hide-and-seek (optional fun scene r1, concept approved 2026-10-07).
// The window layer owns the cardboard prop; this behavior owns the body:
// sink behind the box, peek the eyes over the edge, pop the head up one
// beat later. Duck values are render pixels in the 840 reference frame
// (positive sinks toward the ground); CubismCanvas applies them after the
// ground contract so nothing cancels them, and the box prop always covers
// the feet strip while the body is sunk. Purely standing-state: playBoxPeek
// refuses every other state, so the desk and laptop pipelines never see it.
class BoxBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { None, SlideIn, Sink, EyesPeek, Pop, Hold, SinkBack, Blink, SmilePeek, SlideOut };

    void begin();
    // Animated exit: the body restores its stand first, then the box slides
    // away (the card's "退出先恢复站体再滑走").
    void exit();
    // Interrupts win over the choreography: the box drops at once.
    void cancel();
    // Caught: blink, duck back down half a beat, re-peek with a smile.
    void clicked();
    void advance(double seconds);
    void apply(Parameters& desired) const;
    bool active() const { return phase_ != Phase::None; }
    // The exit request must survive repeated calls (a drag re-raises it
    // every frame) and must not interrupt an already sliding-out box.
    bool exiting() const { return phase_ == Phase::SlideOut; }

    // Window-layer prop state: 0..1 eased slide of the cardboard box.
    double boxSlide() const { return slide_; }
    // Body offset in 840-reference pixels; positive sinks toward the ground.
    double duck() const { return duck_; }

private:
    void glideTo(Phase phase, double target, double seconds);
    // One smoothstep over the current phase duration drives the duck glide.
    double eased() const;
    Phase phase_ = Phase::None;
    double time_ = 0.0;
    double phaseEnd_ = 0.0;
    double duck_ = 0.0;      // current body offset, 840-ref px, + down
    double duckFrom_ = 0.0;  // duck at the phase start
    double duckTo_ = 0.0;    // duck the phase glides toward
    double slide_ = 0.0;     // eased box slide 0..1
    double slideFrom_ = 0.0; // slide at the exit request (leg 1 finishes the glide)
    double hold_ = 0.0;      // remaining hold seconds in the popped state
    int cycle_ = 0;          // hold rotation keeps the loop from looking metronomic
};
