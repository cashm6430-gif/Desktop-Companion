#include "ParameterMotion.h"

#include <QDir>
#include <QtMath>
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

// A seated work cycle only reads correctly when every key is a seated pose.
bool validateSeated(const MotionClip& clip, QString* error) {
    for (const auto& key : clip.keys()) {
        if (key.parameters.value(QStringLiteral("ParamBusyLaptop")) != 1.0) {
            if (error) *error = QStringLiteral("Laptop loop requires seated poses");
            return false;
        }
    }
    return true;
}
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

void ParameterMotion::setState(PetController::State state) {
    preview_ = false;
    sequencePhysics_ = false;
    if (state_ == state && state != PetController::State::Delete
        && state != PetController::State::Grass) return;
    state_ = state;
    actionTime_ = 0.0;
    finishedPending_ = false;
    if (state == PetController::State::Idle) {
        busyChoiceExists_ = false;
        laptopBusy_ = false;
        busyTime_ = 0.0;
        nextBusyChoice_ = 40.0;
    } else if (state == PetController::State::Busy && !busyChoiceExists_) {
        busyChoiceExists_ = true;
        laptopBusy_ = laptopClip_.isValid() && busyRandom_.generateDouble() < 0.4;
    }
    // Keep current parameter values. The next advance blends from the current pose.
}

bool ParameterMotion::loadGrassMotion(const QString& path, QString* error) {
    MotionClip clip;
    clip.setId(QStringLiteral("grass"));
    if (!clip.loadJson(path, error)) return false;
    grassClip_ = clip;
    library_.loadClip(QStringLiteral("grass"), path, nullptr);
    return true;
}

bool ParameterMotion::loadBusyLaptopMotion(const QString& path, QString* error) {
    MotionClip clip;
    clip.setId(QStringLiteral("busy-laptop"));
    if (!clip.loadJson(path, error)) return false;
    if (!validateSeated(clip, error)) return false;
    // A seated work cycle is a loop: the authored seam is the wrap point.
    clip.setLoop(true, 0.0, clip.duration());
    laptopClip_ = clip;
    library_.loadClip(QStringLiteral("busy-laptop"), path, nullptr);
    return true;
}

bool ParameterMotion::loadMotionLibrary(const QString& directory, QString* error) {
    if (!library_.loadDirectory(directory, error)) return false;
    if (const MotionClip* grass = library_.clip(QStringLiteral("grass"))) grassClip_ = *grass;
    if (const MotionClip* idle = library_.clip(QStringLiteral("idle"))) idleClip_ = *idle;
    if (const MotionClip* standing = library_.clip(QStringLiteral("busy-stand"))) busyStandClip_ = *standing;
    if (const MotionClip* remove = library_.clip(QStringLiteral("delete"))) deleteClip_ = *remove;
    if (const MotionClip* laptop = library_.clip(QStringLiteral("busy-laptop"))) {
        if (!validateSeated(*laptop, error)) return false;
        laptopClip_ = *laptop;
        laptopClip_.setLoop(true, laptopClip_.loopStart(), laptopClip_.duration());
    }
    return true;
}

double ParameterMotion::actionDuration(PetController::State state) const {
    switch (state) {
    case PetController::State::Delete: return library_.duration(QStringLiteral("delete"));
    case PetController::State::Grass:
        return grassClip_.isValid() ? grassClip_.duration()
                                    : library_.duration(QStringLiteral("grass"));
    default: return 0.0; // Background states are open-ended.
    }
}

bool ParameterMotion::consumeActionFinished() {
    const bool finished = finishedPending_;
    finishedPending_ = false;
    return finished;
}

void ParameterMotion::forceLaptopBusy() {
    if (!laptopClip_.isValid()) return;
    setState(PetController::State::Busy);
    busyChoiceExists_ = true;
    laptopBusy_ = true;
    busyTime_ = 0;
    nextBusyChoice_ = 40;
}

void ParameterMotion::forceStandingBusy() {
    setState(PetController::State::Busy);
    busyChoiceExists_ = true;
    laptopBusy_ = false;
    busyTime_ = 0;
    nextBusyChoice_ = 40;
}

ParameterMotion::Parameters ParameterMotion::grassPose(double seconds) const {
    return grassClip_.sample(seconds);
}

void ParameterMotion::setPreviewPose(const Parameters& parameters) {
    values_ = parameters;
    grassBend_ = parameters.value(QStringLiteral("ParamGrassSwing"));
    grassTip_ = grassBend_ + parameters.value(QStringLiteral("ParamGrassTipBend")) / 0.85;
    grassBendVelocity_ = grassTipVelocity_ = 0.0;
    previousReach_ = parameters.value(QStringLiteral("ParamGrassReach"));
    previousGripAngle_ = -parameters.value(rightArm)
        - parameters.value(QStringLiteral("ParamElbowRA"))
        - parameters.value(QStringLiteral("ParamWristRA")) + 30.0 * previousReach_;
    leftEyeExpression_ = parameters.value(leftEye, 1.0);
    rightEyeExpression_ = parameters.value(rightEye, 1.0);
    preview_ = true;
    sequencePhysics_ = false;
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
        {angleX, 0.0},
        {angleY, 0.0},
        {angleZ, 0.0},
        {bodyX, 0.0},
        {bodyY, 0.0},
        {bodyZ, 0.0},
        {breath, 0.0},
        {leftArm, 0.0}, {rightArm, 0.0},
        {QStringLiteral("ParamElbowLA"), 0.0}, {QStringLiteral("ParamElbowRA"), 0.0},
        {QStringLiteral("ParamWristRA"), 0.0},
        {leftEye, 1.0}, {rightEye, 1.0},
        {mouth, 0.0}, {cheek, 0.0},
        {QStringLiteral("ParamSmileOpen"), 0.0},
        {QStringLiteral("ParamEyeSmile"), 0.0},
        {QStringLiteral("ParamEyeBallX"), 0.0},
        {QStringLiteral("ParamEyeBallY"), 0.0},
        {QStringLiteral("ParamGrassVisible"), 0.0},
        {QStringLiteral("ParamGrassReach"), 0.0},
        {QStringLiteral("ParamGrassSwing"), 0.0},
        {QStringLiteral("ParamGrassTipBend"), 0.0},
        {QStringLiteral("ParamHandRGrip"), 0.0},
        {QStringLiteral("ParamBusyLaptop"), 0.0},
        {QStringLiteral("ParamSitPose"), 0.0},
        {QStringLiteral("ParamLaptopVisible"), 0.0},
        {QStringLiteral("ParamBusyTypingL"), 0.0},
        {QStringLiteral("ParamBusyTypingR"), 0.0},
        {QStringLiteral("ParamLaptopRock"), 0.0},
    };

    // Idle base layer. Sampled on the global clock so the breathing phase
    // carries across state changes instead of restarting at every switch.
    if (idleClip_.isValid()) {
        const auto base = idleClip_.sample(clock_);
        for (auto it = base.cbegin(); it != base.cend(); ++it) desired[it.key()] = it.value();
    } else {
        // Fallback for callers that loaded a single clip instead of the library.
        desired[angleX] = 2.0 * qSin(clock_ * 0.68);
        desired[angleY] = 1.5 * qSin(clock_ * 0.53);
        desired[angleZ] = 1.0 * qSin(clock_ * 0.91);
        desired[bodyX] = 0.8 * qSin(clock_ * 0.68 - 0.3);
        desired[bodyY] = 0.5 * qSin(clock_ * 1.7);
        desired[breath] = 0.5 + 0.5 * qSin(clock_ * 2.1);
    }

    if (state_ == PetController::State::Busy) {
        // The standing accent is additive: it layers onto the idle base rather
        // than replacing it, so the breathing curve lives in exactly one place.
        if (busyStandClip_.isValid()) {
            const auto accent = busyStandClip_.sample(clock_);
            for (auto it = accent.cbegin(); it != accent.cend(); ++it)
                desired[it.key()] += it.value();
        } else {
            desired[angleY] += -7.0 + 1.8 * qSin(clock_ * 4.3);
            desired[bodyX] += 2.0;
            desired[leftArm] = 9.0 + 5.0 * qSin(clock_ * 10.0);
            desired[rightArm] = -9.0 + 5.0 * qSin(clock_ * 10.0 + pi);
            desired[mouth] = 0.12;
        }
        busyTime_ += seconds;
        // Choice times coincide with the authored loop seam.
        // A delete/grass interruption pauses this clock and retains the choice.
        if (busyTime_ >= nextBusyChoice_) {
            laptopBusy_ = laptopClip_.isValid() && busyRandom_.generateDouble() < 0.4;
            nextBusyChoice_ += 40.0;
        }
        if (laptopBusy_) {
            const auto pose = laptopClip_.sampleLooped(busyTime_);
            for (auto it = pose.cbegin(); it != pose.cend(); ++it) desired[it.key()] = it.value();
            desired[QStringLiteral("ParamSitPose")] = 1;
            desired[QStringLiteral("ParamLaptopVisible")] = smooth((values_.value(QStringLiteral("ParamSitPose")) - 0.85)/0.1);
            const double sitting = values_.value(QStringLiteral("ParamSitPose"));
            desired[leftArm] = -40*sitting;
            desired[rightArm] = -40*sitting;
            const double effort = desired.value(QStringLiteral("ParamBusyTypingR"));
            desired[QStringLiteral("ParamBusyTypingL")] *= 0.5 + 0.5*qSin(busyTime_*17.0);
            desired[QStringLiteral("ParamBusyTypingR")] = effort*(0.5 + 0.5*qSin(busyTime_*17.0+pi));
        }
    } else if (state_ == PetController::State::Idle && actionTime_ < 0.36
               && values_.value(QStringLiteral("ParamBusyLaptop")) >= 0.9) {
        // Put the computer away before unfolding the legs. A short task that
        // never reached the seated body continues directly from its pose.
        desired[QStringLiteral("ParamBusyLaptop")] = values_.value(QStringLiteral("ParamBusyLaptop"));
        desired[QStringLiteral("ParamSitPose")] = values_.value(QStringLiteral("ParamSitPose"));
        desired[leftArm] = -40 * desired.value(QStringLiteral("ParamSitPose"));
        desired[rightArm] = desired.value(leftArm);
    } else if (state_ == PetController::State::Delete) {
        // Anticipation -> swing -> recoil. A rigged arm/prop responds to these
        // parameters continuously; no pose swapping or GIF frame stepping.
        if (deleteClip_.isValid()) {
            const auto accent = deleteClip_.sample(actionTime_);
            for (auto it = accent.cbegin(); it != accent.cend(); ++it)
                desired[it.key()] += it.value();
        } else {
            const double windup = pulse(actionTime_, 0.0, 0.28, 0.48);
            const double strike = pulse(actionTime_, 0.34, 0.59, 0.91);
            const double recoil = pulse(actionTime_, 0.82, 1.02, 1.34);
            desired[rightArm] = -22.0 * windup + 30.0 * strike - 8.0 * recoil;
            desired[leftArm] = 8.0 * windup - 5.0 * strike;
            desired[bodyZ] = -7.0 * windup + 10.0 * strike - 3.0 * recoil;
            desired[angleZ] += -6.0 * windup + 8.0 * strike;
            desired[angleY] += 4.0 * strike;
            desired[mouth] = 0.4 * strike;
        }
    } else if (state_ == PetController::State::Grass) {
        const auto pose = grassClip_.sample(actionTime_);
        for (auto it = pose.begin(); it != pose.end(); ++it) desired[it.key()] = it.value();
    }

    // Short asymmetric blink. It stays procedural across action transitions.
    const double blinkPhase = std::fmod(blinkClock_, 4.3);
    const double eye = 1.0 - pulse(blinkPhase, 0.0, 0.075, 0.16);
    // Time-based exponential blending is stable at both 30 and 60 Hz.
    const double alpha = 1.0 - qExp(-seconds / 0.12);
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        double previous = values_.contains(it.key()) ? values_.value(it.key()) : it.value();
        if (it.key() == leftEye) previous = leftEyeExpression_;
        if (it.key() == rightEye) previous = rightEyeExpression_;
        const double weight = it.key() == QStringLiteral("ParamBusyLaptop") || it.key() == QStringLiteral("ParamSitPose")
            ? 1.0 - qExp(-seconds / 0.28) : alpha;
        const double blended = previous + (it.value() - previous) * weight;
        if (it.key() == leftEye) leftEyeExpression_ = blended;
        if (it.key() == rightEye) rightEyeExpression_ = blended;
        values_[it.key()] = blended;
    }
    // Smooth the authored expression first, then apply the short blink. Feeding
    // blink values through the general 120ms filter prevents full closure.
    values_[leftEye] = leftEyeExpression_ * eye;
    values_[rightEye] = rightEyeExpression_ * eye;
    const double reach = values_.value(QStringLiteral("ParamGrassReach"));
    const double gripAngle = -values_.value(rightArm)
        - values_.value(QStringLiteral("ParamElbowRA"))
        - values_.value(QStringLiteral("ParamWristRA")) + 30.0 * reach;
    const double gripSpeed = (gripAngle - previousGripAngle_) / seconds;
    const double reachSpeed = (reach - previousReach_) / seconds;
    previousGripAngle_ = gripAngle;
    previousReach_ = reach;
    const double visible = values_.value(QStringLiteral("ParamGrassVisible"));
    const double flexTarget = 0.65 * desired.value(QStringLiteral("ParamGrassSwing"))
        - visible * (0.0045 * gripSpeed + 0.035 * reachSpeed);
    updateGrassFlex(seconds, std::clamp(flexTarget, -1.0, 1.0));

    // A one-shot action reports completion once it reaches its authored length.
    if (state_ == PetController::State::Delete || state_ == PetController::State::Grass) {
        const double duration = actionDuration(state_);
        if (duration > 0.0 && actionTime_ >= duration) finishedPending_ = true;
    }
}
