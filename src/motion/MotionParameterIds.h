#pragma once

#include <QString>

// Canonical Cubism parameter ids shared by the motion coordinator and its
// behavior modules. Inline variables: one entity across translation units,
// referenced only at runtime so there is no static-initialization-order
// exposure.
namespace motion {

inline const QString angleX = QStringLiteral("ParamAngleX");
inline const QString angleY = QStringLiteral("ParamAngleY");
inline const QString angleZ = QStringLiteral("ParamAngleZ");
inline const QString bodyX = QStringLiteral("ParamBodyAngleX");
inline const QString bodyY = QStringLiteral("ParamBodyAngleY");
inline const QString bodyZ = QStringLiteral("ParamBodyAngleZ");
inline const QString breath = QStringLiteral("ParamBreath");
inline const QString leftEye = QStringLiteral("ParamEyeLOpen");
inline const QString rightEye = QStringLiteral("ParamEyeROpen");
inline const QString leftArm = QStringLiteral("ParamArmLA");
inline const QString rightArm = QStringLiteral("ParamArmRA");
inline const QString mouth = QStringLiteral("ParamMouthOpenY");
inline const QString cheek = QStringLiteral("ParamCheek");
inline const QString deskVisible = QStringLiteral("ParamDeskVisible");

} // namespace motion
