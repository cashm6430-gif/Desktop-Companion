#pragma once

#include "EdgeHideLayout.h"
#include <QRect>
#include <algorithm>
#include <cmath>

// Runtime-independent state for the opt-in desktop preview. Coordinates are
// Qt logical global coordinates; it never moves a window or consumes events.
class EdgeDockSession final {
public:
    enum class Phase { Normal, Entering, Hidden, Dragging };
    bool active() const { return phase_ != Phase::Normal; }
    Phase phase() const { return phase_; }
    double progress() const { return progress_; }
    double modelSize() const { return size_; }
    edge_dock::Edge edge() const { return edge_; }
    QRectF screen() const { return screen_; }

    bool begin(edge_dock::Edge edge, QRectF screen, double size, QPointF origin) {
        if (active()) return false;
        const auto candidate = edge_hide::continuousLayout(edge, screen, size, origin, 0);
        if (!candidate.valid || !screen.contains(QRectF(origin, QSizeF(size, size)))) return false;
        edge_ = edge; screen_ = screen; size_ = size; origin_ = origin;
        progress_ = 0; phase_ = Phase::Entering; return true;
    }
    void advance(double seconds) {
        if (phase_ != Phase::Entering || !std::isfinite(seconds) || seconds <= 0) return;
        progress_ = (std::min)(1.0, progress_ + seconds);
        if (progress_ >= 1.0) phase_ = Phase::Hidden;
    }
    edge_hide::Layout layout() const {
        if (!active()) return {};
        auto pose = edge_hide::continuousLayout(edge_, screen_, size_, origin_, progress_);
        if (phase_ == Phase::Dragging) {
            pose.angle = pressedAngle_ * edge_hide::ease(progress_);
            pose.modelToScreen = QTransform();
            pose.modelToScreen.translate(cursor_.x(), cursor_.y());
            pose.modelToScreen.rotate(pose.angle);
            pose.modelToScreen.translate(-anchor_.x(), -anchor_.y());
        }
        return pose;
    }
    QRect windowGeometry() const {
        const auto pose = layout();
        if (!pose.valid) return {};
        return pose.modelToScreen.mapRect(QRectF(0, 0, size_, size_))
            .intersected(screen_).toAlignedRect().intersected(screen_.toAlignedRect());
    }
    bool press(QPointF global, const QImage& native) {
        if (phase_ != Phase::Hidden) return false;
        const auto pose = layout();
        if (!pose.headHit(global, native)) return false;
        bool ok = false;
        anchor_ = pose.modelToScreen.inverted(&ok).map(global);
        if (!ok) return false;
        press_ = cursor_ = global; pressedAngle_ = pose.angle; phase_ = Phase::Dragging;
        return true;
    }
    void drag(QPointF global) {
        if (phase_ != Phase::Dragging) return;
        cursor_ = global;
        QPointF direction;
        switch (edge_) {
        case edge_dock::Edge::Left: direction = {1, 0}; break;
        case edge_dock::Edge::Right: direction = {-1, 0}; break;
        case edge_dock::Edge::Top: direction = {0, 1}; break;
        case edge_dock::Edge::Bottom: direction = {0, -1}; break;
        default: return;
        }
        const double threshold = size_ * 0.4;
        progress_ = 1.0 - std::clamp(QPointF::dotProduct(global - press_, direction) / threshold, 0.0, 1.0);
    }
    // Returns the full-size origin. The owner resets after restoring its window.
    bool release(QPointF* restoredOrigin) {
        if (phase_ != Phase::Dragging) return false;
        if (progress_ > 0.01) { cancelDrag(); return false; }
        const QPointF position = layout().modelToScreen.map(QPointF());
        if (restoredOrigin) *restoredOrigin = position;
        return true;
    }
    void cancelDrag() {
        if (phase_ == Phase::Dragging) { progress_ = 1; phase_ = Phase::Hidden; }
    }
    void reset() { phase_ = Phase::Normal; progress_ = 0; edge_ = edge_dock::Edge::None; }
private:
    Phase phase_ = Phase::Normal;
    edge_dock::Edge edge_ = edge_dock::Edge::None;
    QRectF screen_;
    double size_ = 0, progress_ = 0, pressedAngle_ = 0;
    QPointF origin_, press_, cursor_, anchor_;
};
