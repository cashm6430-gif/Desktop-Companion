#include "../src/PetPointerGesture.h"

#include <QtTest/QTest>

class PetPointerGestureTest final : public QObject {
    Q_OBJECT
private slots:
    void deliberateHeadStrokeNeedsReturnAndTime();
    void verticalStrokeWorks();
    void quickHeadDrag();
    void bodyDragThreshold();
    void clickLongPressAndJitterStayPending();
    void patDoesNotBecomeDrag();
    void releaseAndCancelReset();
    void rejectedPatStillAllowsDragging();
};

void PetPointerGestureTest::rejectedPatStillAllowsDragging() {
    PetPointerGesture gesture;
    gesture.press(QPointF(100, 100), true);
    gesture.move(QPointF(106, 100));
    gesture.move(QPointF(102, 100));
    gesture.advance(0.2);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pat);
    gesture.rejectPat();
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    QVERIFY(!gesture.pressedOnHead());
    gesture.move(QPointF(108, 100));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Drag);
}

void PetPointerGestureTest::deliberateHeadStrokeNeedsReturnAndTime() {
    PetPointerGesture gesture;
    gesture.press(QPointF(100, 100), true);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    QVERIFY(gesture.pressedOnHead());
    gesture.advance(0.1);
    gesture.move(QPointF(104, 100));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.move(QPointF(102, 100));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.advance(0.1);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pat);
    QCOMPARE(gesture.displacement(), QPointF(2, 0));
    QVERIFY(gesture.normalizedPatDirection() > 0.0);
    gesture.move(QPointF(97, 100));
    QVERIFY(gesture.normalizedPatDirection() < 0.0);
}

void PetPointerGestureTest::verticalStrokeWorks() {
    PetPointerGesture gesture;
    gesture.press(QPointF(0, 0), true);
    gesture.move(QPointF(0, 5));
    gesture.move(QPointF(0, 2));
    gesture.advance(0.2);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pat);
    QCOMPARE(gesture.displacement(), QPointF(0, 2));
}

void PetPointerGestureTest::quickHeadDrag() {
    PetPointerGesture gesture;
    gesture.press(QPointF(10, 20), true);
    gesture.move(QPointF(28, 20));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.move(QPointF(29, 20));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Drag);
    gesture.advance(1.0);
    gesture.move(QPointF(10, 20));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Drag);
}

void PetPointerGestureTest::bodyDragThreshold() {
    PetPointerGesture gesture;
    gesture.press(QPointF(20, 30), false);
    gesture.move(QPointF(26, 30));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.move(QPointF(24, 35));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Drag);
    QVERIFY(!gesture.pressedOnHead());
    QCOMPARE(gesture.displacement(), QPointF(4, 5));
}

void PetPointerGestureTest::clickLongPressAndJitterStayPending() {
    PetPointerGesture gesture;
    gesture.press(QPointF(20, 30), true);
    gesture.advance(5.0);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    for (int i = 0; i < 20; ++i) {
        gesture.move(QPointF(20.5, 30));
        gesture.move(QPointF(19.5, 30));
    }
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.move(QPointF(25, 30));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.release();
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::None);
}

void PetPointerGestureTest::patDoesNotBecomeDrag() {
    PetPointerGesture gesture;
    gesture.press(QPointF(0, 0), true);
    gesture.move(QPointF(4, 0));
    gesture.move(QPointF(1, 0));
    gesture.advance(0.2);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pat);
    gesture.move(QPointF(100, 0));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pat);
    QCOMPARE(gesture.normalizedPatDirection(), 1.0);
    gesture.move(QPointF(-100, 0));
    QCOMPARE(gesture.normalizedPatDirection(), -1.0);
}

void PetPointerGestureTest::releaseAndCancelReset() {
    PetPointerGesture gesture;
    gesture.press(QPointF(4, 5), false);
    gesture.move(QPointF(20, 20));
    gesture.release();
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::None);
    QVERIFY(!gesture.pressedOnHead());
    QCOMPARE(gesture.displacement(), QPointF());
    gesture.move(QPointF(100, 100));
    gesture.advance(1.0);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::None);
    gesture.press(QPointF(4, 5), true);
    gesture.advance(-1.0);
    gesture.move(QPointF(7, 5));
    gesture.move(QPointF(4, 5));
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
    gesture.cancel();
    gesture.press(QPointF(4, 5), true);
    gesture.advance(0.2);
    QCOMPARE(gesture.mode(), PetPointerGesture::Mode::Pending);
}

QTEST_APPLESS_MAIN(PetPointerGestureTest)
#include "PetPointerGestureTest.moc"
