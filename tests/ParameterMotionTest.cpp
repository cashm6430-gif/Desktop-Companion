#include "../src/ParameterMotion.h"

#include <QtTest/QTest>
#include <cmath>

class ParameterMotionTest final : public QObject {
    Q_OBJECT
private slots:
    void stateTransitionIsContinuous();
    void deleteHasWindupStrikeAndRecoil();
    void backgroundMotionContinuesAfterAction();
    void repeatedDeleteRestartsAction();
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

QTEST_GUILESS_MAIN(ParameterMotionTest)
#include "ParameterMotionTest.moc"
