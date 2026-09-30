#include "MotionClip.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>
#include <algorithm>
#include <cmath>

double MotionClip::smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool MotionClip::validateKeys(const QVector<Key>& keys, QString* error) {
    if (keys.size() < 2 || keys.first().time != 0.0) {
        if (error) *error = QStringLiteral("Motion needs at least two keys starting at zero");
        return false;
    }
    for (int i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        if (!qIsFinite(key.time) || key.time < 0.0 || key.parameters.isEmpty()
            || (i > 0 && key.time <= keys[i - 1].time)) {
            if (error) *error = QStringLiteral("Keyframes need increasing times and identical parameter IDs");
            return false;
        }
        if (i == 0) continue;
        auto ids = key.parameters.keys();
        auto firstIds = keys.first().parameters.keys();
        ids.sort();
        firstIds.sort();
        if (ids != firstIds) {
            if (error) *error = QStringLiteral("Keyframes need increasing times and identical parameter IDs");
            return false;
        }
    }
    return true;
}

bool MotionClip::parseJson(const QByteArray& json, QString* error) {
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        if (error) *error = QStringLiteral("Motion file is not a JSON object");
        return false;
    }
    const auto root = document.object();

    const auto parseParameters = [error](const QJsonObject& values, Parameters& out) {
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!it.value().isDouble() || !qIsFinite(it.value().toDouble())) {
                if (error) *error = QStringLiteral("Invalid keyframe parameter");
                return false;
            }
            out.insert(it.key(), it.value().toDouble());
        }
        return true;
    };

    QVector<Key> keys;
    for (const auto& entry : root.value(QStringLiteral("keyframes")).toArray()) {
        const auto object = entry.toObject();
        const double time = object.value(QStringLiteral("time")).toDouble(-1.0);
        Parameters parameters;
        if (!parseParameters(object.value(QStringLiteral("parameters")).toObject(), parameters))
            return false;
        keys.append({time, parameters});
    }
    if (!keys.isEmpty() && !validateKeys(keys, error)) return false;

    QHash<QString, Track> tracks;
    const auto tracksObject = root.value(QStringLiteral("tracks")).toObject();
    for (auto it = tracksObject.begin(); it != tracksObject.end(); ++it) {
        Track track;
        for (const auto& sample : it.value().toArray()) {
            const auto pair = sample.toArray();
            if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()
                || !qIsFinite(pair[0].toDouble()) || !qIsFinite(pair[1].toDouble())
                || pair[0].toDouble() < 0.0
                || (!track.times.isEmpty() && pair[0].toDouble() <= track.times.last())) {
                if (error) *error = QStringLiteral("Tracks need increasing non-negative times");
                return false;
            }
            track.times.append(pair[0].toDouble());
            track.values.append(pair[1].toDouble());
        }
        if (track.times.size() < 2) {
            if (error) *error = QStringLiteral("A track needs at least two samples");
            return false;
        }
        tracks.insert(it.key(), track);
    }

    if (keys.isEmpty() && tracks.isEmpty()) {
        if (error) *error = QStringLiteral("Motion needs keyframes or tracks");
        return false;
    }

    double duration = root.value(QStringLiteral("duration")).toDouble(-1.0);
    if (!(duration > 0.0)) {
        duration = keys.isEmpty() ? tracks.begin()->times.last() : keys.last().time;
        for (auto it = tracks.begin(); it != tracks.end(); ++it)
            duration = std::max(duration, it.value().times.last());
    }
    if (!qIsFinite(duration) || duration <= 0.0) {
        if (error) *error = QStringLiteral("Motion needs a positive duration");
        return false;
    }

    const bool loop = root.value(QStringLiteral("loop")).toBool(false);
    double loopStart = root.value(QStringLiteral("loopStart")).toDouble(0.0);
    double loopEnd = root.value(QStringLiteral("loopEnd")).toDouble(duration);
    if (!qIsFinite(loopStart) || !qIsFinite(loopEnd) || loopStart < 0.0
        || loopEnd <= loopStart || loopEnd > duration + 1e-9) {
        if (error) *error = QStringLiteral("Loop range must sit inside the duration");
        return false;
    }

    keys_ = keys;
    tracks_ = tracks;
    duration_ = duration;
    loop_ = loop;
    loopStart_ = loopStart;
    loopEnd_ = loopEnd;
    blendSeconds_ = std::max(0.0, root.value(QStringLiteral("blendSeconds")).toDouble(0.12));
    interpolation_ = root.value(QStringLiteral("interpolation")).toString(QStringLiteral("smoothstep"));
    if (interpolation_ != QLatin1String("smoothstep") && interpolation_ != QLatin1String("linear")) {
        if (error) *error = QStringLiteral("Unsupported interpolation");
        return false;
    }
    approval_ = root.value(QStringLiteral("approval")).toString();
    modelVersion_ = root.value(QStringLiteral("modelVersion")).toString();
    revision_ = root.value(QStringLiteral("revision")).toInt(0);
    valid_ = true;
    return true;
}

bool MotionClip::loadJson(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    return parseJson(file.readAll(), error);
}

void MotionClip::setKeys(QVector<Key> keys, double duration) {
    keys_ = std::move(keys);
    duration_ = duration > 0.0 ? duration : (keys_.isEmpty() ? 0.0 : keys_.last().time);
    loopEnd_ = loopEnd_ > 0.0 ? loopEnd_ : duration_;
    valid_ = !keys_.isEmpty();
}

void MotionClip::setTrack(const QString& parameterId, const QVector<double>& times,
                          const QVector<double>& values) {
    if (times.size() != values.size() || times.size() < 2) return;
    tracks_.insert(parameterId, Track{times, values});
    duration_ = std::max(duration_, times.last());
    if (loopEnd_ <= 0.0) loopEnd_ = duration_;
}

void MotionClip::setLoop(bool loop, double loopStart, double loopEnd) {
    loop_ = loop;
    loopStart_ = loopStart;
    loopEnd_ = loopEnd > loopStart ? loopEnd : duration_;
}

double MotionClip::ease(double t) const {
    return interpolation_ == QLatin1String("linear") ? std::clamp(t, 0.0, 1.0) : smooth(t);
}

double MotionClip::sampleTrack(const Track& track, double seconds) const {
    if (track.times.isEmpty()) return 0.0;
    if (seconds <= track.times.first()) return track.values.first();
    if (seconds >= track.times.last()) return track.values.last();
    int next = 1;
    while (track.times[next] < seconds) ++next;
    const double weight = ease((seconds - track.times[next - 1]) / (track.times[next] - track.times[next - 1]));
    return track.values[next - 1] + (track.values[next] - track.values[next - 1]) * weight;
}

MotionClip::Parameters MotionClip::sample(double seconds) const {
    Parameters result;
    for (auto it = tracks_.cbegin(); it != tracks_.cend(); ++it)
        result.insert(it.key(), sampleTrack(it.value(), seconds));
    if (keys_.isEmpty()) return result;
    if (seconds <= keys_.first().time || keys_.size() < 2) {
        for (auto it = keys_.first().parameters.cbegin(); it != keys_.first().parameters.cend(); ++it)
            result.insert(it.key(), it.value());
        return result;
    }
    if (seconds >= keys_.last().time) {
        for (auto it = keys_.last().parameters.cbegin(); it != keys_.last().parameters.cend(); ++it)
            result.insert(it.key(), it.value());
        return result;
    }
    int next = 1;
    while (keys_[next].time < seconds) ++next;
    const auto& a = keys_[next - 1];
    const auto& b = keys_[next];
    const double weight = ease((seconds - a.time) / (b.time - a.time));
    for (auto it = a.parameters.cbegin(); it != a.parameters.cend(); ++it) {
        if (tracks_.contains(it.key())) continue; // Per-parameter track wins.
        result.insert(it.key(), it.value() + (b.parameters.value(it.key()) - it.value()) * weight);
    }
    return result;
}

MotionClip::Parameters MotionClip::sampleLooped(double seconds) const {
    if (!loop_ || loopEnd_ <= loopStart_) return sample(seconds);
    const double period = loopEnd_ - loopStart_;
    double phase = std::fmod(seconds - loopStart_, period);
    if (phase < 0.0) phase += period;
    return sample(loopStart_ + phase);
}
