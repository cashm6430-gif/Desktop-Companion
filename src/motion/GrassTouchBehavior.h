#pragma once

#include "../MotionClip.h"

#include <QHash>
#include <QString>

class MotionLibrary;

// Foreground grass-touch interaction phase machine. The approved fixed grass
// clip remains available when this draft is not selected; the coordinator
// samples whichever is active. The invitation/hold/respond/timeout/release
// boundaries are validated at load time so a mis-authored clip fails loudly
// instead of stranding the pet mid-crouch.
class GrassTouchBehavior {
public:
    using Parameters = QHash<QString, double>;

    enum class Phase { Inactive, Enter, Hold, Respond, Timeout, Release, Finished };

    // Loads grass-touch.motion.json from `directory` via `library` and
    // validates the phase boundaries and the held-palm contract.
    bool configure(const MotionLibrary& library, const QString& directory, QString* error);
    // Phase machine accepts a start only for a valid, unselected clip; the
    // coordinator owns the preview/state guards and the action-clock resets.
    bool canBegin() const;
    void begin();
    void reset();
    bool active() const;
    bool selected() const { return selected_; }
    QString phaseName() const;
    bool respond();
    void lookAt(double horizontal);
    double maxDuration() const;
    // `finished` is set exactly once when the release beat completes.
    void advance(double seconds, bool& finished);

    const MotionClip& clip() const { return clip_; }
    double time() const { return time_; }
    bool holding() const { return phase_ == Phase::Hold; }
    double look() const { return look_; }

private:
    MotionClip clip_;
    Phase phase_ = Phase::Inactive;
    double enterEnd_ = 0.0;
    double holdEnd_ = 0.0;
    double respondEnd_ = 0.0;
    double timeoutEnd_ = 0.0;
    double releaseEnd_ = 0.0;
    double maxHold_ = 0.0;
    double time_ = 0.0;
    double heldTime_ = 0.0;
    double look_ = 0.0;
    double lookTarget_ = 0.0;
    bool selected_ = false;
};
