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
};

namespace {
bool writeJson(QTemporaryFile& file, const QByteArray& json) {
    if (!file.open()) return false;
    file.write(json);
    file.flush();
    return true;
}
} // namespace

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
        QCOMPARE(clip.sample(t).value(QStringLiteral("ParamAngleX")), 2.0 * qSin(t * 0.68));
        QCOMPARE(clip.sample(t).value(QStringLiteral("ParamBreath")), 0.5 + 0.5 * qSin(t * 2.1));
        QCOMPARE(clip.sample(t).value(QStringLiteral("ParamBodyAngleX")), 0.8 * qSin(t * 0.68 - 0.3));
    }
    // A pure-channel clip is a formula, not a sampled range: no clamping.
    QCOMPARE(clip.sample(100.0).value(QStringLiteral("ParamAngleX")), 2.0 * qSin(100.0 * 0.68));
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
        QCOMPARE(clip.sample(t).value(QStringLiteral("ParamArmRA")), expected);
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

QTEST_GUILESS_MAIN(MotionClipTest)
#include "MotionClipTest.moc"
