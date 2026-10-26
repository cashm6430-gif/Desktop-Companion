#include "../src/ParameterMotion.h"

#include <QtMath>
#include <QtTest/QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

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
    const QByteArray stretch = R"({"duration":0.6,"approval":"approved","keyframes":[
      {"time":0,"parameters":{"ParamBusyLaptop":1,"ParamSitPose":1,"ParamDeskVisible":1,"ParamLaptopVisible":1,
        "ParamBusyTypingL":0,"ParamBusyTypingR":0,"ParamAngleY":-8,"ParamEyeLOpen":0.9,"ParamEyeROpen":0.9,
        "ParamSmileOpen":0,"ParamMouthGape":0}},
      {"time":0.3,"parameters":{"ParamBusyLaptop":1,"ParamSitPose":1,"ParamDeskVisible":1,"ParamLaptopVisible":1,
        "ParamBusyTypingL":0,"ParamBusyTypingR":0,"ParamAngleY":10,"ParamEyeLOpen":1,"ParamEyeROpen":0,
        "ParamSmileOpen":1,"ParamMouthGape":0}},
      {"time":0.6,"parameters":{"ParamBusyLaptop":1,"ParamSitPose":1,"ParamDeskVisible":1,"ParamLaptopVisible":1,
        "ParamBusyTypingL":0,"ParamBusyTypingR":0,"ParamAngleY":-8,"ParamEyeLOpen":0.9,"ParamEyeROpen":0.9,
        "ParamSmileOpen":0,"ParamMouthGape":0}}]})";
    for (const auto& entry : {qMakePair(QStringLiteral("turn-ended"), turn),
                              qMakePair(QStringLiteral("head-pat"), pat),
                              qMakePair(QStringLiteral("busy-stretch"), stretch)}) {
        QFile file(dir.filePath(entry.first + QStringLiteral(".motion.json")));
        if (!file.open(QIODevice::WriteOnly) || file.write(entry.second) != entry.second.size()) return false;
    }
    return true;
}

bool writeStandingRestFixture(const QTemporaryDir& dir, const QString& approval = QStringLiteral("pending")) {
    const QByteArray source = R"({"duration":2.6,"approval":"pending","constants":{"ParamAngleZ":0},"keyframes":[
      {"time":0,"parameters":{"ParamAngleX":0,"ParamAngleY":0,"ParamEyeLOpen":1,"ParamEyeROpen":1,
        "ParamEyeSmile":0,"ParamEyeBallX":0,"ParamBodyAngleX":0,"ParamBodyAngleZ":0,"ParamBreath":0.3}},
      {"time":0.9,"parameters":{"ParamAngleX":-2,"ParamAngleY":4,"ParamEyeLOpen":0.1,"ParamEyeROpen":0.1,
        "ParamEyeSmile":0.2,"ParamEyeBallX":-0.4,"ParamBodyAngleX":2.5,"ParamBodyAngleZ":1.4,"ParamBreath":1}},
      {"time":2.6,"parameters":{"ParamAngleX":0,"ParamAngleY":0,"ParamEyeLOpen":1,"ParamEyeROpen":1,
        "ParamEyeSmile":0,"ParamEyeBallX":0,"ParamBodyAngleX":0,"ParamBodyAngleZ":0,"ParamBreath":0.3}}]})";
    auto root = QJsonDocument::fromJson(source).object();
    root.insert(QStringLiteral("approval"), approval);
    const QByteArray json = QJsonDocument(root).toJson();
    QFile file(dir.filePath(QStringLiteral("busy-rest.motion.json")));
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(json) == json.size();
}

bool writePermissionCueFixture(const QTemporaryDir& dir, const QString& approval) {
    QFile source(QStringLiteral("assets/motions/permission-cue.motion.json"));
    if (!source.open(QIODevice::ReadOnly)) return false;
    auto root = QJsonDocument::fromJson(source.readAll()).object();
    root.insert(QStringLiteral("approval"), approval);
    const QByteArray json = QJsonDocument(root).toJson();
    QFile target(dir.filePath(QStringLiteral("permission-cue.motion.json")));
    return target.open(QIODevice::WriteOnly | QIODevice::Truncate) && target.write(json) == json.size();
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

bool writeStationaryGrassInteractionFixture(const QTemporaryDir& dir) {
    if (!writeGrassInteractionFixture(dir)) return false;
    QFile file(dir.filePath(QStringLiteral("grass-touch.motion.json")));
    if (!file.open(QIODevice::ReadOnly)) return false;
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto keys = root.value(QStringLiteral("keyframes")).toArray();
    for (int i = 0; i < keys.size(); ++i) {
        auto key = keys.at(i).toObject();
        auto parameters = key.value(QStringLiteral("parameters")).toObject();
        parameters.insert(QStringLiteral("ParamWristRA"), 0.0);
        key.insert(QStringLiteral("parameters"), parameters);
        keys[i] = key;
    }
    root.insert(QStringLiteral("keyframes"), keys);
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
    void workPosturePreferenceDoesNotStartWorkOrChangeIdle();
    void workPostureFixedChoicesKeepDelayAndChoiceClock();
    void workPostureAutomaticResumesAtNextScheduledChoice();
    void workPostureWaitsForForegroundOwners();
    void workPostureWaitsForLastPauseFrameAndPostureRamps();
    void workPostureWaitsForWaveBoxAndForegroundStates();
    void workPosturePreviewDoesNotChangeSavedPreference();
    void laptopTypingAndShortTaskExit();
    void deskSitPrecedesCoverAndLeavesLast();
    void deskSitWaitsForSustainedWork();
    void workCompanionBeatsReadAndRestore();
    void deskShortTasksReverseWithoutDesk();
    void deskVariantChangesAndReactionsKeepCover();
    void deskInterruptionsKeepActionHandsAndDeadlines();
    void deskDisabledPreservesExistingTrajectory();
    void deskVisibilityIsReservedForActivity();
    void blendSecondsComeFromClip();
    void standingAccentKeepsMovingAfterOneCycle();
    void absentReactionsLeaveExistingMotionAvailable();
    void turnEndedPreservesSeatThenPutsLaptopAway();
    void turnEndedYieldsToUserOverlaysWithoutReplay();
    void stretchPausePlaysOverSeatedLaptopOnlyAndResumesTyping();
    void stretchPokeSkipsToCaughtRecoveryAndResumesTyping();
    void workPauseDispatchRespectsApprovalAndSettledPosture();
    void workPauseFreezesClocksAndPreservesStandingPosture();
    void workPauseRefusesExistingOverlayWithoutPreempting();
    void workPauseInterruptionsDoNotReplay();
    void standingRestRejectsReservedAxesAndUnsafeSway();
    void permissionCueRequiresApprovalAndSettledPosture();
    void permissionCueFreezesClocksAndContactPose();
    void permissionCueRefusesOwnersAndYieldsWithoutReplay();
    void permissionCueRejectsReservedChannelsAndUnsafeHead();
    void dragMotionFollowsAndSettlesWithoutSway();
    void dragInputExpiresWithoutImaginaryTravel();
    void dragMeasuredPathRejectsInvalidSamplesAndTriggersOnce();
    void dragReleaseEyesStayContinuousAndVerticalTailCompletes();
    void dragInputTimingIsStableAndInterruptionsDoNotReplay();
    void memoGazeFollowsNoteRise();
    void boxPeekSinksPeeksAndPopsInOrder();
    void riceBowlSniffsSmilesAndLeaves();
    void riceBowlPokeLeansAwayAndPeeksBack();
    void riceBowlYieldsToDelete();
    void waveRaisesSwingsAndSettles();
    void waveRefusesBusyAndYieldsToDelete();
    void waveLeavingIdleReleasesWithoutPop();
    void userHooksPreemptSpontaneousScenes();
    void boxPeekCaughtBlinksDucksAndRepeeksSmiling();
    void boxPeekExitRestoresStandBeforeBoxLeaves();
    void boxPeekRefusesBusyAndYieldsToDelete();
    void sleepMotionNapsWakesAndYieldsToEvents();
    void headPatHoldReleaseDirectionAndCooldown();
    void headPatWhileBusyLeavesHandsAndComputerAlone();
    void reactionInterruptionsKeepPoseContinuous();
    void invalidReactionConfigurationIsRejected();
    void seatedReactionContextSurvivesUserAndLastStopHandoffs();
    void earlyHeadPatReleaseDoesNotForceClosedEyes();
    void headPatReleaseRestoresAuthoredFacePace();
    void headPatReleaseDoesNotAccelerateUnownedBackgroundChannels();
    void realHeadPatDirectionsPreserveActivityAndRelease();
    void headPatRefusesExistingFaceOverlays();
    void realHeadPatYieldsToWorkAndForegroundEvents();
    void grassInteractionRespondsOnlyDuringHold();
    void grassInteractionTimeoutSkipsResponse();
    void grassInteractionIgnoresLegacyCompletionDeadline();
    void grassInteractionInterruptionsAndRestartStayContinuous();
    void grassInteractionRootAndSoftTipStayBounded();
    void grassInteractionConfigurationRejectsBadPhasesAndGrip();
    void grassTipDragRequiresExplicitDraftAndFiniteBoundedInput();
    void grassTipDragApprovalIsSeparateAndPreservesReviewedTrajectory();
    void grassTipDragDisabledAndZeroInputPreserveExactTrajectory();
    void grassTipDragLagsReboundsAndPreservesOtherChannels();
    void grassTipDragKeepsHoldDeadlineAndClearsOnInterruptions();
    void gazeFollowsCursorAndSettlesToCenter();
    void workReactionsYieldToForeground();
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

void ParameterMotionTest::grassTipDragRequiresExplicitDraftAndFiniteBoundedInput() {
    QTemporaryDir dir;
    QVERIFY(writeStationaryGrassInteractionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(!motion.beginGrassTipDrag(true));
    motion.setState(PetController::State::Grass);
    QVERIFY(motion.beginGrassInteraction());
    QVERIFY(!motion.beginGrassTipDrag(true)); // Enter is not a draggable phase.
    advanceFrames(motion, 210);
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("hold"));
    QVERIFY(!motion.beginGrassTipDrag()); // Approval of the old clip is insufficient.
    motion.updateGrassTipDrag(1.0);
    QCOMPARE(motion.grassTipDragOffset(), 0.0);
    QVERIFY(motion.beginGrassTipDrag(true));
    const auto before = motion.values();
    motion.updateGrassTipDrag(100.0);
    QCOMPARE(motion.grassTipDragOffset(), 0.35);
    QCOMPARE(motion.values(), before); // Input changes force, not the pose directly.
    QVERIFY(!motion.beginGrassTipDrag(true));
    QCOMPARE(motion.grassTipDragOffset(), 0.35);
    motion.updateGrassTipDrag(std::numeric_limits<double>::quiet_NaN());
    motion.updateGrassTipDrag(std::numeric_limits<double>::infinity());
    QCOMPARE(motion.grassTipDragOffset(), 0.35);
    motion.updateGrassTipDrag(-100.0);
    QCOMPARE(motion.grassTipDragOffset(), -0.35);
    for (int i = 0; i < 25; ++i) {
        motion.advance(0.02);
        for (const auto& value : motion.values()) QVERIFY(qIsFinite(value));
        QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamGrassSwing"))) <= 1.0);
        QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamGrassTipBend"))) <= 1.0);
    }
    motion.endGrassTipDrag();
    QVERIFY(!motion.grassTipDragActive());
    QVERIFY(motion.grassTipDragSettling());
    QCOMPARE(motion.grassTipDragOffset(), 0.0);
    motion.endGrassTipDrag(); // Duplicate release must not restart the clock.
    advanceFrames(motion, 61);
    QVERIFY(!motion.grassTipDragSettling());
    QVERIFY(motion.beginGrassTipDrag(true));
    motion.updateGrassTipDrag(1.0);
    QVERIFY(motion.respondToGrass());
    QVERIFY(!motion.grassTipDragActive());
    QVERIFY(!motion.grassTipDragSettling());
    QCOMPARE(motion.grassTipDragOffset(), 0.0);
    QVERIFY(!motion.beginGrassTipDrag(true));
    motion.setPreviewPose(motion.values());
    QVERIFY(!motion.beginGrassTipDrag(true));
}

void ParameterMotionTest::grassTipDragApprovalIsSeparateAndPreservesReviewedTrajectory() {
    QTemporaryDir approvedDir, draftDir;
    QVERIFY(writeStationaryGrassInteractionFixture(approvedDir));
    QVERIFY(writeStationaryGrassInteractionFixture(draftDir));
    const auto configureApproval = [&](const QJsonValue& tipDrag) {
        QFile file(approvedDir.filePath(QStringLiteral("grass-touch.motion.json")));
        if (!file.open(QIODevice::ReadOnly)) return false;
        auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        root.insert(QStringLiteral("approval"), QStringLiteral("approved"));
        auto interaction = root.value(QStringLiteral("interaction")).toObject();
        if (tipDrag.isUndefined()) interaction.remove(QStringLiteral("tipDrag"));
        else interaction.insert(QStringLiteral("tipDrag"), tipDrag);
        root.insert(QStringLiteral("interaction"), interaction);
        const auto bytes = QJsonDocument(root).toJson();
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    const auto approved = QJsonObject{{QStringLiteral("approval"), QStringLiteral("approved")},
                                     {QStringLiteral("revision"), 1}};
    const QList<QPair<QJsonValue, bool>> cases{
        {QJsonValue(QJsonValue::Undefined), false},
        {QJsonObject{{QStringLiteral("revision"), 1}}, false},
        {QJsonObject{{QStringLiteral("approval"), QStringLiteral("pending")},
                     {QStringLiteral("revision"), 1}}, false},
        {QJsonObject{{QStringLiteral("approval"), QStringLiteral("approved")},
                     {QStringLiteral("revision"), 2}}, false},
        {QJsonObject{{QStringLiteral("approval"), QStringLiteral("approved")},
                     {QStringLiteral("revision"), 1.5}}, false},
        {QJsonObject{{QStringLiteral("approval"), QStringLiteral("approved")}}, false},
        {approved, true}};
    for (const auto& entry : cases) {
        QVERIFY(configureApproval(entry.first));
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(approvedDir.path()));
        // The old click clip is approved in every case, including the missing
        // and pending tipDrag records. It cannot grant new input ownership.
        QCOMPARE(motion.library().clip(QStringLiteral("grass-touch"))->approval(), QStringLiteral("approved"));
        QCOMPARE(motion.grassTipDragApproved(), entry.second);
        motion.setState(PetController::State::Grass);
        QVERIFY(motion.beginGrassInteraction());
        QVERIFY(!motion.beginGrassTipDrag()); // Approval never bypasses Enter.
        advanceFrames(motion, 180);
        QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("hold"));
        QCOMPARE(motion.beginGrassTipDrag(), entry.second);
        if (!entry.second) QVERIFY(motion.beginGrassTipDrag(true));
    }
    ParameterMotion adopted, reviewed;
    for (auto* motion : {&adopted, &reviewed}) motion->setProceduralBlink(false);
    QVERIFY(adopted.loadMotionLibrary(approvedDir.path()));
    QVERIFY(reviewed.loadMotionLibrary(draftDir.path()));
    for (auto* motion : {&adopted, &reviewed}) {
        motion->setState(PetController::State::Grass);
        QVERIFY(motion->beginGrassInteraction());
        advanceFrames(*motion, 180);
    }
    QVERIFY(adopted.beginGrassTipDrag());
    QVERIFY(reviewed.beginGrassTipDrag(true));
    for (int i = 0; i < 300; ++i) {
        const double input = i < 20 ? 1.0 : -1.0;
        adopted.updateGrassTipDrag(input);
        reviewed.updateGrassTipDrag(input);
        if (i == 40) { adopted.endGrassTipDrag(); reviewed.endGrassTipDrag(); }
        adopted.advance(0.02);
        reviewed.advance(0.02);
        QCOMPARE(adopted.values(), reviewed.values());
        QCOMPARE(adopted.grassInteractionPhase(), reviewed.grassInteractionPhase());
    }
    QVERIFY(configureApproval(QJsonValue(QJsonValue::Undefined)));
    QVERIFY(adopted.loadMotionLibrary(approvedDir.path()));
    QVERIFY(!adopted.grassTipDragApproved()); // Successful reload cannot keep stale approval.
}

void ParameterMotionTest::grassTipDragDisabledAndZeroInputPreserveExactTrajectory() {
    QTemporaryDir dir;
    QVERIFY(writeStationaryGrassInteractionFixture(dir));
    for (const bool zeroForceDraft : {false, true}) {
        ParameterMotion motion, control;
        for (auto* sampler : {&motion, &control}) {
            QVERIFY(sampler->loadMotionLibrary(dir.path()));
            sampler->setProceduralBlink(false);
            sampler->setState(PetController::State::Grass);
            QVERIFY(sampler->beginGrassInteraction());
        }
        bool began = false;
        for (int i = 0; i < 500; ++i) {
            if (zeroForceDraft && !began && motion.grassInteractionPhase() == QStringLiteral("hold")) {
                QVERIFY(motion.beginGrassTipDrag(true));
                began = true;
            } else if (!zeroForceDraft) {
                QVERIFY(!motion.beginGrassTipDrag());
            }
            motion.updateGrassTipDrag(zeroForceDraft ? 0.0 : 100.0);
            if (i == 220) motion.endGrassTipDrag();
            motion.advance(0.02);
            control.advance(0.02);
            QCOMPARE(motion.values(), control.values());
            QCOMPARE(motion.grassInteractionPhase(), control.grassInteractionPhase());
            QCOMPARE(motion.grassInteractionTime(), control.grassInteractionTime());
            QCOMPARE(motion.consumeActionFinished(), control.consumeActionFinished());
        }
        QCOMPARE(began, zeroForceDraft);
        QVERIFY(!motion.grassTipDragActive());
        QVERIFY(!motion.grassTipDragSettling());
    }
}

void ParameterMotionTest::grassTipDragLagsReboundsAndPreservesOtherChannels() {
    QTemporaryDir dir;
    QVERIFY(writeStationaryGrassInteractionFixture(dir));
    QList<QPair<double, double>> finalOffsets;
    const QString bendAxis = QStringLiteral("ParamGrassSwing");
    const QString tipAxis = QStringLiteral("ParamGrassTipBend");
    for (const int rate : {30, 60, 120}) {
        ParameterMotion motion, control;
        const double dt = 1.0 / rate;
        for (auto* sampler : {&motion, &control}) {
            QVERIFY(sampler->loadMotionLibrary(dir.path()));
            sampler->setProceduralBlink(false);
            sampler->setState(PetController::State::Grass);
            QVERIFY(sampler->beginGrassInteraction());
            for (int i = 0; i < rate * 42 / 10; ++i) sampler->advance(dt);
        }
        QVERIFY(motion.beginGrassTipDrag(true));
        motion.updateGrassTipDrag(1.0);
        const auto samplePair = [&] {
            motion.advance(dt);
            control.advance(dt);
            const auto values = motion.values();
            const auto baseline = control.values();
            return qMakePair(values.value(bendAxis) - baseline.value(bendAxis),
                             values.value(tipAxis) - baseline.value(tipAxis));
        };
        auto offset = samplePair();
        QVERIFY(offset.first > 0.0);
        QVERIFY(offset.second < 0.0); // Tip trails the middle, rather than a rigid turn.
        const double tipDisplacement = offset.first + offset.second / 0.85;
        QVERIFY(tipDisplacement >= 0.0 && tipDisplacement < offset.first);
        for (int i = 1; i < rate * 4 / 10; ++i) samplePair();
        QVERIFY(motion.values().value(bendAxis) - control.values().value(bendAxis) > 0.3);
        motion.endGrassTipDrag();
        QVERIFY(motion.grassTipDragSettling());
        double minimumBend = 0.0, earlyPeak = 0.0, latePeak = 0.0;
        for (int i = 0; i < rate * 14 / 10; ++i) {
            offset = samplePair();
            minimumBend = std::min(minimumBend, offset.first);
            const double amplitude = std::max(qAbs(offset.first), qAbs(offset.second));
            if (i < rate / 2) earlyPeak = std::max(earlyPeak, amplitude);
            if (i >= rate * 9 / 10) latePeak = std::max(latePeak, amplitude);
            const auto values = motion.values();
            const auto baseline = control.values();
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                QVERIFY(qIsFinite(it.value()));
                if (it.key() == bendAxis || it.key() == tipAxis) {
                    QVERIFY(qAbs(it.value()) <= 1.0);
                } else {
                    // Covers palm grip, reach, shoulder/elbow/wrist, body and
                    // every face channel; the drag may own only flexibility.
                    QCOMPARE(it.value(), baseline.value(it.key()));
                }
            }
        }
        QVERIFY(minimumBend < -0.05); // Release crosses equilibrium once.
        QVERIFY(latePeak < earlyPeak * 0.4); // Subsequent oscillation decays.
        QVERIFY(!motion.grassTipDragActive());
        QVERIFY(!motion.grassTipDragSettling());
        QCOMPARE(motion.grassTipDragOffset(), 0.0);
        QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("hold"));
        finalOffsets.append(offset);
    }
    for (const auto& offset : finalOffsets) {
        QVERIFY(qAbs(offset.first - finalOffsets.first().first) < 0.025);
        QVERIFY(qAbs(offset.second - finalOffsets.first().second) < 0.025);
    }
}

void ParameterMotionTest::grassTipDragKeepsHoldDeadlineAndClearsOnInterruptions() {
    QTemporaryDir dir;
    QVERIFY(writeStationaryGrassInteractionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.setState(PetController::State::Grass);
    QVERIFY(motion.beginGrassInteraction());
    advanceFrames(motion, 210);
    QVERIFY(motion.beginGrassTipDrag(true));
    motion.updateGrassTipDrag(1.0);
    advanceFrames(motion, 119);
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("hold"));
    QVERIFY(motion.grassTipDragActive());
    motion.advance(0.02); // Exactly 3 s total Hold, including captured time.
    QCOMPARE(motion.grassInteractionPhase(), QStringLiteral("timeout"));
    QVERIFY(!motion.grassTipDragActive());
    QVERIFY(!motion.grassTipDragSettling());
    QCOMPARE(motion.grassTipDragOffset(), 0.0);
    motion.updateGrassTipDrag(-1.0);
    QVERIFY(!motion.beginGrassTipDrag(true));
    advanceFrames(motion, 130);
    QVERIFY(!motion.grassInteractionActive());
    QVERIFY(motion.consumeActionFinished());

    for (const auto next : {PetController::State::Idle, PetController::State::Busy,
                           PetController::State::Delete, PetController::State::Grass}) {
        ParameterMotion subject, explicitlyCancelled;
        for (auto* sampler : {&subject, &explicitlyCancelled}) {
            QVERIFY(sampler->loadMotionLibrary(dir.path()));
            sampler->setProceduralBlink(false);
            sampler->setState(PetController::State::Grass);
            QVERIFY(sampler->beginGrassInteraction());
            advanceFrames(*sampler, 210);
            QVERIFY(sampler->beginGrassTipDrag(true));
            sampler->updateGrassTipDrag(1.0);
            advanceFrames(*sampler, 10);
        }
        explicitlyCancelled.cancelGrassTipDrag();
        const auto pose = subject.values();
        subject.setState(next);
        explicitlyCancelled.setState(next);
        QCOMPARE(subject.values(), pose); // No pose jump at the ownership handoff.
        QVERIFY(!subject.grassTipDragActive());
        QVERIFY(!subject.grassTipDragSettling());
        QCOMPARE(subject.grassTipDragOffset(), 0.0);
        for (int i = 0; i < 50; ++i) {
            subject.updateGrassTipDrag(-1.0); // Stale mouse input cannot replay.
            subject.advance(0.02);
            explicitlyCancelled.advance(0.02);
            QCOMPARE(subject.values(), explicitlyCancelled.values());
        }
        for (auto* sampler : {&subject, &explicitlyCancelled}) {
            sampler->setState(PetController::State::Grass);
            QVERIFY(sampler->beginGrassInteraction());
            advanceFrames(*sampler, 190);
        }
        QVERIFY(!subject.grassTipDragActive());
        QCOMPARE(subject.grassTipDragOffset(), 0.0);
        QCOMPARE(subject.values(), explicitlyCancelled.values());
        QVERIFY(subject.beginGrassTipDrag(true));
        subject.updateGrassTipDrag(1.0);
        subject.setPreviewPose(subject.values());
        QVERIFY(!subject.grassTipDragActive());
        QVERIFY(!subject.grassTipDragSettling());
        QCOMPARE(subject.grassTipDragOffset(), 0.0);
    }
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

void ParameterMotionTest::turnEndedYieldsToUserOverlaysWithoutReplay() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    for (const QString owner : {QStringLiteral("memo"), QStringLiteral("sleep"), QStringLiteral("drag")}) {
        ParameterMotion motion;
        ParameterMotion reference;
        for (ParameterMotion* player : {&motion, &reference}) {
            QVERIFY(player->loadMotionLibrary(dir.path()));
            advanceFrames(*player, 50);
            if (owner == QStringLiteral("memo")) player->beginMemoMotion();
            else if (owner == QStringLiteral("sleep")) player->beginSleepMotion();
            else {
                player->beginDragMotion();
                player->updateDragMotion(320.0, -80.0);
            }
            advanceFrames(*player, 12);
        }
        const auto before = motion.values();
        QVERIFY2(!motion.playTurnEnded(), qPrintable(owner));
        QCOMPARE(motion.values(), before);
        QVERIFY(!motion.interactionActive());
        if (owner == QStringLiteral("memo")) QVERIFY(motion.memoMotionActive());
        else if (owner == QStringLiteral("sleep")) QVERIFY(motion.sleepMotionActive());
        else QVERIFY(motion.dragMotionActive());

        // A refused cue leaves the existing owner's actual trajectory intact.
        for (int frame = 0; frame < 12; ++frame) {
            motion.advance(0.02);
            reference.advance(0.02);
            QCOMPARE(motion.values(), reference.values());
            QVERIFY(!motion.interactionActive());
        }
        for (ParameterMotion* player : {&motion, &reference}) {
            if (owner == QStringLiteral("memo")) player->cancelMemoMotion();
            else if (owner == QStringLiteral("sleep")) player->cancelSleepMotion();
            else player->cancelDragMotion();
        }
        // Releasing an owner never replays a discarded work-end cue.
        for (int frame = 0; frame < 100; ++frame) {
            motion.advance(0.02);
            reference.advance(0.02);
            QVERIFY(!motion.interactionActive());
            QCOMPARE(motion.values(), reference.values());
        }
        QVERIFY(motion.playTurnEnded());
        QCOMPARE(motion.interactionId(), QStringLiteral("turn-ended"));
        advanceFrames(motion, 35);
        QVERIFY(motion.interactionActive());
        QVERIFY(motion.values().value(QStringLiteral("ParamAngleY")) > 1.0);
        advanceFrames(motion, 60);
        QVERIFY(!motion.interactionActive());
    }
}

void ParameterMotionTest::dragMotionFollowsAndSettlesWithoutSway() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    advanceFrames(motion, 50);
    QVERIFY(!motion.dragMotionActive());
    motion.beginDragMotion();
    QVERIFY(motion.dragMotionActive());
    // Sustained rightward drag (short of the curious look-back threshold):
    // the eyes follow, hair and body lag opposite.
    for (int i = 0; i < 18; ++i) {
        motion.updateDragMotion(600.0, 0.0, 20.0);
        motion.advance(1.0 / 30.0);
    }
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) > 0.5);
    QVERIFY(motion.values().value(QStringLiteral("ParamHairFront")) < -0.4);
    QVERIFY(motion.values().value(QStringLiteral("ParamBodyAngleX")) < -2.0);
    // Crossing the long-drag threshold spends the one curious look-back:
    // the eyes come off the motion toward the user with a soft smile.
    const double eyeDuringFollow = motion.values().value(QStringLiteral("ParamEyeBallX"));
    for (int i = 0; i < 10; ++i) {
        motion.updateDragMotion(600.0, 0.0, 20.0);
        motion.advance(1.0 / 30.0);
    }
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.3);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) < eyeDuringFollow);
    // Release: exponential decay only -- the magnitude never grows back.
    motion.endDragMotion();
    // Half a second after release the body lean must still be plainly visible
    // (this was the "too small to notice" feedback): the slow 0.45s decay
    // keeps most of the lean alive through the look-back window.
    advanceFrames(motion, 15);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamBodyAngleX"))) > 2.0);
    double previousMag = std::abs(motion.values().value(QStringLiteral("ParamEyeBallX")));
    for (int i = 0; i < 30; ++i) {
        motion.advance(1.0 / 30.0);
        const double mag = std::abs(motion.values().value(QStringLiteral("ParamEyeBallX")));
        QVERIFY(mag <= previousMag + 1e-9);
        previousMag = mag;
    }
    // Fully settled: back to rest, no residual offset, no perpetual sway.
    advanceFrames(motion, 120);
    QVERIFY(!motion.dragMotionActive());
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamEyeBallX"))) < 0.05);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamHairFront"))) < 0.05);
    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamBodyAngleX"))) < 0.5);
}

void ParameterMotionTest::dragInputExpiresWithoutImaginaryTravel() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion, background;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(background.loadMotionLibrary(dir.path()));
    motion.setProceduralBlink(false);
    background.setProceduralBlink(false);
    motion.beginDragMotion();
    for (int i = 0; i < 18; ++i) {
        motion.updateDragMotion(600.0, -200.0, 10.0);
        motion.advance(1.0 / 60.0);
        background.advance(1.0 / 60.0);
    }
    QCOMPARE(motion.dragTravelledDistance(), 180.0);
    QVERIFY(!motion.dragCuriousTriggered());
    QVERIFY(motion.values().value(QStringLiteral("ParamBodyAngleX")) < -2.0);
    // Still holding, but no new pointer sample: the old speed must neither
    // earn a long-drag reaction nor keep the pet leaning indefinitely.
    for (int i = 0; i < 60; ++i) {
        motion.advance(1.0 / 60.0);
        background.advance(1.0 / 60.0);
    }
    QVERIFY(motion.dragFollowing());
    QVERIFY(motion.dragInputAge() >= 1.0);
    QCOMPARE(motion.dragTravelledDistance(), 180.0);
    QVERIFY(!motion.dragCuriousTriggered());
    QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamEyeBallX"))) < 0.02);
    QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamHairFront"))) < 0.04);
    QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamBodyAngleX"))
                 - background.values().value(QStringLiteral("ParamBodyAngleX"))) < 0.15);
    for (int i = 0; i < 300; ++i) motion.advance(1.0 / 60.0);
    QCOMPARE(motion.dragTravelledDistance(), 180.0);
    QVERIFY(!motion.dragCuriousTriggered());
    motion.cancelDragMotion();
    QVERIFY(!motion.dragMotionActive());
    QCOMPARE(motion.dragInputAge(), 0.0);
    QCOMPARE(motion.dragTravelledDistance(), 0.0);
}

void ParameterMotionTest::dragMeasuredPathRejectsInvalidSamplesAndTriggersOnce() {
    DragBehavior drag;
    drag.begin();
    drag.update(600.0, 100.0, 100.0);
    drag.advance(0.05);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto& bad : QList<QList<double>>{{nan, 0, 50}, {0, inf, 50},
                                               {0, 0, nan}, {0, 0, inf}, {0, 0, -1}}) {
        drag.update(bad[0], bad[1], bad[2]);
        QCOMPARE(drag.inputAge(), 0.05); // Rejected input does not refresh a stale sample.
        QCOMPARE(drag.travelledDistance(), 100.0);
    }
    drag.advance(nan);
    drag.advance(inf);
    drag.advance(-1.0);
    QCOMPARE(drag.inputAge(), 0.05);
    drag.update(0, 0, 300.0);
    QCOMPARE(drag.travelledDistance(), 400.0);
    QVERIFY(!drag.curiousTriggered());
    drag.advance(2.0);
    QCOMPARE(drag.travelledDistance(), 400.0);
    QVERIFY(!drag.curiousTriggered());
    drag.update(0, 0, 1.0);
    QVERIFY(drag.curiousTriggered());
    drag.advance(0.05);
    DragBehavior::Parameters pose;
    drag.apply(pose);
    QCOMPARE(pose.value(QStringLiteral("ParamEyeBallX")), 0.0);
    QCOMPARE(pose.value(QStringLiteral("ParamEyeBallY")), 0.1);
    QVERIFY(pose.value(QStringLiteral("ParamEyeSmile")) >= 0.35);
    // Real additional movement after the look-back has elapsed cannot start
    // another one within the same drag.
    drag.advance(0.7);
    drag.update(0, 0, 500.0);
    drag.advance(0.05);
    pose.clear();
    drag.apply(pose);
    QCOMPARE(drag.travelledDistance(), 901.0);
    QCOMPARE(pose.value(QStringLiteral("ParamEyeSmile")), 0.0);
    drag.update(std::numeric_limits<double>::max(), 0, std::numeric_limits<double>::max());
    drag.update(0, 0, std::numeric_limits<double>::max());
    QVERIFY(qIsFinite(drag.travelledDistance()));
    drag.advance(0.05);
    pose.clear();
    drag.apply(pose);
    for (double value : pose) QVERIFY(qIsFinite(value));
    drag.cancel();
    drag.begin();
    QCOMPARE(drag.travelledDistance(), 0.0);
    QCOMPARE(drag.inputAge(), 0.0);
    QVERIFY(!drag.curiousTriggered());
}

void ParameterMotionTest::dragReleaseEyesStayContinuousAndVerticalTailCompletes() {
    const QString x = QStringLiteral("ParamEyeBallX"), y = QStringLiteral("ParamEyeBallY");
    const DragBehavior::Parameters background{{x, 0.22}, {y, -0.08}};
    for (const bool releaseDuringCurious : {false, true}) {
        DragBehavior drag;
        drag.begin();
        DragBehavior::Parameters before;
        for (int i = 0; i < 20; ++i) {
            drag.update(600.0, 300.0, 5.0);
            drag.advance(1.0 / 60.0);
            before = background;
            drag.apply(before);
        }
        if (releaseDuringCurious) {
            drag.update(600.0, 300.0, 401.0);
            drag.advance(1.0 / 60.0);
            before = background;
            drag.apply(before);
            QCOMPARE(before.value(x), 0.0);
            QCOMPARE(before.value(y), 0.1);
        } else QVERIFY(before.value(x) > 0.7);
        drag.end();
        auto released = background;
        drag.apply(released);
        // No time has passed: release must not replace the visible look with
        // either neutral eyes or an old pre-curious direction.
        QCOMPARE(released.value(x), before.value(x));
        QCOMPARE(released.value(y), before.value(y));
        double previousX = qAbs(released.value(x) - background.value(x));
        double previousY = qAbs(released.value(y) - background.value(y));
        for (int i = 0; i < 180; ++i) {
            drag.advance(1.0 / 60.0);
            released = background;
            drag.apply(released);
            const double dx = qAbs(released.value(x) - background.value(x));
            const double dy = qAbs(released.value(y) - background.value(y));
            QVERIFY(dx <= previousX + 1e-12);
            QVERIFY(dy <= previousY + 1e-12);
            if (releaseDuringCurious) QVERIFY(released.value(x) <= background.value(x));
            previousX = dx; previousY = dy;
        }
        QVERIFY(!drag.active());
        QCOMPARE(released.value(x), background.value(x));
        QCOMPARE(released.value(y), background.value(y));
    }

    DragBehavior vertical;
    vertical.begin();
    DragBehavior::Parameters pose;
    for (int i = 0; i < 20; ++i) {
        vertical.update(0, 900.0, 5.0);
        vertical.advance(1.0 / 60.0);
        pose.clear(); vertical.apply(pose);
    }
    QVERIFY(pose.value(y) > 0.2);
    vertical.end();
    for (int i = 0; i < 42; ++i) vertical.advance(1.0 / 60.0);
    QVERIFY(vertical.active()); // The 0.7 s nod ending must not truncate the vertical eye tail.
    pose.clear(); vertical.apply(pose);
    QVERIFY(pose.value(y) > 0.02);
    for (int i = 0; i < 120; ++i) vertical.advance(1.0 / 60.0);
    QVERIFY(!vertical.active());
    pose.clear(); vertical.apply(pose);
    QCOMPARE(pose.value(y), 0.0);
}

void ParameterMotionTest::dragInputTimingIsStableAndInterruptionsDoNotReplay() {
    QList<DragBehavior::Parameters> samples;
    for (int rate : {10, 30, 60, 120}) {
        DragBehavior drag;
        drag.begin();
        drag.update(600.0, 450.0, 37.0);
        drag.advance(1.0 / rate);
        DragBehavior::Parameters first;
        drag.apply(first);
        QVERIFY(first.value(QStringLiteral("ParamEyeBallX")) > 0.0);
        if (rate == 10) {
            // A fresh sample still gets the whole first 0.1 s frame. Only
            // subsequent frames have a zero input target.
            QVERIFY(first.value(QStringLiteral("ParamEyeBallX")) > 0.3);
            drag.advance(0.1);
            DragBehavior::Parameters expired;
            drag.apply(expired);
            QVERIFY(expired.value(QStringLiteral("ParamEyeBallX")) < first.value(QStringLiteral("ParamEyeBallX")));
            for (int i = 2; i < rate * 8 / 10; ++i) drag.advance(1.0 / rate);
        } else {
            for (int i = 1; i < rate * 8 / 10; ++i) drag.advance(1.0 / rate);
        }
        DragBehavior::Parameters after;
        drag.apply(after);
        QCOMPARE(drag.travelledDistance(), 37.0);
        QVERIFY(!drag.curiousTriggered());
        QVERIFY(qAbs(drag.inputAge() - 0.8) < 1e-9);
        samples.append(after);
    }
    for (const auto& sample : samples)
        for (auto it = sample.cbegin(); it != sample.cend(); ++it)
            QVERIFY(qAbs(it.value() - samples.front().value(it.key())) < 1e-9);

    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    for (const auto state : {PetController::State::Delete, PetController::State::Grass}) {
        ParameterMotion subject, control;
        for (auto* player : {&subject, &control}) {
            QVERIFY(player->loadMotionLibrary(dir.path()));
            player->setProceduralBlink(false);
            player->beginDragMotion();
            player->updateDragMotion(600.0, 100.0, 80.0);
            player->advance(0.05);
            player->setState(state);
            QVERIFY(!player->dragMotionActive());
            QCOMPARE(player->dragInputAge(), 0.0);
            QCOMPARE(player->dragTravelledDistance(), 0.0);
        }
        for (int i = 0; i < 60; ++i) {
            subject.updateDragMotion(600.0, 100.0, 50.0); // Stale input after ownership loss cannot replay.
            subject.advance(1.0 / 60.0);
            control.advance(1.0 / 60.0);
            QCOMPARE(subject.values(), control.values());
            QVERIFY(!subject.dragMotionActive());
            QVERIFY(!subject.dragCuriousTriggered());
        }
        subject.beginDragMotion();
        QVERIFY(subject.dragFollowing());
        QCOMPARE(subject.dragTravelledDistance(), 0.0);
        QVERIFY(!subject.dragCuriousTriggered());
    }
}

void ParameterMotionTest::memoGazeFollowsNoteRise() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    advanceFrames(motion, 50);
    QVERIFY(!motion.memoMotionActive());
    motion.beginMemoMotion();
    QVERIFY(motion.memoMotionActive());
    // First beat: glance down at the desk edge while the note appears.
    QCOMPARE(motion.memoBubbleRise(), 0.0);
    advanceFrames(motion, 10); // 0.2s, including the generic value blend lag
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallY")) < -0.3);
    // The gaze rides the note up as it floats to half a head high.
    advanceFrames(motion, 24); // 0.72s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallY")) > -0.35);
    QVERIFY(motion.memoBubbleRise() > 0.2 && motion.memoBubbleRise() < 0.9);
    // Arrival on creation: a soft smile only -- the laugh is the completion
    // reward, so the eyes stay open and the mouth stays at rest.
    advanceFrames(motion, 49); // 1.70s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallY")) > 0.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.3);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) < 0.7);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.7);
    QVERIFY(motion.values().value(QStringLiteral("ParamMouthGape")) < 0.1);
    // The overlay ends by itself; the window layer keeps the note art up.
    advanceFrames(motion, 30); // 1.82s
    QVERIFY(!motion.memoMotionActive());
    // Busy typing keeps playing underneath: the memo owns only the gaze.
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) > 0.99);
    motion.beginMemoMotion();
    advanceFrames(motion, 10);
    QVERIFY(motion.memoMotionActive());
    QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) > 0.99);
    // Completion beat: O-mouth laugh with closed smiling eyes and a nod.
    motion.beginMemoCelebrate();
    advanceFrames(motion, 12); // 0.24s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.6);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.5);
    QVERIFY(motion.values().value(QStringLiteral("ParamMouthGape")) > 0.6);
    QVERIFY(motion.values().value(QStringLiteral("ParamAngleY")) < -1.0);
    advanceFrames(motion, 30); // 0.84s
    QVERIFY(!motion.memoMotionActive());
}

void ParameterMotionTest::boxPeekSinksPeeksAndPopsInOrder() {
    // No clips needed: the box choreography is behavior-driven over Idle.
    ParameterMotion motion;
    advanceFrames(motion, 10);
    QVERIFY(!motion.boxPeekActive());
    QVERIFY(motion.playBoxPeek());
    QVERIFY(motion.boxPeekActive());
    // Slide-in leg (0.7s): the box arrives while the body stays at rest.
    advanceFrames(motion, 35); // 0.70s
    QCOMPARE(motion.boxPeekSlide(), 1.0);
    QCOMPARE(motion.boxPeekDuck(), 0.0);
    // Sink (0.65s): the eyes drop toward the box edge before anything peeks.
    advanceFrames(motion, 16); // 0.32s in
    QVERIFY(motion.boxPeekDuck() > 30.0 && motion.boxPeekDuck() < 70.0);
    advanceFrames(motion, 17); // 0.66s: sink done, eyes peek begins
    QVERIFY(motion.boxPeekDuck() > 90.0);
    // Eyes peek (0.5s): gaze lifts over the edge while the duck eases to it.
    advanceFrames(motion, 4); // 0.09s into the peek
    QVERIFY(motion.boxPeekDuck() > 90.0);
    advanceFrames(motion, 18); // 0.36s into the peek
    QVERIFY(motion.boxPeekDuck() > 85.0 && motion.boxPeekDuck() < 95.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallY")) > 0.4);
    QVERIFY(motion.values().value(QStringLiteral("ParamBoxDuck")) > 80.0);
    // Pop (0.7s): the head rises exactly back to the standing height -- the
    // old -80 overshoot clipped the ahoge at the window top (user r2), and
    // the stand already clears the box top by ~85px.
    advanceFrames(motion, 20); // 0.40s into the pop
    QVERIFY(motion.boxPeekDuck() > 5.0 && motion.boxPeekDuck() < 60.0);
    advanceFrames(motion, 18); // 0.76s: pop done, hold at stand height
    QVERIFY(motion.boxPeekDuck() < 5.0);
    // Hold, then the loop sinks back for the next peek by itself.
    advanceFrames(motion, 85); // deep in the first hold (2.4s)
    QVERIFY(motion.boxPeekDuck() < 1.0); // still holding at stand height
    advanceFrames(motion, 45); // sinking back down toward the box
    QVERIFY(motion.boxPeekDuck() > 10.0 && motion.boxPeekDuck() < 40.0);
    advanceFrames(motion, 25); // the eyes beat of the next cycle
    QVERIFY(motion.boxPeekActive());
    QVERIFY(motion.boxPeekDuck() > 90.0); // eyes beat of the next cycle
}

void ParameterMotionTest::riceBowlSniffsSmilesAndLeaves() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    // Idle refuses: the bowl lands on the desk of the seated laptop loop.
    QVERIFY(!motion.playRiceBowl());
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    QVERIFY(motion.playRiceBowl());
    QVERIFY(!motion.playRiceBowl());
    // Slide-in: the bowl arrives while the gaze snaps toward it (capped low).
    advanceFrames(motion, 15); // 0.30s
    QVERIFY(motion.riceBowlSlide() > 0.3 && motion.riceBowlSlide() < 1.0);
    advanceFrames(motion, 15); // 0.60s
    QCOMPARE(motion.riceBowlSlide(), 1.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) > 0.15);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) <= 0.31);
    // Sniff: the eyes close while the delight floaters bloom.
    advanceFrames(motion, 25); // 1.10s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.5);
    QVERIFY(motion.riceBowlDelight() > 0.7);
    // Smile: eyes reopen into the omega mouth (phase edges quantize to the
    // 20ms frame, so every assertion sits mid-beat, never on a boundary).
    advanceFrames(motion, 50); // 2.10s
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) > 0.7);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.8);
    advanceFrames(motion, 20); // 2.50s: the happy hold
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) > 0.9);
    advanceFrames(motion, 90); // 4.40s: still holding
    QVERIFY(motion.riceBowlActive());
    advanceFrames(motion, 35); // 5.10s: the timeout exit is sliding the bowl off
    QVERIFY(motion.riceBowlActive()); // the polite exit relaxes the face first
    QVERIFY(motion.riceBowlSlide() < 1.0);
    advanceFrames(motion, 55); // 6.20s: gone
    QVERIFY(!motion.riceBowlActive());
    QCOMPARE(motion.riceBowlSlide(), 0.0);
    // The busy clock survived the break: typing is back.
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingL")) > 0.001);
}

void ParameterMotionTest::riceBowlPokeLeansAwayAndPeeksBack() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.playRiceBowl());
    advanceFrames(motion, 60); // 1.20s: deep in the sniff
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.5);
    // A click on the aroma zone: she leans away from the bowl.
    motion.riceBowlPoke();
    advanceFrames(motion, 15); // 1.50s: the lean bottoms out
    QVERIFY(motion.values().value(QStringLiteral("ParamAngleY")) < 0.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) < 0.2);
    // Then the curious peek back with a half smile.
    if (qEnvironmentVariableIsSet("RICE_DEBUG")) {
        for (int i = 0; i < 45; ++i) {
            motion.advance(0.02);
            if (i % 5 == 0)
                fprintf(stderr, "pt=%.2f L=%.3f smile=%.3f angleY=%.2f\n",
                    1.50 + (i + 1) * 0.02,
                    motion.values().value(QStringLiteral("ParamEyeLOpen"), -1),
                    motion.values().value(QStringLiteral("ParamSmileOpen"), -1),
                    motion.values().value(QStringLiteral("ParamAngleY"), -99));
        }
    } else advanceFrames(motion, 35); // 2.14s: the peek settled into the hold
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) > 0.4);
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) < 0.9);
    QVERIFY(motion.riceBowlActive());
    // Poke during the peek back is harmless; the break still finishes.
    motion.riceBowlPoke();
    advanceFrames(motion, 240); // 6.00s: resumed hold over, bowl slid off
    QVERIFY(!motion.riceBowlActive());
}

void ParameterMotionTest::riceBowlYieldsToDelete() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.playRiceBowl());
    advanceFrames(motion, 40); // mid sniff
    // A delete lunge owns the stage: the bowl drops at once.
    motion.setState(PetController::State::Delete);
    QVERIFY(!motion.riceBowlActive());
    QCOMPARE(motion.riceBowlSlide(), 0.0);
    // Back at the seated desk, the break can play again. A polite exit
    // keeps the bowl covering while the face relaxes first.
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.playRiceBowl());
    advanceFrames(motion, 40);
    motion.exitRiceBowl();
    QVERIFY(motion.riceBowlExiting());
    QCOMPARE(motion.riceBowlSlide(), 1.0); // still covering during the relax
}

void ParameterMotionTest::waveRaisesSwingsAndSettles() {
    ParameterMotion motion;  // default standing idle
    QVERIFY(motion.playWave());
    QVERIFY(!motion.playWave());  // already waving
    // Raise: the arm lifts while the gaze finds the user first.
    advanceFrames(motion, 15); // 0.30s
    QVERIFY(motion.waveArm() > 15.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) > 0.05);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallX")) <= 0.31);
    advanceFrames(motion, 25); // 0.80s: first swing-out peak
    // The SHOULDER holds; the ELBOW swings the forearm up. Constant ParamArmRA
    // is what keeps the arm from telescoping -- ParamArmRA's keyforms scale the
    // arm, ParamElbowRA is a pure in-plane rotation about the elbow (WaveBehavior.cpp).
    QVERIFY(motion.waveArm() > 48.0 && motion.waveArm() < 56.0);  // shoulder held
    QVERIFY(motion.waveElbow() > 48.0);                           // forearm up
    QVERIFY(motion.waveWrist() > 5.0 && motion.waveWrist() < 10.0); // only follows (r2)
    advanceFrames(motion, 10); // 1.00s: swing-in leg
    QVERIFY(motion.waveElbow() < 30.0);                           // forearm down
    QVERIFY(motion.waveWrist() < 0.0);
    QVERIFY(motion.waveArm() > 48.0 && motion.waveArm() < 56.0);  // arm still held
    // Settle: the arm glides home while the eye smile blooms. The mouth
    // stays the idle closed omega -- the open grin is banned (user r2).
    advanceFrames(motion, 55); // 2.10s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.5);
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) < 0.01);
    QVERIFY(motion.waveArm() < 10.0);
    // Release: the eye smile relaxes instead of popping back to idle. The
    // face channels ride the blend filter, so give the settle a beat before
    // the zero check (the same lag that quantized the earlier phase edges).
    advanceFrames(motion, 40); // 2.90s
    QVERIFY(!motion.waveActive());
    QCOMPARE(motion.waveArm(), 0.0);
    advanceFrames(motion, 30); // 3.50s: the smile filter has fully drained
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) < 0.01);
}

void ParameterMotionTest::waveRefusesBusyAndYieldsToDelete() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    // Seated typing keeps the hands on the keyboard: no wave while busy.
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(!motion.playWave());
    QVERIFY(!motion.waveActive());
    // Back on her feet the wave plays, and a delete lunge drops the arm.
    motion.setState(PetController::State::Idle);
    advanceFrames(motion, 30);
    QVERIFY(motion.playWave());
    advanceFrames(motion, 20); // mid raise
    QVERIFY(motion.waveArm() > 20.0);
    motion.setState(PetController::State::Delete);
    QVERIFY(!motion.waveActive());
    QCOMPARE(motion.waveArm(), 0.0);  // interrupts win: instant drop
    QCOMPARE(motion.waveWrist(), 0.0);
}

void ParameterMotionTest::waveLeavingIdleReleasesWithoutPop() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(motion.playWave());
    advanceFrames(motion, 20); // 0.40s: nearly raised
    QVERIFY(motion.waveArm() > 30.0);
    // A new turn needs her back at work: the arm glides home, no pop.
    motion.setState(PetController::State::Busy);
    QVERIFY(motion.waveActive());  // the quick release is still playing
    advanceFrames(motion, 10); // 0.60s: gliding down
    QVERIFY(motion.waveArm() < 25.0);
    advanceFrames(motion, 20); // 1.00s: release finished
    QVERIFY(!motion.waveActive());
    QCOMPARE(motion.waveArm(), 0.0);
}

void ParameterMotionTest::userHooksPreemptSpontaneousScenes() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    advanceFrames(motion, 10);
    // A spontaneous wave is cut short the moment the user pats her: hooks
    // are commands, the director's scenes are filler (r2 trigger ruling).
    QVERIFY(motion.playWave());
    advanceFrames(motion, 10); // 0.20s: mid raise
    QVERIFY(motion.waveArm() > 5.0);
    QVERIFY(motion.beginHeadPat(0.3));
    QCOMPARE(motion.waveArm(), 0.0);  // instant stand-down, no overlap
    QVERIFY(!motion.waveActive());
    QCOMPARE(motion.interactionId(), QStringLiteral("head-pat"));
    motion.endHeadPat();
    advanceFrames(motion, 60); // the pat releases and drains
    // The turn hook likewise takes the stage away from a box peek.
    QVERIFY(motion.playBoxPeek());
    advanceFrames(motion, 45); // 0.90s: eyes at the box edge
    QVERIFY(motion.boxPeekActive());
    QVERIFY(motion.playTurnEnded());
    QVERIFY(!motion.boxPeekActive());
    QCOMPARE(motion.boxPeekDuck(), 0.0);
    QCOMPARE(motion.interactionId(), QStringLiteral("turn-ended"));
}

void ParameterMotionTest::boxPeekCaughtBlinksDucksAndRepeeksSmiling() {
    ParameterMotion motion;
    advanceFrames(motion, 10);
    QVERIFY(motion.playBoxPeek());
    advanceFrames(motion, 75); // 1.50s: mid eyes-peek, face at the box edge
    motion.boxPeekClicked();
    // The surprised blink lands while the body ducks back below the edge.
    // The face passes through the generic blend, so mid-blink the eyelids
    // are on their way down rather than fully shut.
    advanceFrames(motion, 3); // 1.56s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.7);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) < 0.7);
    QVERIFY(motion.boxPeekDuck() > 35.0);
    // The same-side smile re-peek: eyes over the edge, pleased about it.
    // Half a second in so the eye-smile blend has settled (the face channels
    // ride the exponential filter).
    advanceFrames(motion, 26); // 2.08s
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeSmile")) > 0.7);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeBallY")) > 0.4);
    QVERIFY(motion.boxPeekDuck() > 95.0 && motion.boxPeekDuck() < 110.0);
    // Then the head pops again as usual, back to the standing height.
    advanceFrames(motion, 40); // 2.62s: the pop is gliding home
    QVERIFY(motion.boxPeekDuck() > 15.0 && motion.boxPeekDuck() < 60.0);
    advanceFrames(motion, 35); // 3.32s: popped = stand, hold
    QVERIFY(motion.boxPeekDuck() < 5.0);
    QVERIFY(motion.boxPeekActive());
}

void ParameterMotionTest::boxPeekExitRestoresStandBeforeBoxLeaves() {
    ParameterMotion motion;
    advanceFrames(motion, 10);
    QVERIFY(motion.playBoxPeek());
    advanceFrames(motion, 95); // 1.90s: just into the pop glide
    motion.exitBoxPeek();
    // Leg 1 (0.55s): the stand is restored while the box has not moved yet.
    advanceFrames(motion, 27); // 2.44s: one frame before leg 2 starts
    QVERIFY(motion.boxPeekDuck() < 0.5); // the glide is all but finished
    QCOMPARE(motion.boxPeekSlide(), 1.0);
    // Leg 2 (0.7s): only then the box slides away.
    advanceFrames(motion, 10); // 2.74s
    QCOMPARE(motion.boxPeekDuck(), 0.0);
    QVERIFY(motion.boxPeekSlide() > 0.0 && motion.boxPeekSlide() < 1.0);
    advanceFrames(motion, 45); // 3.64s: past restore+leave, fully gone
    QVERIFY(!motion.boxPeekActive());
    QCOMPARE(motion.boxPeekSlide(), 0.0);
    QCOMPARE(motion.boxPeekDuck(), 0.0);
}

void ParameterMotionTest::boxPeekRefusesBusyAndYieldsToDelete() {
    ParameterMotion motion;
    advanceFrames(motion, 10);
    // Busy (working) refuses: the box choreography covers a standing body.
    motion.setState(PetController::State::Busy);
    QVERIFY(!motion.playBoxPeek());
    motion.setState(PetController::State::Idle);
    QVERIFY(motion.playBoxPeek());
    QVERIFY(motion.boxPeekActive());
    advanceFrames(motion, 10);
    // A delete lunge owns the stage: the box drops instantly.
    motion.setState(PetController::State::Delete);
    QVERIFY(!motion.boxPeekActive());
    QCOMPARE(motion.boxPeekDuck(), 0.0);
    // A new turn needs her back at work: the box exits with its animated
    // stand instead of dropping.
    motion.setState(PetController::State::Idle);
    advanceFrames(motion, 5);
    QVERIFY(motion.playBoxPeek());
    advanceFrames(motion, 10);
    motion.setState(PetController::State::Busy);
    QVERIFY(motion.boxPeekExiting());
    advanceFrames(motion, 10); // the exit keeps finishing the glide-in
    QVERIFY(motion.boxPeekSlide() > 0.3); // still covering while she stands up
}

void ParameterMotionTest::sleepMotionNapsWakesAndYieldsToEvents() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.setProceduralBlink(false); // isolate the authored eye keys from the blink
    advanceFrames(motion, 50);
    QVERIFY(!motion.sleepMotionActive());
    QCOMPARE(motion.sleepMaskEnvelope(), 0.0);

    // Enter: the mask settles on, eyes half-close, the head starts tipping.
    motion.beginSleepMotion();
    advanceFrames(motion, 20); // 0.4s, past the generic value blend lag
    QVERIFY(motion.sleepMaskEnvelope() > 0.3 && motion.sleepMaskEnvelope() < 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamAngleZ")) > 0.5);

    // Asleep: eyes closed, slow breath on its own clock.
    advanceFrames(motion, 40); // 1.0s total
    QVERIFY(motion.sleepMotionActive());
    QCOMPARE(motion.sleepMaskEnvelope(), 1.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) < 0.1);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) < 0.1);
    const double sleepingBreath = motion.values().value(QStringLiteral("ParamBreath"));
    QVERIFY(sleepingBreath > 0.1 && sleepingBreath < 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamAngleZ")) > 4.0);

    // Non-interactive wake (a new turn / an event): 0.8s, then clean handback.
    motion.wakeFromSleep(false);
    advanceFrames(motion, 45); // 0.9s
    QVERIFY(!motion.sleepMotionActive());
    QCOMPARE(motion.sleepMaskEnvelope(), 0.0);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.85);

    // Interactive wake: one eye first, then the other. (The procedural blink is
    // off in this test, so the authored eye-open reads exactly.)
    motion.beginSleepMotion();
    advanceFrames(motion, 50); // into Asleep
    motion.wakeFromSleep(true);
    advanceFrames(motion, 20); // 0.4s: the left eye is opening, the right waits
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) < 0.1);
    advanceFrames(motion, 10); // 0.6s: the left eye fully open
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) < 0.6);
    advanceFrames(motion, 25); // 1.1s: wake finished, both eyes handed back
    QVERIFY(!motion.sleepMotionActive());
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.9);
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeROpen")) > 0.9);

    // A busy turn arriving mid-nap wakes it by itself and the typing owns the
    // face afterwards -- the sleep overlay must never outlive the state.
    motion.beginSleepMotion();
    advanceFrames(motion, 30);
    motion.setState(PetController::State::Busy);
    QVERIFY(motion.sleepMotionActive()); // waking, not cancelled outright
    advanceFrames(motion, 45); // 0.9s
    QVERIFY(!motion.sleepMotionActive());
    QVERIFY(motion.values().value(QStringLiteral("ParamEyeLOpen")) > 0.8);
}

void ParameterMotionTest::stretchPausePlaysOverSeatedLaptopOnlyAndResumesTyping() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    // Idle refuses; the standing busy variant refuses too (the clip keys the
    // full seated desk pose, so it must not jump onto a standing body).
    QVERIFY(!motion.playStretch());
    motion.forceStandingBusy();
    advanceFrames(motion, 50);
    QVERIFY(!motion.playStretch());
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) > 0.99);
    QVERIFY(motion.playStretch());
    QVERIFY(!motion.playStretch());
    QCOMPARE(motion.interactionId(), QStringLiteral("busy-stretch"));
    advanceFrames(motion, 15); // 0.3s: inside the pause, at the peek key
    QVERIFY(motion.interactionActive());
    // Typing stops while paused (blended down from the typing loop); the
    // seated desk stays.
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingL")) < 0.2);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingR")) < 0.2);
    QVERIFY(motion.values().value(QStringLiteral("ParamLaptopVisible")) > 0.99);
    // Delete takes priority over the pause.
    motion.setState(PetController::State::Delete);
    QVERIFY(!motion.interactionActive());
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.playStretch());
    advanceFrames(motion, 40); // 0.8s > the 0.6s fixture: pause over
    QVERIFY(!motion.interactionActive());
    // The busy clock resumed on the laptop variant: typing is back.
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingL")) > 0.001);
}

void ParameterMotionTest::stretchPokeSkipsToCaughtRecoveryAndResumesTyping() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.playStretch());
    advanceFrames(motion, 5); // 0.1s: mid pause, before the peek key at 0.3s
    QVERIFY(motion.stretchActive());
    const double before = motion.interactionTime();
    // A click while peeking is "caught you": the clip jumps to the recovery
    // beat instead of starting a head pat.
    motion.pokeStretch();
    QVERIFY(motion.interactionTime() >= 0.3 - 1e-6);
    QVERIFY(motion.interactionTime() > before);
    motion.pokeStretch(); // idempotent afterwards
    QCOMPARE(motion.interactionTime(), std::max(before, 0.3));
    // The pause still ends on the authored tail and typing resumes.
    advanceFrames(motion, 30); // 0.6s total: past the fixture duration
    QVERIFY(!motion.interactionActive());
    QVERIFY(motion.values().value(QStringLiteral("ParamBusyTypingL")) > 0.001);
    // Poke outside a stretch is a no-op.
    motion.pokeStretch();
    QVERIFY(!motion.interactionActive());
}

void ParameterMotionTest::workPauseDispatchRespectsApprovalAndSettledPosture() {
    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    QVERIFY(writeStandingRestFixture(dir));
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    motion.setDeskWorkMode(true);
    QVERIFY(!motion.playStretch(true)); // review still needs a real Busy state
    motion.forceStandingBusy();
    advanceFrames(motion, 50);
    QVERIFY(!motion.playStretch()); // pending cannot leak through a normal entry
    QVERIFY(motion.playStretch(true));
    QCOMPARE(motion.interactionId(), QStringLiteral("busy-rest"));
    QVERIFY(motion.restActive());
    QVERIFY(!motion.stretchActive()); // no seated caught-peeking click semantics
    QVERIFY(!motion.playStretch(true));
    QVERIFY(motion.cancelPendingInteraction());
    QVERIFY(!motion.interactionActive());

    // Approving the same clip enables the ordinary dispatcher; turning draft
    // preview off must not cancel an approved standing or seated pause.
    QVERIFY(writeStandingRestFixture(dir, QStringLiteral("approved")));
    QVERIFY(motion.loadMotionLibrary(dir.path()));
    QVERIFY(motion.playStretch());
    QVERIFY(!motion.cancelPendingInteraction());
    QVERIFY(motion.restActive());
    motion.cancelInteraction();
    motion.forceLaptopBusy();
    QVERIFY(!motion.playStretch(true)); // a selected variant is not yet a seat
    advanceFrames(motion, 20);
    QVERIFY(!motion.playStretch(true)); // visible sit-down is still underway
    advanceFrames(motion, 180);
    QVERIFY(motion.playStretch());
    QCOMPARE(motion.interactionId(), QStringLiteral("busy-stretch"));
    QVERIFY(!motion.cancelPendingInteraction());
    QVERIFY(motion.stretchActive());
    motion.cancelInteraction();
    motion.forceStandingBusy();
    QVERIFY(!motion.playStretch(true)); // leaving the desk is not settled standing
    advanceFrames(motion, 220);
    QVERIFY(motion.playStretch());
    QCOMPARE(motion.interactionId(), QStringLiteral("busy-rest"));

    // A naturally chosen laptop waits ten seconds before sitting. It must not
    // borrow the standing rest during that delay, even in explicit preview.
    bool sawDelayedLaptop = false;
    for (quint32 seed = 1; seed <= 20 && !sawDelayedLaptop; ++seed) {
        ParameterMotion waiting;
        waiting.setBusyRandomSeed(seed);
        QVERIFY(waiting.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
        QVERIFY(waiting.loadMotionLibrary(dir.path()));
        waiting.setDeskWorkMode(true);
        waiting.setState(PetController::State::Busy);
        if (!waiting.isLaptopBusy()) continue;
        sawDelayedLaptop = true;
        advanceFrames(waiting, 100);
        QCOMPARE(waiting.values().value(QStringLiteral("ParamSitPose")), 0.0);
        QVERIFY(!waiting.playStretch(true));
    }
    QVERIFY(sawDelayedLaptop);
}

void ParameterMotionTest::workPauseFreezesClocksAndPreservesStandingPosture() {
    ParameterMotion motion;
    ParameterMotion oppositeCursor;
    QString error;
    QVERIFY2(motion.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
    QVERIFY2(oppositeCursor.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
    motion.setDeskWorkMode(true);
    oppositeCursor.setDeskWorkMode(true);
    motion.setProceduralBlink(false);
    oppositeCursor.setProceduralBlink(false);
    motion.setGazeTarget(1.0, -0.5, false, true);
    oppositeCursor.setGazeTarget(-1.0, 0.5, false, true);
    motion.forceStandingBusy();
    oppositeCursor.forceStandingBusy();
    advanceFrames(motion, 1999); // 39.98s: one ordinary frame before a choice
    advanceFrames(oppositeCursor, 1999);
    const double busyTime = motion.busyPhaseTime();
    const double workTime = motion.continuousBusyTime();
    const double choice = motion.nextBusyChoiceTime();
    QVERIFY(busyTime < choice && choice - busyTime < 0.03);
    // The real approved standing clip must use the same ordinary entry as
    // the tray and director; no draft-preview exception is needed.
    QVERIFY(motion.playStretch());
    QVERIFY(oppositeCursor.playStretch());
    ParameterMotion::Parameters faceDifferenceAtFullOwnership;
    double fullOwnershipTime = 0.0;
    int faceOwnershipChecks = 0;
    int frames = 0;
    while (motion.workPauseActive() && frames < 140) {
        motion.advance(0.02);
        oppositeCursor.advance(0.02);
        ++frames;
        QCOMPARE(motion.busyPhaseTime(), busyTime);
        QCOMPARE(motion.continuousBusyTime(), workTime);
        QCOMPARE(motion.nextBusyChoiceTime(), choice);
        QVERIFY(!motion.isLaptopBusy());
        for (const QString& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
             QStringLiteral("ParamDeskVisible"), QStringLiteral("ParamLaptopVisible"),
             QStringLiteral("ParamBusyTypingL"), QStringLiteral("ParamBusyTypingR"),
             QStringLiteral("ParamGrassVisible"), QStringLiteral("ParamHandRGrip")})
            QCOMPARE(motion.values().value(id), 0.0);
        if (motion.restActive() && motion.interactionTime() > 0.5) {
            QVERIFY(motion.bubblePulse() < 0.002);
        }
        if (motion.restActive() && motion.interactionTime() >= 0.2
            && motion.interactionTime() <= 2.2) {
            // During full authored ownership, opposite live cursor targets
            // cannot alter the desired face. Only each player's pre-existing
            // displayed face remains, decaying on the documented 120ms blend.
            // Internal gaze values need not reach zero before apply() stands
            // down; test the actual model-facing values rather than a guessed
            // threshold on those private smoothing states.
            const bool first = faceDifferenceAtFullOwnership.isEmpty();
            if (first) fullOwnershipTime = motion.interactionTime();
            for (const QString& id : {QStringLiteral("ParamAngleX"), QStringLiteral("ParamAngleY"),
                 QStringLiteral("ParamEyeBallX"), QStringLiteral("ParamEyeBallY")}) {
                const double difference = motion.values().value(id) - oppositeCursor.values().value(id);
                if (first) faceDifferenceAtFullOwnership.insert(id, difference);
                else {
                    const double expected = faceDifferenceAtFullOwnership.value(id)
                        * qExp(-(motion.interactionTime() - fullOwnershipTime) / 0.12);
                    QVERIFY2(std::abs(difference - expected) < 1e-10, qPrintable(
                        QStringLiteral("cursor leaked into %1: delta %2 expected %3 at %4s")
                            .arg(id).arg(difference, 0, 'g', 15).arg(expected, 0, 'g', 15)
                            .arg(motion.interactionTime())));
                    ++faceOwnershipChecks;
                }
            }
        }
    }
    QVERIFY(!motion.interactionActive());
    QVERIFY(frames >= 129 && frames <= 131);
    QVERIFY(faceOwnershipChecks >= 360);
    advanceFrames(motion, 2);
    QVERIFY(motion.busyPhaseTime() > busyTime);
    QVERIFY(motion.continuousBusyTime() > workTime);
    QCOMPARE(motion.nextBusyChoiceTime(), choice + 40.0);
    QVERIFY(!motion.interactionActive()); // no deferred repeat

    // The already-approved seated branch shares the freeze. At this boundary
    // the old increment/check/subtract order could flip the variant before
    // subtracting, and its reset-to-zero postponed the choice by forty seconds.
    ParameterMotion seated;
    QVERIFY(seated.loadMotionLibrary(QStringLiteral("assets/motions")));
    seated.setDeskWorkMode(true);
    seated.forceLaptopBusy();
    advanceFrames(seated, 1999);
    const double seatedPhase = seated.busyPhaseTime();
    const double seatedWork = seated.continuousBusyTime();
    QVERIFY(seated.playStretch());
    int seatedFrames = 0;
    while (seated.workPauseActive() && seatedFrames++ < 220) {
        seated.advance(0.02);
        QCOMPARE(seated.busyPhaseTime(), seatedPhase);
        QCOMPARE(seated.continuousBusyTime(), seatedWork);
        QCOMPARE(seated.nextBusyChoiceTime(), 40.0);
        QVERIFY(seated.isLaptopBusy());
        for (const QString& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
             QStringLiteral("ParamDeskVisible"), QStringLiteral("ParamLaptopVisible")})
            QVERIFY2(seated.values().value(id) > 0.995, qPrintable(id));
    }
    QVERIFY(!seated.workPauseActive());
    advanceFrames(seated, 2);
    QCOMPARE(seated.nextBusyChoiceTime(), 80.0);
}

void ParameterMotionTest::workPauseRefusesExistingOverlayWithoutPreempting() {
    // Both dispatch branches use the same refusal policy. Each attempt must
    // leave the existing owner alive, not preempt it and then return false.
    for (const bool seated : {false, true}) {
        for (const QString& owner : {QStringLiteral("drag"), QStringLiteral("memo"),
             QStringLiteral("sleep"), QStringLiteral("head-pat"), QStringLiteral("greet"),
             QStringLiteral("wave"), QStringLiteral("box"), QStringLiteral("rice")}) {
            ParameterMotion motion;
            QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
            motion.setDeskWorkMode(true);
            motion.setState(PetController::State::Idle);
            advanceFrames(motion, 50);
            if (owner == QStringLiteral("wave")) QVERIFY(motion.playWave());
            if (owner == QStringLiteral("box")) QVERIFY(motion.playBoxPeek());
            if (seated) motion.forceLaptopBusy();
            else motion.forceStandingBusy();
            if (owner != QStringLiteral("wave") && owner != QStringLiteral("box"))
                advanceFrames(motion, seated ? 200 : 50);
            if (owner == QStringLiteral("drag")) motion.beginDragMotion();
            if (owner == QStringLiteral("memo")) motion.beginMemoMotion();
            if (owner == QStringLiteral("sleep")) motion.beginSleepMotion();
            if (owner == QStringLiteral("head-pat")) QVERIFY(motion.beginHeadPat(1.0));
            if (owner == QStringLiteral("greet")) QVERIFY(motion.playGreet());
            if (owner == QStringLiteral("rice")) {
                if (!seated) continue; // this scene is only meaningful at the desk
                QVERIFY(motion.playRiceBowl());
            }
            const QString interaction = motion.interactionId();
            QVERIFY2(!motion.playStretch(true), qPrintable(owner));
            QCOMPARE(motion.interactionId(), interaction);
            if (owner == QStringLiteral("drag")) QVERIFY(motion.dragMotionActive());
            if (owner == QStringLiteral("memo")) QVERIFY(motion.memoMotionActive());
            if (owner == QStringLiteral("sleep")) QVERIFY(motion.sleepMotionActive());
            if (owner == QStringLiteral("wave")) QVERIFY(motion.waveActive());
            if (owner == QStringLiteral("box")) QVERIFY(motion.boxPeekActive());
            if (owner == QStringLiteral("rice")) QVERIFY(motion.riceBowlActive());
        }
    }
}

void ParameterMotionTest::workPauseInterruptionsDoNotReplay() {
    for (const bool seated : {false, true}) {
        for (const QString& event : {QStringLiteral("stop"), QStringLiteral("delete"),
             QStringLiteral("grass"), QStringLiteral("drag"), QStringLiteral("pat"),
             QStringLiteral("memo"), QStringLiteral("sleep"), QStringLiteral("preview-off")}) {
            ParameterMotion motion;
            QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
            motion.setDeskWorkMode(true);
            if (seated) motion.forceLaptopBusy();
            else motion.forceStandingBusy();
            advanceFrames(motion, seated ? 200 : 50);
            QVERIFY(motion.playStretch());
            advanceFrames(motion, 5);
            QVERIFY(motion.workPauseActive());
            if (event == QStringLiteral("stop")) motion.setState(PetController::State::Idle);
            if (event == QStringLiteral("delete")) motion.setState(PetController::State::Delete);
            if (event == QStringLiteral("grass")) motion.setState(PetController::State::Grass);
            if (event == QStringLiteral("drag")) motion.beginDragMotion();
            if (event == QStringLiteral("pat")) QVERIFY(motion.beginHeadPat(-1.0));
            if (event == QStringLiteral("memo")) motion.beginMemoMotion();
            if (event == QStringLiteral("sleep")) motion.beginSleepMotion();
            if (event == QStringLiteral("preview-off")) {
                QVERIFY(!motion.cancelPendingInteraction());
                QVERIFY(motion.workPauseActive());
                QCOMPARE(motion.interactionId(), seated ? QStringLiteral("busy-stretch")
                                                       : QStringLiteral("busy-rest"));
                // Both real clips are approved: closing draft preview keeps
                // the authored pause alive until its normal tail completes.
                advanceFrames(motion, 220);
                QVERIFY(!motion.workPauseActive());
                QVERIFY(!motion.interactionActive());
                continue;
            }
            QVERIFY2(!motion.workPauseActive(), qPrintable(event));
            if (event == QStringLiteral("pat")) QCOMPARE(motion.interactionId(), QStringLiteral("head-pat"));
            // Let the higher-priority action own its normal time, then restore
            // work. Nothing stores a request to replay the interrupted pause.
            advanceFrames(motion, 20);
            motion.cancelDragMotion();
            motion.cancelMemoMotion();
            motion.cancelSleepMotion();
            motion.cancelInteraction();
            motion.setState(PetController::State::Busy);
            advanceFrames(motion, 200);
            QVERIFY(!motion.workPauseActive());
            QVERIFY(!motion.interactionActive());
        }
    }
}

void ParameterMotionTest::standingRestRejectsReservedAxesAndUnsafeSway() {
    for (const auto& illegal : {qMakePair(QStringLiteral("ParamSitPose"), 1.0),
         qMakePair(QStringLiteral("ParamShiftX"), 1.0), qMakePair(QStringLiteral("ParamShiftY"), 1.0),
         qMakePair(QStringLiteral("ParamArmRA"), 10.0), qMakePair(QStringLiteral("ParamBrowLY"), 1.0),
         qMakePair(QStringLiteral("ParamAngleZ"), 1.0), qMakePair(QStringLiteral("ParamBodyAngleX"), 3.1),
         qMakePair(QStringLiteral("ParamBodyAngleZ"), 2.1)}) {
        QTemporaryDir dir;
        QVERIFY(writeStandingRestFixture(dir));
        QFile file(dir.filePath(QStringLiteral("busy-rest.motion.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        // Put the violation at a later key: the loader must inspect more than
        // the first pose and must not trust preview-only status as validation.
        auto keys = root.value(QStringLiteral("keyframes")).toArray();
        for (int i = 0; i < keys.size(); ++i) {
            auto key = keys.at(i).toObject();
            auto pose = key.value(QStringLiteral("parameters")).toObject();
            pose.insert(illegal.first, i == 1 ? illegal.second : 0.0);
            key.insert(QStringLiteral("parameters"), pose);
            keys.replace(i, key);
        }
        root.insert(QStringLiteral("keyframes"), keys);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();
        ParameterMotion motion;
        QString error;
        QVERIFY2(!motion.loadMotionLibrary(dir.path(), &error), qPrintable(illegal.first));
        QVERIFY2(error.startsWith(QStringLiteral("Standing rest")), qPrintable(error));
    }
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

void ParameterMotionTest::realHeadPatDirectionsPreserveActivityAndRelease() {
    // The fixture tests above isolate player mechanics at three degrees. This
    // checks the shipped approved five-degree clip and its mirrored direction
    // over both real activities; it does not prove Native mesh continuity.
    for (const bool seated : {false, true}) for (const double direction : {-1.0, 1.0}) {
        ParameterMotion reaction, reference;
        QString error;
        for (auto* motion : {&reaction, &reference}) {
            QVERIFY2(motion->loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
            motion->setProceduralBlink(false); // measure authored expression, not blink timing
            motion->setDeskWorkMode(true);
            if (seated) motion->forceLaptopBusy();
            advanceFrames(*motion, 200);
        }
        const auto* clip = reaction.library().clip(QStringLiteral("head-pat"));
        QVERIFY(clip);
        QCOMPARE(clip->approval(), QStringLiteral("approved"));
        QCOMPARE(clip->sample(1.2).value(QStringLiteral("ParamAngleZ")), 5.0);
        QCOMPARE(reaction.values().value(QStringLiteral("ParamDeskVisible")), seated ? 1.0 : 0.0);
        QVERIFY(reaction.beginHeadPat(direction));
        double directionalPeak = 0.0, smilePeak = 0.0;
        bool released = false;
        for (int frame = 0; frame < 140; ++frame) {
            // Idle gets a sustained hold. Busy keeps its real 1.3-second limit
            // and starts the authored recovery without waiting for this call.
            if (!seated && frame == 100) reaction.endHeadPat();
            reaction.advance(0.02);
            reference.advance(0.02);
            if (reaction.interactionActive()) {
                directionalPeak = std::max(directionalPeak,
                    direction * reaction.values().value(QStringLiteral("ParamAngleZ")));
                smilePeak = std::max(smilePeak,
                    reaction.values().value(QStringLiteral("ParamEyeSmile")));
                released |= reaction.headPatReleasing();
            }
            // Hands may keep typing; compare the same advancing activity, not
            // a frozen pose that would hide accidental typing cancellation.
            for (const auto& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
                 QStringLiteral("ParamDeskVisible"), QStringLiteral("ParamLaptopVisible"),
                 QStringLiteral("ParamArmLA"), QStringLiteral("ParamArmRA"),
                 QStringLiteral("ParamElbowLA"), QStringLiteral("ParamElbowRA"),
                 QStringLiteral("ParamWristRA"), QStringLiteral("ParamLaptopRock"),
                 QStringLiteral("ParamBusyTypingL"), QStringLiteral("ParamBusyTypingR")})
                QCOMPARE(reaction.values().value(id), reference.values().value(id));
        }
        QVERIFY2(directionalPeak > 4.6 && directionalPeak <= 5.0,
                 qPrintable(QStringLiteral("seated=%1 direction=%2 peak=%3")
                     .arg(seated).arg(direction).arg(directionalPeak)));
        QVERIFY(smilePeak > 0.9);
        QVERIFY(released);
        QVERIFY(!reaction.interactionActive());
        advanceFrames(reaction, 80);
        advanceFrames(reference, 80);
        for (const auto& id : {QStringLiteral("ParamAngleX"), QStringLiteral("ParamAngleY"),
             QStringLiteral("ParamAngleZ"), QStringLiteral("ParamEyeBallX"), QStringLiteral("ParamEyeBallY"),
             QStringLiteral("ParamEyeLOpen"), QStringLiteral("ParamEyeROpen"),
             QStringLiteral("ParamEyeSmile"), QStringLiteral("ParamMouthForm"),
             QStringLiteral("ParamMouthOpenY"), QStringLiteral("ParamCheek")})
            QVERIFY2(std::abs(reaction.values().value(id) - reference.values().value(id)) < 0.001,
                     qPrintable(QStringLiteral("Head pat did not return %1 to its background").arg(id)));
    }
}

void ParameterMotionTest::headPatRefusesExistingFaceOverlays() {
    for (const QString overlay : {QStringLiteral("drag"), QStringLiteral("memo"), QStringLiteral("sleep")}) {
        ParameterMotion motion;
        QString error;
        QVERIFY2(motion.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
        advanceFrames(motion, 50);
        if (overlay == QStringLiteral("drag")) motion.beginDragMotion();
        else if (overlay == QStringLiteral("memo")) motion.beginMemoMotion();
        else motion.beginSleepMotion();
        advanceFrames(motion, 12);
        const auto before = motion.values();
        QVERIFY(!motion.canBeginHeadPat());
        QVERIFY(!motion.beginHeadPat(1));
        QCOMPARE(motion.values(), before);
        QVERIFY(!motion.interactionActive());
        if (overlay == QStringLiteral("drag")) {
            QVERIFY(motion.dragMotionActive());
            motion.cancelDragMotion();
        } else if (overlay == QStringLiteral("memo")) {
            QVERIFY(motion.memoMotionActive());
            motion.cancelMemoMotion();
        } else {
            QVERIFY(motion.sleepMotionActive());
            motion.cancelSleepMotion();
        }
        // Refusal neither creates an invisible interaction nor spends cooldown.
        QVERIFY(motion.canBeginHeadPat());
        QVERIFY(motion.beginHeadPat(-1));
    }
}

void ParameterMotionTest::realHeadPatYieldsToWorkAndForegroundEvents() {
    ParameterMotion working;
    QString error;
    QVERIFY2(working.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
    advanceFrames(working, 50);
    QVERIFY(working.beginHeadPat(-1));
    advanceFrames(working, 50); // already holding while Idle
    const auto held = working.values();
    const double heldTime = working.interactionTime();
    working.forceStandingBusy();
    QCOMPARE(working.values(), held);
    QCOMPARE(working.interactionTime(), heldTime);
    QVERIFY(working.interactionActive());
    QVERIFY(!working.headPatReleasing());
    advanceFrames(working, 5);
    QVERIFY(!working.headPatReleasing());
    advanceFrames(working, 11); // Busy bounds total hold at 1.3 seconds
    QVERIFY(working.headPatReleasing());
    advanceFrames(working, 40);
    QVERIFY(!working.interactionActive());

    for (const bool seated : {false, true})
    for (const auto foreground : {PetController::State::Delete, PetController::State::Grass}) {
        ParameterMotion motion;
        QVERIFY2(motion.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
        motion.setDeskWorkMode(true);
        if (seated) motion.forceLaptopBusy();
        advanceFrames(motion, 160);
        QVERIFY(motion.beginHeadPat(seated ? -1 : 1));
        advanceFrames(motion, 50);
        QVERIFY(motion.interactionActive());
        const auto before = motion.values();
        motion.setState(foreground);
        QCOMPARE(motion.values(), before);
        QVERIFY(!motion.interactionActive());
        advanceFrames(motion, 10);
        for (double value : motion.values()) QVERIFY(std::isfinite(value));
        motion.setState(seated ? PetController::State::Busy : PetController::State::Idle);
        advanceFrames(motion, 100);
        QVERIFY(!motion.interactionActive()); // interrupted hold is never replayed
    }
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
    // The open grin is banned (user ruling r2): the mouth keeps the closed
    // omega even in the delighted grass beat -- the eye smile carries it.
    QVERIFY(motion.values().value(QStringLiteral("ParamSmileOpen")) < 0.01);
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

void ParameterMotionTest::deskSitPrecedesCoverAndLeavesLast() {
    for (double step : {1.0 / 30.0, 1.0 / 60.0, 0.1}) {
        ParameterMotion motion;
        QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
        motion.setDeskWorkMode(true);
        motion.advance(step);
        motion.forceLaptopBusy();
        bool sawVisibleSit = false, sawCoverOnly = false, sawWork = false, sawTyping = false;
        for (int frame = 0; frame < static_cast<int>(3.0 / step); ++frame) {
            motion.advance(step);
            const auto pose = motion.values();
            const double cover = pose.value(QStringLiteral("ParamDeskVisible"));
            const double sit = pose.value(QStringLiteral("ParamSitPose"));
            QVERIFY(cover >= 0.0 && cover <= 1.0);
            if (cover < 1.0) {
                sawCoverOnly |= cover > 0.0;
                sawVisibleSit |= cover == 0.0 && sit > 0.2 && sit < 1.0;
                // The texture swap and the laptop wait for the opaque cover;
                // the sit itself plays in the open before any furniture.
                QCOMPARE(pose.value(QStringLiteral("ParamBusyLaptop")), 0.0);
                QCOMPARE(pose.value(QStringLiteral("ParamLaptopVisible")), 0.0);
                if (cover > 0.0) QCOMPARE(sit, 1.0);
            } else {
                sawWork |= sit > 0.9;
                sawTyping |= pose.value(QStringLiteral("ParamBusyTypingR")) > 0.1;
            }
            if (pose.value(QStringLiteral("ParamLaptopVisible")) < 0.99) {
                QCOMPARE(pose.value(QStringLiteral("ParamBusyTypingL")), 0.0);
                QCOMPARE(pose.value(QStringLiteral("ParamBusyTypingR")), 0.0);
            }
        }
        QVERIFY(sawVisibleSit && sawCoverOnly && sawWork && sawTyping);
        motion.setState(PetController::State::Idle);
        bool sawPutAway = false, sawReveal = false, sawVisibleStand = false;
        for (int frame = 0; frame < static_cast<int>(4.0 / step); ++frame) {
            motion.advance(step);
            const auto pose = motion.values();
            const double cover = pose.value(QStringLiteral("ParamDeskVisible"));
            const double sit = pose.value(QStringLiteral("ParamSitPose"));
            sawPutAway |= pose.value(QStringLiteral("ParamLaptopVisible")) < 0.9
                && sit > 0.9;
            if (cover > 0.0 && cover < 1.0) {
                sawReveal = true;
                // The desk leaves before she stands: chest already standing,
                // seat still held while the furniture is departing.
                QCOMPARE(pose.value(QStringLiteral("ParamBusyLaptop")), 0.0);
                QCOMPARE(pose.value(QStringLiteral("ParamLaptopVisible")), 0.0);
                QCOMPARE(sit, 1.0);
            }
            if (cover == 0.0) {
                sawVisibleStand |= sit > 0.0 && sit < 1.0;
                QCOMPARE(pose.value(QStringLiteral("ParamBusyLaptop")), 0.0);
            }
        }
        QVERIFY(sawPutAway && sawReveal && sawVisibleStand);
        QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
        QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0);
    }
}

void ParameterMotionTest::deskSitWaitsForSustainedWork() {
    // Live policy: during the first kDeskSitDelay (~10 s) of a work session she
    // stays on her feet -- no seated pose, no desk -- so a launch straight into
    // an active turn does not seat her the moment work begins. forceLaptopBusy
    // primes the clock for captures and the choreography tests, so drive the
    // natural path here.
    for (int seed = 1; seed <= 20; ++seed) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
        motion.setBusyRandomSeed(seed);
        motion.setDeskWorkMode(true);
        motion.setState(PetController::State::Busy);
        for (int i = 1; i <= 95; ++i) {        // 9.5 s, still under the delay
            motion.advance(0.1);
            QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
            QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0);
        }
    }
}

void ParameterMotionTest::workCompanionBeatsReadAndRestore() {
    // 开工致意 / 完成庆祝 drive the silhouette levers (ParamBodyAngleX/Z +
    // ParamShiftX) so they actually read at 280px. They may play over idle OR
    // the seated work pose; the seated pose underneath must be preserved.
    ParameterMotion idle;
    QVERIFY(idle.loadMotionLibrary(QStringLiteral("assets/motions")));
    idle.setState(PetController::State::Idle);
    for (int i = 0; i < 10; ++i) idle.advance(0.02);
    QVERIFY(idle.playGreet());
    QCOMPARE(idle.interactionId(), QStringLiteral("greet"));
    double peakBody = 0.0;
    while (idle.interactionActive()) {
        idle.advance(0.02);
        peakBody = std::max(peakBody, idle.values().value(QStringLiteral("ParamBodyAngleX")));
    }
    QVERIFY2(peakBody > 2.0, qPrintable(QStringLiteral("greet body peak %1").arg(peakBody)));
    for (int i = 0; i < 40; ++i) idle.advance(0.02);
    // Back to the idle base (whose own sway is within ~0.8), well under the peak.
    QVERIFY(std::abs(idle.values().value(QStringLiteral("ParamBodyAngleX"))) < 2.0);

    // Cheer over the seated desk pose leaves the seat/laptop untouched.
    ParameterMotion seated;
    QVERIFY(seated.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    QVERIFY(seated.loadMotionLibrary(QStringLiteral("assets/motions")));
    seated.setDeskWorkMode(true);
    seated.forceLaptopBusy();
    for (int i = 0; i < 60; ++i) seated.advance(0.02);
    QVERIFY(seated.values().value(QStringLiteral("ParamSitPose")) > 0.9);
    QVERIFY(seated.playCheer());
    QCOMPARE(seated.interactionId(), QStringLiteral("cheer"));
    while (seated.interactionActive()) {
        seated.advance(0.02);
        QVERIFY(seated.values().value(QStringLiteral("ParamSitPose")) > 0.9);   // seat preserved
    }
    // A beat never stacks on another reaction.
    QVERIFY(seated.playGreet());
    QVERIFY(!seated.playCheer());
    seated.cancelInteraction();
    QVERIFY(!seated.interactionActive());
}

void ParameterMotionTest::deskShortTasksReverseWithoutDesk() {
    ParameterMotion standing;
    QVERIFY(standing.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    standing.setDeskWorkMode(true);
    standing.forceStandingBusy();
    advanceFrames(standing, 50);
    standing.forceLaptopBusy();
    standing.advance(0.04);
    const double gestureArm = standing.values().value(QStringLiteral("ParamArmLA"));
    QVERIFY(std::abs(gestureArm) > 1.0);
    standing.setState(PetController::State::Idle);
    standing.advance(0.02);
    QVERIFY(std::abs(standing.values().value(QStringLiteral("ParamArmLA"))) > 0.5);
    QVERIFY(std::abs(standing.values().value(QStringLiteral("ParamArmLA")) - gestureArm) < 3.0);
    // A blip barely starts the visible sit ramp before the reversal.
    QVERIFY(standing.values().value(QStringLiteral("ParamSitPose")) < 0.05);

    // A task that ends mid-sit reverses in the open: no furniture is ever
    // shown, and the partial seat ramps back to standing.
    ParameterMotion motion;
    QVERIFY(motion.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    motion.setDeskWorkMode(true);
    motion.advance(0.02);
    motion.forceLaptopBusy();
    advanceFrames(motion, 12); // 0.24s into the 0.7s sit ramp.
    motion.setState(PetController::State::Idle);
    bool sawPartialSeat = false;
    for (int frame = 0; frame < 80; ++frame) {
        motion.advance(0.02);
        const auto pose = motion.values();
        sawPartialSeat |= pose.value(QStringLiteral("ParamSitPose")) > 0.05;
        QCOMPARE(pose.value(QStringLiteral("ParamDeskVisible")), 0.0);
        QCOMPARE(pose.value(QStringLiteral("ParamBusyLaptop")), 0.0);
        QCOMPARE(pose.value(QStringLiteral("ParamLaptopVisible")), 0.0);
    }
    QVERIFY(sawPartialSeat);
    QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0);

    // A complete cycle: sit, work, then reverse out of the revealing desk.
    motion.forceLaptopBusy();
    advanceFrames(motion, 60);
    QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 1.0);
    motion.setState(PetController::State::Idle);
    advanceFrames(motion, 40); // Chest recovers behind the opaque desk.
    motion.forceLaptopBusy(); // Work resumes while the desk was about to go.
    motion.advance(0.02);
    QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 1.0);
    QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 1.0);
    advanceFrames(motion, 30);
    motion.setState(PetController::State::Idle);
    bool sawReveal = false, sawVisibleStand = false;
    for (int frame = 0; frame < 400; ++frame) {
        motion.advance(0.02);
        const auto pose = motion.values();
        const double cover = pose.value(QStringLiteral("ParamDeskVisible"));
        const double sit = pose.value(QStringLiteral("ParamSitPose"));
        if (cover > 0.0 && cover < 1.0) {
            sawReveal = true;
            QCOMPARE(sit, 1.0);
        }
        if (cover == 0.0) sawVisibleStand |= sit > 0.0 && sit < 1.0;
    }
    QVERIFY(sawReveal && sawVisibleStand);
    QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
    QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0);
    QCOMPARE(motion.values().value(QStringLiteral("ParamBusyLaptop")), 0.0);
}

void ParameterMotionTest::deskVariantChangesAndReactionsKeepCover() {
    ParameterMotion variants;
    QVERIFY(variants.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
    variants.setBusyRandomSeed(1234);
    variants.setDeskWorkMode(true);
    variants.advance(0.02);
    variants.forceLaptopBusy();
    bool sawStandingChoice = false, sawDeskAbsent = false;
    for (int frame = 0; frame < 7000; ++frame) {
        variants.advance(0.02);
        const auto pose = variants.values();
        sawStandingChoice |= !variants.isLaptopBusy();
        sawDeskAbsent |= frame > 200 && pose.value(QStringLiteral("ParamDeskVisible")) == 0.0;
        // The sit-down itself plays before the desk arrives, so only the
        // texture/laptop engagement (not SitPose) requires the cover.
        if (pose.value(QStringLiteral("ParamBusyLaptop")) > 0.0
            || pose.value(QStringLiteral("ParamLaptopVisible")) > 0.0)
            QCOMPARE(pose.value(QStringLiteral("ParamDeskVisible")), 1.0);
    }
    QVERIFY(sawStandingChoice && sawDeskAbsent);
    variants.forceStandingBusy();
    advanceFrames(variants, 200);
    QCOMPARE(variants.values().value(QStringLiteral("ParamDeskVisible")), 0.0);

    QTemporaryDir dir;
    QVERIFY(writeReactionFixture(dir));
    for (bool pat : {false, true}) for (int entryFrames : {60, 200}) {
        ParameterMotion reaction;
        QVERIFY(reaction.loadBusyLaptopMotion(QStringLiteral("assets/motions/busy-laptop.motion.json")));
        QVERIFY(reaction.loadMotionLibrary(dir.path()));
        reaction.setDeskWorkMode(true);
        reaction.advance(0.02);
        reaction.forceLaptopBusy();
        advanceFrames(reaction, entryFrames);
        const auto held = reaction.values();
        reaction.setState(PetController::State::Idle);
        QVERIFY(pat ? reaction.beginHeadPat() : reaction.playTurnEnded());
        advanceFrames(reaction, 60);
        QVERIFY(reaction.interactionActive());
        QCOMPARE(reaction.values().value(QStringLiteral("ParamDeskVisible")), 1.0);
        QCOMPARE(reaction.values().value(QStringLiteral("ParamLaptopVisible")),
                 held.value(QStringLiteral("ParamLaptopVisible")));
        QCOMPARE(reaction.values().value(QStringLiteral("ParamSitPose")),
                 held.value(QStringLiteral("ParamSitPose")));
        if (pat) reaction.endHeadPat();
        advanceFrames(reaction, 300);
        QVERIFY(!reaction.interactionActive());
        QCOMPARE(reaction.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
    }
}

void ParameterMotionTest::deskInterruptionsKeepActionHandsAndDeadlines() {
    for (const auto state : {PetController::State::Delete, PetController::State::Grass})
    for (int entryFrames : {4, 20, 200}) {
        ParameterMotion desk, reference;
        QVERIFY(desk.loadMotionLibrary(QStringLiteral("assets/motions")));
        QVERIFY(reference.loadMotionLibrary(QStringLiteral("assets/motions")));
        desk.setDeskWorkMode(true);
        desk.advance(0.02);
        desk.forceLaptopBusy();
        advanceFrames(desk, entryFrames);
        const bool reachedSeat = desk.values().value(QStringLiteral("ParamDeskVisible")) > 0.99;
        auto pose = desk.values();
        pose.remove(QStringLiteral("ParamDeskVisible"));
        reference.setPreviewPose(pose);
        reference.setState(state);
        desk.setState(state);
        QCOMPARE(desk.actionDuration(state), reference.actionDuration(state));
        int deskFinish = -1, referenceFinish = -1;
        bool sawCoverRelease = false;
        bool releasedFullCover = false;
        for (int frame = 0; frame < 400; ++frame) {
            desk.advance(0.02);
            reference.advance(0.02);
            for (const QString& id : {QStringLiteral("ParamArmLA"), QStringLiteral("ParamArmRA"),
                 QStringLiteral("ParamElbowLA"), QStringLiteral("ParamElbowRA"),
                 QStringLiteral("ParamWristRA"), QStringLiteral("ParamGrassVisible"),
                 QStringLiteral("ParamGrassSwing"), QStringLiteral("ParamGrassTipBend")})
                QCOMPARE(desk.values().value(id), reference.values().value(id));
            const bool deskFinished = desk.consumeActionFinished();
            const bool referenceFinished = reference.consumeActionFinished();
            QCOMPARE(deskFinished, referenceFinished);
            if (deskFinished) deskFinish = frame;
            if (referenceFinished) referenceFinish = frame;
            const auto current = desk.values();
            releasedFullCover |= current.value(QStringLiteral("ParamDeskVisible")) > 0.99;
            // A partial cover is the interrupted exit lifting: everything
            // must already be neutral behind it. A bare open frame (the
            // visible sit ramp) is not a cover release.
            if (current.value(QStringLiteral("ParamDeskVisible")) < 1.0
                && (releasedFullCover
                    || current.value(QStringLiteral("ParamDeskVisible")) > 0.0)) {
                sawCoverRelease = true;
                QCOMPARE(current.value(QStringLiteral("ParamBusyLaptop")), 0.0);
                QCOMPARE(current.value(QStringLiteral("ParamSitPose")), 0.0);
                QCOMPARE(current.value(QStringLiteral("ParamLaptopVisible")), 0.0);
            }
        }
        QVERIFY(deskFinish >= 0 && deskFinish == referenceFinish);
        if (reachedSeat) QVERIFY(sawCoverRelease);
        desk.setState(PetController::State::Busy); // Preserved laptop choice resumes.
        for (int frame = 0; frame < 100; ++frame) {
            desk.advance(0.02);
            // The visible sit ramp runs before the desk returns; only the
            // texture/laptop engagement requires the cover.
            if (desk.values().value(QStringLiteral("ParamBusyLaptop")) > 0.0
                || desk.values().value(QStringLiteral("ParamLaptopVisible")) > 0.0)
                QCOMPARE(desk.values().value(QStringLiteral("ParamDeskVisible")), 1.0);
        }
        QVERIFY(desk.values().value(QStringLiteral("ParamSitPose")) > 0.99);
    }
}

void ParameterMotionTest::deskDisabledPreservesExistingTrajectory() {
    ParameterMotion reference, optional;
    for (auto* motion : {&reference, &optional}) {
        QVERIFY(motion->loadMotionLibrary(QStringLiteral("assets/motions")));
        motion->setBusyRandomSeed(1234);
        motion->advance(0.02);
    }
    optional.setDeskWorkMode(false);
    for (const auto state : {PetController::State::Busy, PetController::State::Idle,
         PetController::State::Delete, PetController::State::Grass, PetController::State::Busy}) {
        reference.setState(state);
        optional.setState(state);
        if (state == PetController::State::Busy) {
            reference.forceLaptopBusy();
            optional.forceLaptopBusy();
        }
        for (int frame = 0; frame < 100; ++frame) {
            reference.advance(0.02);
            optional.advance(0.02);
            QCOMPARE(optional.values(), reference.values());
            QVERIFY(!optional.values().contains(QStringLiteral("ParamDeskVisible")));
            QCOMPARE(optional.consumeActionFinished(), reference.consumeActionFinished());
        }
    }
}

void ParameterMotionTest::deskVisibilityIsReservedForActivity() {
    QTemporaryDir dir;
    QFile reaction(dir.filePath(QStringLiteral("turn-ended.motion.json")));
    QVERIFY(reaction.open(QIODevice::WriteOnly));
    reaction.write(R"({"duration":1,"constants":{"ParamDeskVisible":0},"keyframes":[
        {"time":0,"parameters":{"ParamAngleY":0}},
        {"time":1,"parameters":{"ParamAngleY":5}}]})");
    reaction.close();
    ParameterMotion motion;
    QString error;
    QVERIFY(!motion.loadMotionLibrary(dir.path(), &error));
    QVERIFY(error.contains(QStringLiteral("ParamDeskVisible")));
    QVERIFY(error.contains(QStringLiteral("activity")));
}

// Presence gaze (interaction-first r1): the eyes follow the cursor WHEREVER it
// is (near or far), settle to centre only once the cursor has been still for a
// while, and stand down for a state that owns the face. Sign-agnostic on
// purpose -- the pose probe fixes the direction; this proves the coupling, the
// still-cursor idle, and the suppression.
void ParameterMotionTest::gazeFollowsCursorAndSettlesToCenter() {
    ParameterMotion motion;
    motion.setState(PetController::State::Idle);
    QCOMPARE(motion.values().value(QStringLiteral("ParamEyeBallX")), 0.0);

    // Tracking stays on even when the cursor is FAR (near=false): she follows
    // the direction it sits in, wherever the mouse is.
    motion.setGazeTarget(1.0, 0.0, false, true);   // to one side, moving
    advanceFrames(motion, 60);                     // ~1.2 s to settle
    const double far = motion.values().value(QStringLiteral("ParamEyeBallX"));
    QVERIFY(qAbs(far) > 0.1);

    // "Near" only adds the smile -- it must not change the eye direction.
    motion.setGazeTarget(1.0, 0.0, true, true);
    advanceFrames(motion, 20);
    QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamEyeBallX")) - far) < 0.08);

    // Cursor to the side but STILL for a while -> idle: settle back to centre.
    motion.setGazeTarget(1.0, 0.0, false, false);
    advanceFrames(motion, 400);                    // 8 s > kIdleAfter (6 s)
    QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamEyeBallX"))) < 0.15);

    // A delete owns the face; the gaze must stand down and stop adding.
    motion.setState(PetController::State::Delete);
    motion.setGazeTarget(1.0, 0.0, true, true);
    advanceFrames(motion, 40);
    QVERIFY(qAbs(motion.values().value(QStringLiteral("ParamEyeBallX"))) < 0.1);
}

void ParameterMotionTest::workReactionsYieldToForeground() {
    for (const bool seated : {false, true}) {
        for (const bool cheer : {false, true}) {
            for (const auto foreground : {PetController::State::Delete, PetController::State::Grass}) {
                ParameterMotion motion;
                QString error;
                QVERIFY2(motion.loadMotionLibrary(QStringLiteral("assets/motions"), &error), qPrintable(error));
                motion.setDeskWorkMode(true);
                if (seated) {
                    motion.forceLaptopBusy();
                    advanceFrames(motion, 80);
                } else {
                    motion.setState(PetController::State::Idle);
                    advanceFrames(motion, 1);
                }
                QVERIFY(cheer ? motion.playCheer() : motion.playGreet());
                advanceFrames(motion, 10);
                QVERIFY(motion.interactionActive());

                // An event owns the face as well as the body. A work cue left
                // running here would overwrite the deletion/grass face later
                // in applyInteraction(), despite the background state changing.
                motion.setState(foreground);
                QVERIFY(!motion.interactionActive());
                QVERIFY(motion.interactionId().isEmpty());
                motion.advance(0.02);
                for (double value : motion.values()) QVERIFY(std::isfinite(value));

                // Returning to the latest background must not resurrect a
                // greeting or celebration that the event already interrupted.
                motion.setState(seated ? PetController::State::Busy : PetController::State::Idle);
                advanceFrames(motion, 10);
                QVERIFY(!motion.interactionActive());
            }
        }
    }
}

void ParameterMotionTest::workPosturePreferenceDoesNotStartWorkOrChangeIdle() {
    using Preference = ParameterMotion::WorkPosturePreference;
    ParameterMotion subject, reference;
    for (auto* player : {&subject, &reference}) {
        QVERIFY(player->loadMotionLibrary(QStringLiteral("assets/motions")));
        player->setDeskWorkMode(true);
        player->setProceduralBlink(false);
        player->setBusyRandomSeed(1234);
    }
    QCOMPARE(subject.workPosturePreference(), Preference::Automatic);
    for (const auto choice : {Preference::Standing, Preference::Laptop, Preference::Automatic}) {
        const auto before = subject.values();
        subject.setWorkPosturePreference(choice);
        QCOMPARE(subject.workPosturePreference(), choice);
        QCOMPARE(subject.values(), before);
        for (int frame = 0; frame < 30; ++frame) {
            subject.advance(0.02);
            reference.advance(0.02);
            QCOMPARE(subject.values(), reference.values());
            QCOMPARE(subject.busyPhaseTime(), 0.0);
            QCOMPARE(subject.continuousBusyTime(), 0.0);
            QCOMPARE(subject.nextBusyChoiceTime(), 40.0);
            QVERIFY(!subject.isLaptopBusy());
        }
    }
    subject.setWorkPosturePreference(static_cast<Preference>(999));
    QCOMPARE(subject.workPosturePreference(), Preference::Automatic);
    subject.setState(PetController::State::Busy);
    reference.setState(PetController::State::Busy);
    for (int frame = 0; frame < 4200; ++frame) {
        subject.advance(0.02);
        reference.advance(0.02);
        QCOMPARE(subject.values(), reference.values());
        QCOMPARE(subject.isLaptopBusy(), reference.isLaptopBusy());
        QCOMPARE(subject.nextBusyChoiceTime(), reference.nextBusyChoiceTime());
    }
}

void ParameterMotionTest::workPostureFixedChoicesKeepDelayAndChoiceClock() {
    using Preference = ParameterMotion::WorkPosturePreference;
    for (const auto preference : {Preference::Standing, Preference::Laptop}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
        motion.setDeskWorkMode(true);
        motion.setBusyRandomSeed(1234);
        motion.setWorkPosturePreference(preference);
        motion.setState(PetController::State::Busy);
        for (int frame = 0; frame < 475; ++frame) {
            motion.advance(0.02);
            QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0);
            QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
        }
        for (int frame = 0; frame < 4025; ++frame) motion.advance(0.02);
        QCOMPARE(motion.isLaptopBusy(), preference == Preference::Laptop);
        QVERIFY(std::abs(motion.busyPhaseTime() - 90.0) < 1e-8);
        QVERIFY(std::abs(motion.continuousBusyTime() - 90.0) < 1e-8);
        QCOMPARE(motion.nextBusyChoiceTime(), 120.0);
        QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")),
                 preference == Preference::Laptop ? 1.0 : 0.0);
        const auto before = motion.values();
        motion.setState(PetController::State::Idle);
        QCOMPARE(motion.values(), before);
        QCOMPARE(motion.workPosturePreference(), preference);
        advanceFrames(motion, 300);
        QCOMPARE(motion.continuousBusyTime(), 0.0);
        QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0);
        QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
    }
    // A fixed laptop preference is safe without the optional laptop clip.
    ParameterMotion missing;
    missing.setWorkPosturePreference(Preference::Laptop);
    missing.setState(PetController::State::Busy);
    advanceFrames(missing, 2100);
    QVERIFY(!missing.isLaptopBusy());
    QCOMPARE(missing.values().value(QStringLiteral("ParamSitPose")), 0.0);
}

void ParameterMotionTest::workPostureAutomaticResumesAtNextScheduledChoice() {
    using Preference = ParameterMotion::WorkPosturePreference;
    quint32 seed = 1;
    for (; seed < 1000; ++seed) {
        QRandomGenerator random(seed);
        for (int draw = 0; draw < 4; ++draw) random.generateDouble();
        if (random.generateDouble() < 0.4) break; // the 160s boundary chooses laptop
    }
    QVERIFY(seed < 1000);
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
    motion.setDeskWorkMode(true);
    motion.setBusyRandomSeed(seed);
    motion.setWorkPosturePreference(Preference::Standing);
    motion.setState(PetController::State::Busy);
    advanceFrames(motion, 6250); // 125s: initial + 40/80/120 choices consumed
    QCOMPARE(motion.nextBusyChoiceTime(), 160.0);
    const double phase = motion.busyPhaseTime();
    const double work = motion.continuousBusyTime();
    const auto before = motion.values();
    motion.setWorkPosturePreference(Preference::Automatic);
    QCOMPARE(motion.values(), before);
    QCOMPARE(motion.busyPhaseTime(), phase);
    QCOMPARE(motion.continuousBusyTime(), work);
    for (int frame = 0; frame < 1749; ++frame) {
        motion.advance(0.02);
        QVERIFY(!motion.isLaptopBusy());
        QCOMPARE(motion.nextBusyChoiceTime(), 160.0);
    }
    advanceFrames(motion, 2);
    QVERIFY(motion.isLaptopBusy());
    QCOMPARE(motion.nextBusyChoiceTime(), 200.0);
}

void ParameterMotionTest::workPostureWaitsForForegroundOwners() {
    using Preference = ParameterMotion::WorkPosturePreference;
    for (const QString& owner : {QStringLiteral("drag"), QStringLiteral("memo"),
         QStringLiteral("sleep"), QStringLiteral("head-pat"), QStringLiteral("greet"),
         QStringLiteral("rest"), QStringLiteral("rice")}) {
        ParameterMotion subject, reference;
        const bool seated = owner == QStringLiteral("rice");
        for (auto* player : {&subject, &reference}) {
            QVERIFY(player->loadMotionLibrary(QStringLiteral("assets/motions")));
            player->setDeskWorkMode(true);
            player->setProceduralBlink(false);
            player->setBusyRandomSeed(1234);
            player->setWorkPosturePreference(seated ? Preference::Laptop : Preference::Standing);
            player->setState(PetController::State::Busy);
            advanceFrames(*player, 650);
            if (owner == QStringLiteral("drag")) player->beginDragMotion();
            if (owner == QStringLiteral("memo")) player->beginMemoMotion();
            if (owner == QStringLiteral("sleep")) player->beginSleepMotion();
            if (owner == QStringLiteral("head-pat")) QVERIFY(player->beginHeadPat(1.0));
            if (owner == QStringLiteral("greet")) QVERIFY(player->playGreet());
            if (owner == QStringLiteral("rest")) QVERIFY(player->playStretch());
            if (owner == QStringLiteral("rice")) QVERIFY(player->playRiceBowl());
        }
        subject.setWorkPosturePreference(seated ? Preference::Standing : Preference::Laptop);
        for (int frame = 0; frame < 3; ++frame) {
            subject.advance(0.02);
            reference.advance(0.02);
            QCOMPARE(subject.isLaptopBusy(), seated);
            QCOMPARE(subject.values(), reference.values());
            QCOMPARE(subject.busyPhaseTime(), reference.busyPhaseTime());
            QCOMPARE(subject.continuousBusyTime(), reference.continuousBusyTime());
            QCOMPARE(subject.nextBusyChoiceTime(), reference.nextBusyChoiceTime());
        }
        subject.cancelDragMotion();
        subject.cancelMemoMotion();
        subject.cancelSleepMotion();
        subject.cancelInteraction();
        subject.cancelRiceBowl();
        advanceFrames(subject, 400);
        QCOMPARE(subject.isLaptopBusy(), !seated);
        QCOMPARE(subject.values().value(QStringLiteral("ParamDeskVisible")), seated ? 0.0 : 1.0);
    }
}

void ParameterMotionTest::workPostureWaitsForLastPauseFrameAndPostureRamps() {
    using Preference = ParameterMotion::WorkPosturePreference;
    ParameterMotion rest;
    QVERIFY(rest.loadMotionLibrary(QStringLiteral("assets/motions")));
    rest.setDeskWorkMode(true);
    rest.setWorkPosturePreference(Preference::Standing);
    rest.setState(PetController::State::Busy);
    advanceFrames(rest, 650);
    QVERIFY(rest.playStretch());
    const double phase = rest.busyPhaseTime();
    const double work = rest.continuousBusyTime();
    rest.setWorkPosturePreference(Preference::Laptop);
    int frames = 0;
    while (rest.workPauseActive() && frames++ < 200) {
        rest.advance(0.02);
        QVERIFY(!rest.isLaptopBusy()); // includes the frame retiring the pause
        QCOMPARE(rest.busyPhaseTime(), phase);
        QCOMPARE(rest.continuousBusyTime(), work);
    }
    QVERIFY(!rest.workPauseActive());
    rest.advance(0.02);
    QVERIFY(rest.isLaptopBusy());
    QCOMPARE(rest.deskWorkPhase(), QStringLiteral("sit_down"));
    advanceFrames(rest, 10);
    rest.setWorkPosturePreference(Preference::Standing);
    frames = 0;
    bool changed = false;
    while (!changed && frames++ < 300) {
        const QString beforePhase = rest.deskWorkPhase();
        const auto before = rest.values();
        rest.advance(0.02);
        changed = !rest.isLaptopBusy();
        if (changed) {
            QCOMPARE(beforePhase, QStringLiteral("work"));
            QVERIFY(before.value(QStringLiteral("ParamBusyLaptop")) >= 0.995);
            QVERIFY(before.value(QStringLiteral("ParamLaptopVisible")) >= 0.995);
            QCOMPARE(before.value(QStringLiteral("ParamSitPose")), 1.0);
        }
    }
    QVERIFY(changed);
    rest.setWorkPosturePreference(Preference::Laptop); // do not reverse the exit
    frames = 0;
    while (rest.deskWorkPhase() != QStringLiteral("hidden") && frames++ < 400) {
        rest.advance(0.02);
        QVERIFY(!rest.isLaptopBusy());
    }
    QCOMPARE(rest.deskWorkPhase(), QStringLiteral("hidden"));
    rest.advance(0.02);
    QVERIFY(rest.isLaptopBusy());
}

void ParameterMotionTest::workPostureWaitsForWaveBoxAndForegroundStates() {
    using Preference = ParameterMotion::WorkPosturePreference;
    for (const bool box : {false, true}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
        motion.setDeskWorkMode(true);
        motion.setWorkPosturePreference(Preference::Laptop);
        advanceFrames(motion, 50);
        QVERIFY(box ? motion.playBoxPeek() : motion.playWave());
        advanceFrames(motion, 15);
        motion.setState(PetController::State::Busy);
        QVERIFY(!motion.isLaptopBusy());
        int frames = 0;
        while ((box ? motion.boxPeekActive() : motion.waveActive()) && frames++ < 300) {
            motion.advance(0.02);
            QVERIFY(!motion.isLaptopBusy());
        }
        QVERIFY(frames < 300);
        motion.advance(0.02);
        QVERIFY(motion.isLaptopBusy());
        QCOMPARE(motion.values().value(QStringLiteral("ParamSitPose")), 0.0); // still <10s
    }
    for (const auto foreground : {PetController::State::Delete, PetController::State::Grass}) {
        ParameterMotion motion;
        QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
        motion.setDeskWorkMode(true);
        motion.setWorkPosturePreference(Preference::Standing);
        motion.setState(PetController::State::Busy);
        advanceFrames(motion, 650);
        motion.setState(foreground);
        const double phase = motion.busyPhaseTime();
        const double work = motion.continuousBusyTime();
        motion.setWorkPosturePreference(Preference::Laptop);
        advanceFrames(motion, 25);
        QVERIFY(!motion.isLaptopBusy());
        QCOMPARE(motion.busyPhaseTime(), phase);
        QCOMPARE(motion.continuousBusyTime(), work);
        motion.setState(PetController::State::Busy);
        advanceFrames(motion, 200);
        QVERIFY(motion.isLaptopBusy());
        QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 1.0);
    }
}

void ParameterMotionTest::workPosturePreviewDoesNotChangeSavedPreference() {
    using Preference = ParameterMotion::WorkPosturePreference;
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
    motion.setDeskWorkMode(true);
    motion.setWorkPosturePreference(Preference::Standing);
    motion.forceLaptopBusy();
    advanceFrames(motion, 200);
    QVERIFY(motion.isLaptopBusy());
    QCOMPARE(motion.workPosturePreference(), Preference::Standing);
    QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 1.0);
    // The preview turn expires while another real turn leaves the background
    // Busy, so no stateChanged signal will arrive to release the override.
    const double phase = motion.busyPhaseTime();
    const double work = motion.continuousBusyTime();
    motion.endWorkPosturePreview();
    QCOMPARE(motion.busyPhaseTime(), phase);
    QCOMPARE(motion.continuousBusyTime(), work);
    motion.advance(0.02);
    QVERIFY(!motion.isLaptopBusy());
    advanceFrames(motion, 300);
    QCOMPARE(motion.values().value(QStringLiteral("ParamDeskVisible")), 0.0);
    motion.setWorkPosturePreference(Preference::Laptop);
    motion.forceStandingBusy();
    advanceFrames(motion, 100);
    QVERIFY(!motion.isLaptopBusy());
    QCOMPARE(motion.workPosturePreference(), Preference::Laptop);
    motion.setState(PetController::State::Busy);
    motion.advance(0.02);
    QVERIFY(motion.isLaptopBusy());
}

void ParameterMotionTest::permissionCueRequiresApprovalAndSettledPosture() {
    ParameterMotion motion;
    QVERIFY(!motion.playPermissionCue(true)); // absence is harmless
    QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
    motion.setDeskWorkMode(true);
    QVERIFY(!motion.playPermissionCue(true)); // review cannot invent a Busy state
    motion.forceStandingBusy();
    advanceFrames(motion, 50);
    const auto before = motion.values();
    QVERIFY(!motion.playPermissionCue()); // pending must never leak into an ordinary entry
    QCOMPARE(motion.values(), before);
    QVERIFY(motion.playPermissionCue(true));
    QCOMPARE(motion.interactionId(), QStringLiteral("permission-cue"));
    QVERIFY(motion.permissionCueActive());
    QVERIFY(!motion.workPauseActive()); // this short cue does not assert a blocked execution
    QVERIFY(!motion.playPermissionCue(true));
    QVERIFY(motion.cancelPendingInteraction()); // closing draft preview cancels it
    QVERIFY(!motion.interactionActive());

    motion.forceLaptopBusy();
    QVERIFY(!motion.playPermissionCue(true));
    advanceFrames(motion, 20);
    QVERIFY(!motion.playPermissionCue(true)); // sitting/cover transition still owns the pose
    advanceFrames(motion, 180);
    QVERIFY(motion.playPermissionCue(true));
    motion.cancelInteraction();
    motion.forceStandingBusy();
    QVERIFY(!motion.playPermissionCue(true)); // desk exit is not stable standing
    advanceFrames(motion, 220);
    QVERIFY(motion.playPermissionCue(true));
    motion.cancelInteraction();

    // The data gate also works after a future adoption. Approval alone cannot
    // bypass the posture gate, and preview-off leaves an approved clip alive.
    QTemporaryDir approved;
    QVERIFY(writePermissionCueFixture(approved, QStringLiteral("approved")));
    // Reload retains the other library clips; their phase readers must still
    // find the authored configuration in the new fixture directory.
    for (const QString& id : {QStringLiteral("head-pat"), QStringLiteral("grass-touch")}) {
        const QString name = id + QStringLiteral(".motion.json");
        QVERIFY(QFile::copy(QStringLiteral("assets/motions/") + name, approved.filePath(name)));
    }
    QString error;
    QVERIFY2(motion.loadMotionLibrary(approved.path(), &error), qPrintable(error));
    QVERIFY(motion.playPermissionCue());
    QVERIFY(!motion.cancelPendingInteraction());
    QVERIFY(motion.permissionCueActive());
    motion.cancelInteraction();

    ParameterMotion delayed;
    QVERIFY(delayed.loadMotionLibrary(QStringLiteral("assets/motions")));
    delayed.setDeskWorkMode(true);
    delayed.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Laptop);
    delayed.setState(PetController::State::Busy);
    advanceFrames(delayed, 100);
    QVERIFY(delayed.isLaptopBusy());
    QVERIFY(!delayed.playPermissionCue(true)); // ten-second sustained-work protection remains
    advanceFrames(delayed, 650);
    QVERIFY(delayed.playPermissionCue(true));
}

void ParameterMotionTest::permissionCueFreezesClocksAndContactPose() {
    for (const bool seated : {false, true}) {
        for (const bool desk : {false, true}) {
            ParameterMotion motion;
            QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
            motion.setDeskWorkMode(desk);
            motion.setProceduralBlink(false);
            if (seated) motion.forceLaptopBusy();
            else motion.forceStandingBusy();
            advanceFrames(motion, 1999); // one frame before the forty-second choice
            const auto contact = motion.values();
            const double phase = motion.busyPhaseTime();
            const double work = motion.continuousBusyTime();
            const double choice = motion.nextBusyChoiceTime();
            const QString deskPhase = motion.deskWorkPhase();
            QVERIFY(phase < choice && choice - phase < 0.03);
            QVERIFY(motion.playPermissionCue(true));
            int frames = 0;
            double minimumEyes = 1.0;
            while (motion.permissionCueActive() && frames++ < 145) {
                motion.advance(0.02);
                QCOMPARE(motion.busyPhaseTime(), phase);
                QCOMPARE(motion.continuousBusyTime(), work);
                QCOMPARE(motion.nextBusyChoiceTime(), choice);
                QCOMPARE(motion.isLaptopBusy(), seated);
                QCOMPARE(motion.deskWorkPhase(), deskPhase);
                if (!motion.permissionCueActive()) continue;
                for (const QString& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
                     QStringLiteral("ParamLaptopVisible"), QStringLiteral("ParamLaptopRock"),
                     QStringLiteral("ParamBusyTypingL"), QStringLiteral("ParamBusyTypingR"),
                     QStringLiteral("ParamArmLA"), QStringLiteral("ParamArmRA"),
                     QStringLiteral("ParamElbowLA"), QStringLiteral("ParamElbowRA"),
                     QStringLiteral("ParamWristRA"), QStringLiteral("ParamDeskVisible")})
                    QCOMPARE(motion.values().value(id), contact.value(id));
                minimumEyes = std::min(minimumEyes, motion.values().value(QStringLiteral("ParamEyeLOpen")));
                if (motion.interactionTime() > 1.1 && motion.interactionTime() < 1.8) {
                    QVERIFY(std::abs(motion.values().value(QStringLiteral("ParamAngleZ"))) < 0.02);
                    QVERIFY(motion.values().value(QStringLiteral("ParamMouthOpenY")) < 0.001);
                    QVERIFY(motion.bubblePulse() < 0.001);
                }
            }
            QVERIFY(!motion.interactionActive());
            QVERIFY(frames >= 139 && frames <= 141);
            QVERIFY(minimumEyes < 0.2); // the authored slow blink is visible
            advanceFrames(motion, 2);
            QVERIFY(motion.busyPhaseTime() > phase);
            QVERIFY(motion.continuousBusyTime() > work);
            QCOMPARE(motion.nextBusyChoiceTime(), choice + 40.0);
            QVERIFY(!motion.interactionActive()); // no queued cue after resumption
        }
    }
}

void ParameterMotionTest::permissionCueRefusesOwnersAndYieldsWithoutReplay() {
    for (const bool seated : {false, true}) {
        for (const QString& owner : {QStringLiteral("drag"), QStringLiteral("memo"),
             QStringLiteral("sleep"), QStringLiteral("pat"), QStringLiteral("greet"),
             QStringLiteral("rest"), QStringLiteral("rice")}) {
            if (!seated && owner == QStringLiteral("rice")) continue;
            ParameterMotion motion;
            QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
            motion.setDeskWorkMode(true);
            if (seated) motion.forceLaptopBusy();
            else motion.forceStandingBusy();
            advanceFrames(motion, 200);
            if (owner == QStringLiteral("drag")) motion.beginDragMotion();
            if (owner == QStringLiteral("memo")) motion.beginMemoMotion();
            if (owner == QStringLiteral("sleep")) motion.beginSleepMotion();
            if (owner == QStringLiteral("pat")) QVERIFY(motion.beginHeadPat());
            if (owner == QStringLiteral("greet")) QVERIFY(motion.playGreet());
            if (owner == QStringLiteral("rest")) QVERIFY(motion.playStretch());
            if (owner == QStringLiteral("rice")) QVERIFY(motion.playRiceBowl());
            const QString interaction = motion.interactionId();
            QVERIFY2(!motion.playPermissionCue(true), qPrintable(owner));
            QCOMPARE(motion.interactionId(), interaction);
            if (owner == QStringLiteral("drag")) QVERIFY(motion.dragMotionActive());
            if (owner == QStringLiteral("memo")) QVERIFY(motion.memoMotionActive());
            if (owner == QStringLiteral("sleep")) QVERIFY(motion.sleepMotionActive());
            if (owner == QStringLiteral("rice")) QVERIFY(motion.riceBowlActive());
        }
        for (const QString& event : {QStringLiteral("stop"), QStringLiteral("delete"),
             QStringLiteral("grass"), QStringLiteral("drag"), QStringLiteral("pat"),
             QStringLiteral("memo"), QStringLiteral("memo-celebrate"),
             QStringLiteral("sleep"), QStringLiteral("preview-off")}) {
            ParameterMotion motion;
            QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
            motion.setDeskWorkMode(true);
            if (seated) motion.forceLaptopBusy();
            else motion.forceStandingBusy();
            advanceFrames(motion, 200);
            QVERIFY(motion.playPermissionCue(true));
            advanceFrames(motion, 40); // interrupt after eyes/head have taken ownership
            if (event == QStringLiteral("stop")) motion.setState(PetController::State::Idle);
            if (event == QStringLiteral("delete")) motion.setState(PetController::State::Delete);
            if (event == QStringLiteral("grass")) motion.setState(PetController::State::Grass);
            if (event == QStringLiteral("drag")) motion.beginDragMotion();
            if (event == QStringLiteral("pat")) QVERIFY(motion.beginHeadPat(-1.0));
            if (event == QStringLiteral("memo")) motion.beginMemoMotion();
            if (event == QStringLiteral("memo-celebrate")) motion.beginMemoCelebrate();
            if (event == QStringLiteral("sleep")) motion.beginSleepMotion();
            if (event == QStringLiteral("preview-off")) QVERIFY(motion.cancelPendingInteraction());
            QVERIFY2(!motion.permissionCueActive(), qPrintable(event));
            if (event == QStringLiteral("pat")) QCOMPARE(motion.interactionId(), QStringLiteral("head-pat"));
            motion.cancelDragMotion();
            motion.cancelMemoMotion();
            motion.cancelSleepMotion();
            motion.cancelInteraction();
            motion.setState(PetController::State::Busy);
            advanceFrames(motion, 200);
            QVERIFY(!motion.interactionActive());
        }
    }
}

void ParameterMotionTest::permissionCueRejectsReservedChannelsAndUnsafeHead() {
    for (const auto& illegal : {qMakePair(QStringLiteral("ParamArmRA"), 1.0),
         qMakePair(QStringLiteral("ParamSitPose"), 1.0), qMakePair(QStringLiteral("ParamBusyTypingL"), 0.0),
         qMakePair(QStringLiteral("ParamBodyAngleX"), 1.0), qMakePair(QStringLiteral("ParamShiftX"), 1.0),
         qMakePair(QStringLiteral("ParamAngleZ"), 0.1), qMakePair(QStringLiteral("ParamAngleY"), 4.1)}) {
        QTemporaryDir dir;
        QVERIFY(writePermissionCueFixture(dir, QStringLiteral("pending")));
        QFile file(dir.filePath(QStringLiteral("permission-cue.motion.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        auto keys = root.value(QStringLiteral("keyframes")).toArray();
        auto key = keys.at(4).toObject();
        auto parameters = key.value(QStringLiteral("parameters")).toObject();
        parameters.insert(illegal.first, illegal.second); // a later key must not bypass ownership
        key.insert(QStringLiteral("parameters"), parameters);
        keys[4] = key;
        root.insert(QStringLiteral("keyframes"), keys);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto json = QJsonDocument(root).toJson();
        QCOMPARE(file.write(json), qint64(json.size()));
        file.close();
        ParameterMotion motion;
        QString error;
        QVERIFY2(!motion.loadMotionLibrary(dir.path(), &error), qPrintable(illegal.first));
        QVERIFY(!error.isEmpty());
    }
    for (const QString& invalid : {QStringLiteral("duration"), QStringLiteral("loop"),
         QStringLiteral("mode"), QStringLiteral("channels")}) {
        QTemporaryDir dir;
        QVERIFY(writePermissionCueFixture(dir, QStringLiteral("pending")));
        QFile file(dir.filePath(QStringLiteral("permission-cue.motion.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (invalid == QStringLiteral("duration")) root.insert(invalid, 3.0);
        if (invalid == QStringLiteral("loop")) root.insert(invalid, true);
        if (invalid == QStringLiteral("mode")) root.insert(invalid, QStringLiteral("additive"));
        if (invalid == QStringLiteral("channels")) root.insert(invalid, QJsonObject{
            {QStringLiteral("ParamAngleY"), QJsonObject{{QStringLiteral("amplitude"), 1.0}}}});
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto json = QJsonDocument(root).toJson();
        QCOMPARE(file.write(json), qint64(json.size()));
        file.close();
        ParameterMotion motion;
        QString error;
        QVERIFY2(!motion.loadMotionLibrary(dir.path(), &error), qPrintable(invalid));
        QVERIFY(!error.isEmpty());
    }
}

QTEST_GUILESS_MAIN(ParameterMotionTest)
#include "ParameterMotionTest.moc"
