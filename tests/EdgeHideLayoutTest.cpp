#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QLineF>

#include "../src/EdgeHideLayout.h"

class EdgeHideLayoutTest : public QObject {
    Q_OBJECT
    const QRectF screen{-1280, 40, 1280, 984};
    const double size = 280;
    QPointF start(edge_dock::Edge edge) const {
        switch (edge) {
        case edge_dock::Edge::Left: return {screen.left(), 300};
        case edge_dock::Edge::Right: return {screen.right() - size, 300};
        case edge_dock::Edge::Top: return {-800, screen.top()};
        case edge_dock::Edge::Bottom: return {-800, screen.bottom() - size};
        default: return {};
        }
    }
    QList<edge_dock::Edge> edges() const {
        return {edge_dock::Edge::Left, edge_dock::Edge::Right,
                edge_dock::Edge::Top, edge_dock::Edge::Bottom};
    }
private slots:
    void hiddenRotationDoesNotLeak() {
        for (const auto edge : {edge_dock::Edge::Left, edge_dock::Edge::Right, edge_dock::Edge::Top}) {
            for (int i = 0; i <= 30; ++i) {
                const double phase = 0.35 + i * 0.01;
                const auto pose = edge_hide::layout(edge, screen, size, start(edge), phase);
                QVERIFY(pose.valid);
                QVERIFY2(!pose.modelToScreen.mapRect(QRectF(0, 0, size, size)).intersects(screen),
                         qPrintable(QStringLiteral("rotation leaked at %1").arg(phase)));
            }
        }
    }
    void rotationsAndGazeUseSameInverse() {
        for (const auto edge : edges()) {
            const auto pose = edge_hide::layout(edge, screen, size, start(edge), 1.0);
            const QPointF local(size * 0.75, size * 0.25);
            const QPointF gaze = pose.gaze(pose.modelToScreen.map(local));
            QVERIFY(std::abs(gaze.x() - 0.5) < 1e-9);
            QVERIFY(std::abs(gaze.y() + 0.5) < 1e-9);
        }
    }
    void actualNativeHeadIsGrabbedAndBodyIsNot() {
        const QString imagePath = QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("../art/live2d/workflow/edge-hide-concept-20261028/neutral-native.png"));
        const QImage image(imagePath);
        QVERIFY2(!image.isNull(), qPrintable(imagePath));
        for (const auto edge : edges()) {
            const auto pose = edge_hide::layout(edge, screen, size, start(edge), 1.0);
            const QPointF face = pose.modelToScreen.map(QPointF(size * 0.5, size * 0.4));
            QVERIFY(screen.contains(face));
            QVERIFY(pose.headHit(face, image));
            const QPointF torso = pose.modelToScreen.map(QPointF(size * 0.5, size * 0.6));
            QVERIFY(!pose.headHit(torso, image));
            const QPointF transparent = pose.modelToScreen.map(QPointF(size * 0.01, size * 0.01));
            QVERIFY(!pose.headHit(transparent, image));
        }
    }
    void transitionIsContinuousAndReversible() {
        for (const auto edge : edges()) {
            QPointF previous;
            for (int i = 0; i <= 1000; ++i) {
                const auto pose = edge_hide::layout(edge, screen, size, start(edge), i / 1000.0);
                const QPointF current = pose.modelToScreen.map(QPointF(size * 0.5, size * 0.3));
                if (i) QVERIFY(QLineF(previous, current).length() < 5);
                previous = current;
                const auto reverse = edge_hide::layout(edge, screen, size, start(edge), 1.0 - (1000 - i) / 1000.0);
                QVERIFY(QLineF(current, reverse.modelToScreen.map(QPointF(size * 0.5, size * 0.3))).length() < 1e-7);
            }
        }
    }
    void invalidInputsDoNotCreateLayout() {
        QVERIFY(!edge_hide::layout(edge_dock::Edge::None, screen, size, {}, 0).valid);
        QVERIFY(!edge_hide::layout(edge_dock::Edge::Left, {}, size, {}, 0).valid);
        QVERIFY(!edge_hide::layout(edge_dock::Edge::Left, screen, 0, {}, 0).valid);
        QVERIFY(!edge_hide::layout(edge_dock::Edge::Left, screen, size, {}, qQNaN()).valid);
    }
};

QTEST_GUILESS_MAIN(EdgeHideLayoutTest)
#include "EdgeHideLayoutTest.moc"
