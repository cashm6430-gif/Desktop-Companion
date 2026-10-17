#pragma once

#include <QObject>
#include <QString>

class QTimer;

// Claude Desktop ("3p"/Cowork) does not read ~/.claude. It starts its embedded
// Claude Code with `--setting-sources=user` and points that "user" layer at a
// PER-SESSION config dir:
//     %LOCALAPPDATA%\Claude-3p\local-agent-mode-sessions\<acct>\<x>\<session>\.claude
// So every new conversation is a fresh config dir with no hooks in it, and the
// hooks have to be re-seeded as conversations appear.
//
// The pet does that itself now. The previous mechanism was a Scheduled Task that
// ran tools/seed-cowork-hooks.ps1 every two minutes; PowerShell 5.1 still flashes
// a console window even with -WindowStyle Hidden, so the user got a black box
// popping up all day (2026-10-17), and the task pointed at a REPO path that the
// installer never ships -- it only ever worked on the dev machine. Doing it
// in-process is windowless by construction and ships with the app.

struct CoworkSeedResult {
    int sessions = 0;   // session config dirs seen
    int written = 0;    // settings.json actually rewritten this sweep
    int failed = 0;     // files we could not read/parse/write (left untouched)
};

class CoworkHookSeeder : public QObject {
    Q_OBJECT

public:
    explicit CoworkHookSeeder(QObject* parent = nullptr);

    // %LOCALAPPDATA%\Claude-3p\local-agent-mode-sessions
    static QString sessionRoot();
    // The helper this build should point clients at: the one next to the app.
    static QString defaultHelperPath();

    // Merges the pet's hooks into one settings.json text. Pure -- no I/O, so the
    // merge rules are unit-testable. Everything except the pet's own hook entries
    // is preserved (other top-level keys, other events, other tools' groups), and
    // `*changed` says whether the result differs from `existing`. Returns {} and
    // sets `*error` for a file we do not understand: those are NEVER rewritten.
    static QString mergeSettings(const QString& existing, const QString& helperPath,
                                 bool* changed, QString* error);

    // One sweep over the session dirs that already exist. A `.claude` directory is
    // the marker for "this is a real session the app manages" -- we never invent
    // one, so unrelated folders (memory/, spaces/, usage-ledger/) are ignored.
    static CoworkSeedResult seedOnce(const QString& helperPath = defaultHelperPath(),
                                     const QString& root = sessionRoot());

    // Seed shortly after startup, then every `intervalMs`. Safe to call in a
    // capture run too: it touches no rendering state.
    void start(int intervalMs = 120000);

    CoworkSeedResult lastResult() const { return lastResult_; }
    QString lastHelperPath() const { return lastHelperPath_; }

private:
    QTimer* timer_ = nullptr;
    CoworkSeedResult lastResult_;
    QString lastHelperPath_;
};
