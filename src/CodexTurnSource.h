#pragma once

#include <QLocalServer>
#include <QJsonObject>
#include <QObject>
#include <functional>

class CodexTurnSource final : public QObject {
    Q_OBJECT
public:
    explicit CodexTurnSource(QObject* parent = nullptr);
    bool start(const QString& endpoint = serverName());
    bool isListening() const { return server_.isListening(); }
    QString error() const { return lastError_; }
    static QString serverName();
    void setStatusProvider(std::function<QJsonObject()> provider) { statusProvider_ = std::move(provider); }

signals:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);
    // Diagnostic metadata only. Emitted after the matching controller signal;
    // identifiers and prompt contents never leave the protocol handler here.
    void eventReceived(const QString& client, const QString& event);

private:
    void acceptConnections();
    QLocalServer server_;
    QString lastError_;
    std::function<QJsonObject()> statusProvider_;
};
