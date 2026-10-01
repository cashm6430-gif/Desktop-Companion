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
    // The file the pet was last fed by drag-and-drop, empty before the first
    // drop. The eat clip itself does not know the file; this is for the
    // window-layer prop (the rolled-up document) to draw what was dropped.
    QString lastFedFile() const { return lastFedFile_; }
    // Overrides the fallback duration used when no motion player drives the
    // action. Called once at start-up with values from MotionLibrary.
    void setActionDuration(State state, double seconds);

public slots:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);
    void desktopItemDeleted();
    // Drag-and-drop feeding: a file dropped on the character triggers the same
    // eat action as a desktop deletion. Both sources share one trigger so the
    // de-duplication window and the fallback timer cannot drift apart.
    void fileDropped(const QStringList& paths);
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
    void triggerEat();
    void expireOldTurns();
    int durationMs(State state) const;
    QHash<QString, qint64> activeTurns_;
    State state_ = State::Idle;
    QTimer actionTimer_;
    QTimer expiryTimer_;
    QElapsedTimer deleteClock_;
    QString lastFedFile_;
    double deleteDuration_ = MotionLibrary::kDeleteDuration;
    double grassDuration_ = MotionLibrary::kGrassFallback;
};
