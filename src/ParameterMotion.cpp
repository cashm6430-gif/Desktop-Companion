#include "ParameterMotion.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <numbers>

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
const QString deskVisible = QStringLiteral("ParamDeskVisible");
constexpr double pi = 3.14159265358979323846;

const QStringList& deskPostureChannels() {
    static const QStringList ids{
        QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
        QStringLiteral("ParamLaptopVisible"), QStringLiteral("ParamBusyTypingL"),
        QStringLiteral("ParamBusyTypingR"), QStringLiteral("ParamLaptopRock"),
        leftArm, rightArm, QStringLiteral("ParamElbowLA"),
        QStringLiteral("ParamElbowRA"), QStringLiteral("ParamWristRA")
    };
    return ids;
}

// What a short reaction may own. Two groups, and nothing else:
//
//  1. The face and head -- the expressive layer. Head rotation barely moves the
//     chibi silhouette (measured: ParamAngleX 30 moves the outline 0.3px at
//     280px), so most of the "head" performance is really carried here by the
//     eyelids, the eye smile and the gaze.
//  2. The silhouette levers a gesture needs to read at desktop size. Measured at
//     280px: ParamShiftX 60 moves 15.5px, ParamBodyAngleX 12 moves 4.8px,
//     ParamAngleZ 18 moves 2.8px, ParamBodyAngleZ 8 moves 2.4px, ParamHairBack 1
//     moves 1.6px. These are the only axes that relocate the outline, which is
//     what a viewer reads as "the pet did something".
//
// Deliberately NOT owned (see reactionReservedParameters): the sitting/standing
// posture, the laptop and grass prop channels, the arms (a raised arm moves the
// outline 0.2px, so it is only meaningful while holding a prop, which belongs to
// the activity), and vertical shift (the grounding contract cancels it).
const QSet<QString>& reactionParameters() {
    static const QSet<QString> ids{
        angleX, angleY, angleZ, leftEye, rightEye, cheek, mouth,
        QStringLiteral("ParamEyeSmile"), QStringLiteral("ParamEyeBallX"),
        QStringLiteral("ParamEyeBallY"), QStringLiteral("ParamMouthForm"),
        QStringLiteral("ParamSmileOpen"), QStringLiteral("ParamMouthGape"),
        QStringLiteral("ParamBrowLY"), QStringLiteral("ParamBrowRY"),
        bodyX, bodyZ, QStringLiteral("ParamHairFront"), QStringLiteral("ParamHairBack"),
        QStringLiteral("ParamShiftX")
    };
    return ids;
}

// Channels that belong to the activity underneath, listed explicitly so the
// refusal explains itself instead of reading as a typo. Releasing these would let
// a two-second reaction strand the pet mid-crouch, drop a held prop, or fight the
// ground line.
const QHash<QString, QString>& reactionReservedParameters() {
    static const QHash<QString, QString> reserved{
        {QStringLiteral("ParamBusyLaptop"), QStringLiteral("seated/standing posture is the activity's")},
        {deskVisible, QStringLiteral("the work desk belongs to the activity")},
        {QStringLiteral("ParamSitPose"), QStringLiteral("seated/standing posture is the activity's")},
        {QStringLiteral("ParamLaptopVisible"), QStringLiteral("the laptop is a held prop")},
        {QStringLiteral("ParamLaptopRock"), QStringLiteral("the laptop is a held prop")},
        {QStringLiteral("ParamBusyTypingL"), QStringLiteral("the activity animates the hands")},
        {QStringLiteral("ParamBusyTypingR"), QStringLiteral("the activity animates the hands")},
        {QStringLiteral("ParamGrassVisible"), QStringLiteral("the grass is a held prop")},
        {QStringLiteral("ParamGrassReach"), QStringLiteral("the grass is a held prop")},
        {QStringLiteral("ParamGrassSwing"), QStringLiteral("the grass is a held prop")},
        {QStringLiteral("ParamGrassTipBend"), QStringLiteral("the grass is a held prop")},
        {QStringLiteral("ParamHandRGrip"), QStringLiteral("the grass grip is a held prop")},
        {QStringLiteral("ParamShiftY"), QStringLiteral("the grounding contract cancels vertical shift")},
        {QStringLiteral("ParamBreath"), QStringLiteral("breathing belongs to the idle base")},
        {leftArm, QStringLiteral("arms read only while holding a prop, which the activity owns")},
        {rightArm, QStringLiteral("arms read only while holding a prop, which the activity owns")},
        {QStringLiteral("ParamElbowLA"), QStringLiteral("arms read only while holding a prop")},
        {QStringLiteral("ParamElbowRA"), QStringLiteral("arms read only while holding a prop")},
        {QStringLiteral("ParamWristRA"), QStringLiteral("arms read only while holding a prop")}
    };
    return reserved;
}

bool validateReaction(const MotionClip& clip, QString* error) {
    if (!clip.isValid()) return true;
    if (clip.isAdditive() || clip.isLoop()) {
        if (error) *error = QStringLiteral("Reaction %1 must be an absolute one-shot clip").arg(clip.id());
        return false;
    }
    const auto first = clip.sample(0.0);
    for (auto it = first.cbegin(); it != first.cend(); ++it) {
        if (reactionParameters().contains(it.key())) continue;
        const auto reserved = reactionReservedParameters().constFind(it.key());
        if (error) {
            *error = reserved == reactionReservedParameters().constEnd()
                ? QStringLiteral("Reaction %1 cannot own parameter %2; it is not an expressive or "
                                 "silhouette axis").arg(clip.id(), it.key())
                : QStringLiteral("Reaction %1 cannot own parameter %2: %3")
                      .arg(clip.id(), it.key(), reserved.value());
        }
        return false;
    }
    return true;
}

// The stretch pause ("歇一下") is an activity overlay, not a short reaction: it
// deliberately owns the seated/desk/typing channels (stopping typing IS the
// first key pose), so the reaction whitelist does not apply. Instead it must
// carry the full seated desk pose on every key, so an interrupted playback can
// never strand the pet between poses.
bool validateStretch(const MotionClip& clip, QString* error) {
    if (!clip.isValid()) return true;
    if (clip.isAdditive() || clip.isLoop()) {
        if (error) *error = QStringLiteral("Stretch %1 must be an absolute one-shot clip").arg(clip.id());
        return false;
    }
    for (const auto& key : clip.keys()) {
        if (key.parameters.value(QStringLiteral("ParamBusyLaptop")) != 1.0
            || key.parameters.value(QStringLiteral("ParamSitPose")) != 1.0) {
            if (error) *error = QStringLiteral("Stretch %1 requires a full seated desk pose on every key")
                                    .arg(clip.id());
            return false;
        }
    }
    return true;
}

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

ParameterMotion::ParameterMotion() {
    // The seated switch is deliberately slower than the general 120 ms blend.
    // Both numbers used to be hard-coded in advance(); seeding them here keeps
    // the behaviour identical for clips that do not declare their own.
    blendSeconds_.insert(QStringLiteral("ParamBusyLaptop"), 0.28);
    blendSeconds_.insert(QStringLiteral("ParamSitPose"), 0.28);
}

void ParameterMotion::applyBlendOverrides(const MotionClip& clip) {
    for (auto it = clip.blendSecondsPerParameter().cbegin();
         it != clip.blendSecondsPerParameter().cend(); ++it) {
        const auto existing = blendSeconds_.constFind(it.key());
        if (existing != blendSeconds_.constEnd() && existing.value() != it.value()) {
            qWarning() << "Blend time for" << it.key() << "in clip" << clip.id()
                       << "redeclares" << existing.value() << "as" << it.value();
        }
        blendSeconds_.insert(it.key(), it.value());
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

double ParameterMotion::thoughtBubblePulse(double seconds) {
    // The bubble is the "still chewing on it" marker, so it has to agree with
    // the pose it sits next to. The standing accent is a twelve second loop and
    // the bubble rides the same clock: a four second envelope would drift out of
    // phase against it, jump at the loop seam, and read as a second animation
    // rather than as a reaction. Two windows per cycle, one for each stretch the
    // pet is stuck on, with the "?" out of frame for the beat it has just
    // landed on an answer.
    constexpr double period = 12.0;
    constexpr double rise = 0.26;
    constexpr double settle = 0.22;
    constexpr double fall = 0.5;
    constexpr double overshoot = 1.16;
    // Pop in with a small overshoot, settle, hold, fade out. `open` is the
    // frame the pet changes its pose, so the pose reads first and the "?" reads
    // as a reaction to it; `close` is lifted at the eureka beat.
    const auto envelope = [](double phase, double open, double close) {
        if (phase < open || phase > close + fall) return 0.0;
        if (phase < open + rise) return overshoot * smooth((phase - open) / rise);
        if (phase < open + rise + settle)
            return overshoot + (1.0 - overshoot) * smooth((phase - open - rise) / settle);
        if (phase <= close) return 1.0;
        return 1.0 - smooth((phase - close) / fall);
    };
    const double phase = std::fmod(seconds, period) + (seconds < 0.0 ? period : 0.0);
    return std::max(envelope(phase, 0.45, 3.7), envelope(phase, 7.0, 9.9));
}

void ParameterMotion::setState(PetController::State state) {
    preview_ = false;
    sequencePhysics_ = false;
    if ((interaction_ == Interaction::TurnEnded && state != PetController::State::Idle)
        || (interaction_ == Interaction::Stretch && state != PetController::State::Busy)
        || (interaction_ == Interaction::HeadPat
            && (state == PetController::State::Delete || state == PetController::State::Grass)))
        cancelInteraction();
    // A delete (or grass hand-off) interrupting mid-drag wins over the settle
    // nod; the drag overlay clears instantly instead of decaying over it.
    if (dragMotionActive() && (state == PetController::State::Delete
                               || state == PetController::State::Grass))
        cancelDragMotion();
    // Same for the memo gaze: a delete lunge owns the face while it plays.
    if (memoMotionActive() && (state == PetController::State::Delete
                               || state == PetController::State::Grass))
        cancelMemoMotion();
    if (state_ == state && state != PetController::State::Delete
        && state != PetController::State::Grass) return;
    resetGrassInteraction();
    state_ = state;
    if (state != PetController::State::Idle) interactionSeat_.clear();
    actionTime_ = 0.0;
    finishedPending_ = false;
    actionFinishedReported_ = false;
    if (state == PetController::State::Idle) {
        if (interaction_ == Interaction::HeadPat) captureInteractionSeat();
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
    applyBlendOverrides(clip);
    library_.loadClip(QStringLiteral("busy-laptop"), path, nullptr);
    return true;
}

bool ParameterMotion::loadMotionLibrary(const QString& directory, QString* error) {
    if (!library_.loadDirectory(directory, error)) return false;
    for (const QString& id : library_.ids()) applyBlendOverrides(*library_.clip(id));
    if (const MotionClip* grass = library_.clip(QStringLiteral("grass"))) grassClip_ = *grass;
    if (const MotionClip* idle = library_.clip(QStringLiteral("idle"))) idleClip_ = *idle;
    if (const MotionClip* standing = library_.clip(QStringLiteral("busy-stand"))) {
        busyStandClip_ = *standing;
        // A loop by contract: advance() samples the accent with sampleLooped on
        // the busy clock, so the authored seam is the wrap point whether or not
        // the file remembers to declare one. Without this a missing `loop` flag
        // would silently degrade back to the clamping behaviour above.
        busyStandClip_.setLoop(true, busyStandClip_.loopStart(), busyStandClip_.duration());
    }
    if (const MotionClip* remove = library_.clip(QStringLiteral("delete"))) deleteClip_ = *remove;
    if (const MotionClip* laptop = library_.clip(QStringLiteral("busy-laptop"))) {
        if (!validateSeated(*laptop, error)) return false;
        laptopClip_ = *laptop;
        laptopClip_.setLoop(true, laptopClip_.loopStart(), laptopClip_.duration());
    }
    interactionDirectory_ = directory;
    if (!configureInteractions(error)) return false;
    return configureGrassInteraction(directory, error);
}

double ParameterMotion::clipEventTime(const QString& clipId, const QString& event,
                                      double fallback) const {
    const MotionClip* clip = library_.clip(clipId);
    return clip == nullptr ? fallback : clip->eventTime(event, fallback);
}

bool ParameterMotion::configureInteractions(QString* error) {
    const MotionClip* turnEnded = library_.clip(QStringLiteral("turn-ended"));
    const MotionClip* headPat = library_.clip(QStringLiteral("head-pat"));
    const MotionClip* stretch = library_.clip(QStringLiteral("busy-stretch"));
    if (turnEnded && !validateReaction(*turnEnded, error)) return false;
    if (headPat && !validateReaction(*headPat, error)) return false;
    if (stretch && !validateStretch(*stretch, error)) return false;
    double enterEnd = 0.8;
    double holdEnd = 1.6;
    if (headPat) {
        QFile file(QDir(interactionDirectory_).filePath(QStringLiteral("head-pat.motion.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Cannot read head-pat phase configuration");
            return false;
        }
        const auto phase = QJsonDocument::fromJson(file.readAll()).object()
                               .value(QStringLiteral("interaction")).toObject();
        enterEnd = phase.value(QStringLiteral("enterEnd")).toDouble(-1.0);
        holdEnd = phase.value(QStringLiteral("holdEnd")).toDouble(-1.0);
        if (!qIsFinite(enterEnd) || !qIsFinite(holdEnd) || enterEnd <= 0.0
            || holdEnd <= enterEnd || holdEnd >= headPat->duration()) {
            if (error) *error = QStringLiteral("head-pat requires 0 < enterEnd < holdEnd < duration");
            return false;
        }
    }
    cancelInteraction();
    turnEndedClip_ = turnEnded ? *turnEnded : MotionClip{};
    headPatClip_ = headPat ? *headPat : MotionClip{};
    stretchClip_ = stretch ? *stretch : MotionClip{};
    headPatEnterEnd_ = enterEnd;
    headPatHoldEnd_ = holdEnd;
    return true;
}

bool ParameterMotion::interactionActive() const {
    return interaction_ != Interaction::None;
}

QString ParameterMotion::interactionId() const {
    switch (interaction_) {
    case Interaction::TurnEnded: return QStringLiteral("turn-ended");
    case Interaction::HeadPat: return QStringLiteral("head-pat");
    case Interaction::Stretch: return QStringLiteral("busy-stretch");
    default: return {};
    }
}

void ParameterMotion::cancelInteraction() {
    if (interaction_ == Interaction::HeadPat && !headPatReleasing_)
        headPatCooldown_ = 8.0;
    if (state_ == PetController::State::Idle && !interactionSeat_.isEmpty())
        actionTime_ = 0.0;
    interaction_ = Interaction::None;
    interactionTime_ = interactionElapsed_ = 0.0;
    headPatHeld_ = headPatReleasing_ = false;
    releasingReaction_ = false;
    interactionSeat_.clear();
    // Keep the current values: the background recovers through the same
    // expression-before-blink filter as a normal state transition.
}

bool ParameterMotion::playTurnEnded() {
    if (state_ != PetController::State::Idle || preview_ || interactionActive()
        || !turnEndedClip_.isValid()) return false;
    interaction_ = Interaction::TurnEnded;
    interactionTime_ = interactionElapsed_ = 0.0;
    // setState(Idle) retains the previous pose. Keep a settled seated computer
    // while looking up; the usual put-away starts after the reaction.
    captureInteractionSeat();
    return true;
}

bool ParameterMotion::playStretch() {
    if (preview_ || interactionActive() || !stretchClip_.isValid()) return false;
    // The stretch clip keys the full seated desk pose (BusyLaptop/SitPose/
    // DeskVisible/LaptopVisible), so it is only playable over the seated
    // laptop busy variant; a standing busy or another state would jump.
    if (state_ != PetController::State::Busy || !laptopBusy_
        || values_.value(QStringLiteral("ParamBusyLaptop")) < 0.9) return false;
    interaction_ = Interaction::Stretch;
    interactionTime_ = interactionElapsed_ = 0.0;
    return true;
}

void ParameterMotion::captureInteractionSeat() {
    if (values_.value(QStringLiteral("ParamBusyLaptop")) >= 0.9
        || (deskWorkMode_ && deskVisible_ > 0.0)) {
        for (const QString& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
             QStringLiteral("ParamLaptopVisible"), leftArm, rightArm,
             QStringLiteral("ParamElbowLA"), QStringLiteral("ParamElbowRA"),
             QStringLiteral("ParamWristRA"), QStringLiteral("ParamLaptopRock")})
            interactionSeat_.insert(id, values_.value(id));
        if (deskWorkMode_)
            interactionSeat_.insert(deskVisible, deskVisible_);
    }
}

bool ParameterMotion::canBeginHeadPat() const {
    return !preview_ && headPatClip_.isValid() && headPatCooldown_ <= 0.0
        && state_ != PetController::State::Delete && state_ != PetController::State::Grass
        && interaction_ != Interaction::HeadPat;
}

bool ParameterMotion::beginHeadPat(double direction) {
    if (!canBeginHeadPat()) return false;
    cancelInteraction();
    interaction_ = Interaction::HeadPat;
    headPatHeld_ = true;
    headPatBeganBusy_ = state_ == PetController::State::Busy;
    headPatDirection_ = 0.0;
    if (state_ == PetController::State::Idle) captureInteractionSeat();
    updateHeadPat(direction);
    return true;
}

void ParameterMotion::updateHeadPat(double direction) {
    if (interaction_ == Interaction::HeadPat && qIsFinite(direction))
        headPatDirectionTarget_ = std::clamp(direction, -1.0, 1.0);
}

void ParameterMotion::endHeadPat() {
    if (interaction_ != Interaction::HeadPat || headPatReleasing_) return;
    // A light tap released during anticipation should not jump forward to a
    // closed-eye hold pose solely to open the eyes again during release.
    if (interactionTime_ < headPatEnterEnd_) {
        cancelInteraction();
        return;
    }
    headPatHeld_ = false;
    headPatReleasing_ = true;
    interactionTime_ = headPatHoldEnd_;
    headPatCooldown_ = 8.0;
}

void ParameterMotion::advanceInteraction(double seconds) {
    headPatCooldown_ = std::max(0.0, headPatCooldown_ - seconds);
    if (!interactionActive()) return;
    interactionElapsed_ += seconds;
    if (interaction_ == Interaction::TurnEnded) {
        interactionTime_ += seconds;
        if (interactionTime_ >= turnEndedClip_.duration()) {
            // Looking up is not part of the computer put-away interval.
            actionTime_ = 0.0;
            cancelInteraction();
        }
        return;
    }
    if (interaction_ == Interaction::Stretch) {
        interactionTime_ += seconds;
        if (interactionTime_ >= stretchClip_.duration()) {
            // Resume typing from the busy loop's start; the frozen busy clock
            // (see the Busy branch) restarts here, so no choice timer fired
            // and the laptop variant survives the pause untouched.
            busyTime_ = 0.0;
            cancelInteraction();
        }
        return;
    }
    headPatDirection_ += (headPatDirectionTarget_ - headPatDirection_)
        * (1.0 - qExp(-seconds / 0.14));
    const double heldLimit = headPatBeganBusy_ || state_ == PetController::State::Busy
        ? headPatEnterEnd_ + 0.5 : 8.0;
    if (headPatHeld_ && interactionElapsed_ >= heldLimit) endHeadPat();
    if (headPatReleasing_) {
        interactionTime_ += seconds;
        if (interactionTime_ >= headPatClip_.duration()) cancelInteraction();
    } else if (interactionTime_ < headPatEnterEnd_) {
        interactionTime_ = std::min(headPatEnterEnd_, interactionTime_ + seconds);
    } else {
        const double span = headPatHoldEnd_ - headPatEnterEnd_;
        interactionTime_ = headPatEnterEnd_
            + std::fmod(interactionTime_ - headPatEnterEnd_ + seconds, span);
    }
}

void ParameterMotion::applyInteraction(Parameters& desired) const {
    if (!interactionActive()) return;
    const MotionClip& clip = interaction_ == Interaction::TurnEnded ? turnEndedClip_
        : interaction_ == Interaction::Stretch ? stretchClip_ : headPatClip_;
    const auto pose = clip.sample(interactionTime_);
    for (auto it = pose.cbegin(); it != pose.cend(); ++it) desired[it.key()] = it.value();
    if (state_ == PetController::State::Idle && !interactionSeat_.isEmpty()) {
        for (auto it = interactionSeat_.cbegin(); it != interactionSeat_.cend(); ++it)
            desired[it.key()] = it.value();
        desired[QStringLiteral("ParamBusyTypingL")] = 0.0;
        desired[QStringLiteral("ParamBusyTypingR")] = 0.0;
    }
    if (interaction_ == Interaction::HeadPat && pose.contains(angleZ)) {
        // Mirror only the authored head/gaze channels. Adding a small negative
        // delta to a right-leaning pose never actually leaned toward the left.
        desired[angleZ] = pose.value(angleZ) * headPatDirection_;
        if (pose.contains(angleX)) desired[angleX] = pose.value(angleX) * headPatDirection_;
        const QString eyeBallX = QStringLiteral("ParamEyeBallX");
        if (pose.contains(eyeBallX)) desired[eyeBallX] = pose.value(eyeBallX) * headPatDirection_;
    }
}

bool ParameterMotion::configureGrassInteraction(const QString& directory, QString* error) {
    const MotionClip* clip = library_.clip(QStringLiteral("grass-touch"));
    if (!clip) {
        resetGrassInteraction();
        grassTouchClip_ = MotionClip{};
        return true;
    }
    QFile file(QDir(directory).filePath(QStringLiteral("grass-touch.motion.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read grass-touch phase configuration");
        return false;
    }
    const auto phase = QJsonDocument::fromJson(file.readAll()).object()
                           .value(QStringLiteral("interaction")).toObject();
    const double enterEnd = phase.value(QStringLiteral("enterEnd")).toDouble(-1);
    const double holdEnd = phase.value(QStringLiteral("holdEnd")).toDouble(-1);
    const double respondEnd = phase.value(QStringLiteral("respondEnd")).toDouble(-1);
    const double timeoutEnd = phase.value(QStringLiteral("timeoutEnd")).toDouble(-1);
    const double releaseEnd = phase.value(QStringLiteral("releaseEnd")).toDouble(clip->duration());
    const double maxHold = phase.value(QStringLiteral("maxHold")).toDouble(-1);
    if (!qIsFinite(enterEnd) || !qIsFinite(holdEnd) || !qIsFinite(respondEnd)
        || !qIsFinite(timeoutEnd) || !qIsFinite(releaseEnd) || !qIsFinite(maxHold)
        || enterEnd <= 0 || holdEnd <= enterEnd || respondEnd <= holdEnd
        || timeoutEnd <= respondEnd || releaseEnd <= timeoutEnd
        || qAbs(releaseEnd - clip->duration()) > 1e-6 || maxHold <= 0 || maxHold > 3.0
        || clip->isLoop() || clip->isAdditive()) {
        if (error) *error = QStringLiteral("grass-touch requires ordered enter/hold/respond/timeout/release boundaries and 0 < maxHold <= 3");
        return false;
    }
    const auto loopStart = clip->sample(enterEnd);
    const auto loopEnd = clip->sample(holdEnd);
    for (auto it = loopStart.cbegin(); it != loopStart.cend(); ++it) {
        if (!loopEnd.contains(it.key()) || qAbs(it.value() - loopEnd.value(it.key())) > 1e-6) {
            if (error) *error = QStringLiteral("grass-touch Hold must close its parameter seam: %1").arg(it.key());
            return false;
        }
    }
    // During the invitation the palm remains gripping the grass root. Stem
    // and tip flexibility are still driven by the existing soft spring.
    const auto heldRoot = [](const MotionClip::Parameters& pose) {
        return qAbs(pose.value(QStringLiteral("ParamHandRGrip")) - 1.0) < 1e-6
            && qAbs(pose.value(QStringLiteral("ParamGrassVisible")) - 1.0) < 1e-6;
    };
    if (!heldRoot(loopStart) || !heldRoot(loopEnd)) {
        if (error) *error = QStringLiteral("grass-touch Hold requires a visible grass root gripped in the palm");
        return false;
    }
    for (const auto& key : clip->keys()) {
        if (key.time >= enterEnd && key.time <= holdEnd && !heldRoot(clip->sample(key.time))) {
            if (error) *error = QStringLiteral("grass-touch Hold cannot release the palm grip");
            return false;
        }
    }
    resetGrassInteraction();
    grassTouchClip_ = *clip;
    grassEnterEnd_ = enterEnd;
    grassHoldEnd_ = holdEnd;
    grassRespondEnd_ = respondEnd;
    grassTimeoutEnd_ = timeoutEnd;
    grassReleaseEnd_ = releaseEnd;
    grassMaxHold_ = maxHold;
    return true;
}

void ParameterMotion::resetGrassInteraction() {
    grassPhase_ = GrassPhase::Inactive;
    grassInteractionSelected_ = false;
    grassInteractionTime_ = grassHeldTime_ = grassLook_ = grassLookTarget_ = 0.0;
}

bool ParameterMotion::beginGrassInteraction() {
    if (preview_ || state_ != PetController::State::Grass || !grassTouchClip_.isValid()
        || grassInteractionSelected_) return false;
    resetGrassInteraction();
    grassInteractionSelected_ = true;
    grassPhase_ = GrassPhase::Enter;
    actionTime_ = 0.0;
    finishedPending_ = actionFinishedReported_ = false;
    return true;
}

bool ParameterMotion::grassInteractionActive() const {
    return grassInteractionSelected_ && grassPhase_ != GrassPhase::Inactive && grassPhase_ != GrassPhase::Finished;
}

QString ParameterMotion::grassInteractionPhase() const {
    switch (grassPhase_) {
    case GrassPhase::Enter: return QStringLiteral("enter");
    case GrassPhase::Hold: return QStringLiteral("hold");
    case GrassPhase::Respond: return QStringLiteral("respond");
    case GrassPhase::Timeout: return QStringLiteral("timeout");
    case GrassPhase::Release: return QStringLiteral("release");
    default: return {};
    }
}

bool ParameterMotion::respondToGrass() {
    if (state_ != PetController::State::Grass || grassPhase_ != GrassPhase::Hold) return false;
    grassPhase_ = GrassPhase::Respond;
    grassInteractionTime_ = grassHoldEnd_;
    grassLookTarget_ = 0.0;
    return true;
}

void ParameterMotion::lookAtGrassTip(double horizontal) {
    if (grassPhase_ == GrassPhase::Hold && qIsFinite(horizontal))
        grassLookTarget_ = std::clamp(horizontal, -1.0, 1.0);
}

double ParameterMotion::grassInteractionMaxDuration() const {
    if (!grassTouchClip_.isValid()) return 0.0;
    return grassEnterEnd_ + grassMaxHold_
        + std::max(grassRespondEnd_ - grassHoldEnd_, grassTimeoutEnd_ - grassRespondEnd_)
        + grassReleaseEnd_ - grassTimeoutEnd_;
}

// --- Scene-move drag reaction (motion card 4) -------------------------------

void ParameterMotion::beginDragMotion() {
    dragPhase_ = DragPhase::Follow;
    dragLookX_ = dragLookY_ = dragLookTX_ = dragLookTY_ = 0.0;
    dragHairX_ = dragHairTarget_ = dragBodyX_ = dragBodyTarget_ = 0.0;
    dragSpeed_ = dragDistance_ = 0.0;
    dragCuriousDone_ = false;
    dragCuriousTime_ = dragNodTime_ = dragSettleTime_ = 0.0;
}

void ParameterMotion::updateDragMotion(double vx, double vy) {
    if (dragPhase_ != DragPhase::Follow || !qIsFinite(vx) || !qIsFinite(vy)) return;
    dragLookTX_ = std::clamp(vx / 600.0, -1.0, 1.0);
    dragLookTY_ = std::clamp(vy / 900.0, -0.6, 0.6);
    // Hair streams opposite to the motion: it lags behind the body.
    dragHairTarget_ = std::clamp(-vx / 500.0, -1.0, 1.0);
    dragBodyTarget_ = std::clamp(-vx / 110.0, -10.0, 10.0);
    dragSpeed_ = std::sqrt(vx * vx + vy * vy);
}

void ParameterMotion::endDragMotion() {
    if (dragPhase_ != DragPhase::Follow) return;
    dragPhase_ = DragPhase::Settle;
    dragLookTX_ = dragLookTY_ = dragHairTarget_ = dragBodyTarget_ = 0.0;
    dragSpeed_ = 0.0;
    // Release: residual sway decays while the eyes come back to the user,
    // plus one light nod on arrival.
    dragCuriousTime_ = 0.5;
    dragNodTime_ = 0.7;
    dragSettleTime_ = 2.5;
}

void ParameterMotion::cancelDragMotion() {
    dragPhase_ = DragPhase::None;
    dragLookX_ = dragLookY_ = dragLookTX_ = dragLookTY_ = 0.0;
    dragHairX_ = dragHairTarget_ = dragBodyX_ = dragBodyTarget_ = 0.0;
    dragSpeed_ = dragDistance_ = 0.0;
    dragCuriousDone_ = false;
    dragCuriousTime_ = dragNodTime_ = dragSettleTime_ = 0.0;
}

void ParameterMotion::advanceDragMotion(double seconds) {
    if (dragPhase_ == DragPhase::None) return;
    if (dragPhase_ == DragPhase::Follow) {
        dragDistance_ += dragSpeed_ * seconds;
        // One curious look back at the user per long drag: eyes leave the drag
        // direction briefly, then return to following the motion.
        if (!dragCuriousDone_ && dragDistance_ > 400.0) {
            dragCuriousDone_ = true;
            dragCuriousTime_ = 0.6;
        }
        if (dragCuriousTime_ > 0.0) {
            dragCuriousTime_ -= seconds;
            dragLookTX_ = dragLookTY_ = 0.0;
        }
        dragLookX_ += (dragLookTX_ - dragLookX_) * (1.0 - qExp(-seconds / 0.12));
        dragLookY_ += (dragLookTY_ - dragLookY_) * (1.0 - qExp(-seconds / 0.14));
        // Wider time constants than the eyes: hair and body read as mass.
        dragHairX_ += (dragHairTarget_ - dragHairX_) * (1.0 - qExp(-seconds / 0.24));
        dragBodyX_ += (dragBodyTarget_ - dragBodyX_) * (1.0 - qExp(-seconds / 0.18));
    } else {
        // Settle: exponential decay only -- the card explicitly forbids a
        // persistent sine sway after the drag stops. The slow 0.45s constant
        // keeps the lean/hair visibly swinging for a second or so, and a hard
        // cap guarantees the phase always ends.
        if (dragSettleTime_ > 0.0) {
            dragSettleTime_ -= seconds;
            if (dragSettleTime_ <= 0.0) {
                cancelDragMotion();
                return;
            }
        }
        const double decay = qExp(-seconds / 0.45);
        dragLookX_ *= decay; dragLookY_ *= decay;
        dragHairX_ *= decay; dragBodyX_ *= decay;
        if (dragCuriousTime_ > 0.0) dragCuriousTime_ -= seconds;
        if (dragNodTime_ > 0.0) dragNodTime_ -= seconds;
        if (qAbs(dragLookX_) < 0.02 && qAbs(dragHairX_) < 0.02 && qAbs(dragBodyX_) < 0.05
            && dragNodTime_ <= 0.0 && dragCuriousTime_ <= 0.0)
            cancelDragMotion();
    }
}

void ParameterMotion::applyDragMotion(Parameters& desired) const {
    if (dragPhase_ == DragPhase::None) return;
    if (dragPhase_ == DragPhase::Follow) {
        if (dragCuriousTime_ > 0.0) {
            // Looking at the user instead of the drag direction, slightly pleased.
            desired[QStringLiteral("ParamEyeBallX")] = 0.0;
            desired[QStringLiteral("ParamEyeBallY")] = 0.1;
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
        } else {
            auto eyeball = desired.contains(QStringLiteral("ParamEyeBallX"))
                ? desired.value(QStringLiteral("ParamEyeBallX")) : 0.0;
            desired[QStringLiteral("ParamEyeBallX")] = std::clamp(eyeball + 0.6 * dragLookX_, -1.0, 1.0);
            auto eyeballY = desired.contains(QStringLiteral("ParamEyeBallY"))
                ? desired.value(QStringLiteral("ParamEyeBallY")) : 0.0;
            desired[QStringLiteral("ParamEyeBallY")] = std::clamp(eyeballY + 0.4 * dragLookY_, -1.0, 1.0);
        }
    } else if (dragCuriousTime_ > 0.0) {
        // Release look-back: keep the soft smile, but let the eyes glide home
        // through the decaying dragLookX_ -- snapping them to zero and back
        // read as a visible jump once the window expired.
        desired[QStringLiteral("ParamEyeSmile")] = std::max(
            desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
    }
    auto hairFront = desired.contains(QStringLiteral("ParamHairFront"))
        ? desired.value(QStringLiteral("ParamHairFront")) : 0.0;
    desired[QStringLiteral("ParamHairFront")] = std::clamp(hairFront + 0.65 * dragHairX_, -1.0, 1.0);
    auto hairBack = desired.contains(QStringLiteral("ParamHairBack"))
        ? desired.value(QStringLiteral("ParamHairBack")) : 0.0;
    desired[QStringLiteral("ParamHairBack")] = std::clamp(hairBack + 0.45 * dragHairX_, -1.0, 1.0);
    auto bodyX = desired.contains(QStringLiteral("ParamBodyAngleX"))
        ? desired.value(QStringLiteral("ParamBodyAngleX")) : 0.0;
    desired[QStringLiteral("ParamBodyAngleX")] = std::clamp(bodyX + dragBodyX_, -12.0, 12.0);
    if (dragNodTime_ > 0.0) {
        // One light nod on release: a single sine hump, down and back.
        const double p = 1.0 - dragNodTime_ / 0.7;
        auto angleY = desired.contains(QStringLiteral("ParamAngleY"))
            ? desired.value(QStringLiteral("ParamAngleY")) : 0.0;
        desired[QStringLiteral("ParamAngleY")] = angleY - 10.0 * std::sin(std::numbers::pi_v<double> * p);
    }
}


// --- Sticky-note gaze (motion card 5) ---------------------------------------

void ParameterMotion::beginMemoMotion() {
    memoPhase_ = MemoPhase::Desk;
    memoTime_ = 0.0;
    memoUserEnd_ = kMemoUserEnd;
    memoLookX_ = memoLookY_ = 0.0;
    memoNodTime_ = 0.0;
    memoLaugh_ = false;
}

void ParameterMotion::beginMemoCelebrate() {
    memoPhase_ = MemoPhase::User;
    memoTime_ = 0.0;
    memoUserEnd_ = 0.7;
    memoNodTime_ = 0.6;
    memoLaugh_ = true;
}

void ParameterMotion::cancelMemoMotion() {
    memoPhase_ = MemoPhase::None;
    memoTime_ = 0.0;
    memoUserEnd_ = kMemoUserEnd;
    memoLookX_ = memoLookY_ = 0.0;
    memoNodTime_ = 0.0;
    memoLaugh_ = false;
}

double ParameterMotion::memoBubbleRise() const {
    switch (memoPhase_) {
    case MemoPhase::Desk:
        return 0.0;
    case MemoPhase::Rise:
        return smooth(std::clamp((memoTime_ - kMemoDeskEnd) / (kMemoRiseEnd - kMemoDeskEnd), 0.0, 1.0));
    case MemoPhase::User:
        return 1.0;
    default:
        return 0.0;
    }
}

void ParameterMotion::advanceMemoMotion(double seconds) {
    if (memoPhase_ == MemoPhase::None) return;
    memoTime_ += seconds;
    if (memoNodTime_ > 0.0) memoNodTime_ -= seconds;
    double targetX = 0.0, targetY = 0.0;
    if (memoPhase_ == MemoPhase::Desk) {
        // First beat: glance down at the desk edge where the note appears.
        targetX = 0.15;
        targetY = -0.55;
        if (memoTime_ >= kMemoDeskEnd) memoPhase_ = MemoPhase::Rise;
    } else if (memoPhase_ == MemoPhase::Rise) {
        // The gaze rides the note as it floats up to half a head high.
        const double p = smooth(std::clamp(
            (memoTime_ - kMemoDeskEnd) / (kMemoRiseEnd - kMemoDeskEnd), 0.0, 1.0));
        targetY = -0.55 + 1.0 * p;
        targetX = 0.15 * (1.0 - p);
        if (memoTime_ >= kMemoRiseEnd) {
            memoPhase_ = MemoPhase::User;
            memoNodTime_ = 0.6;
        }
    }
    if (memoPhase_ == MemoPhase::User) {
        // Look back at the user, pleased to have been trusted with this.
        targetX = 0.0;
        targetY = 0.1;
        if (memoNodTime_ <= 0.0 && memoTime_ >= memoUserEnd_) {
            cancelMemoMotion();
            return;
        }
    }
    const double alpha = 1.0 - qExp(-seconds / 0.09);
    memoLookX_ += (targetX - memoLookX_) * alpha;
    memoLookY_ += (targetY - memoLookY_) * alpha;
}

void ParameterMotion::applyMemoMotion(Parameters& desired) const {
    if (memoPhase_ == MemoPhase::None) return;
    desired[QStringLiteral("ParamEyeBallX")] = std::clamp(
        desired.value(QStringLiteral("ParamEyeBallX")) + memoLookX_, -1.0, 1.0);
    desired[QStringLiteral("ParamEyeBallY")] = std::clamp(
        desired.value(QStringLiteral("ParamEyeBallY")) + memoLookY_, -1.0, 1.0);
    if (memoPhase_ == MemoPhase::User) {
        if (memoLaugh_) {
            // Completion beat only: delighted laugh -- smiling CLOSED eyes
            // and a round O-shaped mouth (the wide grin art read as creepy).
            // The gape is the approved neutral "ah" art; the canvas gate
            // holds MouthOpenY fully open while it is selected, so the O
            // reads clean. The nod rides on top.
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 1.0);
            desired[leftEye] = 0.0;
            desired[rightEye] = 0.0;
            desired[QStringLiteral("ParamMouthGape")] = 1.0;
        } else {
            // Creating a note: hand the face back with a soft smile, the
            // eyes staying open -- the laugh is the completion reward.
            desired[QStringLiteral("ParamEyeSmile")] = std::max(
                desired.value(QStringLiteral("ParamEyeSmile")), 0.35);
        }
    }
    if (memoNodTime_ > 0.0) {
        const double p = 1.0 - memoNodTime_ / 0.6;
        auto angleY = desired.contains(QStringLiteral("ParamAngleY"))
            ? desired.value(QStringLiteral("ParamAngleY")) : 0.0;
        desired[QStringLiteral("ParamAngleY")] = angleY - 6.0 * std::sin(std::numbers::pi_v<double> * p);
    }
}


void ParameterMotion::advanceGrassInteraction(double seconds) {
    if (!grassInteractionActive()) return;
    if (grassPhase_ != GrassPhase::Hold) grassLookTarget_ = 0.0;
    grassLook_ += (grassLookTarget_ - grassLook_) * (1.0 - qExp(-seconds / 0.16));
    double remaining = seconds;
    while (remaining > 0.0 && grassInteractionActive()) {
        if (grassPhase_ == GrassPhase::Hold) {
            const double consumed = std::min(remaining, grassMaxHold_ - grassHeldTime_);
            grassHeldTime_ += consumed;
            remaining -= consumed;
            const double span = grassHoldEnd_ - grassEnterEnd_;
            grassInteractionTime_ = grassEnterEnd_ + std::fmod(grassHeldTime_, span);
            if (grassHeldTime_ >= grassMaxHold_ - 1e-9) {
                grassPhase_ = GrassPhase::Timeout;
                grassInteractionTime_ = grassRespondEnd_;
                grassLookTarget_ = 0.0;
            }
            continue;
        }
        const double end = grassPhase_ == GrassPhase::Enter ? grassEnterEnd_
            : grassPhase_ == GrassPhase::Respond ? grassRespondEnd_
            : grassPhase_ == GrassPhase::Timeout ? grassTimeoutEnd_ : grassReleaseEnd_;
        const double consumed = std::min(remaining, end - grassInteractionTime_);
        grassInteractionTime_ += consumed;
        remaining -= consumed;
        if (grassInteractionTime_ >= end - 1e-9) {
            if (grassPhase_ == GrassPhase::Enter) {
                grassPhase_ = GrassPhase::Hold;
                grassInteractionTime_ = grassEnterEnd_;
            } else if (grassPhase_ == GrassPhase::Respond || grassPhase_ == GrassPhase::Timeout) {
                grassPhase_ = GrassPhase::Release;
                grassInteractionTime_ = grassTimeoutEnd_;
            } else {
                grassPhase_ = GrassPhase::Finished;
                grassInteractionTime_ = grassReleaseEnd_;
                finishedPending_ = true;
                actionFinishedReported_ = true;
            }
        }
    }
}

double ParameterMotion::actionDuration(PetController::State state) const {
    switch (state) {
    case PetController::State::Delete: return library_.duration(QStringLiteral("delete"));
    case PetController::State::Grass:
        if (grassInteractionSelected_) return grassInteractionMaxDuration();
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

void ParameterMotion::setDeskWorkMode(bool enabled) {
    if (deskWorkMode_ == enabled) return;
    deskWorkMode_ = enabled;
    deskPhase_ = DeskPhase::Hidden;
    deskVisible_ = 0.0;
    if (!enabled) {
        values_.remove(deskVisible);
        interactionSeat_.remove(deskVisible);
        return;
    }
    // Enabling on an already moving laptop pose must cover it immediately.
    // Normal configuration at standing still uses the gradual cover entry.
    if (values_.value(QStringLiteral("ParamBusyLaptop")) > 0.0
        || values_.value(QStringLiteral("ParamSitPose")) > 0.0) {
        deskVisible_ = 1.0;
        deskPhase_ = state_ == PetController::State::Busy && laptopBusy_
            ? DeskPhase::Work : DeskPhase::ExitWork;
    }
    values_[deskVisible] = deskVisible_;
}

void ParameterMotion::advanceDeskWork(double seconds, const Parameters& previous) {
    constexpr double coverSeconds = 0.18;
    constexpr double settled = 0.001;
    const bool interrupted = state_ == PetController::State::Delete
        || state_ == PetController::State::Grass;
    const bool keepReactionDesk = interactionActive() && deskVisible_ > 0.0;
    const bool workWanted = (state_ == PetController::State::Busy && laptopBusy_)
        || keepReactionDesk;
    const auto holdPosture = [&] {
        for (const QString& id : deskPostureChannels())
            values_[id] = previous.value(id);
    };
    const auto recover = [&](const QString& id, double tau) {
        const double value = previous.value(id) * qExp(-seconds / tau);
        values_[id] = std::abs(value) <= settled ? 0.0 : value;
    };

    if (workWanted) {
        if (deskPhase_ == DeskPhase::Hidden || deskPhase_ == DeskPhase::ExitCover)
            deskPhase_ = DeskPhase::EnterCover;
        else if (deskPhase_ == DeskPhase::ExitWork)
            deskPhase_ = DeskPhase::Work; // Cover is already opaque on reversal.
    } else if (deskPhase_ == DeskPhase::EnterCover) {
        deskPhase_ = DeskPhase::ExitCover; // A short task never starts its pose.
    } else if (deskPhase_ == DeskPhase::Work) {
        deskPhase_ = DeskPhase::ExitWork;
    }

    switch (deskPhase_) {
    case DeskPhase::Hidden:
        deskVisible_ = 0.0;
        break;
    case DeskPhase::EnterCover:
        holdPosture();
        deskVisible_ = std::min(1.0, deskVisible_ + seconds / coverSeconds);
        // Hold the posture on this complete-cover frame too; it starts moving
        // on the next frame, when the renderer has already seen opaque cover.
        if (deskVisible_ == 1.0) deskPhase_ = DeskPhase::Work;
        break;
    case DeskPhase::Work:
        deskVisible_ = 1.0;
        if (values_.value(QStringLiteral("ParamLaptopVisible")) < 0.99) {
            values_[QStringLiteral("ParamBusyTypingL")] = 0.0;
            values_[QStringLiteral("ParamBusyTypingR")] = 0.0;
        }
        break;
    case DeskPhase::ExitWork: {
        deskVisible_ = 1.0;
        values_[QStringLiteral("ParamBusyTypingL")] = 0.0;
        values_[QStringLiteral("ParamBusyTypingR")] = 0.0;
        recover(QStringLiteral("ParamLaptopVisible"), kDefaultBlend);
        recover(QStringLiteral("ParamLaptopRock"), kDefaultBlend);
        const bool computerAway = values_.value(QStringLiteral("ParamLaptopVisible")) <= 0.01;
        if (!interrupted && !computerAway) {
            for (const QString& id : {QStringLiteral("ParamBusyLaptop"),
                 QStringLiteral("ParamSitPose"), leftArm, rightArm,
                 QStringLiteral("ParamElbowLA"), QStringLiteral("ParamElbowRA"),
                 QStringLiteral("ParamWristRA")})
                values_[id] = previous.value(id);
            break;
        }
        for (const QString& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose")})
            recover(id, interrupted ? kDefaultBlend : blendSeconds_.value(id, 0.28));
        // A foreground action starts on its original clock and owns its hands
        // immediately. Recovery of the covered posture adds no action delay.
        if (!interrupted) {
            for (const QString& id : {leftArm, rightArm, QStringLiteral("ParamElbowLA"),
                 QStringLiteral("ParamElbowRA"), QStringLiteral("ParamWristRA")})
                recover(id, kDefaultBlend);
        }
        bool neutral = values_.value(QStringLiteral("ParamBusyLaptop")) == 0.0
            && values_.value(QStringLiteral("ParamSitPose")) == 0.0
            && values_.value(QStringLiteral("ParamLaptopVisible")) == 0.0
            && values_.value(QStringLiteral("ParamLaptopRock")) == 0.0;
        if (!interrupted) {
            for (const QString& id : {leftArm, rightArm, QStringLiteral("ParamElbowLA"),
                 QStringLiteral("ParamElbowRA"), QStringLiteral("ParamWristRA")})
                neutral = neutral && values_.value(id) == 0.0;
        }
        if (neutral) deskPhase_ = DeskPhase::ExitCover;
        break;
    }
    case DeskPhase::ExitCover:
        // Foreground hands continue their action. All laptop/posture channels
        // remain neutral while the last cover leaves.
        for (const QString& id : {QStringLiteral("ParamBusyLaptop"), QStringLiteral("ParamSitPose"),
             QStringLiteral("ParamLaptopVisible"), QStringLiteral("ParamBusyTypingL"),
             QStringLiteral("ParamBusyTypingR"), QStringLiteral("ParamLaptopRock")})
            values_[id] = 0.0;
        // Background/action hands have already reached their safe standing
        // context, or work never began. Let that owner blend continuously;
        // canceling a partial entry must not snap standing gesture arms to zero.
        deskVisible_ = std::max(0.0, deskVisible_ - seconds / coverSeconds);
        if (deskVisible_ == 0.0) deskPhase_ = DeskPhase::Hidden;
        break;
    }
    values_[deskVisible] = deskVisible_;
}

ParameterMotion::Parameters ParameterMotion::grassPose(double seconds) const {
    return grassClip_.sample(seconds);
}

void ParameterMotion::setPreviewPose(const Parameters& parameters) {
    cancelInteraction();
    resetGrassInteraction();
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
    const Parameters deskPrevious = deskWorkMode_ ? values_ : Parameters{};
    clock_ += seconds;
    if (interaction_ != Interaction::TurnEnded
        && !(interactionActive() && state_ == PetController::State::Idle && !interactionSeat_.isEmpty()))
        actionTime_ += seconds;
    blinkClock_ += seconds;
    advanceInteraction(seconds);
    advanceGrassInteraction(seconds);
    advanceDragMotion(seconds);
    advanceMemoMotion(seconds);
    // The reaction face releases at the authored pace once the pat is over; see
    // kExpressionReleaseBlend. Cleared below when the interaction ends.
    releasingReaction_ = headPatReleasing_;

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
        // Both shape the open mouth and are only visible while ParamSmileOpen
        // selects the deformable art. Written every frame for the same reason
        // as the brows: a clip value the skeleton omits would freeze on screen.
        {QStringLiteral("ParamMouthForm"), 0.0},
        // Written every frame even though only the thinking pose drives them:
        // a clip parameter that the base skeleton does not carry would keep
        // whatever the clip left behind once the pet leaves that state.
        {QStringLiteral("ParamBrowLY"), 0.0}, {QStringLiteral("ParamBrowRY"), 0.0},
        {QStringLiteral("ParamSmileOpen"), 0.0},
        // Neutral "ah" gape on its own parameter (the exported moc3 clamps
        // ParamSmileOpen at 1, so the third switch state never reached the
        // render). Carried here so a delete bite cannot leave the gape on
        // screen after the state ends.
        {QStringLiteral("ParamMouthGape"), 0.0},
        {QStringLiteral("ParamEyeSmile"), 0.0},
        {QStringLiteral("ParamEyeBallX"), 0.0},
        {QStringLiteral("ParamEyeBallY"), 0.0},
        {QStringLiteral("ParamGrassVisible"), 0.0},
        {QStringLiteral("ParamGrassReach"), 0.0},
        {QStringLiteral("ParamGrassSwing"), 0.0},
        {QStringLiteral("ParamGrassTipBend"), 0.0},
        {QStringLiteral("ParamHandRGrip"), 0.0},
        // HAIR FRONT / BACK: unused until now, but bound and a large share of
        // the silhouette, so the thinking pose drives them as secondary motion.
        // Carried here for the usual reason -- a clip value the skeleton omits
        // would stay on screen after the pet leaves the state.
        {QStringLiteral("ParamHairFront"), 0.0},
        {QStringLiteral("ParamHairBack"), 0.0},
        {QStringLiteral("ParamBusyLaptop"), 0.0},
        {QStringLiteral("ParamSitPose"), 0.0},
        {QStringLiteral("ParamLaptopVisible"), 0.0},
        {QStringLiteral("ParamBusyTypingL"), 0.0},
        {QStringLiteral("ParamBusyTypingR"), 0.0},
        {QStringLiteral("ParamLaptopRock"), 0.0},
        // Whole-rig translation on the outermost ShiftRig warp. Zero renders
        // inert (4 stray pixels of 705600 against the pre-warp export), so
        // carrying it here costs nothing and keeps clip leftovers from
        // sticking. ShiftY is cancelled by the ground contract in
        // CubismCanvas and exists only to match the declared model axes.
        {QStringLiteral("ParamShiftX"), 0.0},
        {QStringLiteral("ParamShiftY"), 0.0},
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
        // It is the standing variant's accent and nobody else's. The seated
        // branch below is an assignment, but only for the parameters its own
        // keyframes carry -- a seated laptop pose has no opinion about mouth
        // shape, cheek or hair, so anything the accent writes there would ride
        // along on the seated pose instead of being overwritten.
        if (!laptopBusy_) {
            if (busyStandClip_.isValid()) {
                // Looped on the busy clock, not sampled on the global one. A
                // track past its last key is clamped rather than wrapped, and the
                // global clock never resets, so sampling this loop on clock_
                // would freeze the whole routine at its seam pose once the pet
                // had been running for one duration -- minutes of a dead pose
                // after twelve seconds.
                const auto accent = busyStandClip_.sampleLooped(busyTime_);
                for (auto it = accent.cbegin(); it != accent.cend(); ++it)
                    desired[it.key()] += it.value();
            } else {
                desired[angleY] += -7.0 + 1.8 * qSin(clock_ * 4.3);
                desired[bodyX] += 2.0;
                desired[leftArm] = 9.0 + 5.0 * qSin(clock_ * 10.0);
                desired[rightArm] = -9.0 + 5.0 * qSin(clock_ * 10.0 + pi);
                desired[mouth] = 0.12;
            }
        }
        busyTime_ += seconds;
        // Choice times coincide with the authored loop seam.
        // A delete/grass interruption pauses this clock and retains the choice.
        if (busyTime_ >= nextBusyChoice_) {
            laptopBusy_ = laptopClip_.isValid() && busyRandom_.generateDouble() < 0.4;
            nextBusyChoice_ += 40.0;
        }
        if (laptopBusy_) {
            // A stretch pause freezes the laptop loop clock: the choice timer
            // must not flip the busy variant mid-stretch, and the loop resumes
            // from its typing start when advanceInteraction ends the stretch.
            if (interaction_ == Interaction::Stretch) busyTime_ -= seconds;
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
    } else if (state_ == PetController::State::Idle && !interactionActive()
               && actionTime_ < 0.36
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
        const auto pose = grassInteractionSelected_ ? grassTouchClip_.sample(grassInteractionTime_)
                                                    : grassClip_.sample(actionTime_);
        for (auto it = pose.begin(); it != pose.end(); ++it) desired[it.key()] = it.value();
        if (grassPhase_ == GrassPhase::Hold)
            desired[QStringLiteral("ParamEyeBallX")] = std::clamp(
                desired.value(QStringLiteral("ParamEyeBallX")) + 0.18 * grassLook_, -1.0, 1.0);
    }
    applyInteraction(desired);
    applyDragMotion(desired);
    applyMemoMotion(desired);

    // The thinking bubble belongs to the standing busy variant alone: the seated
    // variant already tells its story with the laptop, and a delete swing is
    // over before a bubble could read as a thought.
    const bool thinking = state_ == PetController::State::Busy && !laptopBusy_
        && busyStandClip_.isValid();
    const double bubble = thinking ? thoughtBubblePulse(busyTime_) : 0.0;
    // One short filter, so leaving the state retracts the bubble instead of
    // blinking it out, but short enough that the authored pop survives it.
    bubblePulse_ += (bubble - bubblePulse_) * (1.0 - qExp(-seconds / 0.07));

    // Short asymmetric blink. It stays procedural across action transitions.
    const double blinkPhase = std::fmod(blinkClock_, 4.3);
    const double eye = 1.0 - pulse(blinkPhase, 0.0, 0.075, 0.16);
    // Time-based exponential blending is stable at both 30 and 60 Hz.
    const double alpha = 1.0 - qExp(-seconds / kDefaultBlend);
    const Parameters releasingChannels = releasingReaction_ ? headPatClip_.sample(interactionTime_) : Parameters{};
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        double previous = values_.contains(it.key()) ? values_.value(it.key()) : it.value();
        if (it.key() == leftEye) previous = leftEyeExpression_;
        if (it.key() == rightEye) previous = rightEyeExpression_;
        const auto tau = blendSeconds_.constFind(it.key());
        double weight = tau == blendSeconds_.constEnd() ? alpha : 1.0 - qExp(-seconds / tau.value());
        if (releasingChannels.contains(it.key())) {
            // See kExpressionReleaseBlend: hand the reaction's own channels back
            // at the authored pace instead of dwelling in the middle of the
            // cross-fade. Only channels this clip actually declares receive the
            // faster filter; its allowed but unused body/shift/hair channels
            // still belong to the background and keep their existing filter.
            weight = 1.0 - qExp(-seconds / kExpressionReleaseBlend);
        }
        const double blended = previous + (it.value() - previous) * weight;
        if (it.key() == leftEye) leftEyeExpression_ = blended;
        if (it.key() == rightEye) rightEyeExpression_ = blended;
        values_[it.key()] = blended;
    }
    // Smooth the authored expression first, then apply the short blink. Feeding
    // blink values through the general 120ms filter prevents full closure.
    values_[leftEye] = leftEyeExpression_ * eye;
    values_[rightEye] = rightEyeExpression_ * eye;
    // Resolve final activity hands before remembering the grip for the grass
    // spring. Covered poses must not create an unrendered hand-speed impulse.
    if (deskWorkMode_) advanceDeskWork(seconds, deskPrevious);
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
    if (!actionFinishedReported_ && (state_ == PetController::State::Delete
        || (state_ == PetController::State::Grass && !grassInteractionSelected_))) {
        const double duration = actionDuration(state_);
        if (duration > 0.0 && actionTime_ >= duration) {
            finishedPending_ = true;
            actionFinishedReported_ = true;
        }
    }
}
