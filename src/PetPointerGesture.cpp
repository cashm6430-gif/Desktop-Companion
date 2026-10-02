#include "PetPointerGesture.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kBodyDragDistance = 6.0;
constexpr double kHeadDragDistance = 18.0;
constexpr double kStrokeDistance = 2.0;
constexpr double kPatHoldSeconds = 0.2;

bool finitePosition(const QPointF& point) {
    return std::isfinite(point.x()) && std::isfinite(point.y());
}
}

void PetPointerGesture::press(const QPointF& position, bool headHit) {
    cancel();
    if (!finitePosition(position)) return;
    mode_ = Mode::Pending;
    origin_ = position_ = position;
    headHit_ = headHit;
}

void PetPointerGesture::move(const QPointF& position) {
    if (mode_ == Mode::None || !finitePosition(position)) return;
    position_ = position;
    if (mode_ != Mode::Pending) return;

    const QPointF offset = displacement();
    const double distance = std::hypot(offset.x(), offset.y());
    if (distance > (headHit_ ? kHeadDragDistance : kBodyDragDistance)) {
        mode_ = Mode::Drag;
        return;
    }
    if (!headHit_) return;

    // Require one deliberate excursion and a return along the same axis.
    // Cumulative pointer jitter and a motionless long press do not count.
    if (strokeAxis_.isNull() && distance >= kStrokeDistance) {
        strokeAxis_ = offset / distance;
        furthestStroke_ = distance;
    } else if (!strokeAxis_.isNull()) {
        const double projection = QPointF::dotProduct(offset, strokeAxis_);
        furthestStroke_ = std::max(furthestStroke_, projection);
        if (furthestStroke_ - projection >= kStrokeDistance)
            returnedStroke_ = true;
    }
    considerPat();
}

void PetPointerGesture::advance(double seconds) {
    if (mode_ == Mode::None || !std::isfinite(seconds) || seconds <= 0.0) return;
    elapsed_ += seconds;
    considerPat();
}

void PetPointerGesture::considerPat() {
    if (mode_ == Mode::Pending && headHit_ && returnedStroke_ && elapsed_ >= kPatHoldSeconds)
        mode_ = Mode::Pat;
}

double PetPointerGesture::normalizedPatDirection() const {
    return mode_ == Mode::Pat ? std::clamp(displacement().x() / 12.0, -1.0, 1.0) : 0.0;
}

void PetPointerGesture::release() {
    cancel();
}

void PetPointerGesture::rejectPat() {
    if (mode_ != Mode::Pat) return;
    mode_ = Mode::Pending;
    headHit_ = false;
    returnedStroke_ = false;
    move(position_);
}

void PetPointerGesture::cancel() {
    mode_ = Mode::None;
    origin_ = position_ = strokeAxis_ = QPointF();
    furthestStroke_ = elapsed_ = 0.0;
    headHit_ = returnedStroke_ = false;
}
