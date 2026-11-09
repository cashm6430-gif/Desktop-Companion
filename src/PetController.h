#pragma once

#include "MotionLibrary.h"

#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <functional>

// Owns the priority, de-duplication and active-turn bookkeeping, and decides
// which state the pet is in.
// It does not know how long an action looks like: durations are pushed in from
// the motion library, so the clip data and the state machine share one number.
class PetController final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Busy, Delete, Grass };
    Q_ENUM(State)

    using NowMs = std::function<qint64()>;
    // The injectable clock keeps controller event-chain tests deterministic;
    // default construction retains the wall-clock behavior used by hooks.
    explicit PetController(QObject* parent = nullptr, NowMs nowMs = {});
    State state() const { return state_; }
    int activeTurnCount() const { return activeTurns_.size(); }
    int pendingPermissionCount() const;
    int permissionTurnCount() const { return permissions_.size(); }
    bool hasPendingPermission(const QString& sessionId, const QString& turnId,
                              const QString& requestId) const;
    // Only desktop deletion reactions are discarded; work facts stay live.
    void setDesktopDeletionSuppressed(bool suppressed) { desktopDeletionSuppressed_ = suppressed; }
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
    // Replace only a known exact turn's request set. This does not create or
    // stop work, refresh its clock, or imply parallel tools have stopped.
    void applyPermissionSnapshot(const QString& sessionId, const QString& turnId,
                                 const QStringList& requestIds);
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
    void permissionStateChanged();
    // Emitted once per newly observed request in a live turn, after the full
    // request set is committed. A refused/quiet reaction is never queued.
    void permissionRequested(const QString& sessionId, const QString& turnId,
                             const QString& requestId);
    // A newly tracked real turn, after insertion and before any background
    // state change. Delete/Grass can keep Busy hidden while work still starts.
    // Repeated starts for an already active key do not emit another beginning.
    void turnBegan(const QString& sessionId, const QString& turnId);
    // A real stop removed the last active turn across all sessions. The
    // background state has already been restored when this is emitted.
    // Session cleanup, expiry and manual reset deliberately stay silent.
    void allTurnsStopped();
    // A single accepted turn ended (real stop only, like allTurnsStopped).
    // `workedMs` is how long the turn had been active -- the window layer
    // turns real work into an automatic desk-corner summary note.
    void turnEnded(const QString& sessionId, qint64 workedMs);
    // Fired once per accepted eat trigger, right after the delete state is
    // entered. `file` is the dropped or deleted file (empty for the manual
    // tray trigger), `sourcePos` the on-desktop position the file's icon sat
    // at (null when unknown, e.g. drag-and-drop feeds where the file is
    // already on the pet), `icon` the icon to show while eating it.
    void eatTriggered(const QString& file, const QPointF& sourcePos, const QIcon& icon);

private slots:
    void expireOldTurns();

private:
    void setState(State state);
    void restoreBackgroundState();
    void triggerEat(const QString& file, const QPointF& sourcePos, const QIcon& icon);
    int durationMs(State state) const;
    QHash<QString, qint64> activeTurns_;
    QHash<QString, quint64> activeTurnGenerations_;
    quint64 nextTurnGeneration_ = 0;
    QHash<QString, QSet<QString>> permissions_;
    // Keep historical admission bounded without evicting old IDs into a
    // replay window. A saturated live turn remains factual but stops cues.
    QHash<QString, QSet<QString>> permissionNominations_;
    NowMs nowMs_;
    State state_ = State::Idle;
    QTimer actionTimer_;
    QTimer expiryTimer_;
    QElapsedTimer deleteClock_;
    bool actionFallbackEnabled_ = true;
    bool desktopDeletionSuppressed_ = false;
    QString lastFedFile_;
    double deleteDuration_ = MotionLibrary::kDeleteDuration;
    double grassDuration_ = MotionLibrary::kGrassFallback;
};
