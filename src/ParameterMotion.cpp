#include "ParameterMotion.h"

#include <QtMath>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

namespace {
const QString angleX = QStringLiteral("ParamAngleX");
const QString angleY = QStringLiteral("ParamAngleY");
const QString angleZ = QStringLiteral("ParamAngleZ");
const QString bodyX = QStringLiteral("ParamBodyAngleX");
const QString bodyY = QStringLiteral("ParamBodyAngleY");
const QString bodyZ = QStringLiteral("ParamBodyAngleZ");
const QString breath = QStringLiteral("ParamBreath");
const QString leftEye = QStringLiteral("ParamEyeLOpen");
const QString rightEye = QStringLiteral("ParamEyeROpen");
const QString leftArm = QStringLiteral("ParamArmLA");
const QString rightArm = QStringLiteral("ParamArmRA");
const QString mouth = QStringLiteral("ParamMouthOpenY");
const QString cheek = QStringLiteral("ParamCheek");
constexpr double pi = 3.14159265358979323846;
}

double ParameterMotion::smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double ParameterMotion::pulse(double t, double start, double peak, double end) {
    if (t < start || t >= end) return 0.0;
    if (t < peak) return smooth((t - start) / (peak - start));
    return 1.0 - smooth((t - peak) / (end - peak));
}

double ParameterMotion::target(const Parameters& values, const QString& id) {
    return values.value(id, 0.0);
}

void ParameterMotion::setState(PetController::State state) {
    preview_ = false;
    if (state_ == state && state != PetController::State::Delete
        && state != PetController::State::Grass) return;
    state_ = state;
    actionTime_ = 0.0;
    // Keep current parameter values. The next advance blends from the current pose.
}

bool ParameterMotion::loadGrassMotion(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto frames = document.object().value(QStringLiteral("keyframes")).toArray();
    QVector<Keyframe> keys;
    for (const auto& entry : frames) {
        const auto object = entry.toObject();
        const double time = object.value(QStringLiteral("time")).toDouble(-1.0);
        Parameters parameters;
        const auto values = object.value(QStringLiteral("parameters")).toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!it.value().isDouble() || !qIsFinite(it.value().toDouble())) {
                if (error) *error = QStringLiteral("Invalid keyframe parameter");
                return false;
            }
            parameters.insert(it.key(), it.value().toDouble());
        }
        if (!qIsFinite(time) || time < 0 || (!keys.isEmpty() && time <= keys.last().time)
            || parameters.isEmpty() || (!keys.isEmpty() && [&] {
                auto ids = parameters.keys();
                auto firstIds = keys.first().parameters.keys();
                ids.sort(); firstIds.sort();
                return ids != firstIds;
            }())) {
            if (error) *error = QStringLiteral("Keyframes need increasing times and identical parameter IDs");
            return false;
        }
        keys.append({time, parameters});
    }
    if (keys.size() < 2 || keys.first().time != 0.0) {
        if (error) *error = QStringLiteral("Motion needs at least two keys starting at zero");
        return false;
    }
    grassKeys_ = keys;
    return true;
}

ParameterMotion::Parameters ParameterMotion::grassPose(double seconds) const {
    if (grassKeys_.isEmpty()) return {};
    if (seconds <= grassKeys_.first().time) return grassKeys_.first().parameters;
    if (seconds >= grassKeys_.last().time) return grassKeys_.last().parameters;
    int next = 1;
    while (grassKeys_[next].time < seconds) ++next;
    const auto& a = grassKeys_[next - 1];
    const auto& b = grassKeys_[next];
    const double weight = smooth((seconds - a.time) / (b.time - a.time));
    Parameters result;
    for (auto it = a.parameters.begin(); it != a.parameters.end(); ++it)
        result.insert(it.key(), it.value() + (b.parameters.value(it.key()) - it.value()) * weight);
    return result;
}

void ParameterMotion::setPreviewPose(const Parameters& parameters) {
    values_ = parameters;
    grassBend_ = parameters.value(QStringLiteral("ParamGrassSwing"));
    grassTip_ = grassBend_ + parameters.value(QStringLiteral("ParamGrassTipBend")) / 0.85;
    grassBendVelocity_ = grassTipVelocity_ = 0.0;
    previousReach_ = parameters.value(QStringLiteral("ParamGrassReach"));
    previousGripAngle_ = -parameters.value(rightArm) + 55.0 * previousReach_;
    values_[QStringLiteral("ParamEyeLVisible")] = values_.value(leftEye, 1.0);
    values_[QStringLiteral("ParamEyeRVisible")] = values_.value(rightEye, 1.0);
    preview_ = true;
}

void ParameterMotion::updateGrassFlex(double seconds, double target) {
    // Substeps keep the spring stable across frame rates and short UI stalls.
    const int steps = static_cast<int>(std::ceil(seconds * 120.0));
    const double dt = seconds / steps;
    for (int i = 0; i < steps; ++i) {
        grassBendVelocity_ += (90.0 * (target - grassBend_) - 6.0 * grassBendVelocity_) * dt;
        grassBend_ += grassBendVelocity_ * dt;
        grassTipVelocity_ += (55.0 * (grassBend_ - grassTip_) - 5.0 * grassTipVelocity_) * dt;
        grassTip_ += grassTipVelocity_ * dt;
    }
    values_[QStringLiteral("ParamGrassSwing")] = std::clamp(grassBend_, -1.0, 1.0);
    values_[QStringLiteral("ParamGrassTipBend")] = std::clamp(0.85 * (grassTip_ - grassBend_), -1.0, 1.0);
}

void ParameterMotion::advance(double seconds) {
    if (preview_) return;
    if (!qIsFinite(seconds) || seconds <= 0.0) return;
    seconds = std::min(seconds, 0.1); // Avoid a leap after suspend or debugger pause.
    clock_ += seconds;
    actionTime_ += seconds;
    blinkClock_ += seconds;

    Parameters desired{
        {angleX, 2.0 * qSin(clock_ * 0.68)},
        {angleY, 1.5 * qSin(clock_ * 0.53)},
        {angleZ, 1.0 * qSin(clock_ * 0.91)},
        {bodyX, 0.8 * qSin(clock_ * 0.68 - 0.3)},
        {bodyY, 0.5 * qSin(clock_ * 1.7)},
        {bodyZ, 0.0},
        {breath, 0.5 + 0.5 * qSin(clock_ * 2.1)},
        {leftArm, 0.0}, {rightArm, 0.0},
        {mouth, 0.0}, {cheek, 0.0},
        {QStringLiteral("ParamSmileOpen"), 0.0},
        {QStringLiteral("ParamEyeBallX"), 0.0},
        {QStringLiteral("ParamEyeBallY"), 0.0},
        {QStringLiteral("ParamGrassVisible"), 0.0},
        {QStringLiteral("ParamGrassReach"), 0.0},
        {QStringLiteral("ParamGrassSwing"), 0.0},
        {QStringLiteral("ParamGrassTipBend"), 0.0},
        {QStringLiteral("ParamHandRGrip"), 0.0},
    };

    if (state_ == PetController::State::Busy) {
        desired[angleY] += -7.0 + 1.8 * qSin(clock_ * 4.3);
        desired[bodyX] += 2.0;
        desired[leftArm] = 9.0 + 5.0 * qSin(clock_ * 10.0);
        desired[rightArm] = -9.0 + 5.0 * qSin(clock_ * 10.0 + pi);
        desired[mouth] = 0.12;
    } else if (state_ == PetController::State::Delete) {
        // Anticipation -> swing -> recoil. A rigged arm/prop responds to these
        // parameters continuously; no pose swapping or GIF frame stepping.
        const double windup = pulse(actionTime_, 0.0, 0.28, 0.48);
        const double strike = pulse(actionTime_, 0.34, 0.59, 0.91);
        const double recoil = pulse(actionTime_, 0.82, 1.02, 1.34);
        desired[rightArm] = -22.0 * windup + 30.0 * strike - 8.0 * recoil;
        desired[leftArm] = 8.0 * windup - 5.0 * strike;
        desired[bodyZ] = -7.0 * windup + 10.0 * strike - 3.0 * recoil;
        desired[angleZ] += -6.0 * windup + 8.0 * strike;
        desired[angleY] += 4.0 * strike;
        desired[mouth] = 0.4 * strike;
    } else if (state_ == PetController::State::Grass) {
        const auto pose = grassPose(actionTime_);
        for (auto it = pose.begin(); it != pose.end(); ++it) desired[it.key()] = it.value();
    }

    // Short asymmetric blink. It stays procedural across action transitions.
    const double blinkPhase = std::fmod(blinkClock_, 4.3);
    const double eye = 1.0 - pulse(blinkPhase, 0.0, 0.075, 0.16);
    // Blink modulates the authored expression, rather than overwriting winks
    // and smiling closed eyes supplied by the action keyframes.
    desired[leftEye] = desired.value(leftEye, 1.0) * eye;
    desired[rightEye] = desired.value(rightEye, 1.0) * eye;
    desired[QStringLiteral("ParamEyeLVisible")] = desired[leftEye];
    desired[QStringLiteral("ParamEyeRVisible")] = desired[rightEye];

    // Time-based exponential blending is stable at both 30 and 60 Hz.
    const double alpha = 1.0 - qExp(-seconds / 0.12);
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const double previous = values_.contains(it.key()) ? values_.value(it.key()) : it.value();
        values_[it.key()] = previous + (it.value() - previous) * alpha;
    }
    const double reach = values_.value(QStringLiteral("ParamGrassReach"));
    const double gripAngle = -values_.value(rightArm) + 55.0 * reach;
    const double gripSpeed = (gripAngle - previousGripAngle_) / seconds;
    const double reachSpeed = (reach - previousReach_) / seconds;
    previousGripAngle_ = gripAngle;
    previousReach_ = reach;
    const double visible = values_.value(QStringLiteral("ParamGrassVisible"));
    const double flexTarget = 0.65 * desired.value(QStringLiteral("ParamGrassSwing"))
        - visible * (0.0045 * gripSpeed + 0.035 * reachSpeed);
    updateGrassFlex(seconds, std::clamp(flexTarget, -1.0, 1.0));
}
