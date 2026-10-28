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
    using AllowAutomatic = std::function<bool()>;

    static constexpr qint64 kMinimumNoteMs = 30 * 1000;

    WorkReactionCoordinator(PetController* controller, ParameterMotion* motion,
                            QObject* parent = nullptr, StateSink applyState = {},
                            DurationNote durationNote = {}, ObserveAttempt observe = {},
                            AllowAutomatic allowAutomatic = {});

private:
    void applyState(PetController::State state);
    void returnGaze();
    void permissionRequested(const QString& sessionId, const QString& turnId,
                             const QString& requestId);
    void permissionStateChanged();
    void cancelOwnedPermissionCue();
    void report(const QString& id, bool started, const QString& reason);
    bool allowAutomatic() const;

    PetController* controller_;
    ParameterMotion* motion_;
    StateSink stateSink_;
    DurationNote durationNote_;
    ObserveAttempt observe_;
    AllowAutomatic allowAutomatic_;
    bool greeted_ = false;
    QString permissionSession_;
    QString permissionTurn_;
    QString permissionRequest_;
    quint64 permissionCueSerial_ = 0;
};
