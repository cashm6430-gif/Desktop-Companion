#include "../src/CubismPostureTransition.h"

#include <QtTest/QTest>
#include <limits>

namespace Posture = CubismPostureTransition;

namespace {
void comparePoint(Posture::Point actual, Posture::Point expected) {
    QVERIFY(std::isfinite(actual.x));
    QVERIFY(std::isfinite(actual.y));
    QVERIFY(std::abs(actual.x - expected.x) < 1e-12);
    QVERIFY(std::abs(actual.y - expected.y) < 1e-12);
}

double triangleArea(Posture::Point a, Posture::Point b, Posture::Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
} // namespace

class CubismPostureTransitionTest : public QObject {
    Q_OBJECT
private slots:
    void authoredKeysRetainTheirNativeCoordinates() {
        for (double sit : {0.0, 0.35, 0.65, 1.0}) {
            for (double busy : {0.0, 0.35, 0.65, 1.0}) {
                const auto pose = Posture::sampleAuthored(busy, sit);
                QVERIFY(pose.valid);
                QCOMPARE(pose.standingFold, sit);
                QCOMPARE(pose.nativeMaterial(), busy >= 0.5 && sit >= 0.93 ? 1.0 : 0.0);
            }
        }
        QVERIFY(!Posture::sampleAuthored(1, 0.65).seatedMaterial);
        QVERIFY(!Posture::sampleAuthored(1, std::nextafter(0.93, 0.0)).seatedMaterial);
        QVERIFY(Posture::sampleAuthored(1, 0.93).seatedMaterial);
        QCOMPARE(Posture::sampleAuthored(-1, -1).standingFold, 0.0);
        QCOMPARE(Posture::sampleAuthored(2, 2).standingFold, 1.0);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!Posture::sampleAuthored(nan, 0).valid);
        QVERIFY(!Posture::sampleAuthored(0, nan).valid);
    }

    void authoredHandsSupportBeforeLaptopAcquisition() {
        QCOMPARE(Posture::authoredHandGround(0), 0.0);
        QCOMPARE(Posture::authoredHandGround(1), 0.0);
        QCOMPARE(Posture::authoredHandGround(0.65), 1.0);
        QCOMPARE(Posture::authoredSkirtSpread(0), 0.0);
        QCOMPARE(Posture::authoredSkirtSpread(0.65), 1.0);
        for (int frame = 0; frame <= 1000; ++frame) {
            const double sit = frame / 1000.0;
            const double hand = Posture::authoredHandGround(sit);
            const double skirt = Posture::authoredSkirtSpread(sit);
            QVERIFY(hand >= 0 && hand <= 1);
            QVERIFY(skirt >= 0 && skirt <= 1);
            if (sit >= 0.96) QCOMPARE(hand, 0.0);
        }
    }

    void materialSelectorIsBinaryAtArbitraryClocks() {
        const double threshold = 0.74 + 0.19 * 0.5;
        const double below = std::nextafter(threshold, -std::numeric_limits<double>::infinity());
        const double above = std::nextafter(threshold, std::numeric_limits<double>::infinity());
        QVERIFY(!Posture::sample(below, 0.9).seatedMaterial);
        QVERIFY(Posture::sample(above, 0.9).seatedMaterial);
        for (double busy : {-10.0, 0.0, 0.74, 0.803, below, threshold, above, 0.862, 0.93, 1.0, 10.0}) {
            for (double sit : {-0.2, 0.0, 0.137, 0.45, 0.9, 1.0}) {
                const auto pose = Posture::sample(busy, sit);
                QVERIFY(pose.valid);
                QVERIFY(pose.seatedMix >= 0 && pose.seatedMix <= 1);
                QVERIFY(pose.standingFold >= 0 && pose.standingFold <= 1);
                QCOMPARE(pose.seatedMaterial, pose.seatedMix >= 0.5);
                QCOMPARE(pose.nativeMaterial(), pose.seatedMaterial ? 1.0 : 0.0);
            }
        }
        QCOMPARE(Posture::sample(0, 0).standingFold, 0.0);
        QCOMPARE(Posture::sample(0, 0.9).standingFold, 0.93);
        QCOMPARE(Posture::sample(1, 0).standingFold, 1.0);
        QCOMPARE(Posture::sample(1, 0.9).standingFold, 1.0);
    }

    void floorAndBothCollarsCoincideWithoutReflections_data() {
        QTest::addColumn<double>("mirror");
        QTest::newRow("right") << 1.0;
        QTest::newRow("left") << -1.0;
    }

    void floorAndBothCollarsCoincideWithoutReflections() {
        QFETCH(double, mirror);
        const double floor = -0.95;
        const Posture::Point standing{mirror * 0.15, 0.31};
        const Posture::Point seated{mirror * -0.13, 0.11};
        for (double mix : {0.0, 0.125, 0.499, 0.5, 0.501, 0.875, 1.0}) {
            Posture::Point target;
            QVERIFY(Posture::collarTarget(standing, seated, mix, &target));
            for (const auto& collar : {standing, seated}) {
                const Posture::FloorPinnedTransform transform{collar, target, floor};
                QVERIFY(transform.valid());
                Posture::Point result;
                QVERIFY(transform.apply(collar, &result));
                comparePoint(result, target);
                for (double x : {-0.7, 0.0, 0.7}) {
                    QVERIFY(transform.apply({x, floor}, &result));
                    comparePoint(result, {x, floor});
                }
                // The same map covers the chosen body and cloth reference;
                // a triangle above the collar also stays oriented.
                const Posture::Point a{-0.3, -0.6}, b{0.4, -0.4}, c{0.1, 0.8};
                Posture::Point mappedA, mappedB, mappedC;
                QVERIFY(transform.apply(a, &mappedA));
                QVERIFY(transform.apply(b, &mappedB));
                QVERIFY(transform.apply(c, &mappedC));
                const double areaRatio = triangleArea(mappedA, mappedB, mappedC) / triangleArea(a, b, c);
                const double expectedScale = (target.y - floor) / (collar.y - floor);
                QVERIFY(areaRatio > 0);
                QVERIFY(std::abs(areaRatio - expectedScale) < 1e-12);
            }
        }
    }

    void activeGeometryIsIdenticalAtItsEndpoints() {
        const Posture::Point standing{0.15, 0.31}, seated{-0.13, 0.11};
        for (double mix : {0.0, 1.0}) {
            Posture::Point target;
            QVERIFY(Posture::collarTarget(standing, seated, mix, &target));
            const Posture::Point collar = mix == 0 ? standing : seated;
            comparePoint(target, collar);
            const Posture::FloorPinnedTransform transform{collar, target, -0.95};
            const Posture::Point positions[] = {{-0.3, -0.95}, {0.4, -0.4}, {0.1, 0.8}};
            for (const Posture::Point position : positions) {
                Posture::Point result;
                QVERIFY(transform.apply(position, &result));
                comparePoint(result, position);
            }
        }
    }

    void selectorSwitchHasOneContinuousCollarAnchor() {
        const Posture::Point standing{0.15, 0.31}, seated{-0.13, 0.11};
        constexpr double floor = -0.95;
        const double threshold = 0.74 + 0.19 * 0.5;
        Posture::Point previous;
        bool havePrevious = false;
        for (double busy : {threshold - 1e-8, threshold, threshold + 1e-8}) {
            const auto pose = Posture::sample(busy, 0.9);
            QVERIFY(pose.valid);
            Posture::Point target;
            QVERIFY(Posture::collarTarget(standing, seated, pose.seatedMix, &target));
            Posture::Point standingAnchor, seatedAnchor;
            QVERIFY((Posture::FloorPinnedTransform{standing, target, floor}.apply(standing, &standingAnchor)));
            QVERIFY((Posture::FloorPinnedTransform{seated, target, floor}.apply(seated, &seatedAnchor)));
            comparePoint(standingAnchor, seatedAnchor);
            if (havePrevious) {
                QVERIFY(std::abs(target.x - previous.x) < 1e-7);
                QVERIFY(std::abs(target.y - previous.y) < 1e-7);
            }
            previous = target;
            havePrevious = true;
        }
    }

    void invalidInputsLeaveOutputsUntouched() {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (double invalid : {nan, inf, -inf}) {
            QVERIFY(!Posture::sample(invalid, 0).valid);
            QVERIFY(!Posture::sample(0, invalid).valid);
        }
        const Posture::Point unchanged{19, 23};
        Posture::Point output = unchanged;
        for (double invalid : {-0.01, 1.01, nan, inf}) {
            QVERIFY(!Posture::collarTarget({0, 1}, {0, 2}, invalid, &output));
            comparePoint(output, unchanged);
        }
        QVERIFY(!Posture::collarTarget({nan, 1}, {0, 2}, 0.5, &output));
        QVERIFY(!Posture::collarTarget({0, 1}, {0, inf}, 0.5, &output));
        QVERIFY(!Posture::collarTarget({0, 1}, {0, 2}, 0.5, nullptr));
        const Posture::FloorPinnedTransform invalidTransforms[] = {{{0, 0}, {0, 1}, 0},
                {{0, -1}, {0, 1}, 0}, {{0, 1}, {0, 0}, 0}, {{0, 1}, {0, -1}, 0},
                {{nan, 1}, {0, 1}, 0}, {{0, 1}, {inf, 1}, 0}, {{0, 1}, {0, 1}, nan}};
        for (const auto transform : invalidTransforms) {
            QVERIFY(!transform.valid());
            QVERIFY(!transform.apply({0, 0.5}, &output));
            comparePoint(output, unchanged);
        }
        const Posture::FloorPinnedTransform valid{{0, 1}, {1, 2}, 0};
        QVERIFY(valid.valid());
        QVERIFY(!valid.apply({nan, 1}, &output));
        QVERIFY(!valid.apply({0, inf}, &output));
        QVERIFY(!valid.apply({0, 1}, nullptr));
        comparePoint(output, unchanged);
    }
};

QTEST_GUILESS_MAIN(CubismPostureTransitionTest)
#include "CubismPostureTransitionTest.moc"
