#pragma once

#include <algorithm>
#include <cmath>

namespace CubismPostureTransition {
struct Point { double x = 0, y = 0; };

inline bool finite(Point point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

struct Sample {
    double seatedMix = 0;
    double standingFold = 0;
    bool seatedMaterial = false;
    bool valid = false;

    double nativeMaterial() const { return seatedMaterial ? 1.0 : 0.0; }
};

inline Sample sample(double rawBusyLaptop, double rawSitPose) {
    if (!std::isfinite(rawBusyLaptop) || !std::isfinite(rawSitPose)) return {};
    const auto smoothstep = [](double value) {
        value = std::clamp(value, 0.0, 1.0);
        return value * value * (3.0 - 2.0 * value);
    };
    const double mix = smoothstep((rawBusyLaptop - 0.74) / 0.19);
    const double fold = 0.93 * smoothstep(rawSitPose / 0.9) * (1.0 - mix) + mix;
    return {mix, fold, mix >= 0.5, true};
}

// New author graphs own their hip/knee/floor coordinates. Logical SitPose is
// their native axis; the legacy smoothstep must not relabel approved keyforms.
inline Sample sampleAuthored(double rawBusyLaptop, double rawSitPose) {
    if (!std::isfinite(rawBusyLaptop) || !std::isfinite(rawSitPose)) return {};
    const double sit = std::clamp(rawSitPose, 0.0, 1.0);
    const double busy = std::clamp(rawBusyLaptop, 0.0, 1.0);
    return {busy, sit, busy >= 0.5 && sit >= 0.93, true};
}

inline double smoothUnit(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

// How far the visible sit-down has progressed, from logical SitPose alone.
// The card-approved choreography plays the descent before the desk arrives,
// so the collar affine must track SitPose and not only the busy texture mix.
// Starts late enough that the standing knees visibly fold first, completes
// exactly at the 0.93 material handover so both bodies meet aligned.
inline double sitDrive(double sit) {
    return std::isfinite(sit) ? smoothUnit((sit - 0.55) / 0.38) : 0.0;
}

inline double authoredSkirtSpread(double sit) {
    return std::isfinite(sit) ? smoothUnit((sit - 0.10) / 0.55) : 0.0;
}

inline double authoredHandGround(double sit) {
    if (!std::isfinite(sit)) return 0.0;
    // Select the support rig only while its wrists coincide with the old
    // hands. Its Sit keyforms author the reach, plant and return trajectory.
    return smoothUnit((sit - 0.32) / 0.04)
        * (1.0 - smoothUnit((sit - 0.94) / 0.02));
}

inline bool collarTarget(Point standing, Point seated, double seatedMix, Point* result) {
    if (!result || !finite(standing) || !finite(seated)
        || !std::isfinite(seatedMix) || seatedMix < 0 || seatedMix > 1) return false;
    const Point target{std::lerp(standing.x, seated.x, seatedMix),
                       std::lerp(standing.y, seated.y, seatedMix)};
    if (!finite(target)) return false;
    *result = target;
    return true;
}

// Native coordinates point upward: the floor and both collars must retain
// this order. The resulting affine map fixes every point on the floor, moves
// the collar to its target, and has a positive vertical scale (no reflection).
struct FloorPinnedTransform {
    Point collar;
    Point target;
    double floor = 0;

    bool valid() const {
        if (!finite(collar) || !finite(target) || !std::isfinite(floor)
            || !(collar.y > floor) || !(target.y > floor)) return false;
        const double height = collar.y - floor;
        const double targetHeight = target.y - floor;
        const double scale = targetHeight / height;
        return std::isfinite(height) && std::isfinite(targetHeight)
            && std::isfinite(target.x - collar.x) && std::isfinite(target.y - collar.y)
            && std::isfinite(scale) && scale > 0;
    }

    bool apply(Point position, Point* result) const {
        if (!result || !valid() || !finite(position)) return false;
        const double weight = (position.y - floor) / (collar.y - floor);
        const Point mapped{position.x + weight * (target.x - collar.x),
                           position.y + weight * (target.y - collar.y)};
        if (!finite(mapped)) return false;
        *result = mapped;
        return true;
    }
};
} // namespace CubismPostureTransition
