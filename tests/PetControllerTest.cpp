#include "../src/PetController.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <limits>

class PetControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void concurrentTurns();
    void deleteReturnsToCurrentBackground();
    void sessionEndCleansOnlyItsTurns();
    void eatTriggeredCarriesSourcePayload();
    void allTurnsStoppedFollowsBackgroundState();
    void unknownAndDuplicateStopsAreSilent();
    void cleanupAndResetAreSilent();
    void lastStopDuringForegroundAction();
    void interactiveGrassTracksBackgroundTurns();
    void interactiveGrassReplacesOneShotDeadline();
    void interactiveGrassRestartReplacesDeadline();
    void deleteInterruptsInteractiveGrass();
    void interactiveGrassBoundsBadDurations();
    void manualClockDisablesWallClockFallback();
    void reenabledFallbackOnlyStartsWithFutureAction();
};

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
