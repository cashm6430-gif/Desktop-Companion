#include "MotionLibrary.h"

#include <QDir>
#include <QFileInfo>

namespace {
const QString kSuffix = QStringLiteral(".motion.json");
}

bool MotionLibrary::loadClip(const QString& id, const QString& path, QString* error) {
    MotionClip clip;
    if (!clip.loadJson(path, error)) return false;
    clip.setId(id);
    clips_.insert(id, clip);
    return true;
}

bool MotionLibrary::loadDirectory(const QString& directory, QString* error) {
    const QDir dir(directory);
    if (!dir.exists()) {
        if (error) *error = QStringLiteral("Motion directory not found: %1").arg(directory);
        return false;
    }
    bool ok = true;
    QStringList failures;
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.motion.json")}, QDir::Files, QDir::Name)) {
        const QString name = info.fileName();
        const QString id = name.left(name.size() - kSuffix.size());
        QString clipError;
        if (!loadClip(id, info.absoluteFilePath(), &clipError)) {
            ok = false;
            failures.append(QStringLiteral("%1: %2").arg(name, clipError));
        }
    }
    if (!ok && error) *error = failures.join(QStringLiteral("; "));
    return ok;
}

QStringList MotionLibrary::ids() const {
    QStringList result = clips_.keys();
    result.sort();
    return result;
}

double MotionLibrary::duration(const QString& id) const {
    if (const MotionClip* authored = clip(id); authored && authored->duration() > 0.0)
        return authored->duration();
    if (id == QLatin1String("delete")) return kDeleteDuration;
    if (id == QLatin1String("grass")) return kGrassFallback;
    if (id == QLatin1String("busy-laptop")) return kLaptopFallback;
    return 0.0;
}

bool MotionLibrary::isLoop(const QString& id) const {
    if (const MotionClip* authored = clip(id)) return authored->isLoop();
    return id == QLatin1String("idle") || id == QLatin1String("busy") || id == QLatin1String("busy-laptop");
}

bool MotionLibrary::isProcedural(const QString& id) const {
    return !clips_.contains(id);
}
