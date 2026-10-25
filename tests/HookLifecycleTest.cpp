#include "../src/CodexTurnSource.h"
#include "../src/IntegrationStatusModel.h"
#include "../src/PetController.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <memory>

namespace {
QString privateEndpoint() {
    return QStringLiteral("pet-hook-lifecycle-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool isPrivateEndpoint(const QString& endpoint) {
    return endpoint.startsWith(QStringLiteral("pet-hook-lifecycle-"))
        && endpoint != CodexTurnSource::serverName();
}

QString stateName(const PetController& controller) {
    switch (controller.state()) {
    case PetController::State::Idle: return QStringLiteral("idle");
    case PetController::State::Busy: return QStringLiteral("busy");
    case PetController::State::Delete: return QStringLiteral("delete");
    case PetController::State::Grass: return QStringLiteral("grass");
    }
    return QStringLiteral("unknown");
}

struct Harness {
    CodexTurnSource source;
    PetController controller;
    IntegrationStatusModel model;
    const QString endpoint = privateEndpoint();

    Harness() {
        QObject::connect(&source, &CodexTurnSource::turnStarted, &controller, &PetController::turnStarted);
        QObject::connect(&source, &CodexTurnSource::turnStopped, &controller, &PetController::turnStopped);
        QObject::connect(&source, &CodexTurnSource::sessionEnded, &controller, &PetController::sessionEnded);
        QObject::connect(&source, &CodexTurnSource::eventReceived, &model, &IntegrationStatusModel::observeEvent);
        source.setStatusProvider([this] { return snapshot(); });
    }

    bool start() { return isPrivateEndpoint(endpoint) && source.start(endpoint); }
    QJsonObject snapshot() const {
        return model.snapshot(source.isListening(), source.error(), controller.activeTurnCount(), stateName(controller));
    }
};

struct Result {
    bool finished = false;
    bool consumed = true;
    int code = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    QByteArray output;
    QByteArray error;
    qint64 elapsedMs = 0;
    bool hookAccepted() const {
        return finished && consumed && code == 0 && status == QProcess::NormalExit
            && output.trimmed() == QByteArray("{}") && error.trimmed().isEmpty();
    }
};
}

// This fixture deliberately refuses the production endpoint. The optional
// helper override also lets package review run the exact staged helper bytes.
class HookLifecycleTest final : public QObject {
    Q_OBJECT
private:
    QString helper_;

    Result run(const QString& endpoint, QStringList arguments, const QByteArray& input = {},
               const CodexTurnSource* eventSource = nullptr) const {
        Result result;
        if (!isPrivateEndpoint(endpoint)) {
            result.error = "Refused a non-fixture endpoint.";
            return result;
        }
        arguments.prepend(endpoint);
        arguments.prepend(QStringLiteral("--endpoint"));
        std::unique_ptr<QSignalSpy> receipts;
        if (eventSource && eventSource->isListening())
            receipts = std::make_unique<QSignalSpy>(eventSource, &CodexTurnSource::eventReceived);
        QProcess process;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(3000);
        QElapsedTimer elapsed;
        QObject::connect(&process, &QProcess::started, &loop, [&] {
            process.write(input);
            process.closeWriteChannel();
        });
        QObject::connect(&process, &QProcess::finished, &loop, [&](int code, QProcess::ExitStatus status) {
            result.finished = true;
            result.code = code;
            result.status = status;
            loop.quit();
        });
        QObject::connect(&process, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) loop.quit();
        });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        elapsed.start();
        timeout.start();
        process.start(helper_, arguments);
        // Keep the test's event loop alive while the helper waits for IPC. A
        // blocking waitForFinished would starve this same-thread QLocalServer.
        loop.exec();
        if (process.state() != QProcess::NotRunning) {
            process.kill(); // Only this fixture's child process can be killed.
            process.waitForFinished(1000);
        }
        result.elapsedMs = elapsed.elapsed();
        result.output = process.readAllStandardOutput();
        result.error = process.readAllStandardError();
        // A successful pipe write/child exit does not establish consumption.
        // Await the receipt even for duplicate/no-op events whose count stays
        // unchanged. The offline branch deliberately has no receipt to await.
        if (receipts && result.finished && result.code == 0) {
            if (receipts->isEmpty()) receipts->wait(1000);
            result.consumed = receipts->count() == 1;
        }
        return result;
    }

    Result hook(const Harness& harness, const QString& event, const QString& client,
                const QString& session = QStringLiteral("same-session"),
                const QString& turn = QStringLiteral("same-turn")) const {
        QStringList arguments;
        if (!client.isNull()) arguments << QStringLiteral("--client") << client;
        QJsonObject payload{{QStringLiteral("hook_event_name"), event}, {QStringLiteral("session_id"), session}};
        if (!turn.isNull()) payload.insert(QStringLiteral("turn_id"), turn);
        payload.insert(QStringLiteral("prompt"), QStringLiteral("private-prompt-never-in-status"));
        return run(harness.endpoint, arguments, QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n', &harness.source);
    }

private slots:
    void initTestCase() {
        helper_ = qEnvironmentVariable("PET_REVIEW_HELPER");
        if (helper_.isEmpty()) {
            helper_ = QCoreApplication::applicationDirPath() + QStringLiteral("/DesktopCompanionHook");
#ifdef Q_OS_WIN
            helper_ += QStringLiteral(".exe");
#endif
        }
        const QFileInfo helper(helper_);
        QVERIFY2(helper.isAbsolute() && helper.isFile() && helper.isExecutable(), "The fixture helper must be an existing absolute executable.");
        QVERIFY(!isPrivateEndpoint(CodexTurnSource::serverName()));
    }

    void sameIdsAcrossFourClientsStayIndependent() {
        Harness harness;
        QVERIFY(harness.start());
        QSignalSpy receipts(&harness.source, &CodexTurnSource::eventReceived);
        QSignalSpy ended(&harness.controller, &PetController::allTurnsStopped);
        const QStringList clients{QStringLiteral("codex"), QStringLiteral("workbuddy"),
                                  QStringLiteral("claude"), QStringLiteral("opencode")};
        for (int index = 0; index < clients.size(); ++index) {
            QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), clients[index]).hookAccepted());
            QTRY_COMPARE(harness.controller.activeTurnCount(), index + 1);
        }
        QCOMPARE(harness.controller.state(), PetController::State::Busy);
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), clients[0]).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 4); // Duplicate exact key.
        QVERIFY(hook(harness, QStringLiteral("Stop"), clients[0]).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 3);
        QVERIFY(hook(harness, QStringLiteral("Stop"), clients[0]).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 3); // Duplicate cannot stop another client.
        QVERIFY(hook(harness, QStringLiteral("Interrupt"), clients[1]).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 2);
        QVERIFY(hook(harness, QStringLiteral("Stop"), clients[3]).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QCOMPARE(ended.count(), 0);
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), clients[2]).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
        QCOMPARE(harness.controller.state(), PetController::State::Idle);
        QCOMPARE(ended.count(), 0); // Cleanup does not invent a real Stop.
        QCOMPARE(receipts.count(), 10);
        const QJsonObject snapshot = harness.snapshot();
        QCOMPARE(snapshot.value(QStringLiteral("active_turns")).toInt(), 0);
        const QJsonArray rows = snapshot.value(QStringLiteral("clients")).toArray();
        QCOMPARE(rows[0].toObject().value(QStringLiteral("last_event")).toString(), QStringLiteral("stop"));
        QCOMPARE(rows[1].toObject().value(QStringLiteral("last_event")).toString(), QStringLiteral("stop"));
        QCOMPARE(rows[2].toObject().value(QStringLiteral("last_event")).toString(), QStringLiteral("session_end"));
        QCOMPARE(rows[3].toObject().value(QStringLiteral("id")).toString(), QStringLiteral("opencode"));
        QCOMPARE(rows[3].toObject().value(QStringLiteral("last_event")).toString(), QStringLiteral("stop"));
        const QByteArray serialized = QJsonDocument(snapshot).toJson();
        QVERIFY(!serialized.contains("same-session"));
        QVERIFY(!serialized.contains("same-turn"));
        QVERIFY(!serialized.contains("private-prompt"));
    }

    void sessionCleanupAndMissingTurnStopAffectOnlyTheirClient() {
        Harness harness;
        QVERIFY(harness.start());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex"), QStringLiteral("same-session"), QStringLiteral("a")).hookAccepted());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex"), QStringLiteral("same-session"), QStringLiteral("b")).hookAccepted());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("claude")).hookAccepted());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("workbuddy")).hookAccepted());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("opencode")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 5);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("codex"), QStringLiteral("same-session"), QString()).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 4);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("codex"), QStringLiteral("same-session"), QStringLiteral("b")).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 4); // The missing-id stop already removed the latest turn.
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), QStringLiteral("codex")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 3);
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), QStringLiteral("codex")).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 3);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("claude")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 2);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("workbuddy")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), QStringLiteral("opencode")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
    }

    void legacyAndUnknownClientsShareOnlyLegacyScope() {
        Harness harness;
        QVERIFY(harness.start());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QString()).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("other-client")).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 2);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QString(), QStringLiteral("same-session"), QString()).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), QStringLiteral("arbitrary-source")).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 1);
        const QJsonObject latest = harness.snapshot().value(QStringLiteral("last_event")).toObject();
        QCOMPARE(latest.value(QStringLiteral("client")).toString(), QStringLiteral("unknown"));
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("codex")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
    }

    void sessionEncodingCannotAliasControllerDelimiters() {
        Harness harness;
        QVERIFY(harness.start());
        const QString sessionA = QStringLiteral("a") + QChar::Null + QStringLiteral("b");
        const QString turnB = QStringLiteral("b") + QChar::Null + QStringLiteral("c");
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex"), sessionA, QStringLiteral("c")).hookAccepted());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex"), QStringLiteral("a"), turnB).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 2);
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), QStringLiteral("codex"), QStringLiteral("a")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("codex"), sessionA, QStringLiteral("c")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
    }

    void offlineHelperReturnsJsonAndNewListenerRecoversWithoutReplay() {
        Harness harness;
        const Result offline = hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex"));
        QVERIFY(offline.hookAccepted());
        QVERIFY2(offline.elapsedMs < 2500, "An offline pet must not stall the calling agent.");
        QVERIFY(harness.start());
        QCOMPARE(harness.controller.activeTurnCount(), 0); // An offline event is never queued.
        QVERIFY(harness.snapshot().value(QStringLiteral("last_event")).toObject().isEmpty());
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("codex")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("Interrupt"), QStringLiteral("codex")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
    }

    void deliveryOptInReportsAbsentPetWithoutChangingLegacyJsonContract() {
        Harness harness;
        const QByteArray input = QJsonDocument(QJsonObject{
            {QStringLiteral("hook_event_name"), QStringLiteral("UserPromptSubmit")},
            {QStringLiteral("session_id"), QStringLiteral("strict-session")},
            {QStringLiteral("turn_id"), QStringLiteral("strict-turn")}}).toJson(QJsonDocument::Compact) + '\n';
        const QStringList strictArgs{QStringLiteral("--client"), QStringLiteral("opencode"),
                                     QStringLiteral("--require-delivery")};
        const Result offline = run(harness.endpoint, strictArgs, input);
        QVERIFY(offline.finished && offline.status == QProcess::NormalExit);
        QCOMPARE(offline.code, 1);
        QCOMPARE(offline.output.trimmed(), QByteArray("{}"));
        QVERIFY(offline.error.trimmed().isEmpty());
        QVERIFY(offline.elapsedMs < 2500);
        const Result legacy = run(harness.endpoint, {QStringLiteral("--client"), QStringLiteral("opencode")}, input);
        QVERIFY(legacy.hookAccepted());
        QVERIFY(harness.start());
        QCOMPARE(harness.controller.activeTurnCount(), 0); // Neither offline invocation is queued.
        const Result online = run(harness.endpoint, strictArgs, input, &harness.source);
        QVERIFY(online.hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QCOMPARE(harness.snapshot().value(QStringLiteral("last_event")).toObject()
                     .value(QStringLiteral("client")).toString(), QStringLiteral("opencode"));
    }

    void deliveryOptInDryRunStaysLocalAndReturnsValidJson() {
        Harness harness;
        QVERIFY(harness.start());
        QSignalSpy receipts(&harness.source, &CodexTurnSource::eventReceived);
        const QByteArray input = QJsonDocument(QJsonObject{
            {QStringLiteral("hook_event_name"), QStringLiteral("Stop")},
            {QStringLiteral("session_id"), QStringLiteral("strict-session")}}).toJson(QJsonDocument::Compact) + '\n';
        const Result result = run(harness.endpoint,
            {QStringLiteral("--client"), QStringLiteral("opencode"),
             QStringLiteral("--require-delivery"), QStringLiteral("--dry-run-hook")}, input);
        QVERIFY(result.hookAccepted());
        QTest::qWait(25);
        QCOMPARE(receipts.count(), 0);
        QCOMPARE(harness.controller.activeTurnCount(), 0);
    }

    void missingTurnIdsAndResetKeepExistingRecoveryContract() {
        Harness harness;
        QVERIFY(harness.start());
        QSignalSpy ended(&harness.controller, &PetController::turnEnded);
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("workbuddy"), QStringLiteral("same-session"), QString()).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("workbuddy"), QStringLiteral("same-session"), QString()).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
        QCOMPARE(ended.count(), 1);
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("claude")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        harness.controller.resetBusy();
        harness.model.recordReset();
        QCOMPARE(harness.controller.activeTurnCount(), 0);
        QVERIFY(hook(harness, QStringLiteral("Stop"), QStringLiteral("claude")).hookAccepted());
        QCOMPARE(harness.controller.activeTurnCount(), 0);
        QCOMPARE(ended.count(), 1); // Late stop after reset cannot fabricate a completion.
        QVERIFY(hook(harness, QStringLiteral("UserPromptSubmit"), QStringLiteral("claude")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QVERIFY(hook(harness, QStringLiteral("SessionEnd"), QStringLiteral("claude")).hookAccepted());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
    }

    void statusAndManualCommandsUseOnlyExplicitFixtureEndpoint() {
        Harness harness;
        QVERIFY(harness.start());
        QSignalSpy receipts(&harness.source, &CodexTurnSource::eventReceived);
        const Result manual = run(harness.endpoint, {QStringLiteral("--send"), QStringLiteral("start")}, {}, &harness.source);
        QVERIFY(manual.finished && manual.consumed && manual.code == 0 && manual.output.isEmpty() && manual.error.isEmpty());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 1);
        QCOMPARE(receipts.last().at(0).toString(), QStringLiteral("manual"));
        const QJsonObject before = harness.snapshot();
        const Result status = run(harness.endpoint, {QStringLiteral("--status")});
        QVERIFY(status.finished && status.code == 0 && status.error.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(status.output).object(), before);
        QCOMPARE(harness.snapshot(), before);
        QCOMPARE(receipts.count(), 1);
        const Result stop = run(harness.endpoint, {QStringLiteral("--send"), QStringLiteral("stop")}, {}, &harness.source);
        QVERIFY(stop.finished && stop.consumed && stop.code == 0 && stop.output.isEmpty() && stop.error.isEmpty());
        QTRY_COMPARE(harness.controller.activeTurnCount(), 0);
        const Result refused = run(CodexTurnSource::serverName(), {QStringLiteral("--send"), QStringLiteral("start")});
        QVERIFY(!refused.finished); // It never launched a process.
    }
};

QTEST_GUILESS_MAIN(HookLifecycleTest)
#include "HookLifecycleTest.moc"
