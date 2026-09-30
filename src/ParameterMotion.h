#pragma once

#include "MotionLibrary.h"
#include "PetController.h"

#include <QHash>
#include <QRandomGenerator>
#include <QString>

// Turns the pet's state into Cubism parameter values each frame.
//
// Authored clips (grass, seated laptop) come from MotionLibrary; the idle, busy
// and delete curves are still generated procedurally here. Either way, the
// duration of an action is never hard-coded: it is asked from the library so the
// player and the state machine can never disagree.
//
// Values are Cubism parameter units, not screen pixels or pre-rendered frames.
// The model artist must rig these IDs (or provide an explicit mapping).
class ParameterMotion final {
public:
    using Parameters = QHash<QString, double>;

    void setState(PetController::State state);
    void advance(double seconds);
    bool loadGrassMotion(const QString& path, QString* error = nullptr);
    Parameters grassPose(double seconds) const;
    bool loadBusyLaptopMotion(const QString& path, QString* error = nullptr);
    // Loads every assets/motions/*.motion.json into the library and adopts the
    // clips the pet drives directly.
    bool loadMotionLibrary(const QString& directory, QString* error = nullptr);
    const MotionLibrary& library() const { return library_; }
    // Single source of truth for how long the current action runs.
    double actionDuration(PetController::State state) const;
    // Returns true exactly once after a one-shot action reached its duration, so
    // the owner can restore the background state without a parallel timer.
    bool consumeActionFinished();

    void setBusyRandomSeed(quint32 seed) { busyRandom_.seed(seed); }
    void forceLaptopBusy(); // Native review / manual preview, uses the real player.
    bool isLaptopBusy() const { return laptopBusy_; }
    void setPreviewPose(const Parameters& parameters);
    bool isPreview() const { return preview_; }
    bool frozenPhysics() const { return preview_ && !sequencePhysics_; }
    void setSequencePose(const Parameters& parameters) { values_ = parameters; preview_ = true; sequencePhysics_ = true; }
    const Parameters& values() const { return values_; }

private:
    static double smooth(double t);
    static double pulse(double t, double start, double peak, double end);
    void updateGrassFlex(double seconds, double target);

    PetController::State state_ = PetController::State::Idle;
    Parameters values_;
    double clock_ = 0.0;
    double actionTime_ = 0.0;
    double blinkClock_ = 0.0;
    double leftEyeExpression_ = 1.0;
    double rightEyeExpression_ = 1.0;
    MotionLibrary library_;
    MotionClip grassClip_;
    MotionClip laptopClip_;
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
