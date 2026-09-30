#pragma once

#include "MotionLibrary.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>

// Owns the priority, de-duplication and active-turn bookkeeping, and decides
// which state the pet is in.
// It does not know how long an action looks like: durations are pushed in from
// the motion library, so the clip data and the state machine share one number.
class PetController final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Busy, Delete, Grass };
    Q_ENUM(State)

    explicit PetController(QObject* parent = nullptr);
    State state() const { return state_; }
    int activeTurnCount() const { return activeTurns_.size(); }
    // Overrides the fallback duration used when no motion player drives the
    // action. Called once at start-up with values from MotionLibrary.
    void setActionDuration(State state, double seconds);

public slots:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);
    void desktopItemDeleted();
    void playGrass();
    void resetBusy();
    // Raised by the motion player when a one-shot action reached its authored
    // length; falls back to the same duration when running headless.
    void actionFinished();

signals:
    void stateChanged(PetController::State state);

private:
    void setState(State state);
    void restoreBackgroundState();
    void expireOldTurns();
    int durationMs(State state) const;
    QHash<QString, qint64> activeTurns_;
    State state_ = State::Idle;
    QTimer actionTimer_;
    QTimer expiryTimer_;
    QElapsedTimer deleteClock_;
    double deleteDuration_ = MotionLibrary::kDeleteDuration;
    double grassDuration_ = MotionLibrary::kGrassFallback;
};
