#include "CodexTurnSource.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
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
        sendMessage({{QStringLiteral("event"), action},
                     {QStringLiteral("session_id"), hook.value(QStringLiteral("session_id"))},
                     {QStringLiteral("turn_id"), hook.value(QStringLiteral("turn_id"))}});
    }
    // Valid JSON for Stop/Interrupt; no prompt text or hook output is retained.
    std::cout << "{}" << std::endl;
    return 0;
}
