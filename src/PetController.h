#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTimer>

class PetController final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Busy, Delete, Grass };
    Q_ENUM(State)

    explicit PetController(QObject* parent = nullptr);
    State state() const { return state_; }
    int activeTurnCount() const { return activeTurns_.size(); }

public slots:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);
    void desktopItemDeleted();
    void playGrass();
    void resetBusy();

signals:
    void stateChanged(PetController::State state);

private:
    void setState(State state);
    void restoreBackgroundState();
    void expireOldTurns();
    QHash<QString, qint64> activeTurns_;
    State state_ = State::Idle;
    QTimer actionTimer_;
    QTimer expiryTimer_;
    QElapsedTimer deleteClock_;
};
