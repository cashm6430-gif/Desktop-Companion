#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <algorithm>
#include <limits>

// Geometry only; this does not enter a dock state or move a window. Coordinates
// must all be Qt logical global coordinates from the same screen snapshot.
namespace edge_dock {
enum class Edge { None, Left, Right, Top, Bottom };

inline Edge outerEdgeAtRelease(const QRect& window, const QPoint& cursor,
                               const QRect& screen, const QRect& available,
                               const QList<QRect>& screens, int tolerance)
{
    if (!window.isValid() || !screen.isValid() || !available.isValid()
        || !screen.contains(available) || !screen.contains(cursor) || !window.contains(cursor)
        || !window.intersects(screen) || tolerance < 0) return Edge::None;

    struct Candidate { Edge edge; int distance; QPoint outsideScreen; };
    const Candidate candidates[] = {
        {Edge::Left, window.left() - available.left(), QPoint(screen.left() - 1, cursor.y())},
        {Edge::Right, available.right() - window.right(), QPoint(screen.right() + 1, cursor.y())},
        {Edge::Top, window.top() - available.top(), QPoint(cursor.x(), screen.top() - 1)},
        {Edge::Bottom, available.bottom() - window.bottom(), QPoint(cursor.x(), screen.bottom() + 1)}
    };
    Edge best = Edge::None;
    int bestDistance = std::numeric_limits<int>::max();
    for (const auto& candidate : candidates) {
        const int distance = std::max(0, candidate.distance);
        if (distance > tolerance || distance >= bestDistance) continue;
        bool neighbour = false;
        for (const auto& other : screens) {
            if (other != screen && other.contains(candidate.outsideScreen)) {
                neighbour = true;
                break;
            }
        }
        if (!neighbour) {
            best = candidate.edge;
            bestDistance = distance;
        }
    }
    return best; // Equal corner distances resolve left, right, top, bottom.
}
} // namespace edge_dock
