#include "../src/CoworkHookSeeder.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

// Claude Desktop keeps its hooks in a PER-SESSION config dir, so something has to
// keep seeding them as conversations appear. That job used to be a Scheduled Task
// running a PowerShell script every two minutes, which flashed a console window
// (the user's "黑框总是自动弹出来", 2026-10-17) and pointed at a repo path the
// installer never ships. The pet now does it in-process.
//
// These tests pin the two things that make doing it silently acceptable: the
// merge only ever touches the pet's own entries, and a file the code does not
// understand is never rewritten.
class CoworkHookSeederTest : public QObject {
    Q_OBJECT

private:
    static QString helper() { return QStringLiteral("C:\\pet\\DesktopCompanionHook.exe"); }
    static QString quotedHelper() { return QStringLiteral("\"") + helper() + QStringLiteral("\""); }

    // The sweep needs a helper that EXISTS to seed at all, and its file name is
    // also the marker that recognises the pet's own entries (so a re-run replaces
    // instead of stacking). Both matter here: a stand-in called anything else
    // makes the merge look non-idempotent by appending a second group.
    // Note: no QVERIFY here -- it expands to a bare `return`, which a value
    // returning helper cannot do. The caller asserts the file exists instead.
    static QString makeFakeHelper(const QString& dir)
    {
        const QString path = dir + QStringLiteral("/fake/DesktopCompanionHook.exe");
        QDir().mkpath(dir + QStringLiteral("/fake"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.close();
        return path;
    }

    static QString settingsAt(const QString& root, const QString& session)
    {
        return root + QStringLiteral("/acct/00000000/") + session
            + QStringLiteral("/.claude/settings.json");
    }

    // Builds root/acct/00000000/<session>/.claude/settings.json for each session.
    static void makeSession(const QString& root, const QString& session, const QByteArray& json)
    {
        const QString dir = root + QStringLiteral("/acct/00000000/") + session + QStringLiteral("/.claude");
        QVERIFY(QDir().mkpath(dir));
        if (!json.isNull()) {
            QFile file(dir + QStringLiteral("/settings.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(json), qint64(json.size()));
        }
    }

    static QStringList commandsFor(const QJsonObject& root, const QString& event)
    {
        QStringList out;
        for (const QJsonValue& group : root.value(QStringLiteral("hooks")).toObject()
                                          .value(event).toArray()) {
            for (const QJsonValue& hook : group.toObject().value(QStringLiteral("hooks")).toArray())
                out << hook.toObject().value(QStringLiteral("command")).toString();
        }
        return out;
    }

private slots:
    // An empty (or missing) settings.json is the normal case: a brand-new conversation.
    void seedsAFreshSettingsFile()
    {
        bool changed = false;
        QString error;
        const QString merged = CoworkHookSeeder::mergeSettings(QString(), helper(), &changed, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(changed);
        const QJsonObject root = QJsonDocument::fromJson(merged.toUtf8()).object();
        for (const QString& event : {QStringLiteral("UserPromptSubmit"), QStringLiteral("Stop"),
                                     QStringLiteral("SessionEnd")}) {
            const QJsonArray groups = root.value(QStringLiteral("hooks")).toObject().value(event).toArray();
            QCOMPARE(groups.size(), 1);
            const QJsonObject hook = groups.at(0).toObject().value(QStringLiteral("hooks"))
                                         .toArray().at(0).toObject();
            QCOMPARE(hook.value(QStringLiteral("command")).toString(),
                     QStringLiteral("\"") + helper() + QStringLiteral("\""));
            QCOMPARE(hook.value(QStringLiteral("timeout")).toInt(), 5);
            QCOMPARE(hook.value(QStringLiteral("type")).toString(), QStringLiteral("command"));
        }
    }

    // Everything the app owns stays: we are a guest in this file.
    void keepsEveryOtherKeyAndHookGroup()
    {
        // Named raw-string delimiter: the payload itself contains `)"` in
        // "Bash(ls:*)", which would close a plain R"(...)" early.
        const QByteArray existing = R"json({
  "permissions": { "allow": ["Bash(ls:*)"] },
  "env": { "FOO": "1" },
  "hooks": {
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "C:\\other\\notify.exe" } ] }
    ],
    "PreToolUse": [
      { "hooks": [ { "type": "command", "command": "C:\\other\\guard.exe" } ] }
    ]
  }
})json";
        bool changed = false;
        QString error;
        const QString merged = CoworkHookSeeder::mergeSettings(QString::fromUtf8(existing), helper(),
                                                               &changed, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(changed);
        const QJsonObject root = QJsonDocument::fromJson(merged.toUtf8()).object();
        QCOMPARE(root.value(QStringLiteral("permissions")).toObject()
                     .value(QStringLiteral("allow")).toArray().at(0).toString(),
                 QStringLiteral("Bash(ls:*)"));
        QCOMPARE(root.value(QStringLiteral("env")).toObject().value(QStringLiteral("FOO")).toString(),
                 QStringLiteral("1"));
        // The other tool's group in a shared event, and the event we do not manage.
        // Build the expected list in a variable: a braced QStringList{...} inside
        // QCOMPARE would be split on its comma by the preprocessor.
        QStringList expectedSubmit;
        expectedSubmit << QStringLiteral("C:\\other\\notify.exe") << quotedHelper();
        QCOMPARE(commandsFor(root, QStringLiteral("UserPromptSubmit")), expectedSubmit);
        QCOMPARE(commandsFor(root, QStringLiteral("PreToolUse")),
                 QStringList{QStringLiteral("C:\\other\\guard.exe")});
    }

    // Feeding our own output back in must be a no-op, or every sweep would rewrite
    // all six files forever.
    void seedingIsIdempotent()
    {
        bool changed = false;
        QString error;
        const QString first = CoworkHookSeeder::mergeSettings(QString(), helper(), &changed, &error);
        QVERIFY(changed);
        const QString second = CoworkHookSeeder::mergeSettings(first, helper(), &changed, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(!changed, "a second merge must not change anything");
        QCOMPARE(second, first);
        const QJsonObject root = QJsonDocument::fromJson(second.toUtf8()).object();
        for (const QString& event : {QStringLiteral("UserPromptSubmit"), QStringLiteral("Stop"),
                                     QStringLiteral("SessionEnd")}) {
            QCOMPARE(commandsFor(root, event).size(), 1);
        }
    }

    // A stale duplicate (an older install, or the PS script and the pet both having
    // run) collapses back to one entry pointing at the current helper.
    void staleDuplicatesCollapseToOne()
    {
        // Built in code rather than as a raw string literal, because moc's scanner
        // gives up on a raw string that contains \" -- it then silently skips the
        // WHOLE translation unit ("No relevant classes found") and the failure only
        // surfaces as unresolved metaObject/qt_metacall symbols at link time. A raw
        // string with plain backslashes (C:\\path) is fine; backslash-quote is not.
        const auto group = [](const QString& exe) {
            return QJsonObject{{QStringLiteral("hooks"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("command")},
                {QStringLiteral("command"), QStringLiteral("\"") + exe + QStringLiteral("\"")}}}}};
        };
        const QJsonObject existing{{QStringLiteral("hooks"), QJsonObject{
            {QStringLiteral("Stop"),
             QJsonArray{group(QStringLiteral("C:\\old\\DesktopCompanionHook.exe")),
                        group(QStringLiteral("C:\\older\\DesktopCompanionHook.exe"))}}}}};

        bool changed = false;
        QString error;
        const QString merged = CoworkHookSeeder::mergeSettings(
            QString::fromUtf8(QJsonDocument(existing).toJson()), helper(), &changed, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonObject root = QJsonDocument::fromJson(merged.toUtf8()).object();
        QCOMPARE(commandsFor(root, QStringLiteral("Stop")), QStringList{quotedHelper()});
    }

    // A file we cannot read is a file we must not overwrite.
    void refusesToRewriteWhatItCannotParse()
    {
        bool changed = true;
        QString error;
        QCOMPARE(CoworkHookSeeder::mergeSettings(QStringLiteral("{ not json"), helper(), &changed, &error),
                 QString());
        QVERIFY(!error.isEmpty());
        QVERIFY(!changed);
        error.clear();
        QCOMPARE(CoworkHookSeeder::mergeSettings(QStringLiteral("[1,2,3]"), helper(), &changed, &error),
                 QString());
        QVERIFY(!error.isEmpty());
        error.clear();
        QCOMPARE(CoworkHookSeeder::mergeSettings(QStringLiteral(R"({"hooks": 7})"), helper(), &changed, &error),
                 QString());
        QVERIFY(!error.isEmpty());
        error.clear();
        QCOMPARE(CoworkHookSeeder::mergeSettings(QStringLiteral(R"({"hooks": {"Stop": 7}})"), helper(),
                                                 &changed, &error),
                 QString());
        QVERIFY(!error.isEmpty());
    }

    // End to end over a temp tree: one sweep writes, the next one is silent, and
    // the bytes on disk start with '{' -- no BOM, which Codex-style readers reject.
    void sweepWritesOnceAndThenStaysQuiet()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = temp.path();
        makeSession(root, QStringLiteral("s1"), QByteArray());
        makeSession(root, QStringLiteral("s2"), QByteArray("{\n}\n"));
        // A sibling the app owns but that carries no .claude: must be left alone.
        QVERIFY(QDir().mkpath(root + QStringLiteral("/acct/00000000/memory")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/acct/00000000/spaces/x")));
        // The stand-in helper also lives under the scanned root (at depth 0, where
        // there is no .claude), which doubles as proof that a stray top-level
        // directory neither inflates the session count nor gets written into.
        const QString self = makeFakeHelper(root);
        QVERIFY(QFileInfo::exists(self));
        const CoworkSeedResult first = CoworkHookSeeder::seedOnce(self, root);
        QCOMPARE(first.sessions, 2);
        QCOMPARE(first.written, 2);
        QCOMPARE(first.failed, 0);
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/acct/00000000/memory/.claude")));

        for (const QString& session : {QStringLiteral("s1"), QStringLiteral("s2")}) {
            const QString path = settingsAt(root, session);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            QVERIFY2(!bytes.startsWith("\xEF\xBB\xBF"), "settings.json must not carry a UTF-8 BOM");
            QVERIFY(bytes.trimmed().startsWith('{'));
            // Exactly one entry per event: no duplicates stacked by the second pass.
            QCOMPARE(bytes.count("DesktopCompanionHook.exe"), qsizetype(3));
            QJsonParseError parse{};
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
            QCOMPARE(parse.error, QJsonParseError::NoError);
            QVERIFY(document.isObject());
        }

        const CoworkSeedResult second = CoworkHookSeeder::seedOnce(self, root);
        QCOMPARE(second.sessions, 2);
        // Idempotent: the second sweep must find nothing to rewrite, or every
        // interval would rewrite all six files forever.
        QCOMPARE(second.written, 0);
        QCOMPARE(second.failed, 0);
    }

    // Pointing clients at a helper that does not exist would be worse than leaving
    // them unhooked, so the sweep refuses to seed at all.
    void refusesToSeedAMissingHelper()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        makeSession(temp.path(), QStringLiteral("s1"), QByteArray());
        const CoworkSeedResult result = CoworkHookSeeder::seedOnce(
            temp.path() + QStringLiteral("/no-such-helper.exe"), temp.path());
        QCOMPARE(result.sessions, 0);
        QCOMPARE(result.written, 0);
        QVERIFY(!QFileInfo::exists(settingsAt(temp.path(), QStringLiteral("s1"))));
    }

    // No Claude-Desktop install (or a non-Windows session) must be a quiet no-op.
    void missingRootIsANoOp()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const CoworkSeedResult result = CoworkHookSeeder::seedOnce(
            QCoreApplication::applicationFilePath(), temp.path() + QStringLiteral("/absent"));
        QCOMPARE(result.sessions, 0);
        QCOMPARE(result.written, 0);
        QCOMPARE(result.failed, 0);
    }

    // The helper's FILE NAME is the de-dup marker shared with
    // tools/install-hooks.ps1 ("DesktopCompanionHook"), so the helper this build
    // points clients at has to keep that name -- rename it and every sweep would
    // stack a second entry instead of replacing the first.
    void theHelperNameIsTheMarker()
    {
        const QString path = CoworkHookSeeder::defaultHelperPath();
        QVERIFY2(path.endsWith(QStringLiteral("DesktopCompanionHook.exe")), qPrintable(path));
        QVERIFY(path.contains(QStringLiteral("DesktopCompanionHook")));
        // Native separators: the string ends up in another product's config and on a
        // shell command line, so it must not carry Qt's forward slashes.
        QVERIFY2(!path.contains(QLatin1Char('/')), qPrintable(path));
    }
};

// GUILESS, not APPLESS: the sweep resolves the helper from the running
// application's own directory, so the test needs a QCoreApplication instance.
QTEST_GUILESS_MAIN(CoworkHookSeederTest)
#include "CoworkHookSeederTest.moc"
