#pragma once

#include "MotionLibrary.h"
#include "PetController.h"

#include <QHash>
#include <QRandomGenerator>
#include <QString>

// Turns the pet's state into Cubism parameter values each frame.
//
// Authored activity clips and short head/face reactions come from MotionLibrary.
// Procedural fallbacks keep the pet usable when a clip is absent. Durations come
// from the loaded data, keeping the player and controller in step.
//
// Values are Cubism parameter units, not screen pixels or pre-rendered frames.
// The model artist must rig these IDs (or provide an explicit mapping).
class ParameterMotion final {
public:
    using Parameters = QHash<QString, double>;

    ParameterMotion();

    void setState(PetController::State state);
    void advance(double seconds);
    bool loadGrassMotion(const QString& path, QString* error = nullptr);
    Parameters grassPose(double seconds) const;
    bool loadBusyLaptopMotion(const QString& path, QString* error = nullptr);
    // Loads every assets/motions/*.motion.json into the library and adopts the
    // clips the pet drives directly.
    bool loadMotionLibrary(const QString& directory, QString* error = nullptr);
    const MotionLibrary& library() const { return library_; }
    // Time of a named marker on an authored clip, used by the window-layer props
    // so their handovers ride the same timeline the character is animated with
    // (see MotionClip::Event). Returns `fallback` when the clip or marker is
    // absent, which keeps the procedural path usable without the asset.
    double clipEventTime(const QString& clipId, const QString& event, double fallback) const;
    // Single source of truth for how long the current action runs.
    double actionDuration(PetController::State state) const;
    // Returns true exactly once after a one-shot action reached its duration, so
    // the owner can restore the background state without a parallel timer.
    bool consumeActionFinished();

    // Short reactions are a separate layer: they leave the background state,
    // breathing phase, seated body and prop channels in place. Draft clips are
    // loaded for explicit previews as well as the eventual approved actions.
    bool configureInteractions(QString* error = nullptr);
    bool playTurnEnded();
    // Authored stretch-and-peek pause ("歇一下"). Only meaningful while the
    // seated laptop busy loop is playing; the clip carries the full seated
    // pose, so it overrides the loop and resumes typing from its start.
    bool playStretch();
    bool beginHeadPat(double direction = 0.0);
    bool canBeginHeadPat() const;
    void updateHeadPat(double direction);
    void endHeadPat();
    void cancelInteraction();
    bool interactionActive() const;
    QString interactionId() const;
    // Authored clip time, including the bounded hold loop.
    double interactionTime() const { return interactionTime_; }
    // True once a head pat has been released and the authored recovery segment
    // is playing out. The release hands the reaction's own channels back on
    // kExpressionReleaseBlend rather than the generic state-transition blend.
    bool headPatReleasing() const { return headPatReleasing_; }

    // Foreground grass interaction uses its own phase clock; the approved
    // fixed grass clip remains available when this draft is not selected.
    bool beginGrassInteraction();
    bool grassInteractionActive() const;
    QString grassInteractionPhase() const;
    double grassInteractionTime() const { return grassInteractionTime_; }
    bool respondToGrass();
    void lookAtGrassTip(double horizontal);
    double grassInteractionMaxDuration() const;

    // Scene-move drag reaction (motion card 4). The window layer feeds window
    // velocity; the motion layer adds eye/hair/body lag and a settle nod. Pure
    // overlay: it owns no state transitions and never touches the busy clock.
    void beginDragMotion();
    void updateDragMotion(double vx, double vy);
    void endDragMotion();
    void cancelDragMotion();
    bool dragMotionActive() const { return dragPhase_ != DragPhase::None; }

    // Sticky-note gaze (motion card 5). The note itself is window-layer art;
    // the motion layer only carries the eyes: a glance at the desk edge, then
    // the gaze rides the note up as it floats, then back to the user with a
    // delighted laugh -- smiling closed eyes and a round O mouth. beginMemoCelebrate() is the short completion
    // beat (O-mouth laugh with closed smiling eyes) when the user ticks a
    // note off. Pure overlay like the
    // drag reaction: typing and props keep playing underneath.
    void beginMemoMotion();
    void beginMemoCelebrate();
    void cancelMemoMotion();
    bool memoMotionActive() const { return memoPhase_ != MemoPhase::None; }
    // 0..1 rise envelope of the note bubble, in step with the gaze. The window
    // layer holds it at 1 once the overlay ends so the note stays up.
    double memoBubbleRise() const;

    // Eye-mask nap at the desk (motion card 8, reworked per feedback: the
    // pillow prop is gone, a sleep mask covers the eyes instead). A sustained
    // overlay: eyes half-close while the head tips, then full sleep with a
    // slow breath on its own clock. Clicking the head wakes it interactively
    // (one eye first, then the other); a new turn or a delete/grass event
    // wakes it non-interactively over the same authored 0.8s while the event
    // action starts underneath. No new art -- the mask itself is window-layer.
    void beginSleepMotion();
    void wakeFromSleep(bool interactive);
    void cancelSleepMotion();
    bool sleepMotionActive() const { return sleepPhase_ != SleepPhase::None; }
    // 0..1 mask visibility envelope, in step with the enter/wake phases.
    double sleepMaskEnvelope() const;

    void setBusyRandomSeed(quint32 seed) { busyRandom_.seed(seed); }
    void forceLaptopBusy(); // Native review / manual preview, uses the real player.
    // Same, but pins the standing variant so the busy curve can be reviewed
    // without depending on the 40% random laptop choice.
    void forceStandingBusy();
    bool isLaptopBusy() const { return laptopBusy_; }
    // Enable only for a model with an opaque ParamDeskVisible cover. The desk
    // covers before laptop posture changes and leaves after recovery. Disabled
    // by default; preview/sequence poses keep their explicitly supplied values.
    void setDeskWorkMode(bool enabled);
    void setPreviewPose(const Parameters& parameters);
    bool isPreview() const { return preview_; }
    bool frozenPhysics() const { return preview_ && !sequencePhysics_; }
    void setSequencePose(const Parameters& parameters) { values_ = parameters; preview_ = true; sequencePhysics_ = true; }
    const Parameters& values() const { return values_; }
    // Comic "?" bubble next to the head while the pet is thinking. It is
    // presentation, not a MOC3 parameter: the window layer paints it, so it
    // cannot travel inside the parameter map. The player still owns its timing,
    // which keeps the desktop pet and the approval captures on the same bubble
    // at the same instant. 0 hides it, 1 is the resting size, and the pop-in
    // overshoots a little past 1.
    double bubblePulse() const { return bubblePulse_; }

private:
    // Blend time used for every parameter that no clip overrides.
    static constexpr double kDefaultBlend = 0.12;
    // Releasing a head pat hands the face back to the background. Exponential
    // blending needs about three time constants to settle, so at the authored
    // 50ms eye constant the recovery still spends several frames crossing from
    // the closed-eye smile to open eyes: the clip's authored 0.18s recovery
    // turns into a visible half-open "drowsy" face it never asked for. Settling
    // the release in roughly two frames keeps the authored beats in charge.
    static constexpr double kExpressionReleaseBlend = 0.045;
    static double smooth(double t);
    static double pulse(double t, double start, double peak, double end);
    // Pop-in / hold / fade-out envelope of the thinking bubble. One cycle long,
    // matching the standing accent's loop so the "?" cannot drift against the
    // pose it is reacting to.
    static double thoughtBubblePulse(double seconds);
    void updateGrassFlex(double seconds, double target);
    void applyBlendOverrides(const MotionClip& clip);
    void advanceInteraction(double seconds);
    void applyInteraction(Parameters& desired) const;
    void applyDragMotion(Parameters& desired) const;
    void advanceDragMotion(double seconds);
    void advanceMemoMotion(double seconds);
    void applyMemoMotion(Parameters& desired) const;
    void advanceSleepMotion(double seconds);
    void applySleepMotion(Parameters& desired) const;
    void captureInteractionSeat();
    bool configureGrassInteraction(const QString& directory, QString* error);
    void resetGrassInteraction();
    void advanceGrassInteraction(double seconds);
    void advanceDeskWork(double seconds, const Parameters& previous);

    enum class DeskPhase { Hidden, EnterCover, Work, ExitWork, ExitCover };
    bool deskWorkMode_ = false;
    DeskPhase deskPhase_ = DeskPhase::Hidden;
    double deskVisible_ = 0.0;

    enum class GrassPhase { Inactive, Enter, Hold, Respond, Timeout, Release, Finished };
    MotionClip grassTouchClip_;
    GrassPhase grassPhase_ = GrassPhase::Inactive;
    double grassEnterEnd_ = 0.0;
    double grassHoldEnd_ = 0.0;
    double grassRespondEnd_ = 0.0;
    double grassTimeoutEnd_ = 0.0;
    double grassReleaseEnd_ = 0.0;
    double grassMaxHold_ = 0.0;
    double grassInteractionTime_ = 0.0;
    double grassHeldTime_ = 0.0;
    double grassLook_ = 0.0;
    double grassLookTarget_ = 0.0;
    bool grassInteractionSelected_ = false;

    enum class DragPhase { None, Follow, Settle };
    DragPhase dragPhase_ = DragPhase::None;
    double dragLookX_ = 0.0, dragLookY_ = 0.0;          // smoothed eye follow, -1..1
    double dragLookTX_ = 0.0, dragLookTY_ = 0.0;
    double dragHairX_ = 0.0, dragHairTarget_ = 0.0;     // hair lag, streams against velocity
    double dragBodyX_ = 0.0, dragBodyTarget_ = 0.0;     // slight body lean, param units
    double dragSpeed_ = 0.0;                            // last fed speed, px/s
    double dragDistance_ = 0.0;                         // accumulated path length
    bool dragCuriousDone_ = false;
    double dragCuriousTime_ = 0.0;                      // one curious look-back at the user
    double dragNodTime_ = 0.0;                          // release nod, counts down
    double dragSettleTime_ = 0.0;                       // hard cap on the settle phase
    bool actionFinishedReported_ = false;

    enum class MemoPhase { None, Desk, Rise, User };
    // Authored beats: glance at the desk edge, ride the note up, look back.
    static constexpr double kMemoDeskEnd = 0.3;
    static constexpr double kMemoRiseEnd = 1.1;
    static constexpr double kMemoUserEnd = 1.7;
    MemoPhase memoPhase_ = MemoPhase::None;
    double memoTime_ = 0.0;
    double memoUserEnd_ = kMemoUserEnd;                 // celebrate runs a shorter User beat
    double memoLookX_ = 0.0, memoLookY_ = 0.0;          // smoothed gaze, -1..1
    double memoNodTime_ = 0.0;                          // completion nod, counts down
    // The delighted laugh (closed smiling eyes + O mouth) belongs to the
    // completion beat only. Creating a note ends on a soft smile.
    bool memoLaugh_ = false;

    enum class SleepPhase { None, Enter, Asleep, Wake };
    // Authored beats: 0.8s settle into the nap, sleep until woken, 0.8s
    // wake-up (a new start shortens waking to this per the card).
    static constexpr double kSleepEnterEnd = 0.8;
    static constexpr double kSleepWakeEnd = 0.8;
    SleepPhase sleepPhase_ = SleepPhase::None;
    double sleepTime_ = 0.0;                            // phase-local clock
    double sleepBreathClock_ = 0.0;                     // own slow breath, ~7s cycle
    bool sleepWakeInteractive_ = false;

    enum class Interaction { None, TurnEnded, HeadPat, Stretch };
    MotionClip turnEndedClip_;
    MotionClip headPatClip_;
    MotionClip stretchClip_;
    QString interactionDirectory_;
    Interaction interaction_ = Interaction::None;
    double interactionTime_ = 0.0;
    double interactionElapsed_ = 0.0;
    double headPatEnterEnd_ = 0.8;
    double headPatHoldEnd_ = 1.6;
    double headPatDirection_ = 0.0;
    double headPatDirectionTarget_ = 0.0;
    double headPatCooldown_ = 0.0;
    bool headPatHeld_ = false;
    bool headPatReleasing_ = false;
    // True while the release should hand the whole reaction face back quickly.
    bool releasingReaction_ = false;
    bool headPatBeganBusy_ = false;
    Parameters interactionSeat_;

    PetController::State state_ = PetController::State::Idle;
    Parameters values_;
    double clock_ = 0.0;
    double actionTime_ = 0.0;
    double bubblePulse_ = 0.0;
    double blinkClock_ = 0.0;
    double leftEyeExpression_ = 1.0;
    double rightEyeExpression_ = 1.0;
    MotionLibrary library_;
    MotionClip grassClip_;
    MotionClip laptopClip_;
    MotionClip idleClip_;
    MotionClip busyStandClip_;
    MotionClip deleteClip_;
    // Per-parameter blend time (seconds); see applyBlendOverrides().
    QHash<QString, double> blendSeconds_;
    QRandomGenerator busyRandom_{QRandomGenerator::securelySeeded()};
    bool busyChoiceExists_ = false;
    bool laptopBusy_ = false;
    double busyTime_ = 0.0;
    double nextBusyChoice_ = 40.0;
    bool preview_ = false;
    bool sequencePhysics_ = false;
    bool finishedPending_ = false;
    // Two damped modes: the stem follows the grip and the softer tip trails it.
    double grassBend_ = 0.0;
    double grassBendVelocity_ = 0.0;
    double grassTip_ = 0.0;
    double grassTipVelocity_ = 0.0;
    double previousGripAngle_ = 0.0;
    double previousReach_ = 0.0;
};
