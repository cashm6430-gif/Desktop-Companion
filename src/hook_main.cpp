#include "CodexTurnSource.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QDateTime>
#include <QElapsedTimer>
#include <QStringList>
#include <iostream>
#include <string>

namespace {
bool sendMessage(const QJsonObject& payload) {
    QLocalSocket socket;
    socket.connectToServer(CodexTurnSource::serverName());
    if (!socket.waitForConnected(180)) return false;
    QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    socket.write(bytes);
    return socket.waitForBytesWritten(180);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // Explicit test entry: validate the installed hook's JSON response without
    // sending synthetic turns to a user's running pet. Production hook
    // commands never pass this flag and keep the existing IPC behavior.
    bool dryRunHook = false;
    QString client;
    QStringList positional;
    const QStringList arguments = app.arguments();
    for (int index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments[index];
        if (argument == QStringLiteral("--dry-run-hook")) dryRunHook = true;
        else if (argument == QStringLiteral("--client") && index + 1 < arguments.size()
                 && !arguments[index + 1].startsWith(QStringLiteral("--"))) client = arguments[++index];
        else positional.append(argument);
    }
    if (!positional.isEmpty() && positional.front() == QStringLiteral("--status")) {
        QElapsedTimer deadline;
        deadline.start();
        const auto remaining = [&deadline] { return qMax(0, 180 - int(deadline.elapsed())); };
        QLocalSocket socket;
        socket.connectToServer(CodexTurnSource::serverName());
        if (!socket.waitForConnected(remaining())) return 1;
        socket.write("{\"event\":\"status\"}\n");
        if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(remaining())) return 1;
        QByteArray reply;
        for (;;) {
            reply.append(socket.readAll());
            if (reply.size() > 1024 * 1024) return 1;
            const qsizetype newline = reply.indexOf('\n');
            if (newline >= 0) {
                std::cout << reply.left(newline).trimmed().toStdString() << std::endl;
                return 0;
            }
            if (socket.state() == QLocalSocket::UnconnectedState || remaining() <= 0) return 1;
            if (!socket.waitForReadyRead(remaining()) && socket.bytesAvailable() == 0) return 1;
        }
    }
    if (!positional.isEmpty() && positional.front() == QStringLiteral("--send")) {
        if (positional.size() < 2) return 2;
        const QString action = positional[1];
        const QString session = positional.size() > 2 ? positional[2] : QStringLiteral("manual");
        const QString turn = positional.size() > 3 ? positional[3] : QStringLiteral("test");
        return sendMessage({{QStringLiteral("event"), action},
                            {QStringLiteral("client"), QStringLiteral("manual")},
                            {QStringLiteral("session_id"), session},
                            {QStringLiteral("turn_id"), turn}}) ? 0 : 1;
    }

    std::string input;
    std::getline(std::cin, input);
    const QJsonObject hook = QJsonDocument::fromJson(QByteArray::fromStdString(input)).object();
    const QString name = hook.value(QStringLiteral("hook_event_name")).toString();
    QString action;
    if (name == QStringLiteral("UserPromptSubmit")) action = QStringLiteral("start");
    else if (name == QStringLiteral("Stop") || name == QStringLiteral("Interrupt")) action = QStringLiteral("stop");
    else if (name == QStringLiteral("SessionEnd")) action = QStringLiteral("session_end");
    if (!action.isEmpty()) {
        // Hook payloads carry session_id but (except for SessionStart) no
        // turn_id; PetController requires both, so synthesize one turn id per
        // hook invocation. A Stop without turn_id ends the session's latest
        // turn, matching the synthesized id loosely by design.
        QString turn = hook.value(QStringLiteral("turn_id")).toString();
        if (turn.isEmpty() && action == QStringLiteral("start")) {
            turn = QStringLiteral("hook-%1").arg(QDateTime::currentMSecsSinceEpoch());
        }
        // Stop/SessionEnd keep the empty turn_id so PetController ends the
        // session's latest turn (or the whole session) instead of missing.
        QJsonObject message{{QStringLiteral("event"), action},
                            {QStringLiteral("session_id"), hook.value(QStringLiteral("session_id"))},
                            {QStringLiteral("turn_id"), turn}};
        if (!client.isNull()) message.insert(QStringLiteral("client"), client);
        if (!dryRunHook) sendMessage(message);
    }
    // Valid JSON for Stop/Interrupt; no prompt text or hook output is retained.
    std::cout << "{}" << std::endl;
    return 0;
}
