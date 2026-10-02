#pragma once

#include <QPointF>

// Arbitrates a pressed pointer in logical pixels. A pending press does not
// move the window, and a head pat cannot turn into a window drag mid-stroke.
// Coordinates stay in the frame used at press time, even after a drag moves
// the window. The caller owns smoothing and the actual character response.
class PetPointerGesture final {
public:
    enum class Mode { None, Pending, Pat, Drag };

    void press(const QPointF& position, bool headHit);
    void move(const QPointF& position);
    void advance(double seconds);
    // The motion layer may reject a pat after the press (cooldown or priority).
    // Resume ordinary drag arbitration instead of trapping the pointer in Pat.
    void rejectPat();
    void release();
    void cancel();

    Mode mode() const { return mode_; }
    bool pressedOnHead() const { return headHit_; }
    QPointF displacement() const { return position_ - origin_; }
    double normalizedPatDirection() const;

private:
    void considerPat();

    Mode mode_ = Mode::None;
    QPointF origin_;
    QPointF position_;
    QPointF strokeAxis_;
    double furthestStroke_ = 0.0;
    double elapsed_ = 0.0;
    bool headHit_ = false;
    bool returnedStroke_ = false;
};
