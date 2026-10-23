#include "../src/TriggerDirector.h"

#include <QtTest/QtTest>

#include <array>
#include <limits>

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
    void defaultPacesRollAfterEligibility();
    void fixedSeedReplaysNominationTimes();
    void invalidScalesDisableRollsButNotStartup();
    void busyNominationStartsAtMostOneScenePerTick();
    void quietGateSilencesEverySceneAndConsumesStartup();
    void quietGateRestartsEligibility_data();
    void quietGateRestartsEligibility();
    void quietTimeStillAdvancesCooldowns();
    void offFrequencyRetainsItsExistingEligibilitySemantics();

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

void TriggerDirectorTest::defaultPacesRollAfterEligibility() {
    // A fixed seed corpus checks the user-visible contract rather than a
    // particular STL distribution's first floating-point result. At the first
    // eligible tick, no cooldown or refusal can explain a missed nomination.
    // For the same random draw, Sparse's hits must be a subset of Normal's,
    // which must in turn be a subset of Often's. None of them is a forced hit.
    constexpr unsigned int samples = 1024;
    std::array<int, 3> hits{};
    const std::array<TriggerDirector::Frequency, 3> paces{
        TriggerDirector::Frequency::Sparse,
        TriggerDirector::Frequency::Normal,
        TriggerDirector::Frequency::Often};
    for (unsigned int seed = 0; seed < samples; ++seed) {
        std::array<bool, 3> nominated{};
        for (std::size_t pace = 0; pace < paces.size(); ++pace) {
            TriggerDirector director;
            director.seed(seed);
            director.setFrequency(paces[pace]);
            director.setStartStretch([&] { nominated[pace] = true; return true; });
            director.tick(10.0, false, false); // no wave callback: startup gives up
            director.tick(1500.0, false, true); // stretch's first eligible tick
            if (nominated[pace]) ++hits[pace];
        }
        QVERIFY(!nominated[0] || nominated[1]);
        QVERIFY(!nominated[1] || nominated[2]);
    }
    // Strict differences ensure pace scaling has an observable effect. This
    // uses no system entropy or timing; rerunning the corpus is identical.
    QVERIFY(hits[0] > 0);
    QVERIFY(hits[0] < hits[1]);
    QVERIFY(hits[1] < hits[2]);
    QVERIFY(hits[2] < int(samples));
}

void TriggerDirectorTest::fixedSeedReplaysNominationTimes() {
    const auto replay = [](unsigned int seed) {
        TriggerDirector director;
        director.seed(seed);
        QStringList trace;
        const auto callback = [&](const QString& name) {
            return [&, name] {
                trace << name + QLatin1Char(':') + QString::number(director.runningSeconds());
                return true;
            };
        };
        director.setStartBox(callback(QStringLiteral("box")));
        director.setStartWave(callback(QStringLiteral("wave")));
        director.setStartNap(callback(QStringLiteral("nap")));
        director.setStartStretch(callback(QStringLiteral("stretch")));
        director.setStartRice(callback(QStringLiteral("rice")));
        run(director, 3600.0, true, false);
        run(director, 3600.0, false, true);
        return trace;
    };
    const auto first = replay(20261021);
    QVERIFY(first.size() > 5);
    QCOMPARE(replay(20261021), first);
    QVERIFY(replay(20261022) != first);
}

void TriggerDirectorTest::invalidScalesDisableRollsButNotStartup() {
    const std::array<double, 5> scales{0.0, -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (double scale : scales) {
        TriggerDirector director;
        director.setChanceScale(scale);
        int waves = 0, otherStarts = 0;
        director.setStartWave([&] { ++waves; return true; });
        const auto other = [&] { ++otherStarts; return true; };
        director.setStartBox(other);
        director.setStartNap(other);
        director.setStartStretch(other);
        director.setStartRice(other);
        run(director, 3600.0, true, false);
        run(director, 3600.0, false, true);
        QCOMPARE(waves, 1); // startup is an explicit greeting, not a chance roll
        QCOMPARE(otherStarts, 0);
        QCOMPARE(director.startedCount(), 1);
    }
}

void TriggerDirectorTest::busyNominationStartsAtMostOneScenePerTick() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int callsThisTick = 0, stretches = 0, rice = 0;
    director.setStartStretch([&] { ++callsThisTick; ++stretches; return true; });
    director.setStartRice([&] { ++callsThisTick; ++rice; return true; });
    director.tick(10.0, false, false); // finish startup without a wave callback
    // Both work scenes become eligible together; only the priority scene wins.
    director.tick(2700.0, false, true);
    QCOMPARE(callsThisTick, 1);
    QCOMPARE(stretches, 1);
    QCOMPARE(rice, 0);
    for (int tick = 0; tick < 300; ++tick) {
        callsThisTick = 0;
        director.tick(1.0, false, true);
        QVERIFY(callsThisTick <= 1);
    }
    QCOMPARE(stretches, 1);
    QCOMPARE(rice, 1); // after global spacing, not on the stretch's first tick
}

void TriggerDirectorTest::quietGateSilencesEverySceneAndConsumesStartup() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int starts = 0;
    const auto start = [&] { ++starts; return true; };
    director.setStartWave(start);
    director.setStartBox(start);
    director.setStartNap(start);
    director.setStartStretch(start);
    director.setStartRice(start);
    // Even a brief quiet tick consumes the greeting before its 0.8s delay.
    director.tick(0.25, true, false, false);
    director.tick(1.0, true, false);
    QCOMPARE(starts, 0);
    director.tick(3600.0, true, false, false);
    director.tick(3600.0, false, true, false);
    QCOMPARE(director.attemptCount(), 0);
    QCOMPARE(director.startedCount(), 0);
    QCOMPARE(starts, 0);
    QCOMPARE(director.runningSeconds(), 7201.25);
    director.tick(1.0, true, false);
    director.tick(1.0, false, true);
    QCOMPARE(starts, 0); // no greeting, scene or busy-break backlog on recovery
}

void TriggerDirectorTest::quietGateRestartsEligibility_data() {
    QTest::addColumn<bool>("busy");
    QTest::addColumn<double>("due");
    QTest::newRow("standing-box") << false << 480.0;
    QTest::newRow("busy-rest") << true << 1500.0;
}

void TriggerDirectorTest::quietGateRestartsEligibility() {
    QFETCH(bool, busy);
    QFETCH(double, due);
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int starts = 0;
    const auto start = [&] { ++starts; return true; };
    if (busy) director.setStartStretch(start);
    else director.setStartBox(start);
    director.tick(1.0, !busy, busy, false); // consume startup, accrue no due
    run(director, due - 1.0, !busy, busy);
    QCOMPARE(starts, 0);
    director.tick(3600.0, !busy, busy, false); // resets near-due history
    QCOMPARE(director.attemptCount(), 0);
    run(director, due - 1.0, !busy, busy);
    QCOMPARE(starts, 0);
    director.tick(1.0, !busy, busy);
    QCOMPARE(starts, 1); // must earn a fresh full uninterrupted interval
}

void TriggerDirectorTest::quietTimeStillAdvancesCooldowns() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int boxes = 0;
    director.setStartBox([&] { ++boxes; return true; });
    director.tick(1.0, true, false, false);
    run(director, 480.0, true, false);
    QCOMPARE(boxes, 1);
    director.tick(1200.0, true, false, false);
    run(director, 479.0, true, false);
    QCOMPARE(boxes, 1);
    director.tick(1.0, true, false);
    QCOMPARE(boxes, 2); // elapsed quiet time satisfied the existing cooldown
    QCOMPARE(director.runningSeconds(), 2161.0);
}

void TriggerDirectorTest::offFrequencyRetainsItsExistingEligibilitySemantics() {
    TriggerDirector director;
    director.setChanceScale(1000.0);
    int boxes = 0;
    director.setStartBox([&] { ++boxes; return true; });
    director.setFrequency(TriggerDirector::Frequency::Off);
    director.tick(600.0, true, false);
    QCOMPARE(director.attemptCount(), 0);
    director.setFrequency(TriggerDirector::Frequency::Normal);
    director.tick(1.0, true, false); // startup gives up with no wave callback
    QCOMPARE(boxes, 0);
    director.tick(1.0, true, false);
    QCOMPARE(boxes, 1); // frequency Off still accrues due, unlike quiet policy
}
QTEST_MAIN(TriggerDirectorTest)
#include "TriggerDirectorTest.moc"
