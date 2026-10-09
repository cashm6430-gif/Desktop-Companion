#include "CodexTurnSource.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QDateTime>
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
    const bool dryRunHook = argc > 1 && std::string(argv[1]) == "--dry-run-hook";
    if (argc > 1 && std::string(argv[1]) == "--status") {
        QLocalSocket socket;
        socket.connectToServer(CodexTurnSource::serverName());
        if (!socket.waitForConnected(180)) return 1;
        socket.write("{\"event\":\"status\"}\n");
        if (!socket.waitForBytesWritten(180) || !socket.waitForReadyRead(180)) return 1;
        std::cout << socket.readAll().trimmed().toStdString() << std::endl;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--send") {
        if (argc < 3) return 2;
        const QString action = QString::fromLocal8Bit(argv[2]);
        const QString session = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("manual");
        const QString turn = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("test");
        return sendMessage({{QStringLiteral("event"), action},
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
        if (!dryRunHook)
            sendMessage({{QStringLiteral("event"), action},
                         {QStringLiteral("session_id"), hook.value(QStringLiteral("session_id"))},
                         {QStringLiteral("turn_id"), turn}});
    }
    // Valid JSON for Stop/Interrupt; no prompt text or hook output is retained.
    std::cout << "{}" << std::endl;
    return 0;
}
