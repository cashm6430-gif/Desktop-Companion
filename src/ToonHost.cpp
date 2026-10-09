#include "ToonHost.h"

#include "CodexTurnSource.h"
#include "DesktopDeleteSource.h"
#include "PetController.h"
#include "ToonEventBridge.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <windows.h>

namespace {
bool writeReport(const QString& path, const QJsonObject& report) {
    if (path.isEmpty()) return true;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
}

bool readReplay(const QString& path, QJsonArray* events, int* duration, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) {
        *error = QStringLiteral("Replay must be a readable JSON file <= 256 KiB."); return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Replay must contain a JSON object."); return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt() != 1
        || !object.value(QStringLiteral("events")).isArray()) {
        *error = QStringLiteral("Replay requires version:1 and events:[...]."); return false;
    }
    *events = object.value(QStringLiteral("events")).toArray();
    if (events->size() > 512) {
        *error = QStringLiteral("Replay is limited to 512 events."); return false;
    }
    int lastAt = 0;
    const QStringList kinds{QStringLiteral("start"), QStringLiteral("stop"),
        QStringLiteral("session_end"), QStringLiteral("delete"), QStringLiteral("wave")};
    for (const auto& value : *events) {
        const QJsonObject event = value.toObject();
        const double at = event.value(QStringLiteral("at_ms")).toDouble(-1.0);
        if (!value.isObject() || !std::isfinite(at) || at < lastAt || at > 120000
            || std::floor(at) != at || !kinds.contains(event.value(QStringLiteral("event")).toString())) {
            *error = QStringLiteral("Replay events require sorted integral at_ms <= 120000 and a known event.");
            return false;
        }
        const QString kind = event.value(QStringLiteral("event")).toString();
        if ((kind == QStringLiteral("start") || kind == QStringLiteral("stop")
             || kind == QStringLiteral("session_end"))
            && event.value(QStringLiteral("session_id")).toString().isEmpty()) {
            *error = QStringLiteral("Agent events require session_id."); return false;
        }
        if (kind == QStringLiteral("start") && event.value(QStringLiteral("turn_id")).toString().isEmpty()) {
            *error = QStringLiteral("Start events require turn_id."); return false;
        }
        lastAt = static_cast<int>(at);
    }
    const double durationValue = object.value(QStringLiteral("duration_ms")).toDouble(lastAt + 5000);
    if (!std::isfinite(durationValue) || durationValue <= lastAt || durationValue > 150000
        || std::floor(durationValue) != durationValue) {
        *error = QStringLiteral("Replay duration_ms must exceed the last event and be <= 150000."); return false;
    }
    *duration = static_cast<int>(durationValue);
    return true;
}
}

int runToonHost(QApplication& app, const QStringList& arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Optional Qt event host for the candidate Godot toon renderer."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("toon-host"), QStringLiteral("Run the semantic 3D host instead of PetWindow.")});
    parser.addOption({QStringLiteral("godot"), QStringLiteral("Godot executable."), QStringLiteral("exe")});
    parser.addOption({QStringLiteral("toon-project"), QStringLiteral("Imported/staged Godot project directory."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("toon-replay"), QStringLiteral("Isolated simulated events; no real Shell or Codex listener."), QStringLiteral("json")});
    parser.addOption({QStringLiteral("toon-report"), QStringLiteral("Write semantic trace/status without token or file paths."), QStringLiteral("json")});
    parser.addOption({QStringLiteral("toon-render-output"), QStringLiteral("Renderer review evidence directory."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("toon-review-profile"), QStringLiteral("Renderer review profile JSON file."), QStringLiteral("json")});
    parser.addOption({QStringLiteral("toon-record-frames"), QStringLiteral("Record renderer frames; requires --toon-render-output.")});
    parser.addOption({QStringLiteral("toon-quit-after-ms"), QStringLiteral("Optional bounded host run."), QStringLiteral("ms")});
    if (!parser.parse(arguments)) return 2;
    if (parser.isSet(QStringLiteral("help"))) parser.showHelp();
    const QString reportPath = parser.value(QStringLiteral("toon-report"));
    QJsonArray records;
    QString error;
    auto fail = [&](const QString& reason) {
        writeReport(reportPath, {{QStringLiteral("status"), QStringLiteral("failed")},
            {QStringLiteral("error"), reason}, {QStringLiteral("formal_model_changed"), false}});
        qWarning("Toon host: %s", qPrintable(reason));
        // A GUI-subsystem binary has no console. Interactive startup errors
        // must be visible, while automated replay uses its report/exit code.
        if (!parser.isSet(QStringLiteral("toon-replay")) && reportPath.isEmpty())
            QMessageBox::warning(nullptr, QStringLiteral("3渲2桌宠候选宿主"), reason);
        return 2;
    };
    const QFileInfo godot(parser.value(QStringLiteral("godot")));
    const QFileInfo project(parser.value(QStringLiteral("toon-project")));
    if (!godot.isFile() || !project.isDir()
        || !QFileInfo(QDir(project.absoluteFilePath()).filePath(QStringLiteral("project.godot"))).isFile()
        || !QFileInfo(QDir(project.absoluteFilePath()).filePath(QStringLiteral("assets/character.glb"))).isFile())
        return fail(QStringLiteral("Provide --godot <exe> and --toon-project <staged/imported project with assets/character.glb>."));
    if (parser.isSet(QStringLiteral("toon-review-profile"))
        && !QFileInfo(parser.value(QStringLiteral("toon-review-profile"))).isFile())
        return fail(QStringLiteral("--toon-review-profile must name an existing JSON file."));
    if (parser.isSet(QStringLiteral("toon-record-frames"))
        && (!parser.isSet(QStringLiteral("toon-render-output"))
            || parser.value(QStringLiteral("toon-render-output")).isEmpty()))
        return fail(QStringLiteral("--toon-record-frames requires --toon-render-output <dir>."));
    const bool isolatedReplay = parser.isSet(QStringLiteral("toon-replay"));
    QJsonArray replayEvents;
    int replayDuration = 0;
    if (isolatedReplay && !readReplay(parser.value(QStringLiteral("toon-replay")),
        &replayEvents, &replayDuration, &error)) return fail(error);
    int quitAfter = 0;
    if (parser.isSet(QStringLiteral("toon-quit-after-ms"))) {
        bool okay = false;
        quitAfter = parser.value(QStringLiteral("toon-quit-after-ms")).toInt(&okay);
        if (!okay || quitAfter < 1 || quitAfter > 300000)
            return fail(QStringLiteral("--toon-quit-after-ms must be in 1..300000."));
    }

    PetController controller;
    ToonEventBridge bridge(&controller);
    CodexTurnSource codex;
    DesktopDeleteSource desktop;
    QWidget notificationWindow;
    // SHChangeNotify needs an HWND, not a visible Qt pet. Constructing this
    // hidden native window must never call show() or grab foreground focus.
    notificationWindow.setAttribute(Qt::WA_DontShowOnScreen);
    notificationWindow.setAttribute(Qt::WA_ShowWithoutActivating);
    notificationWindow.setWindowFlag(Qt::Tool);
    QObject::connect(&bridge, &ToonEventBridge::trace, &app, [&](const QJsonObject& record) {
        if (records.size() < 4096) records.append(record);
    });
    QObject::connect(&bridge, &ToonEventBridge::quitRequested, &app, &QCoreApplication::quit);
    QObject::connect(&codex, &CodexTurnSource::turnStarted, &bridge, &ToonEventBridge::turnStarted);
    QObject::connect(&codex, &CodexTurnSource::turnStopped, &bridge, &ToonEventBridge::turnStopped);
    QObject::connect(&codex, &CodexTurnSource::sessionEnded, &bridge, &ToonEventBridge::sessionEnded);
    QObject::connect(&desktop, &DesktopDeleteSource::desktopItemDeleted,
                     &bridge, &ToonEventBridge::desktopItemDeleted);
    codex.setStatusProvider([&] {
        QJsonObject status = bridge.status();
        status.insert(QStringLiteral("desktop_watching"), desktop.isWatching());
        status.insert(QStringLiteral("desktop_error"), desktop.error());
        return status;
    });
    if (!isolatedReplay) {
        if (!codex.start()) return fail(QStringLiteral("Codex hook endpoint is already in use or unavailable. Close the previous pet first; its server has not been removed."));
        if (!desktop.start(reinterpret_cast<HWND>(notificationWindow.winId())))
            return fail(QStringLiteral("Desktop Shell listener failed: %1").arg(desktop.error()));
    }
    if (!bridge.start()) return fail(QStringLiteral("Cannot open the loopback renderer listener."));

    QMenu menu;
    QSystemTrayIcon tray;
    tray.setIcon(app.style()->standardIcon(QStyle::SP_ComputerIcon));
    tray.setToolTip(QStringLiteral("鲸鱼娘 · 3渲2技术候选"));
    tray.setContextMenu(&menu);
    menu.addAction(QStringLiteral("播放删除反应（模拟）"), &bridge, [&] { bridge.desktopItemDeleted(); });
    menu.addAction(QStringLiteral("打个招呼"), &bridge, &ToonEventBridge::wave);
    menu.addAction(QStringLiteral("清除忙碌记录"), &bridge, &ToonEventBridge::resetBusy);
    menu.addSeparator();
    menu.addAction(QStringLiteral("退出"), &app, &QCoreApplication::quit);
    if (!isolatedReplay && QSystemTrayIcon::isSystemTrayAvailable()) tray.show();

    QProcess renderer;
    renderer.setProgram(godot.absoluteFilePath());
    QStringList rendererArguments{QStringLiteral("--path"), project.absoluteFilePath(), QStringLiteral("--"),
        QStringLiteral("--host-port"), QString::number(bridge.port()),
        QStringLiteral("--host-token"), bridge.token()};
    if (parser.isSet(QStringLiteral("toon-render-output")))
        rendererArguments << QStringLiteral("--review-output") << parser.value(QStringLiteral("toon-render-output"));
    if (parser.isSet(QStringLiteral("toon-review-profile")))
        rendererArguments << QStringLiteral("--review-profile") << QFileInfo(
            parser.value(QStringLiteral("toon-review-profile"))).absoluteFilePath();
    if (parser.isSet(QStringLiteral("toon-record-frames"))) rendererArguments << QStringLiteral("--record-frames");
    renderer.setArguments(rendererArguments);
    renderer.setWorkingDirectory(project.absoluteFilePath());
    renderer.setProcessChannelMode(QProcess::MergedChannels);
    renderer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
        args->flags |= CREATE_NO_WINDOW;
    });
    QString runtimeOutput;
    auto readRuntimeOutput = [&] {
        // Do not copy command line arguments (which contain the token) into
        // reports. Renderer diagnostics themselves are capped to 64 KiB.
        const QByteArray bytes = renderer.readAll();
        if (runtimeOutput.size() < 65536)
            runtimeOutput += QString::fromUtf8(bytes.left(65536 - runtimeOutput.size()));
    };
    QObject::connect(&renderer, &QProcess::readyRead, &app, readRuntimeOutput);
    int result = 0;
    bool readySeen = false;
    bool replayStarted = false;
    QTimer readyDeadline;
    readyDeadline.setSingleShot(true);
    readyDeadline.setInterval(15000);
    QObject::connect(&readyDeadline, &QTimer::timeout, &app, [&] {
        error = QStringLiteral("Renderer did not complete the versioned ready handshake in 15 seconds.");
        result = 3;
        app.quit();
    });
    QObject::connect(&bridge, &ToonEventBridge::rendererReadyChanged, &app, [&](bool ready) {
        if (!ready) return;
        readySeen = true;
        readyDeadline.stop();
        if (!isolatedReplay || replayStarted) return;
        replayStarted = true;
        for (const auto& value : replayEvents) {
            const QJsonObject event = value.toObject();
            QTimer::singleShot(event.value(QStringLiteral("at_ms")).toInt(), &bridge, [&, event] {
                const QString kind = event.value(QStringLiteral("event")).toString();
                const QString session = event.value(QStringLiteral("session_id")).toString();
                const QString turn = event.value(QStringLiteral("turn_id")).toString();
                if (kind == QStringLiteral("start")) bridge.turnStarted(session, turn);
                else if (kind == QStringLiteral("stop")) bridge.turnStopped(session, turn);
                else if (kind == QStringLiteral("session_end")) bridge.sessionEnded(session);
                else if (kind == QStringLiteral("delete")) bridge.desktopItemDeleted();
                else if (kind == QStringLiteral("wave")) bridge.wave();
            });
        }
        QTimer::singleShot(replayDuration, &app, &QCoreApplication::quit);
    });
    QObject::connect(&renderer, &QProcess::errorOccurred, &app, [&](QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart) {
            error = QStringLiteral("Godot failed to start: %1").arg(renderer.errorString());
            result = 3;
            app.quit();
        }
    });
    QObject::connect(&renderer, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &app,
        [&](int exitCode, QProcess::ExitStatus exitStatus) {
            readRuntimeOutput();
            if (exitCode != 0 || exitStatus != QProcess::NormalExit || !readySeen) {
                error = QStringLiteral("Godot exited before a usable renderer session (code %1).").arg(exitCode);
                result = 3;
            }
            app.quit();
        });
    if (quitAfter) QTimer::singleShot(quitAfter, &app, &QCoreApplication::quit);
    readyDeadline.start();
    renderer.start();
    app.exec();
    if (!readySeen && result == 0) {
        result = 3;
        error = QStringLiteral("Host ended before a renderer was ready.");
    }
    const QJsonObject finalStatus = bridge.status();
    tray.hide();
    bridge.shutdown();
    // Graceful semantic shutdown first; bounded termination is only for this
    // child launched by this host, never an existing user Godot process.
    if (renderer.state() != QProcess::NotRunning && !renderer.waitForFinished(2000)) {
        renderer.terminate();
        if (!renderer.waitForFinished(1000)) {
            renderer.kill();
            renderer.waitForFinished(1000);
        }
    }
    readRuntimeOutput();
    // Renderer code should never print credentials, but the host also strips
    // its token defensively from arbitrary external engine diagnostics.
    runtimeOutput.replace(bridge.token(), QStringLiteral("<redacted>"));
    const bool reportSaved = writeReport(reportPath, {{QStringLiteral("version"), 1},
        {QStringLiteral("status"), result == 0 && readySeen ? QStringLiteral("completed") : QStringLiteral("failed")},
        {QStringLiteral("error"), error},
        {QStringLiteral("isolated_replay"), isolatedReplay},
        {QStringLiteral("real_sources_enabled"), !isolatedReplay},
        {QStringLiteral("ready_seen"), readySeen},
        {QStringLiteral("formal_model_changed"), false},
        {QStringLiteral("visual_approval"), QStringLiteral("pending")},
        {QStringLiteral("final_status"), finalStatus},
        {QStringLiteral("trace"), records},
        {QStringLiteral("renderer_output"), runtimeOutput}});
    if (!reportSaved) return 4;
    return result;
}
