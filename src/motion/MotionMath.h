#pragma once

#include <algorithm>

// Shared easing math for the motion coordinator and its behavior modules.
// These used to be private static members of ParameterMotion; they are pure
// functions, so they live here where every behavior can use the same curve
// definitions without reaching into the coordinator.
namespace motion {

// Smoothstep easing on the clamped 0..1 interval.
inline double smooth(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Rise to a peak at `peak` between `start` and `end`, then fall back to zero.
// Outside the window the value is exactly 0.
inline double pulse(double t, double start, double peak, double end)
{
    if (t < start || t >= end) return 0.0;
    if (t < peak) return smooth((t - start) / (peak - start));
    return 1.0 - smooth((t - peak) / (end - peak));
}

} // namespace motion
