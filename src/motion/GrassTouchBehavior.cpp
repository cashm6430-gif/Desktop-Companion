#include "GrassTouchBehavior.h"

#include "../MotionLibrary.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>
#include <algorithm>

bool GrassTouchBehavior::configure(const MotionLibrary& library, const QString& directory,
                                   QString* error)
{
    const MotionClip* clip = library.clip(QStringLiteral("grass-touch"));
    if (!clip) {
        reset();
        clip_ = MotionClip{};
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
    reset();
    clip_ = *clip;
    enterEnd_ = enterEnd;
    holdEnd_ = holdEnd;
    respondEnd_ = respondEnd;
    timeoutEnd_ = timeoutEnd;
    releaseEnd_ = releaseEnd;
    maxHold_ = maxHold;
    return true;
}

bool GrassTouchBehavior::canBegin() const
{
    return clip_.isValid() && !selected_;
}

void GrassTouchBehavior::begin()
{
    reset();
    selected_ = true;
    phase_ = Phase::Enter;
}

void GrassTouchBehavior::reset()
{
    phase_ = Phase::Inactive;
    selected_ = false;
    time_ = heldTime_ = look_ = lookTarget_ = 0.0;
}

bool GrassTouchBehavior::active() const
{
    return selected_ && phase_ != Phase::Inactive && phase_ != Phase::Finished;
}

QString GrassTouchBehavior::phaseName() const
{
    switch (phase_) {
    case Phase::Enter: return QStringLiteral("enter");
    case Phase::Hold: return QStringLiteral("hold");
    case Phase::Respond: return QStringLiteral("respond");
    case Phase::Timeout: return QStringLiteral("timeout");
    case Phase::Release: return QStringLiteral("release");
    default: return {};
    }
}

bool GrassTouchBehavior::respond()
{
    if (phase_ != Phase::Hold) return false;
    phase_ = Phase::Respond;
    time_ = holdEnd_;
    lookTarget_ = 0.0;
    return true;
}

void GrassTouchBehavior::lookAt(double horizontal)
{
    if (phase_ == Phase::Hold && qIsFinite(horizontal))
        lookTarget_ = std::clamp(horizontal, -1.0, 1.0);
}

double GrassTouchBehavior::maxDuration() const
{
    if (!clip_.isValid()) return 0.0;
    return enterEnd_ + maxHold_
        + std::max(respondEnd_ - holdEnd_, timeoutEnd_ - respondEnd_)
        + releaseEnd_ - timeoutEnd_;
}

void GrassTouchBehavior::advance(double seconds, bool& finished)
{
    if (!active()) return;
    if (phase_ != Phase::Hold) lookTarget_ = 0.0;
    look_ += (lookTarget_ - look_) * (1.0 - qExp(-seconds / 0.16));
    double remaining = seconds;
    while (remaining > 0.0 && active()) {
        if (phase_ == Phase::Hold) {
            const double consumed = std::min(remaining, maxHold_ - heldTime_);
            heldTime_ += consumed;
            remaining -= consumed;
            const double span = holdEnd_ - enterEnd_;
            time_ = enterEnd_ + std::fmod(heldTime_, span);
            if (heldTime_ >= maxHold_ - 1e-9) {
                phase_ = Phase::Timeout;
                time_ = respondEnd_;
                lookTarget_ = 0.0;
            }
            continue;
        }
        const double end = phase_ == Phase::Enter ? enterEnd_
            : phase_ == Phase::Respond ? respondEnd_
            : phase_ == Phase::Timeout ? timeoutEnd_ : releaseEnd_;
        const double consumed = std::min(remaining, end - time_);
        time_ += consumed;
        remaining -= consumed;
        if (time_ >= end - 1e-9) {
            if (phase_ == Phase::Enter) {
                phase_ = Phase::Hold;
                time_ = enterEnd_;
            } else if (phase_ == Phase::Respond || phase_ == Phase::Timeout) {
                phase_ = Phase::Release;
                time_ = timeoutEnd_;
            } else {
                phase_ = Phase::Finished;
                time_ = releaseEnd_;
                finished = true;
            }
        }
    }
}
