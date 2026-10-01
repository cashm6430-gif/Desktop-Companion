#include "../src/PetController.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

class PetControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void concurrentTurns();
    void deleteReturnsToCurrentBackground();
    void sessionEndCleansOnlyItsTurns();
    void eatTriggeredCarriesSourcePayload();
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
