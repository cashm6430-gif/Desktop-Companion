#pragma once

#include <QLocalServer>
#include <QJsonObject>
#include <QObject>
#include <functional>

class CodexTurnSource final : public QObject {
    Q_OBJECT
public:
    explicit CodexTurnSource(QObject* parent = nullptr);
    bool start();
    static QString serverName();
    void setStatusProvider(std::function<QJsonObject()> provider) { statusProvider_ = std::move(provider); }

signals:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);

private:
    void acceptConnections();
    QLocalServer server_;
    std::function<QJsonObject()> statusProvider_;
};
