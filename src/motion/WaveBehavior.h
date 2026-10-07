#pragma once

#include <QHash>
#include <QString>

// Wave hello (optional fun scene r1, concept pending approval 2026-10-07).
// The arm-axis capability probe on the R19 41-param model refuted the old
// "arm axes have no silhouette" finding: the shoulder/elbow/wrist swing
// 13-15k / 7-9k / 3.2k pixels and stay intact. This behavior owns the
// right arm: raise beside her head, swing out/in twice, settle back with
// an omega smile. Standing idle only -- seated typing keeps the hands on
// the keyboard. Every channel is an existing axis, zero mesh deformation;
// no window-layer prop.
class WaveBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { None, Raise, SwingOut, SwingIn, Settle, Release };

    void begin();
    // Leave-idle exit: a quick release glide so the arm never pops.
    void exit();
    // Interrupts win over the choreography: the arm drops at once.
    void cancel();
    void advance(double seconds);
    void apply(Parameters& desired) const;
    bool active() const { return phase_ != Phase::None; }
    bool exiting() const { return phase_ == Phase::Release; }

    // Review/trace getters: current right-arm axis values.
    double arm() const { return ch_.arm; }
    double elbow() const { return ch_.elbow; }
    double wrist() const { return ch_.wrist; }

private:
    struct Channels {
        double arm = 0.0;      // ParamArmRA, -65..65
        double elbow = 0.0;    // ParamElbowRA, -35..55
        double wrist = 0.0;    // ParamWristRA, -25..25
        double gazeX = 0.0;    // EyeBallX toward the user, capped at 0.3
        double eyeSmile = 0.0;
        double smile = 0.0;    // SmileOpen (the omega mouth)
    };

    void glideTo(Phase phase, const Channels& target, double seconds);
    double eased() const;
    void blend(const Channels& target, double e);
    // Concept-card keyframe targets (private so Channels stays encapsulated).
    static Channels raiseTarget();
    static Channels swingOutTarget();
    static Channels swingInTarget();
    static Channels settleTarget();
    Phase phase_ = Phase::None;
    double time_ = 0.0;
    double phaseEnd_ = 0.0;
    Channels ch_;       // current values
    Channels from_;     // snapshot at the phase start
    Channels to_;       // the phase glides toward these
    int swings_ = 0;    // completed out/in swings
};
