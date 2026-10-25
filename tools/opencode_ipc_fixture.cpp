#include "CodexTurnSource.h"
#include "PetController.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <iostream>

namespace {
void output(const QJsonObject& value) {
    std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}
}

// This executable is test-only: it refuses the production IPC endpoint and
// never opens a GUI, settings, hooks, model, or desktop watcher.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 2) return 2;
    const QString endpoint = arguments[1];
    if (!endpoint.startsWith(QStringLiteral("pet-opencode-node-test-"))
        || endpoint == CodexTurnSource::serverName() || endpoint.size() > 100)
        return 2;
    CodexTurnSource source;
    PetController controller;
    QJsonArray receipts;
    QString currentSession;
    QString currentTurn;
    QObject::connect(&source, &CodexTurnSource::turnStarted, &controller, &PetController::turnStarted);
    QObject::connect(&source, &CodexTurnSource::turnStopped, &controller, &PetController::turnStopped);
    QObject::connect(&source, &CodexTurnSource::sessionEnded, &controller, &PetController::sessionEnded);
    const auto identifiers = [&](const QString& session, const QString& turn) {
        currentSession = session;
        currentTurn = turn;
    };
    QObject::connect(&source, &CodexTurnSource::turnStarted, &app, identifiers);
    QObject::connect(&source, &CodexTurnSource::turnStopped, &app, identifiers);
    QObject::connect(&source, &CodexTurnSource::eventReceived, &app,
        [&](const QString& client, const QString& event) {
            receipts.append(QJsonObject{{QStringLiteral("client"), client},
                {QStringLiteral("event"), event}, {QStringLiteral("active_turns"), controller.activeTurnCount()},
                {QStringLiteral("scoped_session"), currentSession}, {QStringLiteral("turn_id"), currentTurn}});
            if (receipts.size() != 2) return;
            const bool valid = receipts[0].toObject().value(QStringLiteral("event")) == QStringLiteral("start")
                && receipts[0].toObject().value(QStringLiteral("active_turns")).toInt() == 1
                && event == QStringLiteral("stop") && controller.activeTurnCount() == 0
                && controller.state() == PetController::State::Idle && client == QStringLiteral("opencode");
            output({{QStringLiteral("passed"), valid}, {QStringLiteral("receipts"), receipts}});
            app.exit(valid ? 0 : 1);
        });
    if (!source.start(endpoint)) return 3;
    output({{QStringLiteral("ready"), true}, {QStringLiteral("endpoint"), endpoint}});
    QTimer::singleShot(10'000, &app, [&] {
        output({{QStringLiteral("passed"), false}, {QStringLiteral("reason"), QStringLiteral("fixture_timeout")}});
        app.exit(4);
    });
    return app.exec();
}
