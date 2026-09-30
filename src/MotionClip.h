#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

// A motion clip is the single source of truth for one authored action:
// its duration, loop range, blend time, approval state, model revision and the
// parameter curves themselves. Two curve formats are supported and can coexist:
//
//   1. Shared keys ("keyframes"): every parameter is listed at every key time.
//      This is the legacy format already used by grass/busy-laptop assets.
//   2. Per-parameter tracks ("tracks"): each parameter carries its own times and
//      values, so slow channels (breath) and fast ones (a wink) can be authored
//      independently. When tracks are present they take precedence per parameter.
//
// Values are Cubism parameter units, not pixels or pre-rendered frames.
class MotionClip final {
public:
    using Parameters = QHash<QString, double>;

    struct Key {
        double time = 0.0;
        Parameters parameters;
    };

    struct Track {
        QVector<double> times;
        QVector<double> values;
    };

    bool loadJson(const QString& path, QString* error = nullptr);
    bool parseJson(const QByteArray& json, QString* error = nullptr);

    const QString& id() const { return id_; }
    void setId(const QString& id) { id_ = id; }

    bool isValid() const { return valid_; }
    double duration() const { return duration_; }
    bool isLoop() const { return loop_; }
    double loopStart() const { return loopStart_; }
    double loopEnd() const { return loopEnd_; }
    double blendSeconds() const { return blendSeconds_; }
    const QString& interpolation() const { return interpolation_; }
    const QString& approval() const { return approval_; }
    int revision() const { return revision_; }
    const QString& modelVersion() const { return modelVersion_; }

    const QVector<Key>& keys() const { return keys_; }
    bool hasTracks() const { return !tracks_.isEmpty(); }

    // Clamped sample: before the first key returns the first key, after the last
    // key returns the last key. This is what one-shot actions use.
    Parameters sample(double seconds) const;
    // Looped sample across [loopStart, loopEnd). Used by idle/busy cycles.
    Parameters sampleLooped(double seconds) const;

    void setKeys(QVector<Key> keys, double duration);
    void setTrack(const QString& parameterId, const QVector<double>& times,
                  const QVector<double>& values);
    void setLoop(bool loop, double loopStart, double loopEnd);

private:
    static double smooth(double t);
    double ease(double t) const;
    double sampleTrack(const Track& track, double seconds) const;
    static bool validateKeys(const QVector<Key>& keys, QString* error);

    QString id_;
    QVector<Key> keys_;
    QHash<QString, Track> tracks_;
    double duration_ = 0.0;
    double loopStart_ = 0.0;
    double loopEnd_ = 0.0;
    double blendSeconds_ = 0.12;
    QString interpolation_ = QStringLiteral("smoothstep");
    QString approval_;
    QString modelVersion_;
    int revision_ = 0;
    bool loop_ = false;
    bool valid_ = false;
};
