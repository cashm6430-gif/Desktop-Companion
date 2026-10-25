#include "../src/CodexTurnSource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>
#include <QUuid>
#include <memory>
#include <vector>

class CodexTurnSourceTest final : public QObject {
    Q_OBJECT
private:
    static QString endpoint() {
        return QStringLiteral("pet-ipc-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    static QByteArray json(const QJsonObject& object) {
        return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    }
    static QJsonObject event(const QString& action, const QString& client = QStringLiteral("codex")) {
        return {{QStringLiteral("event"), action}, {QStringLiteral("session_id"), QStringLiteral("private-session")},
                {QStringLiteral("turn_id"), QStringLiteral("private-turn")}, {QStringLiteral("client"), client}};
    }
    static bool send(QLocalSocket& socket, const QString& name, const QByteArray& bytes) {
        socket.connectToServer(name);
        if (!socket.waitForConnected(1000)) return false;
        const bool written = socket.write(bytes) == bytes.size();
        socket.flush();
        return written;
    }

private slots:
    void acceptedEventsCarryOnlyWhitelistedMetadataAfterController() {
        CodexTurnSource source;
        const QString name = endpoint();
        QVERIFY(source.start(name));
        QVERIFY(source.isListening());
        QVERIFY(source.error().isEmpty());
        QSignalSpy metadata(&source, &CodexTurnSource::eventReceived);
        QStringList order;
        QString expectedClient;
        const auto expectedSession = [&] {
            return QString::fromUtf8(QJsonDocument(QJsonArray{expectedClient, QStringLiteral("private-session")})
                                         .toJson(QJsonDocument::Compact));
        };
        connect(&source, &CodexTurnSource::turnStarted, this, [&](const QString& session, const QString& turn) {
            QCOMPARE(session, expectedSession());
            QCOMPARE(turn, QStringLiteral("private-turn"));
            order.append(QStringLiteral("start"));
        });
        connect(&source, &CodexTurnSource::turnStopped, this, [&](const QString& session, const QString& turn) {
            QCOMPARE(session, expectedSession());
            QVERIFY(turn.isEmpty()); // Legacy Stop has no turn id.
            order.append(QStringLiteral("stop"));
        });
        connect(&source, &CodexTurnSource::sessionEnded, this, [&](const QString& session) {
            QCOMPARE(session, expectedSession());
            order.append(QStringLiteral("session_end"));
        });
        connect(&source, &CodexTurnSource::eventReceived, this, [&](const QString&, const QString& action) {
            QCOMPARE(order.last(), action);
            order.append(QStringLiteral("metadata"));
        });

        const QStringList clients{QStringLiteral("codex"), QStringLiteral("workbuddy"), QStringLiteral("claude"),
                                  QStringLiteral("opencode"), QStringLiteral("manual"),
                                  QStringLiteral("unknown"), QStringLiteral("other")};
        for (qsizetype index = 0; index < clients.size(); ++index) {
            expectedClient = index < 5 ? clients[index] : QStringLiteral("unknown");
            QLocalSocket socket;
            QJsonObject payload = event(QStringLiteral("start"), clients[index]);
            payload.insert(QStringLiteral("prompt"), QStringLiteral("private prompt must not become metadata"));
            QVERIFY(send(socket, name, json(payload)));
            QTRY_COMPARE(metadata.count(), index + 1);
            QCOMPARE(metadata.last().at(0).toString(), expectedClient);
            QCOMPARE(metadata.last().at(1).toString(), QStringLiteral("start"));
            QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
        }
        QJsonObject stop = event(QStringLiteral("stop"));
        expectedClient = QStringLiteral("unknown");
        stop.remove(QStringLiteral("client"));
        stop.remove(QStringLiteral("turn_id"));
        QLocalSocket legacy;
        QVERIFY(send(legacy, name, json(stop)));
        QTRY_COMPARE(metadata.count(), clients.size() + 1);
        QCOMPARE(metadata.last().at(0).toString(), QStringLiteral("unknown"));
        QJsonObject ended = event(QStringLiteral("session_end"));
        ended.insert(QStringLiteral("client"), QJsonArray{QStringLiteral("codex")});
        QLocalSocket endSocket;
        QVERIFY(send(endSocket, name, json(ended)));
        QTRY_COMPARE(metadata.count(), clients.size() + 2);
        QCOMPARE(metadata.last().at(0).toString(), QStringLiteral("unknown"));
        QCOMPARE(metadata.last().at(1).toString(), QStringLiteral("session_end"));
        QCOMPARE(order.size(), (clients.size() + 2) * 2);
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);
    }

    void statusReturnsSnapshotWithoutBecomingRecentEvent() {
        CodexTurnSource source;
        const QString name = endpoint();
        source.setStatusProvider([] { return QJsonObject{{QStringLiteral("active_turns"), 2}}; });
        QVERIFY(source.start(name));
        QSignalSpy metadata(&source, &CodexTurnSource::eventReceived);
        QSignalSpy started(&source, &CodexTurnSource::turnStarted);
        QLocalSocket socket;
        QVERIFY(send(socket, name, json({{QStringLiteral("event"), QStringLiteral("status")},
                                        {QStringLiteral("client"), QStringLiteral("codex")}})));
        QTRY_VERIFY(socket.bytesAvailable() > 0);
        QCOMPARE(QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("active_turns")).toInt(), 2);
        QCOMPARE(metadata.count(), 0);
        QCOMPARE(started.count(), 0);
        QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);
    }

    void invalidOrOversizedRequestsAreDiscarded() {
        CodexTurnSource source;
        const QString name = endpoint();
        QVERIFY(source.start(name));
        QSignalSpy metadata(&source, &CodexTurnSource::eventReceived);
        QSignalSpy started(&source, &CodexTurnSource::turnStarted);
        QSignalSpy stopped(&source, &CodexTurnSource::turnStopped);
        QSignalSpy ended(&source, &CodexTurnSource::sessionEnded);
        QList<QByteArray> invalid{"not-json\n", "[]\n", "{\n", "{} {}\n", "null\n", "{}\n"};
        for (const QString& action : {QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("session_end")}) {
            QJsonObject payload = event(action);
            payload.remove(QStringLiteral("session_id"));
            invalid.append(json(payload));
            payload.insert(QStringLiteral("session_id"), QStringLiteral("  "));
            invalid.append(json(payload));
            payload.insert(QStringLiteral("session_id"), 42);
            invalid.append(json(payload));
        }
        QJsonObject noTurn = event(QStringLiteral("start"));
        noTurn.remove(QStringLiteral("turn_id"));
        invalid.append(json(noTurn));
        noTurn.insert(QStringLiteral("turn_id"), QStringLiteral(" "));
        invalid.append(json(noTurn));
        noTurn.insert(QStringLiteral("turn_id"), 42);
        invalid.append(json(noTurn));
        invalid.append(json(event(QStringLiteral("not-an-event"))));
        invalid.append(QByteArray(4097, 'x') + '\n');
        QJsonObject oversized = event(QStringLiteral("start"));
        oversized.insert(QStringLiteral("padding"), QString(4096, QLatin1Char('x')));
        invalid.append(json(oversized));
        for (const QByteArray& bytes : invalid) {
            QLocalSocket socket;
            QVERIFY(send(socket, name, bytes));
            QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
        }
        QCOMPARE(metadata.count(), 0);
        QCOMPARE(started.count(), 0);
        QCOMPARE(stopped.count(), 0);
        QCOMPARE(ended.count(), 0);
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);
    }

    void fragmentedAndConcurrentRequestsRemainIndependent() {
        CodexTurnSource source;
        const QString name = endpoint();
        QVERIFY(source.start(name));
        QSignalSpy metadata(&source, &CodexTurnSource::eventReceived);
        QLocalSocket fragmented;
        const QByteArray bytes = json(event(QStringLiteral("start")));
        QVERIFY(send(fragmented, name, bytes.left(20)));
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 1);
        // A second accepted connection proves the first fragment was handled
        // without needing a timing-based assertion about the server loop.
        QLocalSocket independent;
        QVERIFY(send(independent, name, json(event(QStringLiteral("stop"), QStringLiteral("claude")))));
        QTRY_COMPARE(metadata.count(), 1);
        QCOMPARE(metadata.first().at(1).toString(), QStringLiteral("stop"));
        QCOMPARE(fragmented.write(bytes.mid(20)), bytes.size() - 20);
        fragmented.flush();
        QTRY_COMPARE(metadata.count(), 2);
        QCOMPARE(metadata.last().at(1).toString(), QStringLiteral("start"));

        std::vector<std::unique_ptr<QLocalSocket>> sockets;
        for (int index = 0; index < 12; ++index) {
            auto socket = std::make_unique<QLocalSocket>();
            QVERIFY(send(*socket, name, json(event(index % 2 ? QStringLiteral("stop") : QStringLiteral("start"),
                                                 index % 2 ? QStringLiteral("workbuddy") : QStringLiteral("codex")))));
            sockets.push_back(std::move(socket));
        }
        QTRY_COMPARE(metadata.count(), 14);
        int codex = 0;
        int workbuddy = 0;
        for (int index = 2; index < metadata.size(); ++index) {
            const QString client = metadata.at(index).at(0).toString();
            if (client == QStringLiteral("codex")) ++codex;
            if (client == QStringLiteral("workbuddy")) ++workbuddy;
        }
        QCOMPARE(codex, 6);
        QCOMPARE(workbuddy, 6);
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);

        // A peer which leaves before completing a line must also be released.
        QLocalSocket incomplete;
        QVERIFY(send(incomplete, name, QByteArray("{\"event\":")));
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 1);
        incomplete.disconnectFromServer();
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);
        QCOMPARE(metadata.count(), 14);
    }

    void sizeLimitIncludesAccumulatedFragments() {
        CodexTurnSource source;
        const QString name = endpoint();
        QVERIFY(source.start(name));
        QSignalSpy metadata(&source, &CodexTurnSource::eventReceived);
        QJsonObject boundary = event(QStringLiteral("start"));
        boundary.insert(QStringLiteral("padding"), QString());
        boundary.insert(QStringLiteral("padding"), QString(4096 - json(boundary).size(), QLatin1Char('x')));
        const QByteArray boundaryBytes = json(boundary);
        QCOMPARE(boundaryBytes.size(), 4096);
        QLocalSocket accepted;
        QVERIFY(send(accepted, name, boundaryBytes));
        QTRY_COMPARE(metadata.count(), 1);
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);

        boundary.insert(QStringLiteral("padding"), boundary.value(QStringLiteral("padding")).toString() + QLatin1Char('x'));
        const QByteArray tooLarge = json(boundary);
        QCOMPARE(tooLarge.size(), 4097);
        QLocalSocket fragmented;
        QVERIFY(send(fragmented, name, tooLarge.left(4000)));
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 1);
        QCOMPARE(metadata.count(), 1);
        QCOMPARE(fragmented.write(tooLarge.mid(4000)), 97);
        fragmented.flush();
        QTRY_COMPARE(fragmented.state(), QLocalSocket::UnconnectedState);
        QCOMPARE(metadata.count(), 1);
        QTRY_COMPARE(source.findChildren<QLocalSocket*>().size(), 0);
    }

    void listenerFailureIsObservableAndCanRecover() {
        CodexTurnSource source;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Codex IPC unavailable:.*")));
        QVERIFY(!source.start(QString(1024, QLatin1Char('x'))));
        QVERIFY(!source.isListening());
        QVERIFY(!source.error().isEmpty());
        QVERIFY(source.start(endpoint()));
        QVERIFY(source.isListening());
        QVERIFY(source.error().isEmpty());
    }
};

QTEST_GUILESS_MAIN(CodexTurnSourceTest)
#include "CodexTurnSourceTest.moc"
