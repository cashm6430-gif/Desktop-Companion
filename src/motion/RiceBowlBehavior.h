#pragma once

#include <QHash>
#include <QString>

// Rice-bowl aroma break (optional fun scene r1, concept approved 2026-10-07).
// The window layer owns the bowl prop and paints the delight floaters (no
// physical gas: hearts/music notes rise beside her head, cartoon-symbol
// style); this behavior owns the face: the gaze snaps to the bowl, the
// eyes close for the sniff (hair settles late via the existing blend),
// then the omega smile with a small nod. A click on the bowl/aroma zone is
// "too close": she leans away, then peeks back curiously. Seated laptop
// busy only -- the bowl needs the desk; every channel is an existing
// expression axis, zero mesh deformation.
class RiceBowlBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { None, SlideIn, Sniff, Smile, Hold, LeanAway, PeekBack, SlideOut };

    void begin();
    // Timeout or tray "吃完啦": the face relaxes while the bowl slides away.
    void exit();
    // Interrupts win over the choreography: the bowl drops at once.
    void cancel();
    // A click on the bowl/aroma zone: lean away, then peek back curiously.
    void poke();
    void advance(double seconds);
    void apply(Parameters& desired) const;
    bool active() const { return phase_ != Phase::None; }
    bool exiting() const { return phase_ == Phase::SlideOut; }

    // Window-layer prop state: 0..1 eased slide of the bowl onto the desk,
    // and 0..1 delight the painter reads for the heart/note floaters.
    double slide() const { return ch_.slide; }
    double delight() const { return ch_.delight; }

private:
    struct Channels {
        double slide = 0.0;    // bowl slide onto the desk, 0..1
        double gazeX = 0.0;    // EyeBallX toward the bowl, capped at 0.3
        double gazeY = 0.0;    // EyeBallY toward the bowl (negative = down)
        double lids = 0.0;     // 0 open .. 1 closed (the sniff)
        double eyeSmile = 0.0;
        double headTilt = 0.0; // AngleY, positive tips toward the bowl
        double smile = 0.0;    // SmileOpen (the omega mouth)
        double delight = 0.0;  // heart/note floater intensity 0..1
    };

    void glideTo(Phase phase, const Channels& target, double seconds);
    double eased() const;
    Phase phase_ = Phase::None;
    double time_ = 0.0;
    double phaseEnd_ = 0.0;
    Channels ch_;       // current values
    Channels from_;     // snapshot at the phase start
    Channels to_;       // the phase glides toward these
};
