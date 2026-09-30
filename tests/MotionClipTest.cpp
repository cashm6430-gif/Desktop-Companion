#include "../src/MotionClip.h"
#include "../src/MotionLibrary.h"
#include "../src/ParameterMotion.h"
#include "../src/PetController.h"

#include <QFile>
#include <QtTest/QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>

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

QTEST_GUILESS_MAIN(MotionClipTest)
#include "MotionClipTest.moc"
