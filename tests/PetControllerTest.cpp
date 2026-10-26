#include "../src/PetController.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <limits>

class PetControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void concurrentTurns();
    void stopWithoutTurnIdEndsLatestSessionTurn();
    void deleteReturnsToCurrentBackground();
    void sessionEndCleansOnlyItsTurns();
    void eatTriggeredCarriesSourcePayload();
    void allTurnsStoppedFollowsBackgroundState();
    void unknownAndDuplicateStopsAreSilent();
    void turnEndedCarriesWorkedMs();
    void cleanupAndResetAreSilent();
    void lastStopDuringForegroundAction();
    void interactiveGrassTracksBackgroundTurns();
    void interactiveGrassReplacesOneShotDeadline();
    void interactiveGrassRestartReplacesDeadline();
    void deleteInterruptsInteractiveGrass();
    void interactiveGrassBoundsBadDurations();
    void manualClockDisablesWallClockFallback();
    void reenabledFallbackOnlyStartsWithFutureAction();
    void permissionSnapshotsReplaceWithoutChangingWork();
    void permissionCleanupFollowsExactRemovedTurns();
    void malformedAndOrphanPermissionSnapshotsAreIgnored();
    void permissionSnapshotsDoNotRenewExpiry();
    void foregroundActionsKeepPermissionFacts();
};

void PetControllerTest::permissionSnapshotsReplaceWithoutChangingWork() {
    qint64 now = 1000;
    PetController controller(nullptr, [&] { return now; });
    QSignalSpy changed(&controller, &PetController::permissionStateChanged);
    QSignalSpy states(&controller, &PetController::stateChanged);
    QSignalSpy ended(&controller, &PetController::turnEnded);
    QSignalSpy stopped(&controller, &PetController::allTurnsStopped);
    controller.turnStarted("session", "turn");
    QCOMPARE(states.size(), 1);
    now = 21000;
    controller.applyPermissionSnapshot("session", "turn", {"first", "second"});
    QCOMPARE(controller.pendingPermissionCount(), 2);
    QCOMPARE(controller.permissionTurnCount(), 1);
    QCOMPARE(controller.activeTurnCount(), 1);
    QCOMPARE(controller.state(), PetController::State::Busy);
    QCOMPARE(changed.size(), 1);
    controller.applyPermissionSnapshot("session", "turn", {"second", "first"});
    QCOMPARE(changed.size(), 1); // Set order and redelivery do not make a new fact.
    controller.applyPermissionSnapshot("session", "turn", {"second", "third"});
    QCOMPARE(controller.pendingPermissionCount(), 2);
    QCOMPARE(changed.size(), 2);
    controller.applyPermissionSnapshot("session", "turn", {});
    QCOMPARE(controller.pendingPermissionCount(), 0);
    QCOMPARE(controller.permissionTurnCount(), 0);
    QCOMPARE(changed.size(), 3);
    controller.applyPermissionSnapshot("session", "turn", {});
    QCOMPARE(changed.size(), 3);
    QCOMPARE(states.size(), 1);
    QCOMPARE(ended.size(), 0);
    QCOMPARE(stopped.size(), 0);
    now = 41000;
    controller.turnStopped("session", "turn");
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended.first().at(1).toLongLong(), qint64(40000));
}

void PetControllerTest::permissionCleanupFollowsExactRemovedTurns() {
    qint64 now = 100;
    PetController controller(nullptr, [&] { return now; });
    QSignalSpy changed(&controller, &PetController::permissionStateChanged);
    QSignalSpy ended(&controller, &PetController::turnEnded);
    QSignalSpy stopped(&controller, &PetController::allTurnsStopped);
    controller.turnStarted("opencode-same", "first");
    controller.applyPermissionSnapshot("opencode-same", "first", {"shared-id", "other"});
    now = 200;
    controller.turnStarted("opencode-same", "latest");
    controller.applyPermissionSnapshot("opencode-same", "latest", {"shared-id"});
    now = 300;
    controller.turnStarted("other-source-same", "latest");
    controller.applyPermissionSnapshot("other-source-same", "latest", {"shared-id"});
    QCOMPARE(controller.pendingPermissionCount(), 4);
    QCOMPARE(controller.permissionTurnCount(), 3);
    controller.turnStopped("opencode-same", QString());
    QCOMPARE(controller.pendingPermissionCount(), 3);
    QCOMPARE(controller.permissionTurnCount(), 2);
    QCOMPARE(controller.activeTurnCount(), 2);
    QCOMPARE(changed.size(), 4);
    controller.turnStopped("opencode-same", "latest");
    QCOMPARE(changed.size(), 4);
    QCOMPARE(ended.size(), 1);
    controller.sessionEnded("opencode-same");
    QCOMPARE(controller.pendingPermissionCount(), 1);
    QCOMPARE(controller.permissionTurnCount(), 1);
    QCOMPARE(controller.activeTurnCount(), 1);
    QCOMPARE(changed.size(), 5);
    QCOMPARE(ended.size(), 1); // Cleanup is bookkeeping, not a successful stop.
    QCOMPARE(stopped.size(), 0);
    controller.resetBusy();
    QCOMPARE(controller.pendingPermissionCount(), 0);
    QCOMPARE(controller.permissionTurnCount(), 0);
    QCOMPARE(controller.activeTurnCount(), 0);
    QCOMPARE(changed.size(), 6);
    QCOMPARE(ended.size(), 1);
    QCOMPARE(stopped.size(), 0);
    controller.resetBusy();
    QCOMPARE(changed.size(), 6);
}

void PetControllerTest::malformedAndOrphanPermissionSnapshotsAreIgnored() {
    PetController controller;
    QSignalSpy changed(&controller, &PetController::permissionStateChanged);
    controller.applyPermissionSnapshot("session", "turn", {"orphan"});
    QCOMPARE(controller.activeTurnCount(), 0);
    QCOMPARE(controller.state(), PetController::State::Idle);
    QCOMPARE(changed.size(), 0);
    controller.turnStarted("session", "turn");
    controller.applyPermissionSnapshot("session", "turn", {"kept"});
    controller.applyPermissionSnapshot("session", "other-turn", {});
    controller.applyPermissionSnapshot("other-session", "turn", {});
    QStringList tooMany;
    for (int i = 0; i < 33; ++i) tooMany.append(QString::number(i));
    for (const QStringList& requests : {QStringList{"valid", " "},
            QStringList{"valid", QString(257, QLatin1Char('x'))},
            QStringList{"valid", QString(1, QChar::Null)},
            QStringList{"valid", "valid"}, tooMany})
        controller.applyPermissionSnapshot("session", "turn", requests);
    QCOMPARE(controller.pendingPermissionCount(), 1);
    QCOMPARE(changed.size(), 1);
    controller.turnStopped("session", "turn");
    QCOMPARE(controller.pendingPermissionCount(), 0);
    QCOMPARE(changed.size(), 2);
    controller.applyPermissionSnapshot("session", "turn", {"late-request"});
    QCOMPARE(controller.pendingPermissionCount(), 0);
    QCOMPARE(changed.size(), 2);
}

void PetControllerTest::permissionSnapshotsDoNotRenewExpiry() {
    constexpr qint64 hour = 60 * 60 * 1000;
    qint64 now = 1000;
    const qint64 oldestStart = now;
    PetController controller(nullptr, [&] { return now; });
    QSignalSpy changed(&controller, &PetController::permissionStateChanged);
    QSignalSpy stopped(&controller, &PetController::allTurnsStopped);
    QSignalSpy ended(&controller, &PetController::turnEnded);
    controller.turnStarted("old", "turn");
    now += 2 * hour;
    const qint64 newerStart = now;
    controller.turnStarted("new", "turn");
    now = oldestStart + 4 * hour + 1;
    controller.applyPermissionSnapshot("old", "turn", {"recent-request"});
    controller.applyPermissionSnapshot("new", "turn", {"newer-request"});
    QVERIFY(QMetaObject::invokeMethod(&controller, "expireOldTurns", Qt::DirectConnection));
    QCOMPARE(controller.activeTurnCount(), 1);
    QCOMPARE(controller.pendingPermissionCount(), 1);
    QCOMPARE(controller.permissionTurnCount(), 1);
    QCOMPARE(controller.state(), PetController::State::Busy);
    QCOMPARE(changed.size(), 3);
    now = newerStart + 4 * hour + 1;
    QVERIFY(QMetaObject::invokeMethod(&controller, "expireOldTurns", Qt::DirectConnection));
    QCOMPARE(controller.activeTurnCount(), 0);
    QCOMPARE(controller.pendingPermissionCount(), 0);
    QCOMPARE(controller.permissionTurnCount(), 0);
    QCOMPARE(controller.state(), PetController::State::Idle);
    QCOMPARE(changed.size(), 4);
    QCOMPARE(stopped.size(), 0);
    QCOMPARE(ended.size(), 0);
}

void PetControllerTest::foregroundActionsKeepPermissionFacts() {
    PetController controller;
    controller.setActionFallbackEnabled(false);
    QSignalSpy stopped(&controller, &PetController::allTurnsStopped);
    controller.turnStarted("session", "turn");
    controller.playGrass();
    controller.applyPermissionSnapshot("session", "turn", {"request"});
    QCOMPARE(controller.state(), PetController::State::Grass);
    QCOMPARE(controller.pendingPermissionCount(), 1);
    QCOMPARE(controller.activeTurnCount(), 1);
    controller.desktopItemDeleted();
    QCOMPARE(controller.state(), PetController::State::Delete);
    controller.applyPermissionSnapshot("session", "turn", {});
    QCOMPARE(controller.state(), PetController::State::Delete);
    QCOMPARE(controller.activeTurnCount(), 1);
    QCOMPARE(stopped.size(), 0);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Busy);
}

void PetControllerTest::concurrentTurns() {
    PetController controller;
    QSignalSpy completed(&controller, &PetController::allTurnsStopped);
    controller.turnStarted("a", "1");
    controller.turnStarted("b", "2");
    QCOMPARE(controller.state(), PetController::State::Busy);
    controller.turnStopped("a", "1");
    QCOMPARE(controller.state(), PetController::State::Busy);
    QCOMPARE(completed.size(), 0);
    controller.turnStopped("b", "2");
    QCOMPARE(controller.state(), PetController::State::Idle);
    QCOMPARE(completed.size(), 1);
}

void PetControllerTest::stopWithoutTurnIdEndsLatestSessionTurn() {
    PetController controller;
    QSignalSpy completed(&controller, &PetController::allTurnsStopped);
    controller.turnStarted("s", "1");
    QTest::qWait(5);
    controller.turnStarted("s", "2");
    QTest::qWait(5);
    controller.turnStarted("o", "3");
    QCOMPARE(controller.state(), PetController::State::Busy);
    // WorkBuddy Stop hooks carry session_id only, no turn_id.
    controller.turnStopped("s", QString());
    QCOMPARE(controller.state(), PetController::State::Busy);
    QCOMPARE(completed.size(), 0);
    // The session's latest turn ("2") was removed: an explicit stop for it is
    // now a silent duplicate, while "1" still holds the state busy.
    controller.turnStopped("s", "2");
    QCOMPARE(completed.size(), 0);
    controller.turnStopped("s", "1");
    QCOMPARE(controller.state(), PetController::State::Busy);
    QCOMPARE(completed.size(), 0);
    controller.turnStopped("o", "3");
    QCOMPARE(controller.state(), PetController::State::Idle);
    QCOMPARE(completed.size(), 1);
}

void PetControllerTest::allTurnsStoppedFollowsBackgroundState() {
    PetController controller;
    QStringList order;
    connect(&controller, &PetController::stateChanged, this,
            [&order](PetController::State state) {
                if (state == PetController::State::Idle) order.append("idle");
            });
    connect(&controller, &PetController::allTurnsStopped, this, [&] {
        order.append("stopped");
        QCOMPARE(controller.activeTurnCount(), 0);
        QCOMPARE(controller.state(), PetController::State::Idle);
    });
    controller.turnStarted("session", "turn");
    // Repeated starts refresh a turn rather than making duplicate work.
    controller.turnStarted("session", "turn");
    QCOMPARE(controller.activeTurnCount(), 1);
    controller.turnStopped("session", "turn");
    QCOMPARE(order, QStringList({"idle", "stopped"}));
}

void PetControllerTest::unknownAndDuplicateStopsAreSilent() {
    PetController controller;
    QSignalSpy completed(&controller, &PetController::allTurnsStopped);
    controller.turnStopped("missing", "turn");
    controller.turnStopped(QString(), QString());
    QCOMPARE(completed.size(), 0);
    controller.turnStarted("a", "1");
    controller.turnStopped("b", "1");
    controller.turnStopped("a", "missing");
    QCOMPARE(controller.activeTurnCount(), 1);
    QCOMPARE(completed.size(), 0);
    controller.turnStopped("a", "1");
    QCOMPARE(completed.size(), 1);
    controller.turnStopped("a", "1");
    QCOMPARE(completed.size(), 1);
}

void PetControllerTest::turnEndedCarriesWorkedMs() {
    PetController controller;
    QSignalSpy ended(&controller, &PetController::turnEnded);
    // A real stop reports how long the turn worked; unknown stops stay silent.
    controller.turnStarted("s", "1");
    QTest::qWait(40);
    controller.turnStopped("s", "1");
    QCOMPARE(ended.size(), 1);
    QVERIFY(ended.first().at(1).toLongLong() >= 35);
    // Duplicate and unknown stops do not report a second time.
    controller.turnStopped("s", "1");
    controller.turnStopped("s", QString());
    QCOMPARE(ended.size(), 1);
    // The session_id-only stop path (WorkBuddy Stop hooks) reports too.
    controller.turnStarted("s", "2");
    QTest::qWait(10);
    controller.turnStopped("s", QString());
    QCOMPARE(ended.size(), 2);
    QVERIFY(ended.last().at(1).toLongLong() >= 5);
    QCOMPARE(ended.last().at(0).toString(), QStringLiteral("s"));
}

void PetControllerTest::cleanupAndResetAreSilent() {
    PetController controller;
    QSignalSpy completed(&controller, &PetController::allTurnsStopped);
    controller.turnStarted("a", "1");
    controller.sessionEnded("a");
    QCOMPARE(controller.state(), PetController::State::Idle);
    QCOMPARE(completed.size(), 0);
    controller.turnStarted("b", "2");
    controller.resetBusy();
    QCOMPARE(controller.state(), PetController::State::Idle);
    QCOMPARE(completed.size(), 0);
    controller.turnStopped("b", "2");
    QCOMPARE(completed.size(), 0);
}

void PetControllerTest::lastStopDuringForegroundAction() {
    for (const auto action : {PetController::State::Delete, PetController::State::Grass}) {
        PetController controller;
        QSignalSpy completed(&controller, &PetController::allTurnsStopped);
        controller.turnStarted("a", "1");
        if (action == PetController::State::Delete) controller.desktopItemDeleted();
        else controller.playGrass();
        controller.turnStopped("a", "1");
        QCOMPARE(completed.size(), 1);
        QCOMPARE(controller.state(), action);
        controller.actionFinished();
        QCOMPARE(controller.state(), PetController::State::Idle);
        QCOMPARE(completed.size(), 1);
    }
}

void PetControllerTest::interactiveGrassTracksBackgroundTurns() {
    PetController controller;
    controller.playInteractiveGrass(15.0);
    controller.turnStarted("a", "1");
    controller.turnStarted("b", "2");
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.turnStopped("a", "1");
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Busy);

    controller.playInteractiveGrass(15.0);
    controller.turnStopped("b", "2");
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Idle);
}

void PetControllerTest::interactiveGrassReplacesOneShotDeadline() {
    PetController controller;
    controller.setActionDuration(PetController::State::Grass, 0.04);
    controller.playGrass();
    QTest::qWait(10);
    controller.playInteractiveGrass(0.3);
    QTest::qWait(70);
    // The older one-shot deadline has passed; holding is still foreground.
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.turnStarted("a", "1");
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Busy, 500);

    // Explicit completion removes its fallback rather than leaving a future
    // background restore that could interrupt the next foreground action.
    controller.playInteractiveGrass(0.25);
    controller.actionFinished();
    controller.setActionDuration(PetController::State::Grass, 0.6);
    controller.playGrass();
    QTest::qWait(300);
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Busy);
}

void PetControllerTest::interactiveGrassRestartReplacesDeadline() {
    PetController controller;
    QSignalSpy states(&controller, &PetController::stateChanged);
    controller.playInteractiveGrass(0.25);
    QTest::qWait(160);
    controller.playInteractiveGrass(0.35);
    QCOMPARE(states.size(), 2); // Retrigger informs the player even in Grass.
    QTest::qWait(130);
    QCOMPARE(controller.state(), PetController::State::Grass);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Idle, 500);
}

void PetControllerTest::deleteInterruptsInteractiveGrass() {
    PetController controller;
    controller.setActionDuration(PetController::State::Delete, 0.5);
    controller.playInteractiveGrass(0.25);
    QTest::qWait(20);
    controller.desktopItemDeleted();
    QCOMPARE(controller.state(), PetController::State::Delete);
    QSignalSpy states(&controller, &PetController::stateChanged);
    controller.playInteractiveGrass(0.25);
    QCOMPARE(states.size(), 0);
    controller.turnStarted("a", "1");
    QTest::qWait(300);
    // Grass's former deadline cannot cut the delete action short.
    QCOMPARE(controller.state(), PetController::State::Delete);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Busy, 500);
}

void PetControllerTest::interactiveGrassBoundsBadDurations() {
    PetController controller;
    controller.playInteractiveGrass(0.001);
    QTest::qWait(70);
    QCOMPARE(controller.state(), PetController::State::Grass);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Idle, 600);
    for (double duration : {0.0, -2.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(), 1.0e100}) {
        controller.playInteractiveGrass(duration);
        QTest::qWait(20);
        QCOMPARE(controller.state(), PetController::State::Grass);
        controller.actionFinished();
        QCOMPARE(controller.state(), PetController::State::Idle);
    }
}

void PetControllerTest::manualClockDisablesWallClockFallback() {
    PetController controller;
    controller.turnStarted("a", "1");
    controller.playInteractiveGrass(0.25);
    controller.setActionFallbackEnabled(false);
    QTest::qWait(300);
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Busy);

    controller.setActionDuration(PetController::State::Grass, 0.04);
    controller.playGrass();
    QTest::qWait(80);
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Busy);

    controller.setActionDuration(PetController::State::Delete, 0.04);
    controller.desktopItemDeleted();
    QTest::qWait(80);
    QCOMPARE(controller.state(), PetController::State::Delete);
    controller.turnStopped("a", "1");
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Idle);
}

void PetControllerTest::reenabledFallbackOnlyStartsWithFutureAction() {
    PetController controller;
    controller.setActionFallbackEnabled(false);
    controller.setActionDuration(PetController::State::Grass, 0.04);
    controller.playGrass();
    controller.setActionFallbackEnabled(true);
    QTest::qWait(80);
    QCOMPARE(controller.state(), PetController::State::Grass);
    controller.actionFinished();
    QCOMPARE(controller.state(), PetController::State::Idle);

    controller.playInteractiveGrass(0.25);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Idle, 600);
    controller.playGrass();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Idle, 300);
}

void PetControllerTest::deleteReturnsToCurrentBackground() {
    PetController controller;
    controller.turnStarted("a", "1");
    controller.desktopItemDeleted();
    QCOMPARE(controller.state(), PetController::State::Delete);
    // The fallback timer runs for the motion library's delete length (2.6 s
    // since delete v5), so the wait has to cover the whole action plus slack.
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Busy, 3500);
}

void PetControllerTest::sessionEndCleansOnlyItsTurns() {
    PetController controller;
    controller.turnStarted("a", "1");
    controller.turnStarted("b", "2");
    controller.sessionEnded("a");
    QCOMPARE(controller.activeTurnCount(), 1);
    controller.sessionEnded("b");
    QCOMPARE(controller.state(), PetController::State::Idle);
}

void PetControllerTest::eatTriggeredCarriesSourcePayload() {
    // A desktop deletion forwards what the snapshot knew: path and position.
    PetController controller;
    QSignalSpy deleted(&controller, &PetController::eatTriggered);
    controller.desktopItemDeleted(QStringLiteral("C:/Users/u/Desktop/a.txt"),
                                  QPointF(100, 200));
    QCOMPARE(controller.state(), PetController::State::Delete);
    QCOMPARE(deleted.size(), 1);
    QCOMPARE(deleted.first().at(0).toString(), QStringLiteral("C:/Users/u/Desktop/a.txt"));
    QCOMPARE(deleted.first().at(1).toPointF(), QPointF(100, 200));

    // A drop carries the file but no source position (it happens on the pet).
    PetController drop;
    QSignalSpy dropped(&drop, &PetController::eatTriggered);
    drop.fileDropped({QStringLiteral("C:/tmp/b.txt")});
    QCOMPARE(dropped.size(), 1);
    QCOMPARE(dropped.first().at(0).toString(), QStringLiteral("C:/tmp/b.txt"));
    QCOMPARE(dropped.first().at(1).toPointF(), QPointF());

    // The bare trigger (tray menu, headless tests) still works and fires with
    // an empty payload.
    PetController bare;
    QSignalSpy manual(&bare, &PetController::eatTriggered);
    bare.desktopItemDeleted();
    QCOMPARE(bare.state(), PetController::State::Delete);
    QCOMPARE(manual.size(), 1);
    QVERIFY(manual.first().at(0).toString().isEmpty());
    QCOMPARE(manual.first().at(1).toPointF(), QPointF());
}

// QTEST_MAIN, not the GUI-less variant: eatTriggered carries a QIcon, and
// icon handling wants a QApplication instance alive.
QTEST_MAIN(PetControllerTest)
#include "PetControllerTest.moc"
