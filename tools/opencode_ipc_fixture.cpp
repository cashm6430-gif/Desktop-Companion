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
    if (arguments.size() != 2 && arguments.size() != 3) return 2;
    const bool permissionMode = arguments.size() == 3 && arguments[2] == QStringLiteral("permission");
    if (arguments.size() == 3 && !permissionMode) return 2;
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
    QObject::connect(&source, &CodexTurnSource::permissionSnapshot,
                     &controller, &PetController::applyPermissionSnapshot);
    const auto identifiers = [&](const QString& session, const QString& turn) {
        currentSession = session;
        currentTurn = turn;
    };
    QObject::connect(&source, &CodexTurnSource::turnStarted, &app, identifiers);
    QObject::connect(&source, &CodexTurnSource::turnStopped, &app, identifiers);
    QObject::connect(&source, &CodexTurnSource::permissionSnapshot, &app,
        [&](const QString& session, const QString& turn, const QStringList&) { identifiers(session, turn); });
    QObject::connect(&source, &CodexTurnSource::eventReceived, &app,
        [&](const QString& client, const QString& event) {
            // Default mode preserves the old lifecycle-only report. A new
            // execution also sends an empty snapshot to clear stale requests.
            if (!permissionMode && event == QStringLiteral("permission_state")) return;
            QJsonObject receipt{{QStringLiteral("client"), client},
                {QStringLiteral("event"), event}, {QStringLiteral("active_turns"), controller.activeTurnCount()},
                {QStringLiteral("scoped_session"), currentSession}, {QStringLiteral("turn_id"), currentTurn}};
            if (permissionMode) {
                receipt.insert(QStringLiteral("pending_permissions"), controller.pendingPermissionCount());
                receipt.insert(QStringLiteral("permission_turns"), controller.permissionTurnCount());
            }
            receipts.append(receipt);
            if (permissionMode) {
                if (receipts.size() > 8) { app.exit(5); return; }
                if (event != QStringLiteral("stop")) return;
                const QStringList expectedEvents{QStringLiteral("start"), QStringLiteral("permission_state"),
                    QStringLiteral("permission_state"), QStringLiteral("permission_state"),
                    QStringLiteral("permission_state"), QStringLiteral("permission_state"), QStringLiteral("stop")};
                const QList<int> expectedCounts{0, 0, 1, 2, 1, 0, 0};
                bool valid = receipts.size() == expectedEvents.size();
                for (int index = 0; valid && index < receipts.size(); ++index) {
                    const auto item = receipts[index].toObject();
                    valid = item.value(QStringLiteral("event")).toString() == expectedEvents[index]
                        && item.value(QStringLiteral("pending_permissions")).toInt() == expectedCounts[index]
                        && item.value(QStringLiteral("permission_turns")).toInt() == (expectedCounts[index] ? 1 : 0)
                        && item.value(QStringLiteral("active_turns")).toInt() == (index == receipts.size() - 1 ? 0 : 1)
                        && item.value(QStringLiteral("client")).toString() == QStringLiteral("opencode");
                }
                valid = valid && controller.state() == PetController::State::Idle;
                output({{QStringLiteral("passed"), valid}, {QStringLiteral("receipts"), receipts}});
                app.exit(valid ? 0 : 1);
                return;
            }
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
