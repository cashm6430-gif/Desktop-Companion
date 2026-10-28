#include "../src/MotionClip.h"
#include "../src/MotionLibrary.h"
#include "../src/ParameterMotion.h"
#include "../src/PetController.h"
#include "../src/WorkReactionCoordinator.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>

class MotionClipTest final : public QObject {
    Q_OBJECT
private slots:
    void sharedKeysInterpolateAndClamp();
    void tracksOverrideSharedKeys();
    void pureTrackClipNeedsNoKeys();
    void loopWrapsInsideItsRange();
    void malformedDataIsRejected();
    void libraryNamesClipsAfterFiles();
    void libraryIsTheSingleDurationSource();
    void channelsEvaluateExactSine();
    void pulsesMatchLegacyRamp();
    void additiveAndBlendMetadataAreParsed();
    void legacyFileWithoutNewFieldsStillLoads();
    void malformedCurvesAreRejected();
    void propMarkersCoincideWithDrawnPoses();
    void idleMatchesLegacyGenerator();
    void loopingActionsSeamlessAtLoopPoint();
    void busyStandReadsAsThinking();
    void thinkingBubblePopsWhileBusy();
    void deleteSwingIsReadable();
    void workStopsUseOneNeutralReactionAfterConcurrentTurns();
    void workDurationNotesUseControllerClockAndThreshold();
    void newTurnsAndForegroundActionsInterruptWorkEnd();
    void workEndDuringForegroundIsDroppedWithoutReplay();
    void cleanupAndDuplicateStopsDoNotInventCompletion();
    void refusedWorkEndIsObservedWithoutQueueing();
    void workEndPreservesSeatedPoseThroughRealSignals();
    void quietWorkTracksConcurrentTurnsWithoutReplay();
    void quietWorkStartedBehindForegroundNeverReplaysGreeting();
    void quietWorkSessionsBehindForegroundConsumeSeparately();
    void quietWorkPreservesForegroundAndManualReactions();
    void quietPolicyLetsRunningReactionsFinish();
    void absentQuietPredicateAllowsWorkReactions();
    void permissionRequestsNominateApprovedCueOnceAndTrackItsOwner();
    void permissionQuietAndRefusedRequestsNeverQueue();
    void permissionCleanupAndNewTurnsCancelOnlyOwnedCue();
    void permissionResolutionDoesNotCancelManualOrOtherReactions();
    void permissionPendingClipCannotEnterAutomaticPath();
};

namespace {
bool writeJson(QTemporaryFile& file, const QByteArray& json) {
    if (!file.open()) return false;
    file.write(json);
    file.flush();
    return true;
}

struct WorkAttempt {
    QString id;
    bool started;
    QString reason;
};

// Uses the same signal wiring as PetWindow, with the real controller/player.
// Only elapsed wall time is substituted: a 30-second turn is exercised without
// sleeping, sending hooks, creating a widget, or changing installed settings.
struct WorkHarness {
    qint64 now = 1000;
    PetController controller{nullptr, [this] { return now; }};
    ParameterMotion motion;
    QList<WorkAttempt> attempts;
    QList<qint64> notes;
    QStringList reactionAtNote;
    bool automaticAllowed = true;
    WorkReactionCoordinator coordinator{&controller, &motion, nullptr,
        [this](PetController::State state) { motion.setState(state); },
        [this](qint64 duration) {
            notes.append(duration);
            reactionAtNote.append(motion.interactionId());
        },
        [this](const QString& id, bool started, const QString& reason) {
            attempts.append({id, started, reason});
        }, [this] { return automaticAllowed; }};

    bool load() {
        controller.setActionFallbackEnabled(false);
        motion.setDeskWorkMode(true);
        return motion.loadMotionLibrary(QStringLiteral("assets/motions"));
    }
    void advance(double seconds) {
        for (int i = 0; i < qRound(seconds / 0.02); ++i) motion.advance(0.02);
    }
    int returns() const {
        return std::count_if(attempts.cbegin(), attempts.cend(), [](const WorkAttempt& attempt) {
            return attempt.id == QStringLiteral("turn-ended");
        });
    }
    int permissionAttempts() const {
        return std::count_if(attempts.cbegin(), attempts.cend(), [](const WorkAttempt& attempt) {
            return attempt.id == QStringLiteral("permission-cue");
        });
    }
    int permissionStarts() const {
        return std::count_if(attempts.cbegin(), attempts.cend(), [](const WorkAttempt& attempt) {
            return attempt.id == QStringLiteral("permission-cue") && attempt.started;
        });
    }
};

// QCOMPARE compares doubles with an absolute tolerance, which would hide a
// changed summation order. The migration must reproduce the curves it replaced
// bit for bit, so compare exactly and print both values at full precision.
void compareExact(double actual, double expected, const char* label, const char* file, int line) {
    if (actual == expected) return;
    QTest::qFail(qPrintable(QStringLiteral("%1 differs: %2 != %3")
        .arg(QString::fromLatin1(label),
             QString::number(actual, 'g', 17),
             QString::number(expected, 'g', 17))), file, line);
}
} // namespace

#define COMPARE_EXACT(actual, expected) \
    compareExact((actual), (expected), #actual, __FILE__, __LINE__)

void MotionClipTest::sharedKeysInterpolateAndClamp() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":2,"keyframes":[
        {"time":0,"parameters":{"ParamA":0}},
        {"time":2,"parameters":{"ParamA":10}}]})"));
    MotionClip clip;
    QString error;
    QVERIFY(clip.loadJson(file.fileName(), &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(clip.duration(), 2.0);
    QVERIFY(!clip.isLoop());
    // smoothstep(0.5) = 0.5, so the midpoint lands exactly halfway.
    QCOMPARE(clip.sample(1.0).value(QStringLiteral("ParamA")), 5.0);
    QCOMPARE(clip.sample(0.5).value(QStringLiteral("ParamA")), 10.0 * 0.15625);
    // Clamped, never extrapolated.
    QCOMPARE(clip.sample(-3.0).value(QStringLiteral("ParamA")), 0.0);
    QCOMPARE(clip.sample(99.0).value(QStringLiteral("ParamA")), 10.0);
}

void MotionClipTest::tracksOverrideSharedKeys() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":4,
        "keyframes":[{"time":0,"parameters":{"ParamA":0}},{"time":4,"parameters":{"ParamA":4}}],
        "tracks":{"ParamA":[[0,0],[1,10],[4,0]],"ParamB":[[0,1],[4,1]]}})"));
    MotionClip clip;
    QVERIFY(clip.loadJson(file.fileName()));
    QVERIFY(clip.hasTracks());
    // ParamA follows its own track (peaks at 10) instead of the shared keys.
    QCOMPARE(clip.sample(1.0).value(QStringLiteral("ParamA")), 10.0);
    QVERIFY(clip.sample(2.0).value(QStringLiteral("ParamA")) < 10.0);
    // ParamB exists only as a track and still resolves.
    QCOMPARE(clip.sample(2.0).value(QStringLiteral("ParamB")), 1.0);
}

void MotionClipTest::pureTrackClipNeedsNoKeys() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"tracks":{"ParamA":[[0,0],[2,1]]}})"));
    MotionClip clip;
    QVERIFY(clip.loadJson(file.fileName()));
    QVERIFY(clip.keys().isEmpty());
    QCOMPARE(clip.duration(), 2.0);
    QCOMPARE(clip.sample(1.0).value(QStringLiteral("ParamA")), 0.5);
}

void MotionClipTest::loopWrapsInsideItsRange() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":8,"loop":true,"interpolation":"linear","keyframes":[
        {"time":0,"parameters":{"ParamA":0}},
        {"time":8,"parameters":{"ParamA":8}}]})"));
    MotionClip clip;
    QVERIFY(clip.loadJson(file.fileName()));
    QVERIFY(clip.isLoop());
    QCOMPARE(clip.loopEnd(), 8.0);
    // Linear interpolation makes the wrap obvious: 9s must read like 1s.
    QCOMPARE(clip.sample(1.0).value(QStringLiteral("ParamA")), 1.0);
    QCOMPARE(clip.sample(9.0).value(QStringLiteral("ParamA")), 8.0); // clamped
    QCOMPARE(clip.sampleLooped(9.0).value(QStringLiteral("ParamA")), 1.0);
    QCOMPARE(clip.sampleLooped(8.0).value(QStringLiteral("ParamA")), 0.0);
}

void MotionClipTest::malformedDataIsRejected() {
    MotionClip clip;
    QTemporaryFile duplicate;
    QVERIFY(writeJson(duplicate, R"({"keyframes":[{"time":0,"parameters":{"A":0}},{"time":0,"parameters":{"A":1}}]})"));
    QVERIFY(!clip.loadJson(duplicate.fileName()));

    QTemporaryFile mismatched;
    QVERIFY(writeJson(mismatched, R"({"keyframes":[{"time":0,"parameters":{"A":0}},{"time":1,"parameters":{"B":1}}]})"));
    QVERIFY(!clip.loadJson(mismatched.fileName()));

    QTemporaryFile shortTrack;
    QVERIFY(writeJson(shortTrack, R"({"tracks":{"A":[[0,1]]}})"));
    QVERIFY(!clip.loadJson(shortTrack.fileName()));

    QTemporaryFile badLoop;
    QVERIFY(writeJson(badLoop, R"({"duration":2,"loop":true,"loopEnd":5,"keyframes":[
        {"time":0,"parameters":{"A":0}},{"time":2,"parameters":{"A":1}}]})"));
    QVERIFY(!clip.loadJson(badLoop.fileName()));
}

void MotionClipTest::libraryNamesClipsAfterFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile first(dir.filePath(QStringLiteral("alpha.motion.json")));
    QFile second(dir.filePath(QStringLiteral("beta.motion.json")));
    QVERIFY(first.open(QIODevice::WriteOnly));
    first.write(R"({"duration":3,"keyframes":[{"time":0,"parameters":{"A":0}},{"time":3,"parameters":{"A":1}}]})");
    first.close();
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.write(R"({"duration":5,"loop":true,"keyframes":[{"time":0,"parameters":{"A":0}},{"time":5,"parameters":{"A":1}}]})");
    second.close();

    MotionLibrary library;
    QString error;
    QVERIFY2(library.loadDirectory(dir.path(), &error), qPrintable(error));
    QCOMPARE(library.ids(), QStringList({QStringLiteral("alpha"), QStringLiteral("beta")}));
    QCOMPARE(library.duration(QStringLiteral("alpha")), 3.0);
    QCOMPARE(library.duration(QStringLiteral("beta")), 5.0);
    QVERIFY(library.isLoop(QStringLiteral("beta")));
    QVERIFY(!library.isLoop(QStringLiteral("alpha")));
    QVERIFY(library.isProcedural(QStringLiteral("idle")));
}

void MotionClipTest::libraryIsTheSingleDurationSource() {
    // The state machine default and the library must never disagree.
    PetController controller;
    MotionLibrary library;
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
    QCOMPARE(motion.actionDuration(PetController::State::Grass), 6.9);
    QCOMPARE(library.duration(QStringLiteral("delete")), MotionLibrary::kDeleteDuration);
    QCOMPARE(motion.actionDuration(PetController::State::Delete), MotionLibrary::kDeleteDuration);
    QCOMPARE(library.duration(QStringLiteral("busy-laptop")), motion.library().duration(QStringLiteral("busy-laptop")));
    QVERIFY(library.duration(QStringLiteral("busy-laptop")) > 0.0);
    controller.setActionDuration(PetController::State::Grass, motion.actionDuration(PetController::State::Grass));
}

void MotionClipTest::channelsEvaluateExactSine() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":12,"loop":true,"channels":{
        "ParamAngleX":[{"type":"sine","amplitude":2.0,"frequency":0.68,"phase":0.0,"offset":0.0}],
        "ParamBreath":[{"type":"sine","amplitude":0.5,"frequency":2.1,"phase":0.0,"offset":0.5}],
        "ParamBodyAngleX":[{"type":"sine","amplitude":0.8,"frequency":0.68,"phase":-0.3,"offset":0.0}]}})"));
    MotionClip clip;
    QString error;
    QVERIFY2(clip.loadJson(file.fileName(), &error), qPrintable(error));
    QVERIFY(clip.hasCurves());
    QVERIFY(clip.keys().isEmpty());
    // The data must reproduce the legacy generator term for term.
    for (const double t : {0.0, 0.37, 1.5, 4.25, 9.9}) {
        COMPARE_EXACT(clip.sample(t).value(QStringLiteral("ParamAngleX")), 2.0 * qSin(t * 0.68));
        COMPARE_EXACT(clip.sample(t).value(QStringLiteral("ParamBreath")), 0.5 + 0.5 * qSin(t * 2.1));
        COMPARE_EXACT(clip.sample(t).value(QStringLiteral("ParamBodyAngleX")), 0.8 * qSin(t * 0.68 - 0.3));
    }
    // A pure-channel clip is a formula, not a sampled range: no clamping.
    COMPARE_EXACT(clip.sample(100.0).value(QStringLiteral("ParamAngleX")), 2.0 * qSin(100.0 * 0.68));
}

void MotionClipTest::pulsesMatchLegacyRamp() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":1.4,"interpolation":"smoothstep","pulses":{
        "ParamArmRA":[
            {"start":0.00,"peak":0.28,"end":0.48,"weight":-22.0},
            {"start":0.34,"peak":0.59,"end":0.91,"weight":30.0},
            {"start":0.82,"peak":1.02,"end":1.34,"weight":-8.0}]}})"));
    MotionClip clip;
    QVERIFY(clip.loadJson(file.fileName()));

    const auto smoothstep = [](double t) {
        t = std::clamp(t, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    const auto legacyPulse = [&smoothstep](double t, double start, double peak, double end) {
        if (t < start || t >= end) return 0.0;
        if (t < peak) return smoothstep((t - start) / (peak - start));
        return 1.0 - smoothstep((t - peak) / (end - peak));
    };
    for (const double t : {0.0, 0.1, 0.28, 0.4, 0.59, 0.82, 1.0, 1.2, 1.4}) {
        const double expected = -22.0 * legacyPulse(t, 0.00, 0.28, 0.48)
            + 30.0 * legacyPulse(t, 0.34, 0.59, 0.91)
            - 8.0 * legacyPulse(t, 0.82, 1.02, 1.34);
        COMPARE_EXACT(clip.sample(t).value(QStringLiteral("ParamArmRA")), expected);
    }
}

void MotionClipTest::additiveAndBlendMetadataAreParsed() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":12,"loop":true,"mode":"additive","blendSeconds":0.2,
        "blendSecondsPerParameter":{"ParamBusyLaptop":0.28,"ParamSitPose":0.28},
        "channels":{"ParamAngleY":[{"type":"sine","amplitude":1.8,"frequency":4.3,"phase":0.0,"offset":-7.0}]}})"));
    MotionClip clip;
    QString error;
    QVERIFY2(clip.loadJson(file.fileName(), &error), qPrintable(error));
    QVERIFY(clip.isAdditive());
    QCOMPARE(clip.blendSeconds(), 0.2);
    QCOMPARE(clip.blendSecondsFor(QStringLiteral("ParamBusyLaptop")), 0.28);
    // Parameters without an override fall back to the clip-wide blend time.
    QCOMPARE(clip.blendSecondsFor(QStringLiteral("ParamAngleX")), 0.2);
}

void MotionClipTest::legacyFileWithoutNewFieldsStillLoads() {
    QTemporaryFile file;
    QVERIFY(writeJson(file, R"({"duration":2,"keyframes":[
        {"time":0,"parameters":{"ParamA":0}},{"time":2,"parameters":{"ParamA":1}}]})"));
    MotionClip clip;
    QVERIFY(clip.loadJson(file.fileName()));
    QVERIFY(!clip.isAdditive());
    QVERIFY(!clip.hasCurves());
    QCOMPARE(clip.channels().size(), 0);
    QCOMPARE(clip.blendSeconds(), 0.12); // The legacy default is preserved.
}

void MotionClipTest::malformedCurvesAreRejected() {
    MotionClip clip;
    QTemporaryFile badType;
    QVERIFY(writeJson(badType, R"({"duration":1,"channels":{"A":[{"type":"cosine"}]}})"));
    QVERIFY(!clip.loadJson(badType.fileName()));

    QTemporaryFile badPulse;
    QVERIFY(writeJson(badPulse, R"({"duration":1,"pulses":{"A":[{"start":0.5,"peak":0.2,"end":1}]}})"));
    QVERIFY(!clip.loadJson(badPulse.fileName()));

    QTemporaryFile badMode;
    QVERIFY(writeJson(badMode, R"({"duration":1,"mode":"blend","channels":{"A":[{"type":"sine"}]}})"));
    QVERIFY(!clip.loadJson(badMode.fileName()));

    QTemporaryFile badBlend;
    QVERIFY(writeJson(badBlend, R"({"duration":1,"blendSecondsPerParameter":{"A":-1},"channels":{"A":[{"type":"sine"}]}})"));
    QVERIFY(!clip.loadJson(badBlend.fileName()));

    QTemporaryFile emptyChannel;
    QVERIFY(writeJson(emptyChannel, R"({"duration":1,"channels":{"A":[]}})"));
    QVERIFY(!clip.loadJson(emptyChannel.fileName()));
}

void MotionClipTest::propMarkersCoincideWithDrawnPoses() {
    // Window-layer props (the deleted icon, the rolled wrap) are drawn in code,
    // but their handovers have to land on the same timeline the character is
    // animated on. Markers make that one authored source instead of a second copy
    // of the numbers in PetWindow.cpp, which had drifted: the code rolled the file
    // flat at 0.80 s while the clip's contract rolled it with the arm rise to 1.05.
    MotionClip clip;
    QString error;
    QVERIFY2(clip.loadJson(QStringLiteral("assets/motions/delete.motion.json"), &error),
             qPrintable(error));
    QVERIFY(clip.hasEvents());
    QCOMPARE(clip.events().size(), 7);
    // Ordered, and every marker lands on a time a parameter is authored at.
    const QVector<double> authoredTimes = clip.authoredTimes();
    QVERIFY(authoredTimes.size() > 20);
    double previous = -1.0;
    for (const auto& event : clip.events()) {
        QVERIFY(!event.name.isEmpty());
        QVERIFY(event.time > previous);
        previous = event.time;
        bool authored = false;
        for (double time : authoredTimes) {
            if (qAbs(time - event.time) < 1e-9) {
                authored = true;
                break;
            }
        }
        QVERIFY2(authored, qPrintable(QStringLiteral("%1 is not on an authored time").arg(event.name)));
    }
    // The prop phases the window layer asks for, and the fallback contract.
    QCOMPARE(clip.eventTime(QStringLiteral("delete.grip"), -1.0), 0.65);
    QCOMPARE(clip.eventTime(QStringLiteral("delete.roll_formed"), -1.0), 1.05);
    QCOMPARE(clip.eventTime(QStringLiteral("delete.lunge"), -1.0), 1.3);
    QCOMPARE(clip.eventTime(QStringLiteral("delete.bite"), -1.0), 1.45);
    QCOMPARE(clip.eventTime(QStringLiteral("delete.consumed"), -1.0), 1.6);
    // An unknown marker must not invent a time.
    QCOMPARE(clip.eventTime(QStringLiteral("delete.nonexistent"), 0.42), 0.42);

    // A marker off the authored timeline is rejected: a handover between two drawn
    // poses is a timing nobody can review.
    MotionClip offBeat;
    QTemporaryFile stray;
    QVERIFY(writeJson(stray, R"({"duration":2,"events":[{"name":"x","time":0.77}],
        "keyframes":[{"time":0,"parameters":{"A":0}},{"time":2,"parameters":{"A":1}}]})"));
    QVERIFY(!offBeat.loadJson(stray.fileName()));

    MotionClip duplicate;
    QTemporaryFile twice;
    QVERIFY(writeJson(twice, R"({"duration":2,"events":[{"name":"x","time":0},{"name":"x","time":2}],
        "keyframes":[{"time":0,"parameters":{"A":0}},{"time":2,"parameters":{"A":1}}]})"));
    QVERIFY(!duplicate.loadJson(twice.fileName()));

    MotionClip unordered;
    QTemporaryFile backwards;
    QVERIFY(writeJson(backwards, R"({"duration":2,"events":[{"name":"b","time":2},{"name":"a","time":0}],
        "keyframes":[{"time":0,"parameters":{"A":0}},{"time":2,"parameters":{"A":1}}]})"));
    QVERIFY(!unordered.loadJson(backwards.fileName()));
}

void MotionClipTest::idleMatchesLegacyGenerator() {
    // The idle base layer is the migration's golden case: it is sampled on every
    // frame, so it must still reproduce the procedural formula it replaced, bit
    // for bit. The busy and delete curves are no longer migration copies -- they
    // were re-authored to be readable -- so they get invariant tests instead.
    MotionClip idle;
    QString error;
    QVERIFY2(idle.loadJson(QStringLiteral("assets/motions/idle.motion.json"), &error),
             qPrintable(error));
    QVERIFY(!idle.isAdditive());
    for (const double t : {0.0, 0.4, 1.25, 3.5, 7.75, 11.9}) {
        const auto pose = idle.sample(t);
        COMPARE_EXACT(pose.value(QStringLiteral("ParamAngleX")), 2.0 * qSin(t * 0.68));
        COMPARE_EXACT(pose.value(QStringLiteral("ParamAngleY")), 1.5 * qSin(t * 0.53));
        COMPARE_EXACT(pose.value(QStringLiteral("ParamAngleZ")), 1.0 * qSin(t * 0.91));
        COMPARE_EXACT(pose.value(QStringLiteral("ParamBodyAngleX")), 0.8 * qSin(t * 0.68 - 0.3));
        COMPARE_EXACT(pose.value(QStringLiteral("ParamBodyAngleY")), 0.5 * qSin(t * 1.7));
        COMPARE_EXACT(pose.value(QStringLiteral("ParamBreath")), 0.5 + 0.5 * qSin(t * 2.1));
    }
}

void MotionClipTest::loopingActionsSeamlessAtLoopPoint() {
    // A looping clip runs off a free-running clock, so the pose it lands on at
    // the end of the cycle has to be the pose it started from. Get that wrong
    // and the motion pops once per cycle -- an easy slip, because a channel
    // whose frequency is not a whole number of cycles across the loop drifts.
    for (const QString& name : {QStringLiteral("busy-stand.motion.json"),
                                QStringLiteral("busy-laptop.motion.json")}) {
        MotionClip clip;
        QString error;
        QVERIFY2(clip.loadJson(QStringLiteral("assets/motions/%1").arg(name), &error),
                 qPrintable(error));
        QVERIFY(clip.isLoop());
        const auto first = clip.sample(0.0);
        const auto last = clip.sample(clip.duration());
        for (auto it = first.cbegin(); it != first.cend(); ++it) {
            const double other = last.value(it.key(), 0.0);
            QVERIFY2(qAbs(it.value() - other) <= 1e-3,
                     qPrintable(QStringLiteral("%1 seams at %2: %3 vs %4")
                         .arg(name, it.key()).arg(it.value()).arg(other)));
        }
    }
}

void MotionClipTest::busyStandReadsAsThinking() {
    // The standing variant is the pet thinking, and the read has to come from
    // the face. The rig cannot help: ParamArmL/R top out at chest height, so no
    // hand can reach the chin, and ParamBusyTypingL/R deform nothing outside the
    // seated branch. An earlier revision raised both arms to the chest, which
    // was rejected for reading as a second typing pose instead of a pause.
    MotionClip busy;
    QString error;
    QVERIFY2(busy.loadJson(QStringLiteral("assets/motions/busy-stand.motion.json"), &error),
             qPrintable(error));
    QVERIFY(busy.isAdditive());
    double armPeak = 0.0;
    double eyesMin = 2.0;
    double eyesMax = -1.0;
    double gaze = 0.0;
    double tilt = 0.0;
    for (double t = 0.0; t <= busy.duration(); t += 1.0 / 60.0) {
        const auto pose = busy.sample(t);
        armPeak = std::max({armPeak, qAbs(pose.value(QStringLiteral("ParamArmLA"))),
                            qAbs(pose.value(QStringLiteral("ParamArmRA")))});
        // Additive offsets: what the model shows is the base 1.0 plus this.
        const double eyes = 1.0 + pose.value(QStringLiteral("ParamEyeLOpen"));
        eyesMin = std::min(eyesMin, eyes);
        eyesMax = std::max(eyesMax, eyes);
        gaze = std::max(gaze, pose.value(QStringLiteral("ParamEyeBallY")));
        tilt = std::max(tilt, qAbs(pose.value(QStringLiteral("ParamAngleZ"))));
    }
    QVERIFY2(armPeak < 12.0, qPrintable(QString::number(armPeak))); // the hands stay down
    // A pose probe put the legible band between two failures: 0.60 reads as
    // sleepy, 0.82 as an ordinary open eye. The offset has to stay inside the
    // pondering squint between them for the whole cycle.
    QVERIFY2(eyesMin > 0.6, qPrintable(QString::number(eyesMin)));
    QVERIFY2(eyesMax < 0.85, qPrintable(QString::number(eyesMax)));
    QVERIFY2(gaze > 0.3, qPrintable(QString::number(gaze)));        // the gaze leaves the centre
    QVERIFY2(tilt > 3.0, qPrintable(QString::number(tilt)));        // and the head is tilted
}

void MotionClipTest::thinkingBubblePopsWhileBusy() {
    // The bubble is presentation, so it cannot live in the clip data -- no
    // drawable in the MOC3 paints it. It is still the player's job to time it,
    // because the approval captures render through this same state machine.
    ParameterMotion motion;
    QString error;
    QVERIFY2(motion.loadMotionLibrary(QStringLiteral("assets/motions"), &error),
             qPrintable(error));
    motion.setBusyRandomSeed(20261001);
    motion.advance(0.05);
    motion.forceStandingBusy();
    double peak = 0.0;
    bool retracted = false;
    for (int frame = 0; frame < 60 * 14; ++frame) {
        motion.advance(1.0 / 60.0);
        peak = std::max(peak, motion.bubblePulse());
        if (peak > 0.95 && motion.bubblePulse() < 0.05) retracted = true;
    }
    QVERIFY2(peak > 1.0, qPrintable(QString::number(peak))); // the pop overshoots
    QVERIFY2(retracted, "the bubble has to come back down, not sit there");
    // Leaving the state has to take the bubble with it.
    motion.setState(PetController::State::Idle);
    for (int frame = 0; frame < 60; ++frame) motion.advance(1.0 / 60.0);
    QVERIFY2(motion.bubblePulse() < 1e-3, qPrintable(QString::number(motion.bubblePulse())));
    // The seated variant already tells its own story with the laptop.
    motion.forceLaptopBusy();
    for (int frame = 0; frame < 60 * 3; ++frame) motion.advance(1.0 / 60.0);
    QVERIFY2(motion.bubblePulse() < 1e-3, qPrintable(QString::number(motion.bubblePulse())));
}

void MotionClipTest::deleteSwingIsReadable() {
    MotionClip remove;
    QString error;
    QVERIFY2(remove.loadJson(QStringLiteral("assets/motions/delete.motion.json"), &error),
             qPrintable(error));
    QVERIFY(remove.isAdditive());
    QCOMPARE(remove.duration(), MotionLibrary::kDeleteDuration);
    double peak = 0.0;
    double gaped = 0.0;        // seconds spent genuinely wide open
    double cheeks = 0.0;
    for (double t = 0.0; t <= remove.duration(); t += 1.0 / 60.0) {
        const auto values = remove.sample(t);
        peak = std::max(peak, values.value(QStringLiteral("ParamArmRA")));
        cheeks = std::max(cheeks, values.value(QStringLiteral("ParamCheek")));
        // The v6 bite is mechanical: Gape selects the neutral art and
        // MouthOpenY deforms it. SmileOpen must stay out of it entirely.
        if (values.value(QStringLiteral("ParamMouthGape")) > 0.9
            && values.value(QStringLiteral("ParamMouthOpenY")) > 0.7)
            gaped += 1.0 / 60.0;
    }
    QVERIFY2(peak > 50.0, qPrintable(QString::number(peak))); // a visible swing, not a twitch
    // The desktop pet is only about 280 px wide, so the mouth has to be held
    // open long enough to register as "about to bite" rather than as one frame.
    QVERIFY2(gaped >= 0.15, qPrintable(QString::number(gaped)));
    // Cheek caps at 0.75: at 1.0 the collar stretches (known artefact).
    QVERIFY2(cheeks > 0.7, qPrintable(QString::number(cheeks)));
    // The smile is an emotion: the mechanical bite must never touch it.
    for (double t = 0.0; t <= remove.duration(); t += 1.0 / 60.0)
        QVERIFY2(remove.sample(t).value(QStringLiteral("ParamSmileOpen")) < 0.01,
                 qPrintable(QString::number(t)));
    // Every curve settles back to neutral, so busy or idle resumes without a jump.
    const auto settled = remove.sample(remove.duration());
    for (auto it = settled.cbegin(); it != settled.cend(); ++it)
        QVERIFY2(qAbs(it.value()) < 1e-9, qPrintable(it.key()));
}

void MotionClipTest::workStopsUseOneNeutralReactionAfterConcurrentTurns() {
    WorkHarness work;
    QVERIFY(work.load());
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
    work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
    QCOMPARE(work.controller.activeTurnCount(), 2);
    QCOMPARE(work.attempts.size(), 1); // Busy-to-Busy is still the same work session.
    work.advance(2.0);
    QVERIFY(!work.motion.interactionActive());
    work.now += 31000;

    // A long individual turn may leave a duration note; it must not celebrate
    // completion while another turn is still working. The old wiring did.
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.controller.state(), PetController::State::Busy);
    QCOMPARE(work.controller.activeTurnCount(), 1);
    QCOMPARE(work.notes, QList<qint64>{31000});
    QCOMPARE(work.returns(), 0);
    QVERIFY(!work.motion.interactionActive());

    // This traverses actual allTurnsStopped -> turnEnded signal order. The
    // second callback records duration and never competes for the player.
    work.controller.turnStopped(QStringLiteral("b"), QStringLiteral("two"));
    QCOMPARE(work.controller.state(), PetController::State::Idle);
    QCOMPARE(work.controller.activeTurnCount(), 0);
    QCOMPARE(work.returns(), 1);
    QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
    QCOMPARE(work.notes.size(), 2);
    QCOMPARE(work.reactionAtNote.last(), QStringLiteral("turn-ended"));
    QVERIFY(work.attempts.last().started);
    for (const auto& attempt : work.attempts)
        QVERIFY(attempt.id != QStringLiteral("cheer"));
    work.advance(4.0);
    QVERIFY(!work.motion.interactionActive());
    QCOMPARE(work.returns(), 1);
}

void MotionClipTest::workDurationNotesUseControllerClockAndThreshold() {
    WorkHarness work;
    QVERIFY(work.load());
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("short"));
    work.advance(2.0);
    work.now += WorkReactionCoordinator::kMinimumNoteMs - 1;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("short"));
    QVERIFY(work.notes.isEmpty());
    work.advance(3.0);

    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("long"));
    work.advance(2.0);
    work.now += WorkReactionCoordinator::kMinimumNoteMs;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("long"));
    QCOMPARE(work.notes, QList<qint64>{WorkReactionCoordinator::kMinimumNoteMs});
    QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
    QCOMPARE(work.returns(), 2);
}

void MotionClipTest::newTurnsAndForegroundActionsInterruptWorkEnd() {
    for (const auto event : {PetController::State::Busy, PetController::State::Delete,
                             PetController::State::Grass}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        work.advance(2.0);
        work.now += 31000;
        work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
        work.advance(0.3);
        if (event == PetController::State::Busy)
            work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
        else if (event == PetController::State::Delete)
            work.controller.desktopItemDeleted();
        else work.controller.playGrass();
        QCOMPARE(work.controller.state(), event);
        QVERIFY(work.motion.interactionId() != QStringLiteral("turn-ended"));
        if (event == PetController::State::Busy) {
            // State is applied before the greeting, so the old reaction has
            // already released its channels when the new work begins.
            QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
            QVERIFY(work.attempts.last().started);
        } else {
            QVERIFY(!work.motion.interactionActive());
        }
        work.advance(4.0);
        QVERIFY(work.motion.interactionId() != QStringLiteral("turn-ended"));
        QCOMPARE(work.returns(), 1);
    }
}

void MotionClipTest::workEndDuringForegroundIsDroppedWithoutReplay() {
    for (const auto event : {PetController::State::Delete, PetController::State::Grass}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        work.advance(2.0);
        if (event == PetController::State::Delete) work.controller.desktopItemDeleted();
        else work.controller.playGrass();
        work.now += 31000;
        work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.controller.activeTurnCount(), 0);
        QCOMPARE(work.controller.state(), event);
        QVERIFY(!work.motion.interactionActive());
        QCOMPARE(work.notes, QList<qint64>{31000});
        QCOMPARE(work.returns(), 1);
        QVERIFY(!work.attempts.last().started);
        QCOMPARE(work.attempts.last().reason, QStringLiteral("activity_owns_stage"));
        work.controller.actionFinished();
        QCOMPARE(work.controller.state(), PetController::State::Idle);
        work.advance(4.0);
        QVERIFY(!work.motion.interactionActive());
        QCOMPARE(work.returns(), 1); // No stale return-gaze/cheer was queued.
    }
}

void MotionClipTest::cleanupAndDuplicateStopsDoNotInventCompletion() {
    for (bool reset : {false, true}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        work.advance(2.0);
        work.now += 31000;
        if (reset) work.controller.resetBusy();
        else work.controller.sessionEnded(QStringLiteral("a"));
        work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.controller.state(), PetController::State::Idle);
        QVERIFY(work.notes.isEmpty());
        QCOMPARE(work.returns(), 0);
        QVERIFY(!work.motion.interactionActive());
    }
    WorkHarness work;
    QVERIFY(work.load());
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    work.advance(2.0);
    work.now += 31000;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.notes.size(), 1);
    QCOMPARE(work.returns(), 1);
}

void MotionClipTest::refusedWorkEndIsObservedWithoutQueueing() {
    WorkHarness work;
    QVERIFY(work.load());
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    work.advance(2.0);
    QVERIFY(work.motion.beginHeadPat(0.4));
    work.now += 31000;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("head-pat"));
    QCOMPARE(work.returns(), 1);
    QVERIFY(!work.attempts.last().started);
    QCOMPARE(work.attempts.last().reason, QStringLiteral("motion_gate_refused"));
    work.motion.cancelInteraction();
    work.advance(4.0);
    QVERIFY(!work.motion.interactionActive());
    QCOMPARE(work.returns(), 1);
}

void MotionClipTest::workEndPreservesSeatedPoseThroughRealSignals() {
    WorkHarness work;
    QVERIFY(work.load());
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    work.motion.forceLaptopBusy();
    work.advance(6.0);
    const auto seated = work.motion.values();
    QVERIFY(seated.value(QStringLiteral("ParamDeskVisible")) > 0.99);
    QVERIFY(seated.value(QStringLiteral("ParamBusyLaptop")) > 0.99);
    work.now += 31000;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
    work.advance(0.6);
    for (const auto& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
                           QStringLiteral("ParamDeskVisible"), QStringLiteral("ParamLaptopVisible"),
                           QStringLiteral("ParamArmLA"), QStringLiteral("ParamArmRA")})
        QCOMPARE(work.motion.values().value(id), seated.value(id));
    QVERIFY(work.motion.values().value(QStringLiteral("ParamBusyTypingR")) < 0.001);
    QCOMPARE(work.notes.size(), 1);
    work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
    QCOMPARE(work.controller.state(), PetController::State::Busy);
    QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
    QCOMPARE(work.returns(), 1);
}

void MotionClipTest::quietWorkTracksConcurrentTurnsWithoutReplay() {
    WorkHarness work;
    QVERIFY(work.load());
    work.automaticAllowed = false;
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.controller.state(), PetController::State::Busy);
    QCOMPARE(work.controller.activeTurnCount(), 1);
    QVERIFY(!work.motion.interactionActive());
    QCOMPARE(work.attempts.size(), 1);
    QCOMPARE(work.attempts.first().id, QStringLiteral("greet"));
    QCOMPARE(work.attempts.first().reason, QStringLiteral("quiet_policy"));
    QVERIFY(!work.attempts.first().started);
    work.advance(2.0);
    QVERIFY(work.motion.busyPhaseTime() > 1.0); // Real Busy still reaches the player.

    // Ending quiet while the session continues does not revive its greeting,
    // even when a second client starts a concurrent turn.
    work.automaticAllowed = true;
    work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
    QCOMPARE(work.controller.activeTurnCount(), 2);
    QCOMPARE(work.attempts.size(), 1);
    QVERIFY(!work.motion.interactionActive());
    work.automaticAllowed = false;
    work.now += WorkReactionCoordinator::kMinimumNoteMs;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.controller.state(), PetController::State::Busy);
    QCOMPARE(work.controller.activeTurnCount(), 1);
    QCOMPARE(work.returns(), 0);
    QVERIFY(work.notes.isEmpty());
    QCOMPARE(work.attempts.last().id, QStringLiteral("duration-note"));
    QCOMPARE(work.attempts.last().reason, QStringLiteral("quiet_policy"));

    work.controller.turnStopped(QStringLiteral("b"), QStringLiteral("two"));
    QCOMPARE(work.controller.state(), PetController::State::Idle);
    QCOMPARE(work.controller.activeTurnCount(), 0);
    QCOMPARE(work.returns(), 1);
    QCOMPARE(work.attempts.size(), 4); // Greeting, two notes, one neutral return.
    for (const auto& attempt : work.attempts) {
        QVERIFY(!attempt.started);
        QCOMPARE(attempt.reason, QStringLiteral("quiet_policy"));
    }
    QVERIFY(work.notes.isEmpty());
    QVERIFY(!work.motion.interactionActive());

    work.automaticAllowed = true;
    work.advance(4.0);
    QCOMPARE(work.attempts.size(), 4);
    QVERIFY(work.notes.isEmpty());
    QVERIFY(!work.motion.interactionActive());
    // A genuinely new work session can greet and leave its own note normally.
    work.controller.turnStarted(QStringLiteral("c"), QStringLiteral("three"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
    QVERIFY(work.attempts.last().started);
    work.advance(2.0);
    work.now += WorkReactionCoordinator::kMinimumNoteMs;
    work.controller.turnStopped(QStringLiteral("c"), QStringLiteral("three"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
    QCOMPARE(work.notes, QList<qint64>{WorkReactionCoordinator::kMinimumNoteMs});
}

void MotionClipTest::quietWorkStartedBehindForegroundNeverReplaysGreeting() {
    for (const auto foreground : {PetController::State::Delete, PetController::State::Grass}) {
        WorkHarness work;
        QVERIFY(work.load());
        if (foreground == PetController::State::Delete) work.controller.desktopItemDeleted();
        else work.controller.playGrass();
        work.automaticAllowed = false;
        QSignalSpy beginnings(&work.controller, &PetController::turnBegan);
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.controller.state(), foreground);
        QCOMPARE(work.controller.activeTurnCount(), 1);
        QCOMPARE(beginnings.size(), 1);
        QCOMPARE(work.attempts.size(), 1);
        QCOMPARE(work.attempts.first().id, QStringLiteral("greet"));
        QCOMPARE(work.attempts.first().reason, QStringLiteral("quiet_policy"));
        QVERIFY(!work.attempts.first().started);
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
        QCOMPARE(work.controller.activeTurnCount(), 2);
        QCOMPARE(beginnings.size(), 2); // A duplicate start is not a new turn.
        QCOMPARE(work.attempts.size(), 1);
        work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.controller.activeTurnCount(), 1);
        QCOMPARE(work.controller.state(), foreground);
        QCOMPARE(work.returns(), 0);

        work.automaticAllowed = true;
        work.controller.actionFinished();
        QCOMPARE(work.controller.state(), PetController::State::Busy);
        QCOMPARE(work.controller.activeTurnCount(), 1);
        QVERIFY(!work.motion.interactionActive());
        QCOMPARE(work.attempts.size(), 1);
        work.advance(2.0);
        QVERIFY(work.motion.busyPhaseTime() > 1.0);
        QVERIFY(!work.motion.interactionActive());
        work.controller.turnStopped(QStringLiteral("b"), QStringLiteral("two"));
        QCOMPARE(work.controller.activeTurnCount(), 0);
        QCOMPARE(work.controller.state(), PetController::State::Idle);
        QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
        QCOMPARE(work.returns(), 1);
        work.advance(4.0);
        // The consumed greeting belongs only to its real work session.
        work.controller.turnStarted(QStringLiteral("c"), QStringLiteral("three"));
        QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
        QVERIFY(work.attempts.last().started);
    }
}

void MotionClipTest::quietWorkSessionsBehindForegroundConsumeSeparately() {
    for (const auto foreground : {PetController::State::Delete, PetController::State::Grass}) {
        WorkHarness work;
        QVERIFY(work.load());
        if (foreground == PetController::State::Delete) work.controller.desktopItemDeleted();
        else work.controller.playGrass();
        work.automaticAllowed = false;
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.controller.activeTurnCount(), 0);
        QCOMPARE(work.controller.state(), foreground);
        QCOMPARE(work.attempts.size(), 2); // One quiet greet, one foreground-owned return.
        work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
        QCOMPARE(work.controller.activeTurnCount(), 1);
        QCOMPARE(work.attempts.size(), 3);
        QCOMPARE(work.attempts.last().id, QStringLiteral("greet"));
        QCOMPARE(work.attempts.last().reason, QStringLiteral("quiet_policy"));
        QVERIFY(!work.attempts.last().started);
        work.automaticAllowed = true;
        work.controller.actionFinished();
        QCOMPARE(work.controller.state(), PetController::State::Busy);
        QVERIFY(!work.motion.interactionActive());
        QCOMPARE(work.attempts.size(), 3);
    }
}

void MotionClipTest::quietWorkPreservesForegroundAndManualReactions() {
    for (const auto foreground : {PetController::State::Delete, PetController::State::Grass}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.automaticAllowed = false;
        work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
        if (foreground == PetController::State::Delete) work.controller.desktopItemDeleted();
        else work.controller.playGrass();
        QCOMPARE(work.controller.state(), foreground);
        QVERIFY(!work.motion.playGreet());
        work.now += WorkReactionCoordinator::kMinimumNoteMs;
        work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
        QCOMPARE(work.controller.activeTurnCount(), 0);
        QCOMPARE(work.controller.state(), foreground);
        QVERIFY(!work.motion.interactionActive());
        QVERIFY(work.notes.isEmpty());
        QCOMPARE(work.returns(), 1);
        const auto ended = std::find_if(work.attempts.cbegin(), work.attempts.cend(),
            [](const WorkAttempt& attempt) { return attempt.id == QStringLiteral("turn-ended"); });
        QVERIFY(ended != work.attempts.cend());
        QVERIFY(!ended->started);
        QCOMPARE(ended->reason, QStringLiteral("activity_owns_stage"));
        QCOMPARE(work.attempts.last().id, QStringLiteral("duration-note"));
        QCOMPARE(work.attempts.last().reason, QStringLiteral("quiet_policy"));

        work.controller.actionFinished();
        QCOMPARE(work.controller.state(), PetController::State::Idle);
        work.advance(4.0);
        QVERIFY(!work.motion.interactionActive());
        // Quiet applies only to automatic nominations. Direct user entries
        // remain usable after the foreground action has released the stage.
        QVERIFY(work.motion.playGreet());
        QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
        work.advance(2.0);
        QVERIFY(work.motion.playTurnEnded());
        QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
        work.advance(4.0);
        work.automaticAllowed = true;
        work.advance(4.0);
        QCOMPARE(work.returns(), 1);
        QVERIFY(work.notes.isEmpty());
        QVERIFY(!work.motion.interactionActive());
    }
}

void MotionClipTest::quietPolicyLetsRunningReactionsFinish() {
    WorkHarness work;
    QVERIFY(work.load());
    work.controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
    work.automaticAllowed = false;
    work.advance(0.2);
    QCOMPARE(work.motion.interactionId(), QStringLiteral("greet"));
    work.advance(2.0);
    QVERIFY(!work.motion.interactionActive());
    work.now += WorkReactionCoordinator::kMinimumNoteMs;
    work.controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QVERIFY(!work.motion.interactionActive());
    QVERIFY(work.notes.isEmpty());

    work.automaticAllowed = true;
    work.controller.turnStarted(QStringLiteral("b"), QStringLiteral("two"));
    work.advance(2.0);
    work.now += WorkReactionCoordinator::kMinimumNoteMs;
    work.controller.turnStopped(QStringLiteral("b"), QStringLiteral("two"));
    QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
    work.automaticAllowed = false;
    work.advance(0.2);
    QCOMPARE(work.motion.interactionId(), QStringLiteral("turn-ended"));
    work.advance(4.0);
    QVERIFY(!work.motion.interactionActive());
    QCOMPARE(work.notes, QList<qint64>{WorkReactionCoordinator::kMinimumNoteMs});
    QCOMPARE(work.returns(), 2);
}

void MotionClipTest::absentQuietPredicateAllowsWorkReactions() {
    qint64 now = 1000;
    PetController controller{nullptr, [&now] { return now; }};
    ParameterMotion motion;
    QVERIFY(motion.loadMotionLibrary(QStringLiteral("assets/motions")));
    QList<qint64> notes;
    WorkReactionCoordinator coordinator{&controller, &motion, nullptr, {},
        [&notes](qint64 duration) { notes.append(duration); }};
    controller.turnStarted(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(motion.interactionId(), QStringLiteral("greet"));
    for (int i = 0; i < 100; ++i) motion.advance(0.02);
    now += WorkReactionCoordinator::kMinimumNoteMs;
    controller.turnStopped(QStringLiteral("a"), QStringLiteral("one"));
    QCOMPARE(motion.interactionId(), QStringLiteral("turn-ended"));
    QCOMPARE(notes, QList<qint64>{WorkReactionCoordinator::kMinimumNoteMs});
}

void MotionClipTest::permissionRequestsNominateApprovedCueOnceAndTrackItsOwner() {
    for (const bool seated : {false, true}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.motion.setWorkPosturePreference(seated ? ParameterMotion::WorkPosturePreference::Laptop
                                                    : ParameterMotion::WorkPosturePreference::Standing);
        work.controller.turnStarted("opencode", "execution");
        work.advance(seated ? 16.0 : 3.0);
        const double phase = work.motion.busyPhaseTime();
        const double continuous = work.motion.continuousBusyTime();
        work.controller.applyPermissionSnapshot("opencode", "execution", {"first", "second"});
        QCOMPARE(work.permissionAttempts(), 2);
        QCOMPARE(work.permissionStarts(), 1); // One screen owner, not a queue of two cues.
        QVERIFY(work.motion.permissionCueActive());
        work.advance(0.4);
        QCOMPARE(work.motion.busyPhaseTime(), phase);
        QCOMPARE(work.motion.continuousBusyTime(), continuous);
        work.controller.applyPermissionSnapshot("opencode", "execution", {"first"});
        QVERIFY(work.motion.permissionCueActive()); // An unbound request was resolved.
        work.controller.applyPermissionSnapshot("opencode", "execution", {});
        QVERIFY(!work.motion.permissionCueActive());
        QCOMPARE(work.controller.activeTurnCount(), 1);
        QCOMPARE(work.controller.state(), PetController::State::Busy);
        QVERIFY(work.notes.isEmpty());
        QCOMPARE(work.returns(), 0);
        work.controller.applyPermissionSnapshot("opencode", "execution", {"first", "second"});
        QCOMPARE(work.permissionAttempts(), 2); // Neither handled ID gets another nomination.
        QVERIFY(!work.motion.permissionCueActive());
        work.controller.applyPermissionSnapshot("opencode", "execution", {"first", "second", "third"});
        QCOMPARE(work.permissionStarts(), 2);
        QVERIFY(work.motion.permissionCueActive());
        work.advance(3.2);
        QVERIFY(!work.motion.permissionCueActive());
        QVERIFY(work.motion.busyPhaseTime() > phase);
        work.controller.applyPermissionSnapshot("opencode", "execution", {"third", "second", "first"});
        QCOMPARE(work.permissionAttempts(), 3);
        QVERIFY(!work.motion.permissionCueActive());
        QCOMPARE(work.controller.activeTurnCount(), 1);
    }
}

void MotionClipTest::permissionQuietAndRefusedRequestsNeverQueue() {
    for (const QString& owner : {QStringLiteral("quiet"), QStringLiteral("head-pat"),
         QStringLiteral("drag"), QStringLiteral("delete"), QStringLiteral("grass"),
         QStringLiteral("transition")}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.motion.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
        work.controller.turnStarted("opencode", "execution");
        work.advance(3.0);
        if (owner == QStringLiteral("quiet")) work.automaticAllowed = false;
        if (owner == QStringLiteral("head-pat")) QVERIFY(work.motion.beginHeadPat());
        if (owner == QStringLiteral("drag")) work.motion.beginDragMotion();
        if (owner == QStringLiteral("delete")) work.controller.desktopItemDeleted();
        if (owner == QStringLiteral("grass")) work.controller.playGrass();
        if (owner == QStringLiteral("transition")) {
            work.motion.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Laptop);
            work.advance(0.02); // Laptop target selected, sustained-work sit protection still active.
        }
        work.controller.applyPermissionSnapshot("opencode", "execution", {"consumed"});
        QCOMPARE(work.permissionAttempts(), 1);
        QCOMPARE(work.permissionStarts(), 0);
        QCOMPARE(work.attempts.last().reason, owner == QStringLiteral("quiet")
            ? QStringLiteral("quiet_policy")
            : owner == QStringLiteral("delete") || owner == QStringLiteral("grass")
                ? QStringLiteral("activity_owns_stage") : QStringLiteral("motion_gate_refused"));
        if (owner == QStringLiteral("head-pat"))
            QCOMPARE(work.motion.interactionId(), QStringLiteral("head-pat"));
        work.automaticAllowed = true;
        work.motion.cancelInteraction();
        work.motion.cancelDragMotion();
        work.controller.actionFinished();
        work.advance(16.0);
        QVERIFY2(!work.motion.permissionCueActive(), qPrintable(owner));
        work.controller.applyPermissionSnapshot("opencode", "execution", {"consumed"});
        work.controller.applyPermissionSnapshot("opencode", "execution", {});
        work.controller.applyPermissionSnapshot("opencode", "execution", {"consumed"});
        QCOMPARE(work.permissionAttempts(), 1);
        QVERIFY(!work.motion.permissionCueActive());
        work.controller.applyPermissionSnapshot("opencode", "execution", {"fresh"});
        QCOMPARE(work.permissionStarts(), 1); // Only a genuinely new request can try again.
        QCOMPARE(work.controller.activeTurnCount(), 1);
    }
    WorkHarness running;
    QVERIFY(running.load());
    running.motion.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
    running.controller.turnStarted("a", "one");
    running.advance(3.0);
    running.controller.applyPermissionSnapshot("a", "one", {"already-playing"});
    QVERIFY(running.motion.permissionCueActive());
    running.automaticAllowed = false;
    running.advance(0.3);
    QVERIFY(running.motion.permissionCueActive()); // Quiet only guards new nominations.
    running.advance(3.0);
    QVERIFY(!running.motion.permissionCueActive());
    QCOMPARE(running.permissionStarts(), 1);
}

void MotionClipTest::permissionCleanupAndNewTurnsCancelOnlyOwnedCue() {
    for (const QString& event : {QStringLiteral("stop"), QStringLiteral("session"),
         QStringLiteral("reset"), QStringLiteral("expiry"), QStringLiteral("new-turn"),
         QStringLiteral("delete"), QStringLiteral("grass")}) {
        WorkHarness work;
        QVERIFY(work.load());
        work.motion.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
        work.controller.turnStarted("opencode", "execution");
        work.controller.turnStarted("other-source", "parallel");
        work.advance(3.0);
        work.controller.applyPermissionSnapshot("opencode", "execution", {"bound"});
        QVERIFY(work.motion.permissionCueActive());
        work.advance(0.3);
        if (event == QStringLiteral("stop")) work.controller.turnStopped("opencode", "execution");
        if (event == QStringLiteral("session")) work.controller.sessionEnded("opencode");
        if (event == QStringLiteral("reset")) work.controller.resetBusy();
        if (event == QStringLiteral("expiry")) {
            work.now += 4LL * 60 * 60 * 1000 + 1;
            QVERIFY(QMetaObject::invokeMethod(&work.controller, "expireOldTurns", Qt::DirectConnection));
        }
        if (event == QStringLiteral("new-turn")) work.controller.turnStarted("third-source", "fresh");
        if (event == QStringLiteral("delete")) work.controller.desktopItemDeleted();
        if (event == QStringLiteral("grass")) work.controller.playGrass();
        QVERIFY2(!work.motion.permissionCueActive(), qPrintable(event));
        if (event == QStringLiteral("stop") || event == QStringLiteral("session")) {
            QCOMPARE(work.controller.activeTurnCount(), 1);
            QCOMPARE(work.controller.state(), PetController::State::Busy);
            QCOMPARE(work.controller.pendingPermissionCount(), 0);
        } else if (event == QStringLiteral("new-turn")) {
            QCOMPARE(work.controller.activeTurnCount(), 3);
            QCOMPARE(work.controller.pendingPermissionCount(), 1); // New work does not resolve approval.
        } else if (event == QStringLiteral("reset") || event == QStringLiteral("expiry")) {
            QCOMPARE(work.controller.activeTurnCount(), 0);
            QCOMPARE(work.controller.pendingPermissionCount(), 0);
        } else {
            QCOMPARE(work.controller.activeTurnCount(), 2);
            QCOMPARE(work.controller.pendingPermissionCount(), 1);
        }
        work.controller.actionFinished();
        work.advance(4.0);
        QVERIFY(!work.motion.permissionCueActive());
        QCOMPARE(work.permissionStarts(), 1);
        QCOMPARE(work.returns(), 0); // Concurrent stop or bookkeeping is never all-done.
    }
}

void MotionClipTest::permissionResolutionDoesNotCancelManualOrOtherReactions() {
    WorkHarness work;
    QVERIFY(work.load());
    work.motion.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
    work.controller.turnStarted("opencode", "execution");
    work.advance(3.0);
    work.controller.applyPermissionSnapshot("opencode", "execution", {"bound"});
    const quint64 automaticSerial = work.motion.permissionCueSerial();
    QVERIFY(automaticSerial > 0);
    work.advance(3.0);
    QVERIFY(!work.motion.permissionCueActive());
    QVERIFY(work.motion.playPermissionCue()); // Explicit ordinary/manual preview is a new player instance.
    QVERIFY(work.motion.permissionCueSerial() != automaticSerial);
    work.controller.applyPermissionSnapshot("opencode", "execution", {});
    QVERIFY(work.motion.permissionCueActive()); // Old binding cannot cancel this manual instance.
    work.advance(3.0);
    work.controller.applyPermissionSnapshot("opencode", "execution", {"next"});
    QVERIFY(work.motion.permissionCueActive());
    QVERIFY(work.motion.beginHeadPat());
    QCOMPARE(work.motion.interactionId(), QStringLiteral("head-pat"));
    work.controller.applyPermissionSnapshot("opencode", "execution", {});
    QCOMPARE(work.motion.interactionId(), QStringLiteral("head-pat"));
}

void MotionClipTest::permissionPendingClipCannotEnterAutomaticPath() {
    QTemporaryDir draft;
    QVERIFY(draft.isValid());
    const QDir source(QStringLiteral("assets/motions"));
    for (const QString& name : source.entryList({QStringLiteral("*.motion.json")}, QDir::Files))
        QVERIFY(QFile::copy(source.filePath(name), draft.filePath(name)));
    const QString path = draft.filePath(QStringLiteral("permission-cue.motion.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto data = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    data.insert(QStringLiteral("approval"), QStringLiteral("pending"));
    data.insert(QStringLiteral("conceptApproval"), QStringLiteral("pending"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(data).toJson());
    file.close();
    WorkHarness work;
    QVERIFY(work.load());
    QString error;
    QVERIFY2(work.motion.loadMotionLibrary(draft.path(), &error), qPrintable(error));
    work.motion.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
    work.controller.turnStarted("a", "one");
    work.advance(3.0);
    work.controller.applyPermissionSnapshot("a", "one", {"pending-clip"});
    QCOMPARE(work.permissionAttempts(), 1);
    QCOMPARE(work.permissionStarts(), 0);
    QVERIFY(!work.motion.permissionCueActive());
    QCOMPARE(work.controller.pendingPermissionCount(), 1);
    QCOMPARE(work.controller.activeTurnCount(), 1);
}

QTEST_GUILESS_MAIN(MotionClipTest)
#include "MotionClipTest.moc"
