#include "../src/ParameterMotion.h"

#include <QtMath>
#include <QtTest/QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace {
bool writeReactionFixture(const QTemporaryDir& dir) {
    const QByteArray turn = R"({"duration":1.8,"approval":"pending","keyframes":[
      {"time":0,"parameters":{"ParamAngleY":-8,"ParamEyeLOpen":1,"ParamEyeROpen":1,"ParamEyeSmile":0}},
      {"time":0.5,"parameters":{"ParamAngleY":7,"ParamEyeLOpen":1,"ParamEyeROpen":1,"ParamEyeSmile":0}},
      {"time":1.1,"parameters":{"ParamAngleY":-2,"ParamEyeLOpen":0,"ParamEyeROpen":0,"ParamEyeSmile":1}},
      {"time":1.8,"parameters":{"ParamAngleY":0,"ParamEyeLOpen":1,"ParamEyeROpen":1,"ParamEyeSmile":0}}]})";
    const QByteArray pat = R"({"duration":2.2,"approval":"pending","interaction":{"enterEnd":0.8,"holdEnd":1.6},
      "constants":{"ParamMouthOpenY":0,"ParamSmileOpen":0},"keyframes":[
      {"time":0,"parameters":{"ParamAngleZ":0,"ParamEyeLOpen":1,"ParamEyeROpen":1,"ParamEyeSmile":0}},
      {"time":0.8,"parameters":{"ParamAngleZ":3,"ParamEyeLOpen":0,"ParamEyeROpen":0,"ParamEyeSmile":1}},
      {"time":1.6,"parameters":{"ParamAngleZ":3,"ParamEyeLOpen":0,"ParamEyeROpen":0,"ParamEyeSmile":1}},
      {"time":2.2,"parameters":{"ParamAngleZ":0,"ParamEyeLOpen":1,"ParamEyeROpen":1,"ParamEyeSmile":0}}]})";
    for (const auto& entry : {qMakePair(QStringLiteral("turn-ended"), turn),
                              qMakePair(QStringLiteral("head-pat"), pat)}) {
        QFile file(dir.filePath(entry.first + QStringLiteral(".motion.json")));
        if (!file.open(QIODevice::WriteOnly) || file.write(entry.second) != entry.second.size()) return false;
    }
    return true;
}

void advanceFrames(ParameterMotion& motion, int frames) {
    for (int i = 0; i < frames; ++i) motion.advance(0.02);
}

bool writeGrassInteractionFixture(const QTemporaryDir& dir, double maxHold = 3.0) {
    QJsonArray keys;
    const auto key = [&](double time, double wrist, double reach, double eyeX, double smile) {
        const bool visible = time > 0 && time < 8.6;
        keys.append(QJsonObject{{QStringLiteral("time"), time},
            {QStringLiteral("parameters"), QJsonObject{
                {QStringLiteral("ParamGrassVisible"), visible ? 1.0 : 0.0},
                {QStringLiteral("ParamHandRGrip"), visible ? 1.0 : 0.0},
                {QStringLiteral("ParamGrassReach"), reach},
                {QStringLiteral("ParamGrassSwing"), 0.0},
                {QStringLiteral("ParamArmRA"), visible ? 50.0 : 0.0},
                {QStringLiteral("ParamElbowRA"), visible ? 14.0 : 0.0},
                {QStringLiteral("ParamWristRA"), wrist},
                {QStringLiteral("ParamEyeBallX"), eyeX},
                {QStringLiteral("ParamEyeLOpen"), 1.0},
                {QStringLiteral("ParamEyeROpen"), 1.0},
                {QStringLiteral("ParamEyeSmile"), smile}
            }}});
    };
    key(0, 0, 0, 0, 0);
    key(3.6, 0, 1, 0, 0);
    key(4.0, 20, 1, 0, 0);
    key(4.4, 0, 1, 0, 0);
    key(5.2, -22, 0.5, 0, 1);
    key(6.0, 0, 1, 0, 0);
    key(6.5, 0, 1, 0.75, 0);
    key(7.0, 0, 1, 0, 0);
    key(8.6, 0, 0, 0, 0);
    const QJsonObject root{
        {QStringLiteral("duration"), 8.6}, {QStringLiteral("approval"), QStringLiteral("pending")},
        {QStringLiteral("interaction"), QJsonObject{
            {QStringLiteral("enterEnd"), 3.6}, {QStringLiteral("holdEnd"), 4.4},
            {QStringLiteral("respondEnd"), 6.0}, {QStringLiteral("timeoutEnd"), 7.0},
            {QStringLiteral("releaseEnd"), 8.6}, {QStringLiteral("maxHold"), maxHold}
        }}, {QStringLiteral("keyframes"), keys}
    };
    QFile file(dir.filePath(QStringLiteral("grass-touch.motion.json")));
    const auto json = QJsonDocument(root).toJson();
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(json) == json.size();
}
}

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
    void blendSecondsComeFromClip();
    void standingAccentKeepsMovingAfterOneCycle();
    void absentReactionsLeaveExistingMotionAvailable();
    void turnEndedPreservesSeatThenPutsLaptopAway();
    void headPatHoldReleaseDirectionAndCooldown();
    void headPatWhileBusyLeavesHandsAndComputerAlone();
    void reactionInterruptionsKeepPoseContinuous();
    void invalidReactionConfigurationIsRejected();
    void seatedReactionContextSurvivesUserAndLastStopHandoffs();
    void earlyHeadPatReleaseDoesNotForceClosedEyes();
    void headPatReleaseRestoresAuthoredFacePace();
    void headPatReleaseDoesNotAccelerateUnownedBackgroundChannels();
    void grassInteractionRespondsOnlyDuringHold();
    void grassInteractionTimeoutSkipsResponse();
    void grassInteractionIgnoresLegacyCompletionDeadline();
    void grassInteractionInterruptionsAndRestartStayContinuous();
    void grassInteractionRootAndSoftTipStayBounded();
    void grassInteractionConfigurationRejectsBadPhasesAndGrip();
};

void ParameterMotionTest::grassInteractionRespondsOnlyDuringHold() {
    QTemporaryDir dir;
    QVERIFY(writeGrassInteractionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(!motion.beginGrassInteraction());
    motion.setState(PetController::State::Grass);
    QVERIFY(motion.beginGrassInteraction());
    QVERIFY(!motion.beginGrassInteraction());
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("enter"));
    QVERIFY(!motion.respondToGrass());
    advanceFrames(motion, 180);
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("hold"));
    const auto held = motion.values();
    motion.lookAtGrassTip(1);
    QCOMPARE(motion.values(), held);
    advanceFrames(motion, 30);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) > 0.15);
    const auto beforeClick = motion.values();
    QVERIFY(motion.respondToGrass());
    QCOMPARE(motion.values(), beforeClick);
    QCOMPARE(motion.grassInteractionTime(), 4.4);
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("respond"));
    QVERIFY(!motion.respondToGrass());
    motion.lookAtGrassTip(-1); // The response owns the gaze again.
    advanceFrames(motion, 80);
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("release"));
    QVERIFY(!motion.consumeActionFinished());
    advanceFrames(motion, 80);
    QVERIFY(!motion.grassInteractionActive());
    QVERIFY(motion.grassInteractionPhase().isEmpty());
    QVERIFY(motion.consumeActionFinished());
    QVERIFY(!motion.consumeActionFinished());
    advanceFrames(motion, 150);
    QVERIFY(!motion.consumeActionFinished());
    QVERIFY(!motion.respondToGrass());
    QCOMPARE(motion.library().clip(QStringLiteral("grass-touch"))->approval(), QStringLiteral("pending"));
}

void ParameterMotionTest::grassInteractionTimeoutSkipsResponse() {
    QTemporaryDir dir;
    QVERIFY(writeGrassInteractionFixture(dir));
    for (const double step : {1.0 / 30.0, 1.0 / 60.0}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(dir.path()));
        motion.setState(PetController::State::Grass);
        QVERIFY(motion.beginGrassInteraction());
        bool sawHold = false, sawTimeout = false, sawRelease = false;
        double highestTimeoutGaze = 0;
        for (int i = 0; i < static_cast<int>(9.3 / step); ++i) {
            motion.advance(step);
            const auto phase = motion.grassInteractionPhase();
            QVERIFY(phase != QStringLiteral("respond"));
            sawHold |= phase == QStringLiteral("hold");
            sawTimeout |= phase == QStringLiteral("timeout");
            sawRelease |= phase == QStringLiteral("release");
            if (phase == QStringLiteral("timeout"))
                highestTimeoutGaze = std::max(highestTimeoutGaze,
                    motion.values().value(QStringLiteral("ParamEyeBallX")));
        }
        QVERIFY(sawHold && sawTimeout && sawRelease);
        QVERIFY(highestTimeoutGaze > 0.5);
        QVERIFY(!motion.grassInteractionActive());
        QVERIFY(motion.consumeActionFinished());
        motion.advance(step);
        QVERIFY(!motion.consumeActionFinished());
    }
}

void ParameterMotionTest::grassInteractionIgnoresLegacyCompletionDeadline() {
    QTemporaryDir dir;
    QVERIFY(writeGrassInteractionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadGrassMotion(QStringLiteral("assets/motions/grass.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QCOMPARE(motion.actionDuration(PetController::State::Grass), 6.9);
    QVERIFY(qAbs(motion.grassInteractionMaxDuration() - 9.8) < 1e-9);
    motion.setState(PetController::State::Grass);
    QVERIFY(motion.beginGrassInteraction());
    QVERIFY(qAbs(motion.actionDuration(PetController::State::Grass) - 9.8) < 1e-9);
    advanceFrames(motion, 325); // 6.5 s: click just before the 3 s wait expires.
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("hold"));
    QVERIFY(motion.respondToGrass());
    advanceFrames(motion, 25); // 7 s is already beyond the old fixed clip.
    QVERIFY(motion.grassInteractionActive());
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("respond"));
    QVERIFY(!motion.consumeActionFinished());
    advanceFrames(motion, 134);
    QVERIFY(motion.grassInteractionActive());
    motion.advance(0.02); // Actual response path finishes at 9.7 s.
    QVERIFY(!motion.grassInteractionActive());
    QVERIFY(motion.consumeActionFinished());
}

void ParameterMotionTest::grassInteractionInterruptionsAndRestartStayContinuous() {
    QTemporaryDir dir;
    QVERIFY(writeGrassInteractionFixture(dir));
    for (const auto next : {PetController::State::Busy, PetController::State::Delete, PetController::State::Idle}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(dir.path()));
        motion.setState(PetController::State::Grass);
        QVERIFY(motion.beginGrassInteraction());
        advanceFrames(motion, 200);
        const auto before = motion.values();
        motion.setState(next);
        QCOMPARE(motion.values(), before);
        QVERIFY(!motion.grassInteractionActive());
        QVERIFY(motion.grassInteractionPhase().isEmpty());
        QVERIFY(!motion.respondToGrass());
        QVERIFY(!motion.consumeActionFinished());
        motion.setState(PetController::State::Grass);
        QVERIFY(motion.beginGrassInteraction());
        advanceFrames(motion, 190);
        const auto hold = motion.values();
        motion.setState(PetController::State::Grass); // Re-trigger starts Enter.
        QCOMPARE(motion.values(), hold);
        QVERIFY(!motion.grassInteractionActive());
        QVERIFY(motion.beginGrassInteraction());
        QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("enter"));
        QCOMPARE(motion.values(), hold);
    }
}

void ParameterMotionTest::grassInteractionRootAndSoftTipStayBounded() {
    QTemporaryDir dir;
    QVERIFY(writeGrassInteractionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.setState(PetController::State::Grass);
    QVERIFY(motion.beginGrassInteraction());
    advanceFrames(motion, 210);
    double tipMotion = 0;
    for (int i = 0; i < 60; ++i) {
        motion.lookAtGrassTip(i % 2 ? -20 : 20); // Invalid extent is clamped.
        motion.advance(0.02);
        QVERIFY(motion.values().value(QStringLiteral("ParamHandRGrip")) > 0.99);
        QVERIFY(motion.values().value(QStringLiteral("ParamGrassVisible")) > 0.99);
        QVERIFY(motion.values().value(QStringLiteral("ParamGrassReach")) > 0.99);
        QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamGrassSwing"))) <= 1);
        QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamGrassTipBend"))) <= 1);
        QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamEyeBallX"))) <= 0.18);
        tipMotion = std::max(tipMotion, qAbs(motion.values().value(QStringLiteral("ParamGrassTipBend"))));
    }
    QVERIFY(tipMotion > 0.05); // Wrist motion excites the softer trailing tip.
    QVERIFY(motion.respondToGrass());
    advanceFrames(motion, 175);
    QVERIFY(!motion.grassInteractionActive());
    QVERIFY(motion.values().value(QStringLiteral("ParamGrassVisible")) < 0.01);
}

void ParameterMotionTest::grassInteractionConfigurationRejectsBadPhasesAndGrip() {
    QTemporaryDir dir;
    QVERIFY(writeGrassInteractionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(writeGrassInteractionFixture(dir, 3.1));
    QString error;
    QVERIFY(!motion.loadMotionLibrary(dir.path(), &error));
    QVERIFY(error.contains(QStringLiteral("maxHold")));
    motion.setState(PetController::State::Grass);
    QVERIFY(motion.beginGrassInteraction()); // Retains the last valid metadata.
    QVERIFY(qAbs(motion.grassInteractionMaxDuration() - 9.8) < 1e-9);
    QVERIFY(writeGrassInteractionFixture(dir));
    QFile fixture(dir.filePath(QStringLiteral("grass-touch.motion.json")));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(fixture.readAll()).object();
    fixture.close();
    auto keys = root.value(QStringLiteral("keyframes")).toArray();
    for (int index : {1, 3}) {
        auto key = keys[index].toObject();
        auto params = key.value(QStringLiteral("parameters")).toObject();
        params.insert(QStringLiteral("ParamHandRGrip"), 0.0);
        key.insert(QStringLiteral("parameters"), params);
        keys[index] = key;
    }
    root.insert(QStringLiteral("keyframes"), keys);
    QVERIFY(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate));
    fixture.write(QJsonDocument(root).toJson());
    fixture.close();
    QVERIFY(!motion.loadMotionLibrary(dir.path(), &error));
    QVERIFY(error.contains(QStringLiteral("grip")));
}

void ParameterMotionTest::seatedReactionContextSurvivesUserAndLastStopHandoffs() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    for (const bool startWithTurnEnded : {false, true}) {
        ParameterMotion motion;
        QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
        QVERIFY(motion.loadMotionLibrary(dir.path()));
        motion.forceLaptopBusy();
        advanceFrames(motion, 200);
        auto seated = motion.values();
        if (startWithTurnEnded) {
            motion.setState(PetController::State::Idle);
            QVERIFY(motion.playTurnEnded());
            advanceFrames(motion, 40);
            QVERIFY(motion.beginHeadPat());
        } else {
            QVERIFY(motion.beginHeadPat());
            advanceFrames(motion, 20);
            // The final turn stops while the user's hand is still on the head.
            seated = motion.values();
            motion.setState(PetController::State::Idle);
        }
        advanceFrames(motion, 42);
        QVERIFY(motion.interactionActive());
        for (const auto& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
                               QStringLiteral("ParamLaptopVisible"), QStringLiteral("ParamArmLA"),
                               QStringLiteral("ParamArmRA")})
            QCOMPARE(motion.values().value(id), seated.value(id));
        const auto beforeRelease = motion.values();
        motion.endHeadPat();
        QCOMPARE(motion.values(), beforeRelease);
        advanceFrames(motion, 31);
        QVERIFY(!motion.interactionActive());
        QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
        advanceFrames(motion, 12);
        QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) < 0.1);
        QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
        advanceFrames(motion, 150);
        QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) < 0.001);
    }
}

void ParameterMotionTest::earlyHeadPatReleaseDoesNotForceClosedEyes() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    advanceFrames(motion, 50);
    QVERIFY(motion.beginHeadPat());
    advanceFrames(motion, 10);
    const auto before = motion.values();
    QVERIFY(before.value(QStringLiteral("ParamEyeLOpen")) > 0.9);
    motion.endHeadPat();
    QVERIFY(!motion.interactionActive());
    QCOMPARE(motion.values(), before);
    motion.advance(0.02);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) >= before.value(QStringLiteral("ParamEyeLOpen")));
    advanceFrames(motion, 30);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.99);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) < 0.001);
    QVERIFY(!motion.beginHeadPat());
}

void ParameterMotionTest::absentReactionsLeaveExistingMotionAvailable() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(!motion.playTurnEnded());
    QVERIFY(!motion.beginHeadPat());
    motion.setState(PetController::State::Busy);
    advanceFrames(motion, 50);
    QVERIFY(!motion.interactionActive());
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamArmRA"))) > 1.0);
}

void ParameterMotionTest::turnEndedPreservesSeatThenPutsLaptopAway() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    const auto seated = motion.values();
    QVERIFY(seated.value(QStringLiteral("ParamLaptopVisible")) > 0.99);
    motion.setState(PetController::State::Idle);
    QVERIFY(motion.playTurnEnded());
    QCOMPARE(motion.values(), seated);
    QCOMPARE(motion.interactionId(), QStringLiteral("turn-ended"));
    advanceFrames(motion, 60);
    QVERIFY(motion.interactionActive());
    for (const auto& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
                           QStringLiteral("ParamLaptopVisible"), QStringLiteral("ParamArmLA"),
                           QStringLiteral("ParamArmRA")})
        QCOMPARE(motion.values().value(id), seated.value(id));
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingR")) < 0.001);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.8);
    advanceFrames(motion, 35);
    QVERIFY(!motion.interactionActive());
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    advanceFrames(motion, 12);
    QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) < 0.1);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    advanceFrames(motion, 150);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) < 0.001);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) < 0.001);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.9);
}

void ParameterMotionTest::headPatHoldReleaseDirectionAndCooldown() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    advanceFrames(motion, 50);
    const auto initial = motion.values();
    QVERIFY(motion.canBeginHeadPat());
    QVERIFY(motion.beginHeadPat(1));
    QVERIFY(!motion.canBeginHeadPat());
    QCOMPARE(motion.values(), initial);
    advanceFrames(motion, 70);
    QVERIFY(motion.interactionTime() >= 0.8 && motion.interactionTime() < 1.6);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.98);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.03);
    QVERIFY(motion.values().value(QStringLiteral("ParamAngleZ")) > 2.8);
    const double before = motion.values().value(QStringLiteral("ParamAngleZ"));
    motion.updateHeadPat(-1);
    QCOMPARE(motion.values().value(QStringLiteral("ParamAngleZ")), before);
    motion.advance(0.02);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamAngleZ")) - before) < 0.2);
    advanceFrames(motion, 40);
    QVERIFY(motion.values().value(QStringLiteral("ParamAngleZ")) < -2.5);
    const auto held = motion.values();
    motion.endHeadPat();
    QCOMPARE(motion.values(), held);
    QCOMPARE(motion.interactionTime(), 1.6);
    advanceFrames(motion, 31);
    QVERIFY(!motion.interactionActive());
    QVERIFY(!motion.canBeginHeadPat());
    QVERIFY(!motion.beginHeadPat());
    advanceFrames(motion, 390);
    QVERIFY(motion.beginHeadPat());
    advanceFrames(motion, 410); // Held for eight seconds: bounded release begins.
    QVERIFY(motion.interactionTime() > 1.6);
    advanceFrames(motion, 25);
    QVERIFY(!motion.interactionActive());
    QVERIFY(!motion.beginHeadPat());
}

void ParameterMotionTest::headPatReleaseRestoresAuthoredFacePace() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    advanceFrames(motion, 50);
    QVERIFY(motion.beginHeadPat());
    advanceFrames(motion, 70); // Settled into the closed-eye hold.
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.98);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.03);

    const QString eyeOpen = QStringLiteral("ParamEyeLOpen");
    const QString eyeSmile = QStringLiteral("ParamEyeSmile");
    motion.endHeadPat();
    // The release is part of the interaction: endHeadPat() jumps the clip to the
    // authored recovery segment and the interaction retires once it plays out.
    QVERIFY(motion.headPatReleasing());
    QVERIFY(motion.interactionTime() >= 1.6);

    // The authored head-pat recovery opens the eyes over about 0.18s. Exponential
    // blending needs about three time constants to settle, so a slow release
    // leaves the face half-open ("drowsy") across several frames the clip never
    // asked for. The release therefore hands the reaction's own channels back on
    // kExpressionReleaseBlend. The effect is verified end-to-end by capturing the
    // scene and counting half-blended frames after release: 3 -> 1
    // (tools/review_interactions.py head-pat; build/_zoom_review/drowsy_window.py).
    // This test guards that a release still settles; it is deliberately not
    // sensitive enough to separate 45ms from 120ms, because in that window the
    // authored curve dominates the value.
    //
    // Settle in a window clear of the procedural blink (4.3s period, ~0.2s long,
    // and it multiplies the eye values after the blend), so this measures the
    // release rather than a blink.
    const double settledOpen = motion.values().value(eyeOpen);
    const double settledSmile = motion.values().value(eyeSmile);
    advanceFrames(motion, 80); // 1.6s: past any blink, well inside recovery.
    const double movedOpen = motion.values().value(eyeOpen) - settledOpen;
    const double movedSmile = settledSmile - motion.values().value(eyeSmile);
    QVERIFY2(movedOpen > 0.5 && movedSmile > 0.4,
             qPrintable(QStringLiteral("released face barely moved: eyeOpen +%1 eyeSmile -%2")
                            .arg(movedOpen).arg(movedSmile)));

    advanceFrames(motion, 40);
    QVERIFY(motion.values().value(eyeOpen) > 0.9);
    QVERIFY(motion.values().value(eyeSmile) < 0.1);
}

void ParameterMotionTest::headPatReleaseDoesNotAccelerateUnownedBackgroundChannels() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    QFile busy(dir.filePath(QStringLiteral("busy-stand.motion.json")));
    QVERIFY(busy.open(QIODevice::WriteOnly));
    busy.write(R"({"duration":12,"mode":"additive","loop":true,"channels":{
        "ParamBodyAngleX":[{"type":"sine","amplitude":5,"frequency":7}],
        "ParamBodyAngleZ":[{"type":"sine","amplitude":4,"frequency":5}],
        "ParamShiftX":[{"type":"sine","amplitude":15,"frequency":3}],
        "ParamHairFront":[{"type":"sine","amplitude":0.8,"frequency":4}],
        "ParamHairBack":[{"type":"sine","amplitude":0.6,"frequency":5}]},
        "constants":{"ParamArmLA":9,"ParamArmRA":-9}})");
    busy.close();
    for (const bool working : {false, true}) {
        ParameterMotion reaction, reference;
        for (auto* motion : {&reaction, &reference}) {
            QVERIFY(motion->loadMotionLibrary(dir.path()));
            if (working) motion->forceStandingBusy();
            advanceFrames(*motion, 50);
        }
        QVERIFY(reaction.beginHeadPat(1));
        for (int frame = 0; frame < 45; ++frame) {
            reaction.advance(0.02);
            reference.advance(0.02);
        }
        reaction.endHeadPat();
        QVERIFY(reaction.headPatReleasing());
        // The fixture owns head/face only. The activity continues moving these
        // other channels: their values must match the player without a pat on
        // every release frame, including an ordinary Idle background.
        for (int frame = 0; frame < 45; ++frame) {
            reaction.advance(0.02);
            reference.advance(0.02);
            for (const auto& id : {QStringLiteral("ParamBodyAngleX"), QStringLiteral("ParamBodyAngleY"),
                 QStringLiteral("ParamBodyAngleZ"), QStringLiteral("ParamShiftX"),
                 QStringLiteral("ParamHairFront"), QStringLiteral("ParamHairBack"),
                 QStringLiteral("ParamArmLA"), QStringLiteral("ParamArmRA"), QStringLiteral("ParamBreath")})
                QCOMPARE(reaction.values().value(id), reference.values().value(id));
        }
        QVERIFY(!reaction.interactionActive());
    }
}

void ParameterMotionTest::headPatWhileBusyLeavesHandsAndComputerAlone() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion reaction, reference;
    for (auto* motion : {&reaction, &reference}) {
        QVERIFY(motion->loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
        QVERIFY(motion->loadMotionLibrary(dir.path()));
        motion->forceLaptopBusy();
        advanceFrames(*motion, 50);
    }
    QVERIFY(reaction.beginHeadPat());
    double typingLow = 1, typingHigh = 0;
    for (int frame = 0; frame < 100; ++frame) {
        reaction.advance(0.02);
        reference.advance(0.02);
        for (const auto& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
             QStringLiteral("ParamLaptopVisible"), QStringLiteral("ParamArmLA"), QStringLiteral("ParamArmRA"),
             QStringLiteral("ParamBusyTypingL"), QStringLiteral("ParamBusyTypingR")})
            QCOMPARE(reaction.values().value(id), reference.values().value(id));
        typingLow = std::min(typingLow, reaction.values().value(QStringLiteral("ParamBusyTypingR")));
        typingHigh = std::max(typingHigh, reaction.values().value(QStringLiteral("ParamBusyTypingR")));
        if (frame == 70) QVERIFY(reaction.interactionTime() > 1.6);
    }
    QVERIFY(typingHigh - typingLow > 0.2);
    QVERIFY(!reaction.interactionActive());
    QCOMPARE(reaction.library().clip(QStringLiteral("head-pat"))->approval(), QStringLiteral("pending"));
}

void ParameterMotionTest::reactionInterruptionsKeepPoseContinuous() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    for (const auto next : {PetController::State::Busy, PetController::State::Delete, PetController::State::Grass}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(dir.path()));
        QVERIFY(motion.playTurnEnded());
        advanceFrames(motion, 25);
        const auto before = motion.values();
        motion.setState(next);
        QVERIFY(!motion.interactionActive());
        QCOMPARE(motion.values(), before);
    }
    for (const auto next : {PetController::State::Delete, PetController::State::Grass}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(dir.path()));
        QVERIFY(motion.beginHeadPat());
        advanceFrames(motion, 50);
        const auto before = motion.values();
        motion.setState(next);
        QVERIFY(!motion.interactionActive());
        QCOMPARE(motion.values(), before);
        motion.setState(PetController::State::Idle);
        advanceFrames(motion, 60);
        QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) < 0.001);
        QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.9);
    }
}

void ParameterMotionTest::invalidReactionConfigurationIsRejected() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QFile invalid(dir.filePath(QStringLiteral("head-pat.motion.json")));
    QVERIFY(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate));
    invalid.write(R"({"duration":2.2,"interaction":{"enterEnd":1.6,"holdEnd":0.8},"constants":{"ParamAngleZ":3}})");
    invalid.close();
    QString error;
    QVERIFY(!motion.loadMotionLibrary(dir.path(), &error));
    QVERIFY(error.contains(QStringLiteral("enterEnd")));
    // Bad metadata must not replace the previous usable reaction.
    QVERIFY(motion.beginHeadPat());
    advanceFrames(motion, 50);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.8);
    motion.cancelInteraction();
    QVERIFY(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate));
    invalid.write(R"({"duration":2.2,"interaction":{"enterEnd":0.8,"holdEnd":1.6},"constants":{"ParamArmRA":40}})");
    invalid.close();
    QVERIFY(!motion.loadMotionLibrary(dir.path(), &error));
    QVERIFY(error.contains(QStringLiteral("ParamArmRA")));
}

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

void ParameterMotionTest::blendSecondsComeFromClip() {
    // A clip that declares a slower blend must actually slow that parameter down:
    // the blend times used to be hard-coded in advance().
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile clip(dir.filePath(QStringLiteral("idle.motion.json")));
    QVERIFY(clip.open(QIODevice::WriteOnly));
    clip.write(R"({"duration":8,"blendSeconds":0.12,
        "blendSecondsPerParameter":{"ParamAngleX":0.9},
        "constants":{"ParamAngleX":30.0,"ParamAngleY":30.0}})");
    clip.close();

    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    // Start both parameters from zero so the approach to 30 is observable.
    motion.setPreviewPose({{QStringLiteral("ParamAngleX"), 0.0}, {QStringLiteral("ParamAngleY"), 0.0}});
    motion.setState(PetController::State::Idle);
    motion.advance(0.02);
    const double slow = motion.values().value(QStringLiteral("ParamAngleX"));
    const double fast = motion.values().value(QStringLiteral("ParamAngleY"));
    // Same target, but X was declared with a 0.9 s blend against the 0.12 s default.
    QVERIFY(qAbs(fast - slow) > 1.0);
    QVERIFY(qAbs(slow - 30.0 * (1.0 - qExp(-0.02 / 0.9))) < 1e-9);
    QVERIFY(qAbs(fast - 30.0 * (1.0 - qExp(-0.02 / 0.12))) < 1e-9);
}

void ParameterMotionTest::standingAccentKeepsMovingAfterOneCycle() {
    // The standing accent is an additive loop, and it has to be sampled on the
    // busy clock. Sampling it on the global clock instead looked identical in
    // the approval captures -- those start a fresh process at t = 0 -- while
    // breaking the desktop pet completely: a track past its last key is clamped
    // rather than wrapped, so once the pet had been open for one duration every
    // accent curve sat frozen on its seam pose and the standing variant stopped
    // moving for the rest of the session. Run the clock well past one cycle
    // first, the way an open pet would, and then require the pose to still
    // travel inside a single busy cycle.
    ParameterMotion motion;
    QString error;
    QVERIFY2(motion.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
    for (int frame = 0; frame < 60 * 20; ++frame) motion.advance(1.0 / 60.0);
    motion.setBusyRandomSeed(20260930);
    motion.forceStandingBusy();
    double low = 1e9;
    double high = -1e9;
    for (int frame = 0; frame < 60 * 12; ++frame) {
        motion.advance(1.0 / 60.0);
        const double pitch = motion.values().value(QStringLiteral("ParamAngleY"));
        low = qMin(low, pitch);
        high = qMax(high, pitch);
    }
    // Idle alone sways the pitch by 1.5 degrees; the accent adds an order more.
    QVERIFY2(high - low > 12.0, qPrintable(QStringLiteral("%1..%2").arg(low).arg(high)));
}

QTEST_GUILESS_MAIN(ParameterMotionTest)
#include "ParameterMotionTest.moc"
