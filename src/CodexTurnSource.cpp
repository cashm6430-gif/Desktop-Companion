#include "CodexTurnSource.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QDebug>

CodexTurnSource::CodexTurnSource(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, &CodexTurnSource::acceptConnections);
}

QString CodexTurnSource::serverName() {
    return QStringLiteral("deepseek-whale-companion-v1");
}

bool CodexTurnSource::start() {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (server_.listen(serverName())) return true;
    qWarning() << "Codex IPC unavailable:" << server_.errorString();
    return false;
}

void CodexTurnSource::acceptConnections() {
    while (server_.hasPendingConnections()) {
        QLocalSocket* socket = server_.nextPendingConnection();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            QByteArray bytes = socket->property("buffer").toByteArray() + socket->readAll();
            if (bytes.size() > 4096) { socket->disconnectFromServer(); return; }
            const qsizetype newline = bytes.indexOf('\n');
            if (newline < 0) { socket->setProperty("buffer", bytes); return; }
            const QJsonDocument doc = QJsonDocument::fromJson(bytes.left(newline));
            const QJsonObject data = doc.object();
            const QString event = data.value(QStringLiteral("event")).toString();
            const QString session = data.value(QStringLiteral("session_id")).toString();
            const QString turn = data.value(QStringLiteral("turn_id")).toString();
            if (event == QStringLiteral("start")) emit turnStarted(session, turn);
            else if (event == QStringLiteral("stop")) emit turnStopped(session, turn);
            else if (event == QStringLiteral("session_end")) emit sessionEnded(session);
            else if (event == QStringLiteral("status") && statusProvider_) {
                socket->write(QJsonDocument(statusProvider_()).toJson(QJsonDocument::Compact) + '\n');
                socket->flush();
            }
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}
