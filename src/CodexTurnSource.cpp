#include "CodexTurnSource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QSet>
#include <QDebug>

namespace {
QString diagnosticClient(const QJsonValue& value) {
    const QString client = value.toString();
    if (client == QStringLiteral("codex") || client == QStringLiteral("workbuddy")
        || client == QStringLiteral("claude") || client == QStringLiteral("opencode")
        || client == QStringLiteral("manual"))
        return client;
    return QStringLiteral("unknown");
}

QString controllerSession(const QString& client, const QString& session) {
    // A session id belongs to its client. JSON also escapes embedded NULs and
    // separators, so neither a source prefix nor PetController's key delimiter
    // can make distinct (client, session) pairs alias one another.
    return QString::fromUtf8(QJsonDocument(QJsonArray{client, session}).toJson(QJsonDocument::Compact));
}
}

CodexTurnSource::CodexTurnSource(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, &CodexTurnSource::acceptConnections);
}

QString CodexTurnSource::serverName() {
    return QStringLiteral("deepseek-whale-companion-v1");
}

bool CodexTurnSource::start(const QString& endpoint) {
    if (server_.isListening()) return true;
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (server_.listen(endpoint)) {
        lastError_.clear();
        return true;
    }
    lastError_ = server_.errorString();
    qWarning() << "Codex IPC unavailable:" << lastError_;
    return false;
}

void CodexTurnSource::acceptConnections() {
    while (server_.hasPendingConnections()) {
        QLocalSocket* socket = server_.nextPendingConnection();
        socket->setParent(this);
        const auto readRequest = [this, socket] {
            if (socket->property("handled").toBool()) return;
            QByteArray bytes = socket->property("buffer").toByteArray() + socket->readAll();
            const auto finish = [socket] {
                socket->setProperty("handled", true);
                socket->setProperty("buffer", QByteArray());
                socket->disconnectFromServer();
                if (socket->state() == QLocalSocket::UnconnectedState) socket->deleteLater();
            };
            if (bytes.size() > 4096) { finish(); return; }
            const qsizetype newline = bytes.indexOf('\n');
            if (newline < 0) { socket->setProperty("buffer", bytes); return; }
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(bytes.left(newline), &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) { finish(); return; }
            const QJsonObject data = doc.object();
            const QString event = data.value(QStringLiteral("event")).toString();
            if (event == QStringLiteral("status")) {
                if (statusProvider_) {
                    socket->write(QJsonDocument(statusProvider_()).toJson(QJsonDocument::Compact) + '\n');
                    socket->flush();
                }
                finish();
                return;
            }
            const bool permissionState = event == QStringLiteral("permission_state");
            if (event != QStringLiteral("start") && event != QStringLiteral("stop")
                && !permissionState
                && event != QStringLiteral("session_end")) { finish(); return; }
            const QString session = data.value(QStringLiteral("session_id")).toString();
            const QString turn = data.value(QStringLiteral("turn_id")).toString();
            if (session.trimmed().isEmpty()
                || (data.contains(QStringLiteral("turn_id")) && !data.value(QStringLiteral("turn_id")).isString())
                || (event == QStringLiteral("start") && turn.trimmed().isEmpty())) { finish(); return; }
            const QString client = diagnosticClient(data.value(QStringLiteral("client")));
            QStringList requestIds;
            if (permissionState) {
                // Only the audited OpenCode adapter supplies this contract.
                // Reject the complete snapshot on any malformed member: a
                // partial set would incorrectly resolve omitted requests.
                if (client != QStringLiteral("opencode") || turn.trimmed().isEmpty()
                    || session.size() > 1024 || session.contains(QChar::Null)
                    || turn.size() > 256 || turn.contains(QChar::Null)
                    || !data.value(QStringLiteral("request_ids")).isArray()) { finish(); return; }
                const QJsonArray requests = data.value(QStringLiteral("request_ids")).toArray();
                if (requests.size() > 32) { finish(); return; }
                QSet<QString> unique;
                for (const QJsonValue& request : requests) {
                    const QString id = request.toString();
                    if (!request.isString() || id.trimmed().isEmpty()
                        || id.size() > 256 || id.contains(QChar::Null)
                        || unique.contains(id)) { finish(); return; }
                    unique.insert(id);
                    requestIds.append(id);
                }
            }
            const QString scopedSession = controllerSession(client, session);
            if (event == QStringLiteral("start")) emit turnStarted(scopedSession, turn);
            else if (event == QStringLiteral("stop")) emit turnStopped(scopedSession, turn);
            else if (event == QStringLiteral("session_end")) emit sessionEnded(scopedSession);
            else if (permissionState) emit permissionSnapshot(scopedSession, turn, requestIds);
            emit eventReceived(client, event);
            finish();
        };
        connect(socket, &QLocalSocket::readyRead, this, readRequest);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        if (socket->bytesAvailable() > 0) readRequest();
    }
}
