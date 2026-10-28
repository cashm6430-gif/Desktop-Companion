#pragma once

#include "EdgeDockGeometry.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <algorithm>
#include <cmath>

// Isolated preview geometry. It does not move a live desktop window or enable
// the feature. All coordinates here are Qt logical global coordinates.
namespace edge_hide {
constexpr double kHeadCut = 390.0 / 840.0;

inline double ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
inline QPointF mix(const QPointF& a, const QPointF& b, double t) {
    return a + (b - a) * t;
}

struct Layout {
    QTransform modelToScreen;
    QRectF screen;
    double size = 0;
    double angle = 0;
    bool valid = false;

    QPointF gaze(const QPointF& cursor) const {
        bool invertible = false;
        const auto inverse = modelToScreen.inverted(&invertible);
        if (!valid || !invertible || size <= 0) return {};
        const QPointF local = inverse.map(cursor);
        return {(local.x() - size / 2) / (size / 2),
                (local.y() - size / 2) / (size / 2)};
    }

    bool headHit(const QPointF& point, const QImage& nativeFrame) const {
        if (!valid || !screen.contains(point) || nativeFrame.isNull()) return false;
        bool invertible = false;
        const auto inverse = modelToScreen.inverted(&invertible);
        if (!invertible) return false;
        const QPointF p = inverse.map(point);
        if (p.x() < 0 || p.x() >= size || p.y() < 0 || p.y() >= size * kHeadCut) return false;
        const int x = std::clamp(int(p.x() * nativeFrame.width() / size), 0, nativeFrame.width() - 1);
        const int y = std::clamp(int(p.y() * nativeFrame.height() / size), 0, nativeFrame.height() - 1);
        return nativeFrame.pixelColor(x, y).alpha() >= 16;
    }
};

// progress 0 = upright and visible at the edge, 1 = peeking. Side/top first
// move completely outside before rotating, then return until only the head is
// visible. The diagonal margin prevents a rotating image corner from leaking.
inline Layout layout(edge_dock::Edge edge, const QRectF& screen, double size,
                     const QPointF& initialOrigin, double progress) {
    Layout result;
    if (edge == edge_dock::Edge::None || !screen.isValid() || size <= 0
        || !std::isfinite(size) || !std::isfinite(progress)
        || !std::isfinite(initialOrigin.x()) || !std::isfinite(initialOrigin.y())) return result;
    progress = std::clamp(progress, 0.0, 1.0);
    const double margin = (std::sqrt(2.0) - 1.0) * size / 2.0 + 2.0;
    QPointF hidden = initialOrigin, peek = initialOrigin;
    double finalAngle = 0;
    switch (edge) {
    case edge_dock::Edge::Left:
        hidden.setX(screen.left() - size - margin);
        peek.setX(screen.left() - size * (1.0 - kHeadCut)); finalAngle = 90; break;
    case edge_dock::Edge::Right:
        hidden.setX(screen.right() + margin);
        peek.setX(screen.right() - size * kHeadCut); finalAngle = -90; break;
    case edge_dock::Edge::Top:
        hidden.setY(screen.top() - size - margin);
        peek.setY(screen.top() - size * (1.0 - kHeadCut)); finalAngle = 180; break;
    case edge_dock::Edge::Bottom:
        peek.setY(screen.bottom() - size * kHeadCut); break;
    default: return result;
    }
    QPointF origin;
    if (edge == edge_dock::Edge::Bottom) {
        origin = mix(initialOrigin, peek, ease(progress));
    } else if (progress < 0.35) {
        origin = mix(initialOrigin, hidden, ease(progress / 0.35));
    } else if (progress < 0.65) {
        origin = hidden;
        result.angle = finalAngle * ease((progress - 0.35) / 0.30);
    } else {
        origin = mix(hidden, peek, ease((progress - 0.65) / 0.35));
        result.angle = finalAngle;
    }
    result.modelToScreen.translate(origin.x() + size / 2, origin.y() + size / 2);
    result.modelToScreen.rotate(result.angle);
    result.modelToScreen.translate(-size / 2, -size / 2);
    result.screen = screen;
    result.size = size;
    result.valid = true;
    return result;
}

// Candidate entry: move and turn around a visible point on the face rather
// than hiding the entire character before turning. Keep the previously
// reviewed endpoints exactly; the earlier layout remains available for audit.
inline Layout continuousLayout(edge_dock::Edge edge, const QRectF& screen, double size,
                               const QPointF& initialOrigin, double progress) {
    if (!std::isfinite(progress)) return {};
    const Layout first = layout(edge, screen, size, initialOrigin, 0.0);
    const Layout last = layout(edge, screen, size, initialOrigin, 1.0);
    if (!first.valid || !last.valid) return {};
    const double t = ease(progress);
    const QPointF face(size * 0.5, size * 0.4);
    const QPointF anchor = mix(first.modelToScreen.map(face), last.modelToScreen.map(face), t);
    Layout result;
    result.screen = screen;
    result.size = size;
    result.angle = last.angle * t;
    result.modelToScreen.translate(anchor.x(), anchor.y());
    result.modelToScreen.rotate(result.angle);
    result.modelToScreen.translate(-face.x(), -face.y());
    result.valid = true;
    return result;
}
} // namespace edge_hide
