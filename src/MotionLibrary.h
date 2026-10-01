#pragma once

#include "MotionClip.h"

#include <QHash>
#include <QString>
#include <QStringList>

// Owns every motion the pet can play and is the only place that knows how long
// an action lasts. Authored clips are loaded from assets/motions/*.motion.json
// (clip id = file name without the extension). Idle, busy and delete are still
// generated procedurally in C++; they are described here so the state machine
// never hard-codes a duration again.
class MotionLibrary final {
public:
    // Fallback only: the authored clip wins while assets/motions/delete.motion.json
    // exists. Keep it equal to that clip so both sources cannot drift apart.
    static constexpr double kDeleteDuration = 2.6;
    static constexpr double kGrassFallback = 6.9;
    static constexpr double kLaptopFallback = 8.0;

    bool loadDirectory(const QString& directory, QString* error = nullptr);
    bool loadClip(const QString& id, const QString& path, QString* error = nullptr);

    const MotionClip* clip(const QString& id) const {
        const auto it = clips_.constFind(id);
        return it == clips_.constEnd() ? nullptr : &it.value();
    }
    QStringList ids() const;
    bool contains(const QString& id) const { return clips_.contains(id); }

    // Duration of a named motion, preferring an authored clip over the fallback.
    double duration(const QString& id) const;
    bool isLoop(const QString& id) const;
    bool isProcedural(const QString& id) const;

private:
    QHash<QString, MotionClip> clips_;
};
