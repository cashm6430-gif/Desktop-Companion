#pragma once

#include "MotionLibrary.h"

#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPointF>
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
    // Native review advances its own frame clock. Disable only the wall-clock
    // deadline there; explicit actionFinished() still restores the background.
    // Re-enabling affects future starts and does not restart a current action.
    void setActionFallbackEnabled(bool enabled);

public slots:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);
    // Desktop deletion, carrying what the pre-delete snapshot knew about the
    // file: its path, the icon-grid position (logical global coordinates) and
    // the icon it had while it existed. All three are optional -- the tray
    // menu's manual "play delete action" and headless tests call it bare.
    void desktopItemDeleted(const QString& path = QString(),
                            const QPointF& iconPos = QPointF(),
                            const QIcon& icon = QIcon());
    // Drag-and-drop feeding: a file dropped on the character triggers the same
    // eat action as a desktop deletion. Both sources share one trigger so the
    // de-duplication window and the fallback timer cannot drift apart.
    void fileDropped(const QStringList& paths);
    void playGrass();
    // Interactive grass owns an enter/hold/respond/release sequence whose
    // maximum length can exceed the original one-shot clip. Replace that
    // clip's fallback with a finite deadline; the player may finish sooner.
    void playInteractiveGrass(double maximumSeconds);
    void resetBusy();
    // Raised by the motion player when a one-shot action reached its authored
    // length; falls back to the same duration when running headless.
    void actionFinished();

signals:
    void stateChanged(PetController::State state);
    // A real stop removed the last active turn across all sessions. The
    // background state has already been restored when this is emitted.
    // Session cleanup, expiry and manual reset deliberately stay silent.
    void allTurnsStopped();
    // Fired once per accepted eat trigger, right after the delete state is
    // entered. `file` is the dropped or deleted file (empty for the manual
    // tray trigger), `sourcePos` the on-desktop position the file's icon sat
    // at (null when unknown, e.g. drag-and-drop feeds where the file is
    // already on the pet), `icon` the icon to show while eating it.
    void eatTriggered(const QString& file, const QPointF& sourcePos, const QIcon& icon);

private:
    void setState(State state);
    void restoreBackgroundState();
    void triggerEat(const QString& file, const QPointF& sourcePos, const QIcon& icon);
    void expireOldTurns();
    int durationMs(State state) const;
    QHash<QString, qint64> activeTurns_;
    State state_ = State::Idle;
    QTimer actionTimer_;
    QTimer expiryTimer_;
    QElapsedTimer deleteClock_;
    bool actionFallbackEnabled_ = true;
    QString lastFedFile_;
    double deleteDuration_ = MotionLibrary::kDeleteDuration;
    double grassDuration_ = MotionLibrary::kGrassFallback;
};
