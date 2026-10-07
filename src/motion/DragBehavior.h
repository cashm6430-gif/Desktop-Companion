#pragma once

#include <QHash>
#include <QString>

// Scene-move drag reaction (motion card 4). The window layer feeds window
// velocity; this behavior adds eye/hair/body lag and a settle nod. Pure
// overlay: it owns no state transitions and never touches the busy clock.
class DragBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { None, Follow, Settle };

    void begin();
    void update(double vx, double vy);
    void end();
    void cancel();
    void advance(double seconds);
    void apply(Parameters& desired) const;
    bool active() const { return phase_ != Phase::None; }

private:
    Phase phase_ = Phase::None;
    double lookX_ = 0.0, lookY_ = 0.0;               // smoothed eye follow, -1..1
    double lookTX_ = 0.0, lookTY_ = 0.0;
    double hairX_ = 0.0, hairTarget_ = 0.0;          // hair lag, streams against velocity
    double bodyX_ = 0.0, bodyTarget_ = 0.0;          // slight body lean, param units
    double speed_ = 0.0;                             // last fed speed, px/s
    double distance_ = 0.0;                          // accumulated path length
    bool curiousDone_ = false;
    double curiousTime_ = 0.0;                       // one curious look-back at the user
    double nodTime_ = 0.0;                           // release nod, counts down
    double settleTime_ = 0.0;                        // hard cap on the settle phase
};
