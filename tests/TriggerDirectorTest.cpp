#include "../src/TriggerDirector.h"

#include <QtTest/QtTest>

// The director is driven by explicit ticks with an injected clock, so every
// test is deterministic: the chance scale pins the per-tick rolls and the
// start callbacks are scripted.
class TriggerDirectorTest final : public QObject {
    Q_OBJECT

private slots:
    void offFrequencySilencesEverything();
    void idleDueNominatesAndGlobalIntervalSpreadsScenes();
    void busyScenesWaitForTheirThresholdAndRefusalsBackOff();
    void sparsePaceDoublesCooldowns();
    void startupGreetsOncePerLaunch();

private:
    // Drives `seconds` of one-second ticks. `idle`/`busy` select the state
    // for the whole span (both false = neither, e.g. delete/grass states).
    static void run(TriggerDirector& director, double seconds, bool idle, bool busy) {
        for (double t = 0.0; t < seconds - 1e-9; t += 1.0)
            director.tick(1.0, idle, busy);
    }
};

void TriggerDirectorTest::offFrequencySilencesEverything() {
    TriggerDirector director;
    director.setFrequency(TriggerDirector::Frequency::Off);
    director.setChanceScale(1000.0);  // would hit every tick if enabled
    int starts = 0;
    director.setStartBox([&] { ++starts; return true; });
    director.setStartWave([&] { ++starts; return true; });
    director.setStartNap([&] { ++starts; return true; });
    director.setStartStretch([&] { ++starts; return true; });
    director.setStartRice([&] { ++starts; return true; });
    run(director, 3600, true, false);
    run(director, 3600, false, true);
    QCOMPARE(starts, 0);
    QCOMPARE(director.startedCount(), 0);
}

void TriggerDirectorTest::idleDueNominatesAndGlobalIntervalSpreadsScenes() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    director.seed(7);
    int boxes = 0, waves = 0;
    director.setStartBox([&] { ++boxes; return true; });
    director.setStartWave([&] { ++waves; return true; });
    // The startup greeting fires once at ~1s; standing scenes wait for their
    // idle dues (box 480s, wave 720s), so the first five minutes stay quiet.
    run(director, 300, true, false);
    QCOMPARE(boxes, 0);
    QCOMPARE(waves, 1);
    // 300 -> 540s: the box crosses its 480s due and (pinned chance) starts.
    run(director, 240, true, false);
    QCOMPARE(boxes, 1);
    // 540 -> 1140s: wave crosses its due and the global 300s spacing at
    // ~780s, so exactly one more scene; the box is inside its 20-min
    // cooldown and must not repeat.
    run(director, 600, true, false);
    QCOMPARE(boxes, 1);
    QCOMPARE(waves, 2);
}

void TriggerDirectorTest::busyScenesWaitForTheirThresholdAndRefusalsBackOff() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int stretches = 0;
    bool allow = false;
    director.setStartStretch([&] {
        if (allow) ++stretches;
        return allow;
    });
    // 20 minutes busy: below the 25-minute threshold, no nomination at all.
    run(director, 1200, false, true);
    QCOMPARE(stretches, 0);
    QCOMPARE(director.attemptCount(), 0);
    // 1200 -> 1690s: the threshold crosses at 1500. The gate refuses, and
    // the 90s backoff holds the attempts to exactly three (1500/1590/1680)
    // instead of one per tick.
    run(director, 490, false, true);
    QCOMPARE(stretches, 0);
    QCOMPARE(director.attemptCount(), 3);
    // The gate opens: the next attempt after the backoff succeeds, and the
    // 25-minute scene cooldown keeps it at one success.
    allow = true;
    run(director, 200, false, true);
    QCOMPARE(stretches, 1);
    QCOMPARE(director.lastStarted(), QStringLiteral("stretch"));
}

void TriggerDirectorTest::sparsePaceDoublesCooldowns() {
    int boxesNormal = 0, boxesSparse = 0;
    // Normal pace: first success ~480s, cooldown 1200s -> the second box
    // fires at ~1680s within the 2400s run.
    TriggerDirector normal;
    normal.setChanceScale(1000.0);
    normal.setStartBox([&] { ++boxesNormal; return true; });
    run(normal, 2400, true, false);
    QCOMPARE(boxesNormal, 2);
    // Sparse pace: cooldown 2400s -> at 2400s the first success is still
    // the only one.
    TriggerDirector sparse;
    sparse.setFrequency(TriggerDirector::Frequency::Sparse);
    sparse.setChanceScale(1000.0);
    sparse.setStartBox([&] { ++boxesSparse; return true; });
    run(sparse, 2400, true, false);
    QCOMPARE(boxesSparse, 1);
}

void TriggerDirectorTest::startupGreetsOncePerLaunch() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int waves = 0;
    director.setStartWave([&] { ++waves; return true; });
    // Before the 0.8s delay: nothing.
    director.tick(0.5, true, false);
    QCOMPARE(waves, 0);
    director.tick(0.5, true, false);
    QCOMPARE(waves, 1);
    QCOMPARE(director.lastStarted(), QStringLiteral("wave"));
    // Once per launch: over the next five minutes no second greeting (the
    // spontaneous wave needs its 720s idle due, which this span never
    // reaches -- the greeting does not arm the wave scene's cooldown, but
    // it does consume the global interval).
    run(director, 300, true, false);
    QCOMPARE(waves, 1);
}
QTEST_MAIN(TriggerDirectorTest)
#include "TriggerDirectorTest.moc"
