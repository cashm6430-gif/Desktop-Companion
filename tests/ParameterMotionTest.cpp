#include "../src/ParameterMotion.h"

#include <QtTest/QTest>
#include <QTemporaryFile>
#include <cmath>

class ParameterMotionTest final : public QObject {
    Q_OBJECT
private slots:
    void stateTransitionIsContinuous();
    void deleteHasWindupStrikeAndRecoil();
    void backgroundMotionContinuesAfterAction();
    void repeatedDeleteRestartsAction();
    void grassKeysAndTransitions();
    void grassExpressionsSurviveBlink();
    void proceduralBlinkClosesAndRecovers();
    void grassFlexLagsReboundsAndSettles();
    void jointFlexAndInterruptedRecovery();
    void grassForwardHoldAndWristGesture();
    void malformedMotionDoesNotReplaceLoadedKeys();
    void laptopSelectionAndInterruptions();
    void laptopTypingAndShortTaskExit();
};

void ParameterMotionTest::laptopSelectionAndInterruptions() {
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    motion.setBusyRandomSeed(1234);
    int seated = 0;
    for (int session = 0; session < 60; ++session) {
        motion.setState(PetController::State::Idle);
        motion.setState(PetController::State::Busy);
        const bool choice = motion.isLaptopBusy();
        seated += choice;
        for (int frame = 0; frame < 300; ++frame) {
            motion.advance(0.02);
            motion.setState(PetController::State::Busy); // another active turn
            QCOMPARE(motion.isLaptopBusy(), choice);
        }
        motion.setState(PetController::State::Delete);
        for (int frame = 0; frame < 70; ++frame) motion.advance(0.02);
        motion.setState(PetController::State::Busy);
        QCOMPARE(motion.isLaptopBusy(), choice);
    }
    QVERIFY(seated > 10 && seated < 40);
}

void ParameterMotionTest::laptopTypingAndShortTaskExit() {
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    motion.advance(0.02);
    motion.forceLaptopBusy();
    double minimum = 1, maximum = 0;
    for (int i = 0; i < 200; ++i) {
        motion.advance(0.02);
        const double hand = motion.values().value(QStringLiteral("ParamBusyTypingR"));
        minimum = std::min(minimum, hand); maximum = std::max(maximum, hand);
    }
    QVERIFY(maximum - minimum > 0.3);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    for (int i = 0; i < 90; ++i) motion.advance(0.02);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingR")) < 0.04);
    const auto before = motion.values();
    motion.setState(PetController::State::Idle);
    QCOMPARE(motion.values(), before);
    for (int i = 0; i < 15; ++i) motion.advance(0.02);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) < 0.1);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingR")) < 0.01);
    for (int i = 0; i < 150; ++i) motion.advance(0.02);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) < 0.001);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingR")) < 0.001);
    // Stop during the first tenth of a second must also fade from that pose.
    motion.forceLaptopBusy();
    motion.advance(0.1);
    const double early = motion.values().value(QStringLiteral("ParamBusyLaptop"));
    motion.setState(PetController::State::Idle);
    motion.advance(0.02);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) < early);
}

void ParameterMotionTest::stateTransitionIsContinuous() {
    ParameterMotion motion;
    for (int i = 0; i < 50; ++i) motion.advance(0.02);
    const double before = motion.values().value(QStringLiteral("ParamArmRA"));
    motion.setState(PetController::State::Busy);
    QCOMPARE(motion.values().value(QStringLiteral("ParamArmRA")), before);
    motion.advance(0.02);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamArmRA")) - before) < 5.0);
    for (int i = 0; i < 50; ++i) motion.advance(0.02);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamArmRA"))) > 1.0);
}

void ParameterMotionTest::deleteHasWindupStrikeAndRecoil() {
    ParameterMotion motion;
    motion.advance(0.02);
    motion.setState(PetController::State::Delete);
    for (int i = 0; i < 15; ++i) motion.advance(0.02);
    const double windup = motion.values().value(QStringLiteral("ParamArmRA"));
    for (int i = 0; i < 18; ++i) motion.advance(0.02);
    const double strike = motion.values().value(QStringLiteral("ParamArmRA"));
    for (int i = 0; i < 40; ++i) motion.advance(0.02);
    const double recovery = motion.values().value(QStringLiteral("ParamArmRA"));
    QVERIFY(windup < -5.0);
    QVERIFY(strike > 5.0);
    QVERIFY(recovery < strike);
}

void ParameterMotionTest::backgroundMotionContinuesAfterAction() {
    ParameterMotion motion;
    motion.setState(PetController::State::Busy);
    for (int i = 0; i < 30; ++i) motion.advance(0.02);
    motion.setState(PetController::State::Delete);
    for (int i = 0; i < 70; ++i) motion.advance(0.02);
    const double before = motion.values().value(QStringLiteral("ParamArmRA"));
    motion.setState(PetController::State::Busy);
    QCOMPARE(motion.values().value(QStringLiteral("ParamArmRA")), before);
    motion.advance(0.02);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamArmRA")) - before) < 5.0);
}

void ParameterMotionTest::repeatedDeleteRestartsAction() {
    ParameterMotion motion;
    motion.setState(PetController::State::Delete);
    for (int i = 0; i < 60; ++i) motion.advance(0.02);
    motion.setState(PetController::State::Delete);
    for (int i = 0; i < 15; ++i) motion.advance(0.02);
    QVERIFY(motion.values().value(QStringLiteral("ParamArmRA")) < -5.0);
}

void ParameterMotionTest::grassKeysAndTransitions() {
    ParameterMotion motion;
    QVERIFY(motion.loadGrassMotion(QStringLiteral("assets/motions/grass.motion.json")));
    QCOMPARE(motion.grassPose(3.6).value(QStringLiteral("ParamGrassReach")), 1.0);
    QCOMPARE(motion.grassPose(3.6).value(QStringLiteral("ParamHandRGrip")), 1.0);
    QCOMPARE(motion.grassPose(6.9).value(QStringLiteral("ParamGrassVisible")), 0.0);
    QCOMPARE(motion.grassPose(6.9).value(QStringLiteral("ParamHandRGrip")), 0.0);
    const auto middle = motion.grassPose(2.8);
    QVERIFY(middle.value(QStringLiteral("ParamGrassReach")) > 0.1);
    QVERIFY(middle.value(QStringLiteral("ParamGrassReach")) < 1.0);
    motion.advance(0.04);
    motion.setState(PetController::State::Grass);
    for (int i = 0; i < 90; ++i) motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamGrassReach")) > 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamHandRGrip")) > 0.99);
    const auto before = motion.values();
    motion.setState(PetController::State::Busy);
    QCOMPARE(motion.values(), before);
    motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamGrassReach")) < before.value(QStringLiteral("ParamGrassReach")));
    QVERIFY(motion.values().value(QStringLiteral("ParamHandRGrip")) < before.value(QStringLiteral("ParamHandRGrip")));
    motion.setPreviewPose({{QStringLiteral("ParamArmRA"), 25.0}});
    motion.advance(0.04);
    QCOMPARE(motion.values().value(QStringLiteral("ParamArmRA")), 25.0);
}

void ParameterMotionTest::malformedMotionDoesNotReplaceLoadedKeys() {
    ParameterMotion motion;
    QVERIFY(motion.loadGrassMotion(QStringLiteral("assets/motions/grass.motion.json")));
    const auto before = motion.grassPose(3.6);
    QVERIFY(!motion.loadGrassMotion(QStringLiteral("missing.motion.json")));
    QCOMPARE(motion.grassPose(3.6), before);
    QTemporaryFile invalid;
    QVERIFY(invalid.open());
    invalid.write(R"({"keyframes":[{"time":0,"parameters":{"A":0}},{"time":0,"parameters":{"A":1}}]})");
    invalid.flush();
    QVERIFY(!motion.loadGrassMotion(invalid.fileName()));
    QCOMPARE(motion.grassPose(3.6), before);
}

void ParameterMotionTest::grassExpressionsSurviveBlink() {
    ParameterMotion motion;
    QVERIFY(motion.loadGrassMotion(QStringLiteral("assets/motions/grass.motion.json")));
    motion.setState(PetController::State::Grass);
    for (int i = 0; i < 55; ++i) motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.2);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) > 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.95);
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) > 0.99);
    for (int i = 55; i < 90; ++i) motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamArmRA")) > 50.0);
    for (int i = 90; i < 140; ++i) motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.1);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) < 0.1);
    QVERIFY(motion.values().value(QStringLiteral("ParamMouthOpenY")) > 0.9);
    motion.setState(PetController::State::Idle);
    for (int i = 0; i < 30; ++i) motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) < 0.01);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) < 0.01);
}

void ParameterMotionTest::proceduralBlinkClosesAndRecovers() {
    ParameterMotion motion;
    motion.advance(0.075);
    QCOMPARE(motion.values().value(QStringLiteral("ParamEyeLOpen")), 0.0);
    QCOMPARE(motion.values().value(QStringLiteral("ParamEyeROpen")), 0.0);
    motion.advance(0.085);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.99);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) > 0.99);
    // A blink must not reopen an authored wink when the blink finishes.
    QTemporaryFile heldWink;
    QVERIFY(heldWink.open());
    heldWink.write(R"({"keyframes":[{"time":0,"parameters":{"ParamEyeLOpen":0,"ParamEyeROpen":1}},
        {"time":10,"parameters":{"ParamEyeLOpen":0,"ParamEyeROpen":1}}]})");
    heldWink.flush();
    ParameterMotion wink;
    QVERIFY(wink.loadGrassMotion(heldWink.fileName()));
    wink.setPreviewPose(wink.grassPose(0));
    wink.setState(PetController::State::Grass);
    wink.advance(0.075);
    QCOMPARE(wink.values().value(QStringLiteral("ParamEyeLOpen")), 0.0);
    QCOMPARE(wink.values().value(QStringLiteral("ParamEyeROpen")), 0.0);
    wink.advance(0.085);
    QCOMPARE(wink.values().value(QStringLiteral("ParamEyeLOpen")), 0.0);
    QVERIFY(wink.values().value(QStringLiteral("ParamEyeROpen")) > 0.99);
}

void ParameterMotionTest::grassFlexLagsReboundsAndSettles() {
    // A held gesture isolates elasticity from the authored motion curves.
    QTemporaryFile gesture;
    QVERIFY(gesture.open());
    gesture.write(R"({"keyframes":[{"time":0,"parameters":{"ParamGrassSwing":1,"ParamGrassVisible":1}},{"time":4,"parameters":{"ParamGrassSwing":1,"ParamGrassVisible":1}}]})");
    gesture.flush();
    ParameterMotion slow, fast;
    QVERIFY(slow.loadGrassMotion(gesture.fileName()));
    QVERIFY(fast.loadGrassMotion(gesture.fileName()));
    slow.setState(PetController::State::Grass);
    fast.setState(PetController::State::Grass);
    double peak = 0.0, tipLag = 0.0;
    for (int i = 0; i < 36; ++i) {
        slow.advance(1.0 / 30.0);
        fast.advance(1.0 / 60.0);
        fast.advance(1.0 / 60.0);
        peak = std::max(peak, slow.values().value(QStringLiteral("ParamGrassSwing")));
        tipLag = std::max(tipLag, std::abs(slow.values().value(QStringLiteral("ParamGrassTipBend"))));
        QVERIFY(std::abs(slow.values().value(QStringLiteral("ParamGrassSwing"))
            - fast.values().value(QStringLiteral("ParamGrassSwing"))) < 0.025);
    }
    QVERIFY(peak > 0.70); // Overshoots the held target of 0.65.
    QVERIFY(tipLag > 0.15);
    slow.setState(PetController::State::Idle);
    bool rebounded = false;
    for (int i = 0; i < 150; ++i) {
        slow.advance(1.0 / 30.0);
        const double bend = slow.values().value(QStringLiteral("ParamGrassSwing"));
        rebounded |= bend < -0.02;
        QVERIFY(std::abs(bend) <= 1.0);
    }
    QVERIFY(rebounded);
    QVERIFY(std::abs(slow.values().value(QStringLiteral("ParamGrassSwing"))) < 0.001);
    QVERIFY(std::abs(slow.values().value(QStringLiteral("ParamGrassTipBend"))) < 0.001);
}

void ParameterMotionTest::jointFlexAndInterruptedRecovery() {
    // Hold the shoulder and reach still: elbow or wrist alone excites the grass.
    for (const auto& joint : {QStringLiteral("ParamElbowRA"), QStringLiteral("ParamWristRA")}) {
        QTemporaryFile gesture;
        QVERIFY(gesture.open());
        gesture.write(QStringLiteral(R"({"keyframes":[{"time":0,"parameters":{"%1":25,"ParamGrassVisible":1}},
            {"time":5,"parameters":{"%1":25,"ParamGrassVisible":1}}]})").arg(joint).toUtf8());
        gesture.flush();
        ParameterMotion motion;
        QVERIFY(motion.loadGrassMotion(gesture.fileName()));
        motion.setPreviewPose({{joint, 0}, {QStringLiteral("ParamGrassVisible"), 1}});
        motion.setState(PetController::State::Grass);
        double peak = 0;
        for (int i = 0; i < 30; ++i) {
            motion.advance(1.0 / 60.0);
            peak = std::max(peak, std::abs(motion.values().value(QStringLiteral("ParamGrassSwing"))));
        }
        QVERIFY(peak > 0.1);
        QVERIFY(motion.values().value(joint) > 24);
        const auto before = motion.values();
        motion.setState(PetController::State::Busy);
        QCOMPARE(motion.values(), before);
        motion.advance(1.0 / 60.0);
        QVERIFY(motion.values().value(joint) < before.value(joint));
        for (int i = 0; i < 60; ++i) motion.advance(1.0 / 60.0);
        QVERIFY(motion.values().value(joint) < 0.02);
    }
}

void ParameterMotionTest::grassForwardHoldAndWristGesture() {
    ParameterMotion motion;
    QVERIFY(motion.loadGrassMotion(QStringLiteral("assets/motions/grass.motion.json")));
    const auto arrival = motion.grassPose(3.6);
    for (double time : {3.8, 3.95, 4.15, 4.35}) {
        const auto held = motion.grassPose(time);
        for (const auto& id : {QStringLiteral("ParamGrassReach"), QStringLiteral("ParamArmRA"),
             QStringLiteral("ParamElbowRA"), QStringLiteral("ParamHandRGrip")})
            QCOMPARE(held.value(id), arrival.value(id));
    }
    QVERIFY(motion.grassPose(3.95).value(QStringLiteral("ParamWristRA"))
        - arrival.value(QStringLiteral("ParamWristRA")) > 30);
    motion.setState(PetController::State::Grass);
    for (int i = 0; i < 105; ++i) motion.advance(0.04);
    QVERIFY(motion.values().value(QStringLiteral("ParamGrassReach")) > 0.99);
    QVERIFY(motion.values().value(QStringLiteral("ParamWristRA")) > 18);
    const auto before = motion.values();
    motion.setState(PetController::State::Idle);
    QCOMPARE(motion.values(), before);
    for (int i = 0; i < 30; ++i) motion.advance(0.04);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamWristRA"))) < 0.01);
}

QTEST_GUILESS_MAIN(ParameterMotionTest)
#include "ParameterMotionTest.moc"
