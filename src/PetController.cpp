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
    actionTimer_.start(1400);
}

void PetController::playGrass() {
    if (state_ == State::Delete) return;
    if (state_ == State::Grass) emit stateChanged(State::Grass);
    else setState(State::Grass);
    actionTimer_.start(6900);
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
