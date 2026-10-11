#pragma once

#include <QHash>
#include <QRandomGenerator>
#include <QString>

// Presence gaze (interaction-first r1). The pet's eyes -- and a small head
// turn -- follow the cursor, look at the user when they come near, and glance
// around when the user has left the mouse alone. It is the BASE layer of the
// face: it adds to ParamEyeBallX/Y and a small ParamAngleX/Y, and never removes
// what the background state or an authored clip already put there.
//
// Pure overlay, exactly like DragBehavior: no state transitions, no assets,
// existing expression axes only. The window layer feeds the normalised cursor
// each frame; the behaviour smooths it -- the eyes faster than the head.
//
// State (aligned with how desktop mascots usually behave, 2026-10-11):
//   1. FOLLOW -- the eyes (small head turn) track the cursor EVERYWHERE,
//      saturating when it is far; a tiny saccadic micro-drift keeps a held gaze
//      from freezing. Default whenever the cursor is moving / recently moved.
//   2. LOOK AT YOU -- while the cursor is NEAR or on the pet (nearCursor), she
//      locks on and adds a soft eye smile. "Near" only adds this beat; it never
//      switches tracking off.
//   3. IDLE -- once the cursor has been STILL for kIdleAfter seconds (the user
//      stepped away from the mouse), she settles to centre and takes a short
//      glance every few seconds. Any cursor movement snaps back to FOLLOW.
//
// Suppression is the caller's job (ParameterMotion::gazeSuppressed): whenever a
// higher-priority owner holds the face -- delete, grass, memo, drag, sleep, box,
// rice, wave, head pat, stretch -- advance() is told to stand down and the gaze
// relaxes to centre instead of fighting for the eyes.
//
// SIGN NOTE (confirmed by pose probe 2026-10-11): the model convention is
// ParamEyeBallX / ParamAngleX positive = toward SCREEN-RIGHT, ParamEyeBallY /
// ParamAngleY positive = SCREEN-UP. The gains in the .cpp encode that, so a
// cursor to the right looks right and a cursor above looks up. See the design
// card presence-gaze-r1 for the probe sheet.
class GazeBehavior {
public:
    using Parameters = QHash<QString, double>;

    // Normalised cursor direction from the window centre: nx positive = right,
    // ny positive = down, both roughly -1..1 (clamped inside). `nearCursor` is
    // true while the cursor is on or very close to the pet ("look at you" beat).
    // `cursorActive` is true on frames the cursor actually moved; the false
    // frames accumulate the idle timer.
    void setTarget(double nx, double ny, bool nearCursor, bool cursorActive);
    // `suppressed` is true while a higher-priority behaviour owns the face.
    void advance(double seconds, bool suppressed);
    // Adds the gaze onto `desired` (additive: authored background gaze survives).
    void apply(Parameters& desired) const;
    void reset();
    // Deterministic glance schedule. The default is a fixed seed (project
    // convention: fixed dt/seed), so two fresh instances -- and two runs --
    // replay identical glances; call seed() to vary them.
    void seed(quint32 s) { rng_.seed(s); }

    // Review trace: the smoothed directional look (-1..1), not model units.
    double eyeX() const { return eyeX_; }
    double eyeY() const { return eyeY_; }

private:
    void scheduleGlance();

    double targetX_ = 0.0, targetY_ = 0.0;
    bool near_ = false;
    bool cursorActive_ = true;    // assume live until told otherwise (test-safe)
    bool hasTarget_ = false;      // no cursor feed yet -> no micro-drift
    double stillSeconds_ = 0.0;   // seconds since the cursor last moved
    bool suppressed_ = false;
    // Smoothed directional look, -1..1 (the eyes); the head is a deadzoned
    // fraction of the same signal.
    double eyeX_ = 0.0, eyeY_ = 0.0;
    double headX_ = 0.0, headY_ = 0.0;
    double smile_ = 0.0;
    // Idle glance schedule, active only while the cursor has been still.
    double idle_ = 0.0;
    double nextGlance_ = 3.0;
    double glanceLeft_ = 0.0;
    double glanceX_ = 0.0, glanceY_ = 0.0;
    // Saccadic micro-drift: a tiny random eye offset re-aimed every ~1 s so a
    // held gaze never freezes.
    double saccadeX_ = 0.0, saccadeY_ = 0.0;
    double saccadeClock_ = 0.0;
    double nextSaccade_ = 0.7;
    // Fixed default seed (project convention: fixed dt/seed) so two instances
    // and two runs replay the same glances; call seed() to vary them.
    QRandomGenerator rng_{20261011u};
};
