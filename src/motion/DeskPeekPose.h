#pragma once

#include "MotionParameterIds.h"
#include "../MotionClip.h"

#include <algorithm>

namespace motion {

// Shared by the Native approval fixture and the live player. Layer onto the
// finished pose so these deltas never enter the next frame's base blend.
inline void applyDeskPeekPose(MotionClip::Parameters& pose, const MotionClip::Parameters& deltas,
                             double side, double weight, bool seated) {
    const auto add = [&](const QString& axis, double scale = 1.0) {
        pose[axis] += deltas.value(axis) * scale * weight;
    };
    const QString eyeX = QStringLiteral("ParamEyeBallX");
    const QString eyeY = QStringLiteral("ParamEyeBallY");
    add(eyeX, side);
    add(eyeY, seated ? 1.0 : 0.32);
    for (const auto& axis : {eyeX, eyeY}) pose[axis] = std::clamp(pose.value(axis), -1.0, 1.0);
    add(angleX, side * (seated ? 0.65 : 1.0));
    add(angleY, seated ? 1.0 : 0.35);
    add(angleZ, side * (seated ? 0.8 : 1.0));
    if (!seated) {
        add(bodyX, side);
        add(bodyZ, side);
    }
    add(QStringLiteral("ParamEyeSmile"));
    add(QStringLiteral("ParamMouthForm"));
    add(cheek);
    for (const auto& axis : {leftEye, rightEye}) pose[axis] *= 1.0 + deltas.value(axis) * weight;
}

} // namespace motion
