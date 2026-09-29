#include "../src/PetController.h"

#include <QtTest/QTest>

class PetControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void concurrentTurns();
    void deleteReturnsToCurrentBackground();
    void sessionEndCleansOnlyItsTurns();
};

void PetControllerTest::concurrentTurns() {
    PetController controller;
    controller.turnStarted("a", "1");
    controller.turnStarted("b", "2");
    QCOMPARE(controller.state(), PetController::State::Busy);
    controller.turnStopped("a", "1");
    QCOMPARE(controller.state(), PetController::State::Busy);
    controller.turnStopped("b", "2");
    QCOMPARE(controller.state(), PetController::State::Idle);
}

void PetControllerTest::deleteReturnsToCurrentBackground() {
    PetController controller;
    controller.turnStarted("a", "1");
    controller.desktopItemDeleted();
    QCOMPARE(controller.state(), PetController::State::Delete);
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), PetController::State::Busy, 1800);
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

QTEST_GUILESS_MAIN(PetControllerTest)
#include "PetControllerTest.moc"
