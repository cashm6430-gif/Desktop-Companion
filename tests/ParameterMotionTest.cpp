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
    void malformedMotionDoesNotReplaceLoadedKeys();
};

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
}

QTEST_GUILESS_MAIN(ParameterMotionTest)
#include "ParameterMotionTest.moc"
