#include "PetController.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

PetController::PetController(QObject* parent) : QObject(parent) {
    actionTimer_.setSingleShot(true);
    connect(&actionTimer_, &QTimer::timeout, this, &PetController::restoreBackgroundState);
    expiryTimer_.setInterval(60 * 1000);
    connect(&expiryTimer_, &QTimer::timeout, this, &PetController::expireOldTurns);
    expiryTimer_.start();
}

void PetController::setState(State state) {
    if (state_ == state) return;
    state_ = state;
    emit stateChanged(state_);
}

void PetController::restoreBackgroundState() {
    setState(activeTurns_.isEmpty() ? State::Idle : State::Busy);
}

void PetController::setActionDuration(State state, double seconds) {
    if (!(seconds > 0.0)) return;
    if (state == State::Delete) deleteDuration_ = seconds;
    else if (state == State::Grass) grassDuration_ = seconds;
}

void PetController::setActionFallbackEnabled(bool enabled) {
    actionFallbackEnabled_ = enabled;
    if (!enabled) actionTimer_.stop();
}

int PetController::durationMs(State state) const {
    const double seconds = state == State::Delete ? deleteDuration_ : grassDuration_;
    return static_cast<int>(seconds * 1000.0 + 0.5);
}

void PetController::actionFinished() {
    // A one-shot action ended. Stop the fallback timer and fall back to whatever
    // the background was doing; concurrent turns keep the pet busy.
    actionTimer_.stop();
    if (state_ == State::Delete || state_ == State::Grass) restoreBackgroundState();
}

void PetController::turnStarted(const QString& sessionId, const QString& turnId) {
    if (sessionId.isEmpty() || turnId.isEmpty()) return;
    activeTurns_.insert(sessionId + QChar::Null + turnId, QDateTime::currentMSecsSinceEpoch());
    if (state_ != State::Delete && state_ != State::Grass) restoreBackgroundState();
}

void PetController::turnStopped(const QString& sessionId, const QString& turnId) {
    qint64 endedStartedMs = -1;
    const QString exactKey = sessionId + QChar::Null + turnId;
    if (activeTurns_.contains(exactKey)) {
        endedStartedMs = activeTurns_.value(exactKey);
        activeTurns_.remove(exactKey);
    }
    bool removed = endedStartedMs >= 0;
    if (!removed && turnId.isEmpty()) {
        // Harness stop hooks (e.g. WorkBuddy Stop) carry session_id only and no
        // turn_id; end the session's most recent turn so the clock winds down.
        const QString prefix = sessionId + QChar::Null;
        QString latest;
        qint64 latestMs = -1;
        for (auto it = activeTurns_.constBegin(); it != activeTurns_.constEnd(); ++it) {
            const qint64 startedMs = it.value();
            if (it.key().startsWith(prefix) && startedMs > latestMs) {
                latest = it.key();
                latestMs = startedMs;
            }
        }
        if (!latest.isEmpty()) {
            activeTurns_.remove(latest);
            endedStartedMs = latestMs;
            removed = true;
        }
    }
    if (state_ != State::Delete && state_ != State::Grass) restoreBackgroundState();
    if (removed) {
        if (activeTurns_.isEmpty()) emit allTurnsStopped();
        emit turnEnded(sessionId, qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - endedStartedMs));
    }
}

void PetController::sessionEnded(const QString& sessionId) {
    if (sessionId.isEmpty()) return;
    const QString prefix = sessionId + QChar::Null;
    for (auto it = activeTurns_.begin(); it != activeTurns_.end();) {
        if (it.key().startsWith(prefix)) it = activeTurns_.erase(it);
        else ++it;
    }
    if (state_ != State::Delete && state_ != State::Grass) restoreBackgroundState();
}

void PetController::desktopItemDeleted(const QString& path, const QPointF& iconPos, const QIcon& icon) {
    triggerEat(path, iconPos, icon);
}

void PetController::fileDropped(const QStringList& paths) {
    if (paths.isEmpty()) return;
    lastFedFile_ = paths.first();
    // A drop happens on the pet itself, so there is no separate source
    // position to lunge from -- the window layer already holds the icon.
    triggerEat(paths.first(), QPointF(), QIcon());
}

void PetController::triggerEat(const QString& file, const QPointF& sourcePos, const QIcon& icon) {
    // One shared entry for every "eat" source, so the de-dup window, the
    // retrigger broadcast and the fallback timer behave identically whether
    // the file came from a desktop deletion or a drag-and-drop feed.
    if (deleteClock_.isValid() && deleteClock_.elapsed() < 450) return;
    deleteClock_.restart();
    if (state_ == State::Delete) emit stateChanged(State::Delete);
    else setState(State::Delete);
    if (actionFallbackEnabled_) actionTimer_.start(durationMs(State::Delete));
    // Emitted after the state entry, so a listener can read state() and its
    // clock keeps in step with the clip. Suppressed during the de-dup window
    // together with the trigger itself.
    emit eatTriggered(file, sourcePos, icon);
}

void PetController::playGrass() {
    if (state_ == State::Delete) return;
    if (state_ == State::Grass) emit stateChanged(State::Grass);
    else setState(State::Grass);
    if (actionFallbackEnabled_) actionTimer_.start(durationMs(State::Grass));
}

void PetController::playInteractiveGrass(double maximumSeconds) {
    if (state_ == State::Delete) return;
    const double seconds = std::isfinite(maximumSeconds) && maximumSeconds > 0.0
        ? std::clamp(maximumSeconds, 0.25, 30.0) : 15.0;
    if (state_ == State::Grass) emit stateChanged(State::Grass);
    else setState(State::Grass);
    // Restart the same timer rather than leaving the one-shot deadline live.
    // Delete also replaces this timer, so an interrupted grass session cannot
    // restore the background in the middle of eating a file.
    if (actionFallbackEnabled_)
        actionTimer_.start(static_cast<int>(seconds * 1000.0 + 0.5));
}

void PetController::resetBusy() {
    activeTurns_.clear();
    if (state_ != State::Delete && state_ != State::Grass) restoreBackgroundState();
}

void PetController::expireOldTurns() {
    constexpr qint64 maxAgeMs = 12LL * 60 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = activeTurns_.begin(); it != activeTurns_.end();) {
        if (now - it.value() > maxAgeMs) it = activeTurns_.erase(it);
        else ++it;
    }
    if (state_ != State::Delete && state_ != State::Grass) restoreBackgroundState();
}
