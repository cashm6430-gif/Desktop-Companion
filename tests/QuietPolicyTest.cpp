#include "../src/QuietPolicy.h"

#include <QtTest/QtTest>
#include <limits>

namespace {
QDateTime localAt(const QTime& time) {
    return QDateTime(QDate(2026, 10, 23), time);
}
}

class QuietPolicyTest final : public QObject {
    Q_OBJECT

private slots:
    void nightBoundaries_data();
    void nightBoundaries();
    void snoozeUsesMonotonicTimeAcrossWallClockJumps();
    void reasonsRemainIndependentAndResumeOnlyEndsSnooze();
    void repeatedSnoozeReplacesTheDeadline();
    void largeDurationsCannotOverflow();
};

void QuietPolicyTest::nightBoundaries_data() {
    QTest::addColumn<QTime>("time");
    QTest::addColumn<bool>("quiet");
    QTest::newRow("before-night") << QTime(22, 59, 59, 999) << false;
    QTest::newRow("night-inclusive") << QTime(23, 0) << true;
    QTest::newRow("midnight") << QTime(0, 0) << true;
    QTest::newRow("before-morning") << QTime(5, 59, 59, 999) << true;
    QTest::newRow("morning-exclusive") << QTime(6, 0) << false;
    QTest::newRow("daytime") << QTime(12, 0) << false;
}

void QuietPolicyTest::nightBoundaries() {
    QFETCH(QTime, time);
    QFETCH(bool, quiet);
    const auto wall = localAt(time);
    QuietPolicy policy([&] { return wall; }, [] { return qint64(0); });
    QVERIFY(policy.nightEnabled());
    QCOMPARE(policy.suppressAutomatic(), quiet);
    QCOMPARE(policy.reason(), quiet ? QStringLiteral("night") : QString{});
    policy.setNightEnabled(false);
    QVERIFY(!policy.suppressAutomatic());
}

void QuietPolicyTest::snoozeUsesMonotonicTimeAcrossWallClockJumps() {
    auto wall = localAt(QTime(12, 0));
    qint64 monotonic = 1000;
    QuietPolicy policy([&] { return wall; }, [&] { return monotonic; });
    policy.setNightEnabled(false);
    policy.snoozeFor(60 * 1000);
    QCOMPARE(policy.snoozeRemainingMs(), qint64(60000));
    wall = wall.addYears(1); // clock correction does not expire the pause
    monotonic += 10000;
    QCOMPARE(policy.snoozeRemainingMs(), qint64(50000));
    wall = wall.addYears(-2); // nor does a backward correction extend it
    monotonic += 49999;
    QCOMPARE(policy.snoozeRemainingMs(), qint64(1));
    QCOMPARE(policy.reason(), QStringLiteral("snoozed"));
    ++monotonic;
    QCOMPARE(policy.snoozeRemainingMs(), qint64(0));
    QVERIFY(!policy.suppressAutomatic());
    monotonic += 100000;
    QCOMPARE(policy.snoozeRemainingMs(), qint64(0));
}

void QuietPolicyTest::reasonsRemainIndependentAndResumeOnlyEndsSnooze() {
    auto wall = localAt(QTime(23, 30));
    qint64 monotonic = 0;
    QuietPolicy policy([&] { return wall; }, [&] { return monotonic; });
    policy.setFullscreenPresent(true);
    QCOMPARE(policy.reason(), QStringLiteral("night"));
    policy.snoozeFor(1000);
    QCOMPARE(policy.reason(), QStringLiteral("snoozed"));
    policy.resume();
    QCOMPARE(policy.snoozeRemainingMs(), qint64(0));
    QCOMPARE(policy.reason(), QStringLiteral("night"));
    policy.setNightEnabled(false);
    QCOMPARE(policy.reason(), QStringLiteral("fullscreen"));
    policy.resume();
    QCOMPARE(policy.reason(), QStringLiteral("fullscreen"));
    policy.setFullscreenPresent(false);
    QVERIFY(!policy.suppressAutomatic());

    policy.setNightEnabled(true);
    policy.setFullscreenPresent(true);
    policy.snoozeFor(1000);
    monotonic += 1000;
    QCOMPARE(policy.reason(), QStringLiteral("night"));
    wall = localAt(QTime(6, 0));
    QCOMPARE(policy.reason(), QStringLiteral("fullscreen"));
    policy.setFullscreenPresent(false);
    QVERIFY(!policy.suppressAutomatic());
}

void QuietPolicyTest::repeatedSnoozeReplacesTheDeadline() {
    qint64 monotonic = 1234;
    QuietPolicy policy([] { return localAt(QTime(12, 0)); }, [&] { return monotonic; });
    policy.snoozeFor(60000);
    monotonic += 10000;
    policy.snoozeFor(1000);
    QCOMPARE(policy.snoozeRemainingMs(), qint64(1000));
    monotonic += 999;
    QVERIFY(policy.suppressAutomatic());
    ++monotonic;
    QVERIFY(!policy.suppressAutomatic());
    policy.snoozeFor(1000);
    policy.snoozeFor(0);
    QVERIFY(!policy.suppressAutomatic());
    policy.snoozeFor(1000);
    policy.snoozeFor(-1);
    QVERIFY(!policy.suppressAutomatic());
}

void QuietPolicyTest::largeDurationsCannotOverflow() {
    qint64 monotonic = std::numeric_limits<qint64>::min();
    QuietPolicy policy([] { return localAt(QTime(12, 0)); }, [&] { return monotonic; });
    policy.snoozeFor(std::numeric_limits<qint64>::max());
    ++monotonic;
    QCOMPARE(policy.snoozeRemainingMs(), std::numeric_limits<qint64>::max() - 1);
    monotonic = 0;
    QCOMPARE(policy.snoozeRemainingMs(), qint64(0));
    monotonic = std::numeric_limits<qint64>::max() - 1;
    policy.snoozeFor(1000);
    ++monotonic;
    QCOMPARE(policy.snoozeRemainingMs(), qint64(999));
}

QTEST_APPLESS_MAIN(QuietPolicyTest)
#include "QuietPolicyTest.moc"
