#include "../src/MotionClip.h"
#include "../src/MotionLibrary.h"
#include "../src/ParameterMotion.h"
#include "../src/PetController.h"

#include <QFile>
#include <QtMath>
#include <QtTest/QTest>
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
};

namespace {
bool writeJson(QTemporaryFile& file, const QByteArray& json) {
    if (!file.open()) return false;
    file.write(json);
    file.flush();
    return true;
}

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

QTEST_GUILESS_MAIN(MotionClipTest)
#include "MotionClipTest.moc"
