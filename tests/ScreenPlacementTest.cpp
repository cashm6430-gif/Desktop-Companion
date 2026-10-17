#include <QtTest>

#include "../src/ScreenPlacement.h"

// The pet draws window-layer art (the thought bubble, the sticky note) inside
// the rendered frame, in the right third of the widget. These tests pin the one
// property that keeps that art visible: the WINDOW, not just a strip of it, has
// to end up on the screen the user dragged her to.
//
// Regression: 2026-10-17. constrainPositionToScreen used to allow a 184 px
// overhang past the right edge ("keep a visible strip"), which put the whole
// thought bubble off-screen while the character still looked fine. Reproduced
// live at 1920x1080 with the window at x 1760 and the bubble at screen x
// 1946..2002; dragging the window back on screen made it reappear, so the
// overhang -- not the painter -- was the bug.
class ScreenPlacementTest : public QObject {
    Q_OBJECT

private:
    // A 1920x1080 screen with a 40 px taskbar, i.e. the machine this was found on.
    static QRect machineScreen() { return QRect(0, 0, 1920, 1040); }

    // The comic-thought cloud's box as a fraction of the widget, measured off
    // the 840 px approval capture (x 0.664..0.857 w, y 0.071..0.35 h).
    static QRectF bubbleBox(const QSize& window)
    {
        return QRectF(window.width() * 0.664, window.height() * 0.071,
                      window.width() * (pet_placement::kArtRightEdge - 0.664),
                      window.height() * 0.279);
    }

private slots:
    void keepsTheWholeWindowOnScreen()
    {
        const QRect screen = machineScreen();
        const QSize window(280, 280);
        for (const QPoint& pushed : {QPoint(-900, 300), QPoint(1824, 300), QPoint(5000, 300),
                                     QPoint(500, -500), QPoint(500, 1039), QPoint(500, 5000)}) {
            const QPoint placed = pet_placement::clampToScreen(screen, window, pushed);
            const QRect rect(placed, window);
            QVERIFY2(screen.contains(rect), qPrintable(QStringLiteral("%1,%2 -> %3,%4")
                .arg(pushed.x()).arg(pushed.y()).arg(placed.x()).arg(placed.y())));
        }
    }

    void flushEdgesArePreserved()
    {
        const QRect screen = machineScreen();
        const QSize window(280, 280);
        // snapToEdges docks flush; the clamp must not fight it on the next drag.
        QCOMPARE(pet_placement::clampToScreen(screen, window, QPoint(1640, 300)), QPoint(1640, 300));
        QCOMPARE(pet_placement::clampToScreen(screen, window, QPoint(0, 0)), QPoint(0, 0));
        QCOMPARE(pet_placement::clampToScreen(screen, window, QPoint(0, 760)), QPoint(0, 760));
    }

    // The case the user hit: she hangs off the right edge, so the "?" -- which
    // lives at 0.857 w -- is parked off-screen. The old 96 px-strip bound is
    // used as the input on purpose: it is what a drag could actually leave.
    void thoughtBubbleStaysOnScreen()
    {
        const QRect screen = machineScreen();
        const QSize window(280, 280);
        const QPoint oldBound(screen.right() - 96, 300);
        const QRectF bubble = bubbleBox(window);
        QVERIFY2(!screen.contains(QRect(oldBound.x() + int(bubble.right()), oldBound.y() + int(bubble.top()),
                                       1, 1)),
                 "the old bound really did park the bubble off-screen");
        const QPoint healed = pet_placement::clampToScreen(screen, window, oldBound);
        const QRectF placed(bubble.translated(healed));
        QVERIFY2(screen.contains(placed.toAlignedRect()),
                 qPrintable(QStringLiteral("bubble %1,%2 %3x%4")
                     .arg(placed.x()).arg(placed.y()).arg(placed.width()).arg(placed.height())));
    }

    void bubbleSurvivesEveryScaleAndPush()
    {
        const QRect screen = machineScreen();
        for (int px = 180; px <= 560; px += 20) {
            const QSize window(px, px);
            const QRectF bubble = bubbleBox(window);
            for (int x = -800; x <= 2400; x += 37) {
                for (int y = -300; y <= 1300; y += 61) {
                    const QPoint placed = pet_placement::clampToScreen(screen, window, QPoint(x, y));
                    const QRectF box = bubble.translated(placed);
                    QVERIFY2(screen.contains(box.toAlignedRect()),
                             qPrintable(QStringLiteral("px=%1 push=%2,%3 -> bubble %4,%5")
                                 .arg(px).arg(x).arg(y).arg(box.x()).arg(box.y())));
                }
            }
        }
    }

    void screenSmallerThanThePetKeepsTheReachableCorner()
    {
        const QRect screen(0, 0, 200, 200);
        const QSize window(280, 280);
        QCOMPARE(pet_placement::clampToScreen(screen, window, QPoint(160, 160)), QPoint(0, 0));
        QCOMPARE(pet_placement::clampToScreen(screen, window, QPoint(-40, -40)), QPoint(0, 0));
    }

    void degenerateInputIsLeftAlone()
    {
        const QPoint position(700, 400);
        QCOMPARE(pet_placement::clampToScreen(QRect(), QSize(280, 280), position), position);
        QCOMPARE(pet_placement::clampToScreen(QRect(0, 0, 1920, 1040), QSize(), position), position);
    }
};

QTEST_APPLESS_MAIN(ScreenPlacementTest)
#include "ScreenPlacementTest.moc"
