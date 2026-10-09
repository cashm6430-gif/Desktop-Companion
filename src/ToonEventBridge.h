#pragma once

#include "PetController.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// The Qt host owns activity truth and transient action arbitration. The
// renderer receives semantic requests, never Live2D parameters or file paths.
// Each connection starts a new acknowledged epoch; foreground requests are
// discarded on disconnect, while active agent turns remain in PetController.
class ToonEventBridge final : public QObject {
    Q_OBJECT
public:
    static constexpr int kProtocolVersion = 1;
    static constexpr qsizetype kMaximumLineBytes = 16384;
    static constexpr int kActionTtlMs = 12000;

    explicit ToonEventBridge(PetController* controller, QObject* parent = nullptr);
    ~ToonEventBridge() override;
    bool start();
    quint16 port() const { return server_.serverPort(); }
    QString token() const { return token_; }
    bool rendererReady() const { return ready_; }
    QString foregroundRequestId() const { return foregroundRequestId_; }
    QJsonObject status() const;
    void shutdown();

public slots:
    void turnStarted(const QString& sessionId, const QString& turnId);
    void turnStopped(const QString& sessionId, const QString& turnId);
    void sessionEnded(const QString& sessionId);
    void desktopItemDeleted(const QString& path = QString(),
                            const QPointF& iconPos = QPointF(),
                            const QIcon& icon = QIcon());
    void resetBusy();
    bool wave();

signals:
    void rendererReadyChanged(bool ready);
    void quitRequested();
    // Safe for committed replay reports: tokens, file paths and hook session
    // identifiers are deliberately absent from this semantic trace.
    void trace(const QJsonObject& record);

private:
    void acceptConnection();
    void readMessages(QTcpSocket* socket);
    void handleMessage(QTcpSocket* socket, const QJsonObject& message);
    void dropConnection(QTcpSocket* socket, const QString& reason);
    bool send(QJsonObject message);
    void publishActivity();
    bool requestAction(const QString& name, const QPointF& iconPos = QPointF());
    void clearForeground(const QString& reason, bool notifyRenderer);
    void record(const QString& event, const QJsonObject& fields = {});

    PetController* controller_;
    QTcpServer server_;
    QPointer<QTcpSocket> peer_;
    QTimer handshakeTimer_;
    QTimer actionTimer_;
    QTimer activityPoll_;
    QByteArray buffer_;
    QString token_;
    QString sessionId_;
    QString connectionId_;
    QString foregroundRequestId_;
    QString foregroundAction_;
    QSet<QString> capabilities_;
    QJsonObject rendererDetails_;
    qint64 outboundSeq_ = 0;
    qint64 inboundSeq_ = 0;
    qint64 revision_ = 0;
    int lastActiveTurns_ = -1;
    int requestCounter_ = 0;
    bool authenticated_ = false;
    bool ready_ = false;
    bool shuttingDown_ = false;
};
