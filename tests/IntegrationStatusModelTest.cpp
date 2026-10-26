#include "../src/IntegrationStatusModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {
QDateTime utcAt(int seconds = 0) {
    return QDateTime::fromString(QStringLiteral("2026-10-23T04:05:06.123Z"),
                                 Qt::ISODateWithMs).addSecs(seconds);
}

QJsonObject snapshot(const IntegrationStatusModel& model, int active = 0,
                     const QString& state = QStringLiteral("idle")) {
    return model.snapshot(true, {}, active, state);
}

QJsonObject clientReceipt(const QJsonObject& status, const QString& id) {
    for (const auto& value : status.value(QStringLiteral("clients")).toArray()) {
        const auto client = value.toObject();
        if (client.value(QStringLiteral("id")).toString() == id) return client;
    }
    return {};
}
} // namespace

class IntegrationStatusModelTest final : public QObject {
    Q_OBJECT

private slots:
    void emptyModelHasFixedSourcesWithoutPresenceClaims();
    void sourcesAreWhitelistedAndUnknownSourcesShareOneSlot();
    void onlyRecognizedEventsChangeReceipts();
    void stopAndSessionEndDoNotClaimSuccess();
    void injectedClockUsesUtcAndLastReceivedOrder();
    void countsAndStateComeFromCurrentControllerSnapshot();
    void resetKeepsClientHistoryWithoutInventingCompletion();
    void manySourcesDoNotGrowStorageOrKeepArbitraryInput();
    void permissionSummaryUsesFactsWithoutExposingRequests();
};

void IntegrationStatusModelTest::emptyModelHasFixedSourcesWithoutPresenceClaims() {
    IntegrationStatusModel model;
    const auto status = model.snapshot(false, QStringLiteral("listener unavailable"), 0,
                                       QStringLiteral("idle"));
    QCOMPARE(status.keys(), QStringList({QStringLiteral("active_turns"),
        QStringLiteral("clients"), QStringLiteral("last_event"),
        QStringLiteral("listener_error"), QStringLiteral("listening"),
        QStringLiteral("pending_permissions"), QStringLiteral("permission_turns"),
        QStringLiteral("pet_state")}));
    QVERIFY(!status.value(QStringLiteral("listening")).toBool());
    QCOMPARE(status.value(QStringLiteral("listener_error")).toString(),
             QStringLiteral("listener unavailable"));
    QCOMPARE(status.value(QStringLiteral("active_turns")).toInt(), 0);
    QCOMPARE(status.value(QStringLiteral("pet_state")).toString(), QStringLiteral("idle"));
    QVERIFY(status.value(QStringLiteral("last_event")).toObject().isEmpty());

    const auto clients = status.value(QStringLiteral("clients")).toArray();
    QCOMPARE(clients.size(), 6);
    const QStringList expectedIds{QStringLiteral("codex"), QStringLiteral("workbuddy"),
        QStringLiteral("claude"), QStringLiteral("opencode"), QStringLiteral("unknown"),
        QStringLiteral("manual")};
    for (int i = 0; i < clients.size(); ++i) {
        const auto client = clients[i].toObject();
        QCOMPARE(client.keys(), QStringList({QStringLiteral("id"),
            QStringLiteral("last_event"), QStringLiteral("last_seen")}));
        QCOMPARE(client.value(QStringLiteral("id")).toString(), expectedIds[i]);
        QVERIFY(client.value(QStringLiteral("last_event")).toString().isEmpty());
        QVERIFY(client.value(QStringLiteral("last_seen")).toString().isEmpty());
    }
}

void IntegrationStatusModelTest::sourcesAreWhitelistedAndUnknownSourcesShareOneSlot() {
    auto time = utcAt();
    IntegrationStatusModel model(nullptr, [&] { return time; });
    const QStringList ids{QStringLiteral("codex"), QStringLiteral("workbuddy"),
        QStringLiteral("claude"), QStringLiteral("opencode"), QStringLiteral("unknown"),
        QStringLiteral("manual")};
    for (const auto& id : ids) {
        model.observeEvent(id, QStringLiteral("start"));
        const auto status = snapshot(model);
        QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
                     .value(QStringLiteral("client")).toString(), id);
        QCOMPARE(clientReceipt(status, id).value(QStringLiteral("last_event")).toString(),
                 QStringLiteral("start"));
        time = time.addSecs(1);
    }
    model.observeEvent(QStringLiteral("arbitrary-app/session-secret"), QStringLiteral("stop"));
    const auto status = snapshot(model);
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
                 .value(QStringLiteral("client")).toString(), QStringLiteral("unknown"));
    QCOMPARE(clientReceipt(status, QStringLiteral("unknown"))
                 .value(QStringLiteral("last_event")).toString(), QStringLiteral("stop"));
    QCOMPARE(clientReceipt(status, QStringLiteral("codex"))
                 .value(QStringLiteral("last_event")).toString(), QStringLiteral("start"));
    QCOMPARE(clientReceipt(status, QStringLiteral("opencode"))
                 .value(QStringLiteral("last_event")).toString(), QStringLiteral("start"));
    QCOMPARE(status.value(QStringLiteral("clients")).toArray().size(), 6);
}

void IntegrationStatusModelTest::onlyRecognizedEventsChangeReceipts() {
    auto time = utcAt();
    int clockCalls = 0;
    IntegrationStatusModel model(nullptr, [&] { ++clockCalls; return time; });
    QSignalSpy changed(&model, &IntegrationStatusModel::changed);
    const QStringList ignored{QString{}, QStringLiteral("status"), QStringLiteral("reset"),
        QStringLiteral("success"), QStringLiteral("START"), QStringLiteral(" start "),
        QStringLiteral("prompt: private content")};
    const auto empty = snapshot(model);
    for (const auto& event : ignored) model.observeEvent(QStringLiteral("codex"), event);
    QCOMPARE(snapshot(model), empty);
    QCOMPARE(changed.size(), 0);
    QCOMPARE(clockCalls, 0);

    model.observeEvent(QStringLiteral("codex"), QStringLiteral("start"));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(clockCalls, 1);
    const auto before = snapshot(model);
    time = time.addDays(1);
    for (const auto& event : ignored) model.observeEvent(QStringLiteral("claude"), event);
    QCOMPARE(snapshot(model), before);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(clockCalls, 1);
}

void IntegrationStatusModelTest::stopAndSessionEndDoNotClaimSuccess() {
    IntegrationStatusModel model(nullptr, [] { return utcAt(); });
    model.observeEvent(QStringLiteral("codex"), QStringLiteral("stop"));
    auto status = snapshot(model);
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
                 .value(QStringLiteral("event")).toString(), QStringLiteral("stop"));
    model.observeEvent(QStringLiteral("claude"), QStringLiteral("session_end"));
    status = snapshot(model);
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
                 .value(QStringLiteral("event")).toString(), QStringLiteral("session_end"));
    const auto json = QJsonDocument(status).toJson(QJsonDocument::Compact);
    QVERIFY(!json.contains("success"));
    QVERIFY(!json.contains("completed"));
    QVERIFY(!json.contains("thinking"));
    QVERIFY(!json.contains("waiting"));
}

void IntegrationStatusModelTest::injectedClockUsesUtcAndLastReceivedOrder() {
    auto time = QDateTime::fromString(QStringLiteral("2026-10-23T12:05:06.123+08:00"),
                                      Qt::ISODateWithMs);
    IntegrationStatusModel model(nullptr, [&] { return time; });
    model.observeEvent(QStringLiteral("codex"), QStringLiteral("start"));
    auto status = snapshot(model);
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
                 .value(QStringLiteral("at")).toString(),
             QStringLiteral("2026-10-23T04:05:06.123Z"));
    QCOMPARE(clientReceipt(status, QStringLiteral("codex"))
                 .value(QStringLiteral("last_seen")).toString(),
             QStringLiteral("2026-10-23T04:05:06.123Z"));

    time = time.addDays(-1);
    model.observeEvent(QStringLiteral("workbuddy"), QStringLiteral("stop"));
    time = time.addYears(1); // Reading does not turn receipt times into heartbeat times.
    status = snapshot(model);
    const auto latest = status.value(QStringLiteral("last_event")).toObject();
    QCOMPARE(latest.value(QStringLiteral("client")).toString(), QStringLiteral("workbuddy"));
    QCOMPARE(latest.value(QStringLiteral("at")).toString(),
             QStringLiteral("2026-10-22T04:05:06.123Z"));
    QCOMPARE(clientReceipt(status, QStringLiteral("codex"))
                 .value(QStringLiteral("last_seen")).toString(),
             QStringLiteral("2026-10-23T04:05:06.123Z"));
}

void IntegrationStatusModelTest::countsAndStateComeFromCurrentControllerSnapshot() {
    IntegrationStatusModel model(nullptr, [] { return utcAt(); });
    model.observeEvent(QStringLiteral("codex"), QStringLiteral("start"));
    model.observeEvent(QStringLiteral("codex"), QStringLiteral("start"));
    model.observeEvent(QStringLiteral("claude"), QStringLiteral("start"));
    model.observeEvent(QStringLiteral("workbuddy"), QStringLiteral("start"));
    auto status = snapshot(model, 2, QStringLiteral("delete"));
    QCOMPARE(status.value(QStringLiteral("active_turns")).toInt(), 2);
    QCOMPARE(status.value(QStringLiteral("pet_state")).toString(), QStringLiteral("delete"));
    model.observeEvent(QStringLiteral("codex"), QStringLiteral("session_end"));
    model.observeEvent(QStringLiteral("claude"), QStringLiteral("stop"));
    status = snapshot(model, 3, QStringLiteral("grass"));
    QCOMPARE(status.value(QStringLiteral("active_turns")).toInt(), 3);
    QCOMPARE(status.value(QStringLiteral("pet_state")).toString(), QStringLiteral("grass"));
    status = snapshot(model, -42, QStringLiteral("waiting_for_approval"));
    QCOMPARE(status.value(QStringLiteral("active_turns")).toInt(), 0);
    QCOMPARE(status.value(QStringLiteral("pet_state")).toString(), QStringLiteral("unknown"));
    status = model.snapshot(false, QStringLiteral("socket closed"), 1, QStringLiteral("busy"));
    QCOMPARE(status.value(QStringLiteral("active_turns")).toInt(), 1);
    QCOMPARE(status.value(QStringLiteral("pet_state")).toString(), QStringLiteral("busy"));
    QVERIFY(!status.value(QStringLiteral("listening")).toBool());
    QCOMPARE(status.value(QStringLiteral("listener_error")).toString(), QStringLiteral("socket closed"));
}

void IntegrationStatusModelTest::resetKeepsClientHistoryWithoutInventingCompletion() {
    auto time = utcAt();
    IntegrationStatusModel model(nullptr, [&] { return time; });
    QSignalSpy changed(&model, &IntegrationStatusModel::changed);
    model.observeEvent(QStringLiteral("codex"), QStringLiteral("start"));
    model.observeEvent(QStringLiteral("opencode"), QStringLiteral("start"));
    const auto before = clientReceipt(snapshot(model), QStringLiteral("codex"));
    const auto openCodeBefore = clientReceipt(snapshot(model), QStringLiteral("opencode"));
    time = time.addSecs(2);
    model.recordReset();
    auto status = snapshot(model, 0);
    QCOMPARE(changed.size(), 3);
    QCOMPARE(clientReceipt(status, QStringLiteral("codex")), before);
    QCOMPARE(clientReceipt(status, QStringLiteral("opencode")), openCodeBefore);
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject(),
             (QJsonObject{{QStringLiteral("client"), QStringLiteral("manual")},
                          {QStringLiteral("event"), QStringLiteral("reset")},
                          {QStringLiteral("at"), QStringLiteral("2026-10-23T04:05:08.123Z")}}));
    QCOMPARE(clientReceipt(status, QStringLiteral("manual"))
                 .value(QStringLiteral("last_event")).toString(), QStringLiteral("reset"));
    // The model reports the supplied count even while a reset signal is in flight.
    QCOMPARE(snapshot(model, 4, QStringLiteral("busy"))
                 .value(QStringLiteral("active_turns")).toInt(), 4);
    time = time.addSecs(1);
    model.observeEvent(QStringLiteral("workbuddy"), QStringLiteral("start"));
    status = snapshot(model, 1, QStringLiteral("busy"));
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
                 .value(QStringLiteral("client")).toString(), QStringLiteral("workbuddy"));
    QCOMPARE(clientReceipt(status, QStringLiteral("manual"))
                 .value(QStringLiteral("last_event")).toString(), QStringLiteral("reset"));
    QVERIFY(!QJsonDocument(status).toJson().contains("success"));
}

void IntegrationStatusModelTest::manySourcesDoNotGrowStorageOrKeepArbitraryInput() {
    IntegrationStatusModel model(nullptr, [] { return utcAt(); });
    for (int i = 0; i < 5000; ++i) {
        model.observeEvent(QStringLiteral("private-client-payload-%1").arg(i),
                           i % 2 ? QStringLiteral("stop") : QStringLiteral("start"));
    }
    const auto status = snapshot(model);
    QCOMPARE(status.value(QStringLiteral("clients")).toArray().size(), 6);
    QCOMPARE(clientReceipt(status, QStringLiteral("unknown"))
                 .value(QStringLiteral("last_event")).toString(), QStringLiteral("stop"));
    const auto json = QJsonDocument(status).toJson(QJsonDocument::Compact);
    QVERIFY(json.size() < 1024);
    QVERIFY(!json.contains("private-client"));
    QVERIFY(!json.contains("session"));
    QVERIFY(!json.contains("turn_id"));
    QVERIFY(!json.contains("prompt"));
    QVERIFY(!json.contains("path"));
    QVERIFY(!json.contains("payload"));
}

void IntegrationStatusModelTest::permissionSummaryUsesFactsWithoutExposingRequests() {
    IntegrationStatusModel model;
    model.observeEvent(QStringLiteral("opencode"), QStringLiteral("permission_state"));
    const auto status = model.snapshot(true, {}, 3, QStringLiteral("busy"), 4, 2);
    QCOMPARE(status.value(QStringLiteral("active_turns")).toInt(), 3);
    QCOMPARE(status.value(QStringLiteral("pending_permissions")).toInt(), 4);
    QCOMPARE(status.value(QStringLiteral("permission_turns")).toInt(), 2);
    QCOMPARE(status.value(QStringLiteral("pet_state")).toString(), QStringLiteral("busy"));
    QCOMPARE(status.value(QStringLiteral("last_event")).toObject()
        .value(QStringLiteral("event")).toString(), QStringLiteral("permission_state"));
    const auto empty = model.snapshot(true, {}, 1, QStringLiteral("busy"), -1, 10);
    QCOMPARE(empty.value(QStringLiteral("pending_permissions")).toInt(), 0);
    QCOMPARE(empty.value(QStringLiteral("permission_turns")).toInt(), 1);
    const auto bytes = QJsonDocument(status).toJson();
    QVERIFY(!bytes.contains("request_ids"));
    QVERIFY(!bytes.contains("session_id"));
    QVERIFY(!bytes.contains("prompt"));
    QVERIFY(!bytes.contains("success"));
}

QTEST_APPLESS_MAIN(IntegrationStatusModelTest)
#include "IntegrationStatusModelTest.moc"
