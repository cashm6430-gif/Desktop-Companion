#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

#include <algorithm>

namespace pet_placement {

// Where the pet's own window-layer art lives, as a fraction of the widget.
// The thought bubble and the sticky note are painted inside the rendered frame
// rather than by a drawable in the MOC3 (see PetWindow's comic-thought-cloud
// section), and both sit in the RIGHT third of the widget: measured off the
// 840 px capture, the "?" cloud's box spans window-x 0.664..0.857 w and
// window-y 0.071..0.35 h. That is why a partial overhang past a screen edge is
// not a cosmetic issue -- it cuts the bubble while the character still looks
// perfectly fine, which reads as "the bubble is broken".
constexpr double kArtRightEdge = 0.857;

// Keeps the whole window inside `available` (a screen's available geometry).
//
// A visible strip used to be enough here ("so the pet is never lost"), but a
// strip is only enough to *grab* her: the pet window is 280 px and its art
// reaches 0.857 of that from the left, so leaving her up to 184 px past the
// right edge (which the old bound allowed) parked the entire thought bubble
// off-screen. Reproduced 2026-10-17 at 1920x1080: window at x 1760 -> the
// bubble renders at screen x 1946..2002, invisible; moving the window back on
// screen brings it straight back.
//
// If the screen is smaller than the pet the top-left corner wins, so the pet
// is never pushed off the one edge it could still be reached from.
inline QPoint clampToScreen(const QRect& available, const QSize& window, const QPoint& position)
{
    if (!available.isValid() || window.isEmpty()) return position;
    const int maxX = std::max(available.left(), available.right() + 1 - window.width());
    const int maxY = std::max(available.top(), available.bottom() + 1 - window.height());
    return QPoint(std::clamp(position.x(), available.left(), maxX),
                  std::clamp(position.y(), available.top(), maxY));
}

} // namespace pet_placement
