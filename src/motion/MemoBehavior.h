#pragma once

#include <QHash>
#include <QString>

// Sticky-note gaze (motion card 5). The note itself is window-layer art;
// this behavior only carries the eyes: a glance at the desk edge, then the
// gaze rides the note up as it floats, then back to the user. beginCelebrate()
// is the short completion beat (O-mouth laugh with closed smiling eyes) when
// the user ticks a note off. Pure overlay like the drag reaction: typing and
// props keep playing underneath.
class MemoBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { None, Desk, Rise, User };

    void begin();
    void beginCelebrate();
    void cancel();
    void advance(double seconds);
    void apply(Parameters& desired) const;
    bool active() const { return phase_ != Phase::None; }
    // 0..1 rise envelope of the note bubble, in step with the gaze. The window
    // layer holds it at 1 once the overlay ends so the note stays up.
    double bubbleRise() const;

private:
    // Authored beats: glance at the desk edge, ride the note up, look back.
    static constexpr double kDeskEnd = 0.3;
    static constexpr double kRiseEnd = 1.1;
    static constexpr double kUserEnd = 1.7;

    Phase phase_ = Phase::None;
    double time_ = 0.0;
    double userEnd_ = kUserEnd;                         // celebrate runs a shorter User beat
    double lookX_ = 0.0, lookY_ = 0.0;                  // smoothed gaze, -1..1
    double nodTime_ = 0.0;                              // completion nod, counts down
    // The delighted laugh (closed smiling eyes + O mouth) belongs to the
    // completion beat only. Creating a note ends on a soft smile.
    bool laugh_ = false;
};
