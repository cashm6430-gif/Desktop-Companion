#include "WorkReactionCoordinator.h"

#include "ParameterMotion.h"

#include <utility>

WorkReactionCoordinator::WorkReactionCoordinator(
    PetController* controller, ParameterMotion* motion, QObject* parent,
    StateSink applyState, DurationNote durationNote, ObserveAttempt observe,
    AllowAutomatic allowAutomatic)
    : QObject(parent), controller_(controller), motion_(motion),
      stateSink_(std::move(applyState)), durationNote_(std::move(durationNote)),
      observe_(std::move(observe)), allowAutomatic_(std::move(allowAutomatic)) {
    connect(controller_, &PetController::stateChanged, this,
            [this](PetController::State state) { this->applyState(state); });
    connect(controller_, &PetController::turnBegan, this,
            [this](const QString&, const QString&) {
        // A fresh real task supersedes this request's old automatic cue. It
        // does not clear another turn's permission facts or other reactions.
        cancelOwnedPermissionCue();
        if (controller_->state() != PetController::State::Delete
            && controller_->state() != PetController::State::Grass) return;
        // A foreground action can hide the entire Busy entry. Consume a quiet
        // greeting at the real turn start so leaving quiet before that action
        // ends cannot replay it. Count one starts a fresh work session even
        // when the previous session also ended behind the same foreground.
        if (controller_->activeTurnCount() == 1) greeted_ = false;
        if (greeted_ || this->allowAutomatic()) return;
        greeted_ = true;
        report(QStringLiteral("greet"), false, QStringLiteral("quiet_policy"));
    });
    connect(controller_, &PetController::allTurnsStopped, this,
            [this] { returnGaze(); });
    connect(controller_, &PetController::turnEnded, this,
            [this](const QString&, qint64 workedMs) {
        if (workedMs < kMinimumNoteMs || !durationNote_) return;
        if (!this->allowAutomatic()) {
            report(QStringLiteral("duration-note"), false, QStringLiteral("quiet_policy"));
            return;
        }
        durationNote_(workedMs);
    });
    connect(controller_, &PetController::permissionStateChanged, this,
            [this] { permissionStateChanged(); });
    connect(controller_, &PetController::permissionRequested, this,
            [this](const QString& session, const QString& turn, const QString& request) {
        permissionRequested(session, turn, request);
    });
}

void WorkReactionCoordinator::applyState(PetController::State state) {
    // Apply the real background first. In particular a fresh Busy cancels an
    // old return-gaze before this session's greeting is offered.
    if (stateSink_) stateSink_(state);
    else motion_->setState(state);
    if (state != PetController::State::Busy) cancelOwnedPermissionCue();

    if (state == PetController::State::Idle) {
        greeted_ = false;
    } else if (state == PetController::State::Busy && !greeted_) {
        // Quiet consumes this session's greeting. It never delays real Busy,
        // queues a later cue, or cancels a reaction that is already running.
        greeted_ = true;
        if (!allowAutomatic()) {
            report(QStringLiteral("greet"), false, QStringLiteral("quiet_policy"));
            return;
        }
        const bool started = motion_->playGreet();
        report(QStringLiteral("greet"), started,
               started ? QStringLiteral("started") : QStringLiteral("motion_gate_refused"));
    }
}

void WorkReactionCoordinator::permissionRequested(const QString& sessionId, const QString& turnId,
                                                   const QString& requestId) {
    // The controller consumes each admission before emitting this signal.
    // Refusal is final: no timer, idle transition, or quiet-policy release
    // may replay a request that the user may already have handled.
    if (!controller_->hasPendingPermission(sessionId, turnId, requestId)) return;
    if (controller_->state() != PetController::State::Busy) {
        report(QStringLiteral("permission-cue"), false, QStringLiteral("activity_owns_stage"));
        return;
    }
    if (!allowAutomatic()) {
        report(QStringLiteral("permission-cue"), false, QStringLiteral("quiet_policy"));
        return;
    }
    const bool started = motion_->playPermissionCue();
    if (started) {
        permissionSession_ = sessionId;
        permissionTurn_ = turnId;
        permissionRequest_ = requestId;
        permissionCueSerial_ = motion_->permissionCueSerial();
    }
    report(QStringLiteral("permission-cue"), started,
           started ? QStringLiteral("started") : QStringLiteral("motion_gate_refused"));
}

void WorkReactionCoordinator::permissionStateChanged() {
    if (!permissionCueSerial_) return;
    if (!controller_->hasPendingPermission(permissionSession_, permissionTurn_, permissionRequest_))
        cancelOwnedPermissionCue();
}

void WorkReactionCoordinator::cancelOwnedPermissionCue() {
    if (permissionCueSerial_ && motion_->permissionCueActive()
        && motion_->permissionCueSerial() == permissionCueSerial_)
        motion_->cancelInteraction();
    permissionCueSerial_ = 0;
    permissionSession_.clear();
    permissionTurn_.clear();
    permissionRequest_.clear();
}

void WorkReactionCoordinator::returnGaze() {
    // Do not steal Delete/Grass or replay a refused cue once it becomes Idle.
    if (controller_->activeTurnCount() != 0
        || controller_->state() != PetController::State::Idle) {
        report(QStringLiteral("turn-ended"), false, QStringLiteral("activity_owns_stage"));
        return;
    }
    if (!allowAutomatic()) {
        report(QStringLiteral("turn-ended"), false, QStringLiteral("quiet_policy"));
        return;
    }
    const bool started = motion_->playTurnEnded();
    report(QStringLiteral("turn-ended"), started,
           started ? QStringLiteral("started") : QStringLiteral("motion_gate_refused"));
}

void WorkReactionCoordinator::report(const QString& id, bool started, const QString& reason) {
    if (observe_) observe_(id, started, reason);
}

bool WorkReactionCoordinator::allowAutomatic() const {
    return !allowAutomatic_ || allowAutomatic_();
}
