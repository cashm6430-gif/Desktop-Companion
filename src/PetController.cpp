#include "PetController.h"

#include <QDateTime>

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
    activeTurns_.remove(sessionId + QChar::Null + turnId);
    if (state_ != State::Delete && state_ != State::Grass) restoreBackgroundState();
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

void PetController::desktopItemDeleted() {
    if (deleteClock_.isValid() && deleteClock_.elapsed() < 450) return;
    deleteClock_.restart();
    if (state_ == State::Delete) emit stateChanged(State::Delete);
    else setState(State::Delete);
    actionTimer_.start(durationMs(State::Delete));
}

void PetController::playGrass() {
    if (state_ == State::Delete) return;
    if (state_ == State::Grass) emit stateChanged(State::Grass);
    else setState(State::Grass);
    actionTimer_.start(durationMs(State::Grass));
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
