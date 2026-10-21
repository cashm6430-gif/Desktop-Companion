#pragma once

#include "PetController.h"

#include <QObject>
#include <QString>
#include <functional>

class ParameterMotion;

// Owns the live controller-to-motion work reaction wiring. A stop is not proof
// of task success: only the last real turn gets one neutral return-gaze cue.
// The duration note is independent and never nominates a second reaction.
class WorkReactionCoordinator final : public QObject {
public:
    using StateSink = std::function<void(PetController::State)>;
    using DurationNote = std::function<void(qint64)>;
    using ObserveAttempt = std::function<void(const QString&, bool, const QString&)>;

    static constexpr qint64 kMinimumNoteMs = 30 * 1000;

    WorkReactionCoordinator(PetController* controller, ParameterMotion* motion,
                            QObject* parent = nullptr, StateSink applyState = {},
                            DurationNote durationNote = {}, ObserveAttempt observe = {});

private:
    void applyState(PetController::State state);
    void returnGaze();
    void report(const QString& id, bool started, const QString& reason);

    PetController* controller_;
    ParameterMotion* motion_;
    StateSink stateSink_;
    DurationNote durationNote_;
    ObserveAttempt observe_;
    bool greeted_ = false;
};
