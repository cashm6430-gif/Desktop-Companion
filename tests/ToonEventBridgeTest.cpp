#include "../src/ToonEventBridge.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QtTest>

#include <functional>

namespace {
bool spin(const std::function<bool()>& predicate, int milliseconds = 2000) {
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        if (predicate()) return true;
        if (elapsed.elapsed() >= milliseconds) return false;
        QCoreApplication::processEvents();
        QTest::qWait(1);
    }
}

struct Renderer {
    QTcpSocket socket;
    QByteArray pending;
    QList<QJsonObject> messages;
    QString session;
    QString connection;
    qint64 sequence = 0;

    void drain() {
        QCoreApplication::processEvents();
        pending += socket.readAll();
        qsizetype newline = -1;
        while ((newline = pending.indexOf('\n')) >= 0) {
            messages.append(QJsonDocument::fromJson(pending.left(newline)).object());
            pending.remove(0, newline + 1);
        }
    }
    QJsonObject take(const QString& type) {
        drain();
        for (qsizetype i = 0; i < messages.size(); ++i) {
            if (messages[i].value(QStringLiteral("type")).toString() == type)
                return messages.takeAt(i);
        }
        return {};
    }
    QJsonObject wait(const QString& type) {
        QJsonObject found;
        spin([&] { found = take(type); return !found.isEmpty(); });
        return found;
    }
    void raw(const QJsonObject& object) {
        socket.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
    }
    void send(QJsonObject object) {
        object.insert(QStringLiteral("v"), 1);
        object.insert(QStringLiteral("session_id"), session);
        object.insert(QStringLiteral("connection_id"), connection);
        object.insert(QStringLiteral("seq"), ++sequence);
        raw(object);
    }
    void ack(const QString& id, const QString& status = QStringLiteral("finished")) {
        send({{QStringLiteral("type"), QStringLiteral("action.ack")},
            {QStringLiteral("request_id"), id}, {QStringLiteral("status"), status}});
    }
    bool open(ToonEventBridge& bridge, bool deleteSupported = true, bool fragmented = false) {
        socket.connectToHost(QHostAddress::LocalHost, bridge.port());
        if (!spin([&] { return socket.state() == QAbstractSocket::ConnectedState; })) return false;
        const QJsonObject hello{{QStringLiteral("v"), 1},
            {QStringLiteral("type"), QStringLiteral("hello")}, {QStringLiteral("token"), bridge.token()}};
        if (fragmented) {
            const QByteArray bytes = QJsonDocument(hello).toJson(QJsonDocument::Compact) + '\n';
            socket.write(bytes.left(17));
            socket.flush();
            QTest::qWait(10);
            socket.write(bytes.mid(17));
            socket.flush();
        } else raw(hello);
        const auto welcome = wait(QStringLiteral("welcome"));
        if (welcome.isEmpty()) return false;
        session = welcome.value(QStringLiteral("session_id")).toString();
        connection = welcome.value(QStringLiteral("connection_id")).toString();
        QJsonArray capabilities{QStringLiteral("activity.idle"), QStringLiteral("activity.busy"),
            QStringLiteral("work.finished"), QStringLiteral("wave")};
        if (deleteSupported) capabilities.append(QStringLiteral("delete.react"));
        send({{QStringLiteral("type"), QStringLiteral("ready")},
            {QStringLiteral("capabilities"), capabilities}});
        return spin([&] { return bridge.rendererReady(); })
            && !wait(QStringLiteral("activity.snapshot")).isEmpty();
    }
    QJsonObject latestSnapshot(int expectedActiveTurns) {
        QJsonObject latest;
        spin([&] {
            for (;;) {
                const auto current = take(QStringLiteral("activity.snapshot"));
                if (current.isEmpty()) break;
                latest = current;
            }
            return !latest.isEmpty()
                && latest.value(QStringLiteral("active_turns")).toInt() == expectedActiveTurns;
        });
        return latest;
    }
};
}

class ToonEventBridgeTest final : public QObject {
    Q_OBJECT
private slots:
    void concurrentActivityAndForeground();
    void dedupAndOldAcknowledgement();
    void disconnectRestoresOnlyCurrentTruth();
    void wrongTokenAndMessageLimit();
    void wrongEpochSequenceAndUnsupportedAction();
    void deletionPreemptsWorkReaction();
    void newActivityCancelsOldCompletion();
    void boundedFallback();
};

void ToonEventBridgeTest::concurrentActivityAndForeground() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer renderer;
    QVERIFY(renderer.open(bridge, true, true));
    bridge.turnStarted(QStringLiteral("s1"), QStringLiteral("t1"));
    bridge.turnStarted(QStringLiteral("s2"), QStringLiteral("t2"));
    auto snapshot = renderer.latestSnapshot(2);
    QCOMPARE(snapshot.value(QStringLiteral("active_turns")).toInt(), 2);
    QVERIFY(snapshot.value(QStringLiteral("busy")).toBool());
    bridge.desktopItemDeleted(QStringLiteral("C:/fake-private-name.txt"), QPointF(100, 200));
    const auto action = renderer.wait(QStringLiteral("action.request"));
    QCOMPARE(action.value(QStringLiteral("name")).toString(), QStringLiteral("delete.react"));
    QCOMPARE(action.value(QStringLiteral("ttl_ms")).toInt(), ToonEventBridge::kActionTtlMs);
    QVERIFY(!QJsonDocument(action).toJson().contains("fake-private-name"));
    QCOMPARE(controller.state(), PetController::State::Delete);
    bridge.turnStopped(QStringLiteral("s1"), QStringLiteral("t1"));
    snapshot = renderer.latestSnapshot(1);
    QCOMPARE(snapshot.value(QStringLiteral("active_turns")).toInt(), 1);
    QVERIFY(snapshot.value(QStringLiteral("busy")).toBool());
    renderer.ack(action.value(QStringLiteral("request_id")).toString());
    QVERIFY(spin([&] { return bridge.foregroundRequestId().isEmpty(); }));
    QCOMPARE(controller.state(), PetController::State::Busy);
    bridge.turnStopped(QStringLiteral("s2"), QStringLiteral("t2"));
    const auto finish = renderer.wait(QStringLiteral("action.request"));
    QCOMPARE(finish.value(QStringLiteral("name")).toString(), QStringLiteral("work.finished"));
    snapshot = renderer.latestSnapshot(0);
    QCOMPARE(snapshot.value(QStringLiteral("active_turns")).toInt(), 0);
    QVERIFY(!snapshot.value(QStringLiteral("busy")).toBool());
    renderer.ack(finish.value(QStringLiteral("request_id")).toString());
    QVERIFY(spin([&] { return bridge.foregroundRequestId().isEmpty(); }));
    bridge.turnStopped(QStringLiteral("s2"), QStringLiteral("t2"));
    QTest::qWait(20);
    QVERIFY(renderer.take(QStringLiteral("action.request")).isEmpty());
}

void ToonEventBridgeTest::dedupAndOldAcknowledgement() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer renderer;
    QVERIFY(renderer.open(bridge));
    bridge.desktopItemDeleted();
    const auto first = renderer.wait(QStringLiteral("action.request"));
    QVERIFY(!first.isEmpty());
    bridge.desktopItemDeleted();
    QTest::qWait(10);
    QVERIFY(renderer.take(QStringLiteral("action.request")).isEmpty());
    QTest::qWait(460);
    bridge.desktopItemDeleted();
    const auto second = renderer.wait(QStringLiteral("action.request"));
    const QString current = second.value(QStringLiteral("request_id")).toString();
    QVERIFY(!current.isEmpty());
    QVERIFY(current != first.value(QStringLiteral("request_id")).toString());
    const auto cancel = renderer.wait(QStringLiteral("action.cancel"));
    QCOMPARE(cancel.value(QStringLiteral("request_id")), first.value(QStringLiteral("request_id")));
    QCOMPARE(controller.state(), PetController::State::Delete);
    renderer.ack(first.value(QStringLiteral("request_id")).toString());
    QTest::qWait(20);
    QCOMPARE(bridge.foregroundRequestId(), current);
    renderer.ack(current, QStringLiteral("started"));
    QTest::qWait(20);
    QCOMPARE(bridge.foregroundRequestId(), current);
    renderer.ack(current);
    QVERIFY(spin([&] { return bridge.foregroundRequestId().isEmpty(); }));
    QCOMPARE(controller.state(), PetController::State::Idle);
}

void ToonEventBridgeTest::disconnectRestoresOnlyCurrentTruth() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer first;
    QVERIFY(first.open(bridge));
    bridge.turnStarted(QStringLiteral("s"), QStringLiteral("a"));
    bridge.turnStarted(QStringLiteral("s"), QStringLiteral("b"));
    bridge.desktopItemDeleted();
    QVERIFY(!first.wait(QStringLiteral("action.request")).isEmpty());
    const QString firstEpoch = first.connection;
    first.socket.abort();
    QVERIFY(spin([&] { return !bridge.rendererReady(); }));
    QCOMPARE(bridge.foregroundRequestId(), QString());
    QCOMPARE(controller.state(), PetController::State::Busy);
    bridge.turnStopped(QStringLiteral("s"), QStringLiteral("a"));
    QTest::qWait(460);
    bridge.desktopItemDeleted(); // dropped, never replayed at reconnect
    QCOMPARE(controller.state(), PetController::State::Busy);
    Renderer second;
    QVERIFY(second.open(bridge));
    QVERIFY(second.connection != firstEpoch);
    // Consume a fresh snapshot through a concurrent start and stop, then
    // check that the reconnect did not resurrect any deletion request.
    bridge.turnStarted(QStringLiteral("temp"), QStringLiteral("c"));
    bridge.sessionEnded(QStringLiteral("temp"));
    const auto snapshot = second.latestSnapshot(1);
    QCOMPARE(snapshot.value(QStringLiteral("active_turns")).toInt(), 1);
    QVERIFY(snapshot.value(QStringLiteral("busy")).toBool());
    QVERIFY(second.take(QStringLiteral("action.request")).isEmpty());
    bridge.sessionEnded(QStringLiteral("s"));
    QCOMPARE(controller.activeTurnCount(), 0);
    QVERIFY(second.take(QStringLiteral("action.request")).isEmpty());
}

void ToonEventBridgeTest::wrongTokenAndMessageLimit() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QSignalSpy trace(&bridge, &ToonEventBridge::trace);
    QVERIFY(bridge.start());
    Renderer wrong;
    wrong.socket.connectToHost(QHostAddress::LocalHost, bridge.port());
    QVERIFY(spin([&] { return wrong.socket.state() == QAbstractSocket::ConnectedState; }));
    wrong.raw({{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("hello")},
        {QStringLiteral("token"), QStringLiteral("wrong-secret")}});
    QVERIFY(spin([&] { return wrong.socket.state() == QAbstractSocket::UnconnectedState; }));
    QVERIFY(!bridge.rendererReady());
    Renderer oversize;
    oversize.socket.connectToHost(QHostAddress::LocalHost, bridge.port());
    QVERIFY(spin([&] { return oversize.socket.state() == QAbstractSocket::ConnectedState; }));
    oversize.socket.write(QByteArray(ToonEventBridge::kMaximumLineBytes + 1, 'x'));
    oversize.socket.flush();
    QVERIFY(spin([&] { return oversize.socket.state() == QAbstractSocket::UnconnectedState; }));
    Renderer valid;
    QVERIFY(valid.open(bridge));
    for (const auto& call : trace) {
        const QByteArray bytes = QJsonDocument(call[0].toJsonObject()).toJson();
        QVERIFY(!bytes.contains(bridge.token().toUtf8()));
        QVERIFY(!bytes.contains("wrong-secret"));
    }
}

void ToonEventBridgeTest::wrongEpochSequenceAndUnsupportedAction() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer renderer;
    QVERIFY(renderer.open(bridge, false));
    bridge.desktopItemDeleted();
    QCOMPARE(controller.state(), PetController::State::Idle);
    QVERIFY(renderer.take(QStringLiteral("action.request")).isEmpty());
    QVERIFY(bridge.wave());
    const auto request = renderer.wait(QStringLiteral("action.request"));
    const auto current = request.value(QStringLiteral("request_id")).toString();
    QJsonObject ack{{QStringLiteral("v"), 1}, {QStringLiteral("type"), QStringLiteral("action.ack")},
        {QStringLiteral("session_id"), renderer.session}, {QStringLiteral("connection_id"), QStringLiteral("old-epoch")},
        {QStringLiteral("seq"), 100}, {QStringLiteral("request_id"), current},
        {QStringLiteral("status"), QStringLiteral("finished")}};
    renderer.raw(ack);
    QTest::qWait(20);
    QCOMPARE(bridge.foregroundRequestId(), current);
    ack.insert(QStringLiteral("connection_id"), renderer.connection);
    ack.insert(QStringLiteral("seq"), renderer.sequence); // duplicate ready sequence
    renderer.raw(ack);
    QTest::qWait(20);
    QCOMPARE(bridge.foregroundRequestId(), current);
    renderer.ack(current, QStringLiteral("rejected"));
    QVERIFY(spin([&] { return bridge.foregroundRequestId().isEmpty(); }));
}

void ToonEventBridgeTest::deletionPreemptsWorkReaction() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer renderer;
    QVERIFY(renderer.open(bridge));
    bridge.turnStarted(QStringLiteral("s"), QStringLiteral("t"));
    bridge.turnStopped(QStringLiteral("s"), QStringLiteral("t"));
    const auto work = renderer.wait(QStringLiteral("action.request"));
    QCOMPARE(work.value(QStringLiteral("name")).toString(), QStringLiteral("work.finished"));
    bridge.desktopItemDeleted();
    const auto deletion = renderer.wait(QStringLiteral("action.request"));
    const auto cancel = renderer.wait(QStringLiteral("action.cancel"));
    QCOMPARE(cancel.value(QStringLiteral("request_id")), work.value(QStringLiteral("request_id")));
    renderer.ack(work.value(QStringLiteral("request_id")).toString(), QStringLiteral("interrupted"));
    QTest::qWait(20);
    QCOMPARE(bridge.foregroundRequestId(), deletion.value(QStringLiteral("request_id")).toString());
    bridge.turnStarted(QStringLiteral("next"), QStringLiteral("turn"));
    renderer.ack(deletion.value(QStringLiteral("request_id")).toString());
    QVERIFY(spin([&] { return bridge.foregroundRequestId().isEmpty(); }));
    QCOMPARE(controller.state(), PetController::State::Busy);
}

void ToonEventBridgeTest::boundedFallback() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer renderer;
    QVERIFY(renderer.open(bridge));
    bridge.turnStarted(QStringLiteral("s"), QStringLiteral("t"));
    bridge.desktopItemDeleted();
    const auto deletion = renderer.wait(QStringLiteral("action.request"));
    QVERIFY(!deletion.isEmpty());
    QVERIFY(spin([&] { return bridge.foregroundRequestId().isEmpty(); }, 14000));
    const auto cancel = renderer.wait(QStringLiteral("action.cancel"));
    QCOMPARE(cancel.value(QStringLiteral("reason")).toString(), QStringLiteral("timeout"));
    QCOMPARE(controller.state(), PetController::State::Busy);
}

void ToonEventBridgeTest::newActivityCancelsOldCompletion() {
    PetController controller;
    ToonEventBridge bridge(&controller);
    QVERIFY(bridge.start());
    Renderer renderer;
    QVERIFY(renderer.open(bridge));
    bridge.turnStarted(QStringLiteral("old"), QStringLiteral("turn"));
    bridge.turnStopped(QStringLiteral("old"), QStringLiteral("turn"));
    const auto finished = renderer.wait(QStringLiteral("action.request"));
    QCOMPARE(finished.value(QStringLiteral("name")).toString(), QStringLiteral("work.finished"));
    bridge.turnStarted(QStringLiteral("new"), QStringLiteral("turn"));
    const auto cancel = renderer.wait(QStringLiteral("action.cancel"));
    QCOMPARE(cancel.value(QStringLiteral("request_id")), finished.value(QStringLiteral("request_id")));
    QCOMPARE(cancel.value(QStringLiteral("reason")).toString(), QStringLiteral("activity_changed"));
    QCOMPARE(bridge.foregroundRequestId(), QString());
    renderer.ack(finished.value(QStringLiteral("request_id")).toString());
    const auto snapshot = renderer.latestSnapshot(1);
    QVERIFY(snapshot.value(QStringLiteral("busy")).toBool());
    QCOMPARE(controller.state(), PetController::State::Busy);
}

QTEST_GUILESS_MAIN(ToonEventBridgeTest)
#include "ToonEventBridgeTest.moc"
