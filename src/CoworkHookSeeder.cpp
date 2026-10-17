#include "CoworkHookSeeder.h"

#include "StartupTrace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

namespace {

// The same three events tools/install-hooks.ps1 wires for Claude. The desktop app
// has no Interrupt hook; SessionEnd is what closes a turn when a conversation is
// abandoned, and Stop when it finishes normally.
const QStringList kEvents{
    QStringLiteral("UserPromptSubmit"), QStringLiteral("Stop"), QStringLiteral("SessionEnd")};

// A hook group belongs to the pet when any of its commands references the helper.
// Same marker install-hooks.ps1 uses, so the two never fight over an entry.
const QString kMarker = QStringLiteral("DesktopCompanionHook");

bool isPetGroup(const QJsonValue& group)
{
    const QJsonArray hooks = group.toObject().value(QStringLiteral("hooks")).toArray();
    for (const QJsonValue& hook : hooks) {
        if (hook.toObject().value(QStringLiteral("command")).toString().contains(kMarker))
            return true;
    }
    return false;
}

QJsonObject petGroup(const QString& helperPath)
{
    // Quoted absolute path, the form the desktop app's own config already accepts.
    const QJsonObject hook{{QStringLiteral("type"), QStringLiteral("command")},
                           {QStringLiteral("command"), QStringLiteral("\"") + helperPath + QStringLiteral("\"")},
                           {QStringLiteral("timeout"), 5}};
    return QJsonObject{{QStringLiteral("hooks"), QJsonArray{hook}}};
}

} // namespace

CoworkHookSeeder::CoworkHookSeeder(QObject* parent)
    : QObject(parent) {}

QString CoworkHookSeeder::sessionRoot()
{
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty()) return {};
    return local + QStringLiteral("/Claude-3p/local-agent-mode-sessions");
}

QString CoworkHookSeeder::defaultHelperPath()
{
    // Native separators: this string goes into another product's config and ends up
    // on a shell command line, so it should look like every other path there
    // (Qt's applicationDirPath uses forward slashes).
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/DesktopCompanionHook.exe"));
}

QString CoworkHookSeeder::mergeSettings(const QString& existing, const QString& helperPath,
                                        bool* changed, QString* error)
{
    if (changed) *changed = false;
    if (error) error->clear();

    QJsonObject root;
    if (!existing.trimmed().isEmpty()) {
        QJsonParseError parse{};
        const QJsonDocument document = QJsonDocument::fromJson(existing.toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            // A file we cannot read is a file we must not rewrite: the app owns it.
            if (error) *error = QStringLiteral("settings.json is not a JSON object: %1")
                                    .arg(parse.errorString());
            return {};
        }
        root = document.object();
    }

    if (root.contains(QStringLiteral("hooks")) && !root.value(QStringLiteral("hooks")).isObject()) {
        if (error) *error = QStringLiteral("settings.json has a non-object \"hooks\"");
        return {};
    }
    QJsonObject hooks = root.value(QStringLiteral("hooks")).toObject();

    for (const QString& event : kEvents) {
        if (hooks.contains(event) && !hooks.value(event).isArray()) {
            if (error) *error = QStringLiteral("hook event \"%1\" is not an array").arg(event);
            return {};
        }
        QJsonArray kept;
        for (const QJsonValue& group : hooks.value(event).toArray()) {
            if (!isPetGroup(group)) kept.append(group);
        }
        kept.append(petGroup(helperPath));
        hooks.insert(event, kept);
    }

    // Every other key, every other event and every other tool's groups survive.
    root.insert(QStringLiteral("hooks"), hooks);
    const QString merged = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (changed) *changed = (merged != existing);
    return merged;
}

CoworkSeedResult CoworkHookSeeder::seedOnce(const QString& helperPath, const QString& root)
{
    CoworkSeedResult result;
    if (root.isEmpty() || helperPath.isEmpty()) return result;
    // Seeding a path that does not exist would leave the client calling a missing
    // helper: worse than not seeding at all.
    if (!QFileInfo::exists(helperPath)) return result;

    const QDir rootDir(root);
    if (!rootDir.exists()) return result;  // not a Claude Desktop machine

    const auto dirs = [](const QString& path) {
        return QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    };
    for (const QFileInfo& account : dirs(root)) {
        for (const QFileInfo& bucket : dirs(account.absoluteFilePath())) {
            for (const QFileInfo& session : dirs(bucket.absoluteFilePath())) {
                // `.claude` is the marker for a session dir the app manages. Never
                // invent one: siblings like memory/ or spaces/ must be left alone.
                const QString claudeDir = session.absoluteFilePath() + QStringLiteral("/.claude");
                if (!QFileInfo(claudeDir).isDir()) continue;
                ++result.sessions;

                const QString settings = claudeDir + QStringLiteral("/settings.json");
                QString existing;
                if (QFileInfo::exists(settings)) {
                    QFile file(settings);
                    if (!file.open(QIODevice::ReadOnly)) {
                        ++result.failed;
                        startup::trace(QStringLiteral("cowork hooks: cannot read %1").arg(settings));
                        continue;
                    }
                    existing = QString::fromUtf8(file.readAll());
                }

                QString error;
                bool changed = false;
                const QString merged = mergeSettings(existing, helperPath, &changed, &error);
                if (merged.isEmpty() || !error.isEmpty()) {
                    ++result.failed;
                    startup::trace(QStringLiteral("cowork hooks: left %1 alone (%2)").arg(settings, error));
                    continue;
                }
                if (!changed) continue;

                // QString::toUtf8 never emits a BOM, which is the whole point: Codex
                // refuses a BOM'd config, so the BOM-less write is a contract here.
                // QSaveFile replaces atomically (temp beside the target, then move).
                QSaveFile out(settings);
                if (!out.open(QIODevice::WriteOnly)
                    || out.write(merged.toUtf8()) < 0 || !out.commit()) {
                    ++result.failed;
                    // An "access denied" here is almost always an integrity-level
                    // mismatch, not a bug: a config file created by an elevated
                    // (High) process is High-labelled, and a pet running at a lower
                    // level cannot write it -- measured 2026-10-17 (the pet was Low
                    // because of how it had been launched). Say so, rather than
                    // leaving a silent failure.
                    startup::trace(QStringLiteral("cowork hooks: write failed for %1 (%2)")
                        .arg(settings, out.errorString()));
                    continue;
                }
                ++result.written;
            }
        }
    }
    return result;
}

void CoworkHookSeeder::start(int intervalMs)
{
    bool firstSweep = true;
    const auto sweep = [this, &firstSweep] {
        lastHelperPath_ = defaultHelperPath();
        lastResult_ = seedOnce(lastHelperPath_);
        // Quiet by design: only the first sweep and anything that actually moved
        // leave a line, so the trace file does not grow all day.
        if (firstSweep || lastResult_.written || lastResult_.failed) {
            startup::trace(QStringLiteral("cowork hooks: %1 session dir(s), %2 written, %3 failed")
                .arg(lastResult_.sessions).arg(lastResult_.written).arg(lastResult_.failed));
        }
        firstSweep = false;
    };
    // After the first paint, so a capture run is not delayed by a filesystem walk.
    QTimer::singleShot(1200, this, sweep);
    timer_ = new QTimer(this);
    timer_->setInterval(std::max(10000, intervalMs));
    connect(timer_, &QTimer::timeout, this, sweep);
    timer_->start();
}
