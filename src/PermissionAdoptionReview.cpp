#include "PermissionAdoptionReview.h"
#include "CodexTurnSource.h"
#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

namespace {
// This review creates a private receiver, never the normal listener or hooks.
bool request(const QString& endpoint, const QJsonObject& payload) {
    if (!endpoint.startsWith(QStringLiteral("pet-permission-adoption-"))
        || endpoint == CodexTurnSource::serverName()) return false;
    QLocalSocket socket;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool complete = false;
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QLocalSocket::connected, &loop, [&] {
        socket.write(QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
    });
    QObject::connect(&socket, &QLocalSocket::disconnected, &loop, [&] { complete = true; loop.quit(); });
    socket.connectToServer(endpoint);
    deadline.start(1500);
    loop.exec();
    return complete;
}
QJsonObject event(const QString& action, const QString& client, const QString& session) {
    return {{QStringLiteral("event"), action}, {QStringLiteral("client"), client},
            {QStringLiteral("session_id"), session}, {QStringLiteral("turn_id"), QStringLiteral("fixture-turn")}};
}
}

int runPermissionAdoptionReview(QApplication& app, PetController& controller,
                               PetWindow& window, const QString& output, const QString& scenario) {
    const bool seated = scenario == QStringLiteral("permission-adoption-laptop");
    if ((!seated && scenario != QStringLiteral("permission-adoption-standing"))
        || !app.property("desktopCompanionReviewSettingsIsolated").toBool()) return 2;
    const QDir directory(output);
    window.prepareLiveInteractionReview(560);
    window.setInteractionPreviewEnabled(false);
    window.setAutomaticWorkNotesEnabled(false);
    window.setWorkPosturePreference(seated ? ParameterMotion::WorkPosturePreference::Laptop
                                          : ParameterMotion::WorkPosturePreference::Standing);
    window.move(-10000, -10000);
    const auto endpoint = QStringLiteral("pet-permission-adoption-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    CodexTurnSource source;
    QObject::connect(&source, &CodexTurnSource::turnStarted, &controller, &PetController::turnStarted);
    QObject::connect(&source, &CodexTurnSource::turnStopped, &controller, &PetController::turnStopped);
    QObject::connect(&source, &CodexTurnSource::sessionEnded, &controller, &PetController::sessionEnded);
    QObject::connect(&source, &CodexTurnSource::permissionSnapshot, &controller, &PetController::applyPermissionSnapshot);
    if (!source.start(endpoint)) return 2;
    const auto life = [&](const QString& action, const QString& client = QStringLiteral("opencode"),
                          const QString& session = QStringLiteral("fixture-primary")) {
        return request(endpoint, event(action, client, session));
    };
    const auto permissions = [&](const QStringList& ids) {
        auto message = event(QStringLiteral("permission_state"), QStringLiteral("opencode"),
                             QStringLiteral("fixture-primary"));
        message.insert(QStringLiteral("request_ids"), QJsonArray::fromStringList(ids));
        return request(endpoint, message);
    };
    const auto active = [&] { return window.pointerReviewState().value(QStringLiteral("reaction")).toString()
        == QStringLiteral("permission-cue"); };
    QTimer timer, deadline;
    timer.setInterval(1);
    timer.setTimerType(Qt::PreciseTimer);
    deadline.setSingleShot(true);
    QJsonObject checks;
    QJsonArray frames;
    int frame = 0;
    bool finished = false;
    constexpr int count = 840;
    constexpr double dt = .05;
    double duplicateTime = 0;
    const auto finish = [&](bool passed, const QString& failure = {}) {
        if (finished) return;
        finished = true;
        timer.stop();
        deadline.stop();
        QJsonObject result{{QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), QStringLiteral("private_source_controller_live_window_approved_automatic_entry")},
            {QStringLiteral("passed"), passed}, {QStringLiteral("failure"), failure},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("frame_count"), frames.size()}, {QStringLiteral("checks"), checks},
            {QStringLiteral("frames"), frames},
            {QStringLiteral("privacy"), QJsonObject{{QStringLiteral("isolated_settings"), true},
                {QStringLiteral("private_endpoint_only"), true}, {QStringLiteral("real_hooks_used"), false},
                {QStringLiteral("model_prompt_sent"), false}, {QStringLiteral("desktop_files_changed"), false}}}};
        QSaveFile file(directory.filePath(QStringLiteral("scene.json")));
        const auto bytes = QJsonDocument(result).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            app.exit(2); return;
        }
        app.exit(passed ? 0 : 1);
    };
    const auto check = [&](const QString& key, bool passed) {
        checks.insert(key, passed);
        if (!passed) finish(false, key);
        return passed;
    };
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { finish(false, QStringLiteral("wall_time_limit")); });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        // IPC requests run a nested event loop. Do not advance another frame
        // until the current frame's request, checks and capture are complete.
        timer.stop();
        QString action;
        if (frame == 10 && !check(QStringLiteral("private_start"), life(QStringLiteral("start")))) return;
        if (frame == 80 && !check(QStringLiteral("parallel_client"), life(QStringLiteral("start"),
                QStringLiteral("codex"), QStringLiteral("fixture-secondary")))) return;
        if (frame == 300) {
            if (!check(QStringLiteral("stable_preference"), window.motionParameters().value(QStringLiteral("ParamSitPose"))
                    >= (seated ? .99 : 0) && (seated || window.motionParameters().value(QStringLiteral("ParamSitPose")) < .001))
                || !check(QStringLiteral("ordinary_auto_start"), permissions({QStringLiteral("a")}) && active())) return;
            action = QStringLiteral("automatic_a");
        }
        if (frame == 304) {
            duplicateTime = window.pointerReviewState().value(QStringLiteral("reaction_time")).toDouble();
            if (!check(QStringLiteral("duplicate_keeps_phase"), permissions({QStringLiteral("a")})
                    && active() && window.pointerReviewState().value(QStringLiteral("reaction_time")).toDouble() == duplicateTime)) return;
        }
        if (frame == 310 && !check(QStringLiteral("new_request_does_not_restart_busy_cue"),
                permissions({QStringLiteral("a"), QStringLiteral("b")}) && active())) return;
        if (frame == 316 && !check(QStringLiteral("unbound_resolution_keeps_cue"),
                permissions({QStringLiteral("a")}) && active())) return;
        if (frame == 320) {
            window.setInteractionPreviewEnabled(false);
            if (!check(QStringLiteral("preview_off_retains_approved"), active())) return;
        }
        if (frame == 326) {
            if (!check(QStringLiteral("bound_resolution_cancels_only_cue"), permissions({}) && !active()
                    && controller.activeTurnCount() == 2)) return;
            action = QStringLiteral("bound_a_resolved");
        }
        if (frame == 340 && !check(QStringLiteral("same_request_never_replays"),
                permissions({QStringLiteral("a")}) && !active())) return;
        if (frame == 350 && !permissions({})) { finish(false, QStringLiteral("clear_a")); return; }
        if (frame == 360) {
            if (!check(QStringLiteral("later_new_request_plays"), permissions({QStringLiteral("c")}) && active())) return;
            action = QStringLiteral("automatic_c");
        }
        if (frame == 420 && !check(QStringLiteral("finite_cue_returns_to_busy"), !active()
                && controller.activeTurnCount() == 2 && controller.pendingPermissionCount() == 1)) return;
        if (frame == 430 && !permissions({})) { finish(false, QStringLiteral("clear_c")); return; }
        if (frame == 440) {
            app.setProperty("desktopCompanionReviewSuppressAutomatic", true);
            if (!check(QStringLiteral("quiet_consumes_nomination"), permissions({QStringLiteral("q")}) && !active()
                    && controller.pendingPermissionCount() == 1)) return;
        }
        if (frame == 446) {
            app.setProperty("desktopCompanionReviewSuppressAutomatic", false);
            if (!check(QStringLiteral("quiet_resume_no_replay"), permissions({QStringLiteral("q")}) && !active())) return;
        }
        if (frame == 470 && !permissions({})) { finish(false, QStringLiteral("clear_q")); return; }
        if (frame == 480 && !check(QStringLiteral("pre_new_turn_cue"), permissions({QStringLiteral("d")}) && active())) return;
        if (frame == 490 && !check(QStringLiteral("new_turn_interrupts_cue"), life(QStringLiteral("start"),
                QStringLiteral("codex"), QStringLiteral("fixture-third")) && !active() && controller.activeTurnCount() == 3)) return;
        if (frame == 500 && !life(QStringLiteral("stop"), QStringLiteral("codex"), QStringLiteral("fixture-third"))) {
            finish(false, QStringLiteral("third_stop")); return;
        }
        if (frame == 510 && !permissions({})) { finish(false, QStringLiteral("clear_d")); return; }
        if (frame == 520 && !window.setUserMemo(QStringLiteral("fixture memo"))) {
            finish(false, QStringLiteral("memo_foreground_failed")); return;
        }
        if (frame == 522 && !check(QStringLiteral("foreground_refuses_without_queue"),
                permissions({QStringLiteral("e")}) && !active())) return;
        if (frame == 530 && !window.foldMemo()) { finish(false, QStringLiteral("memo_fold_failed")); return; }
        if (frame == 535 && !check(QStringLiteral("foreground_exit_no_replay"),
                permissions({QStringLiteral("e")}) && !active())) return;
        if (frame == 550 && !permissions({})) { finish(false, QStringLiteral("clear_e")); return; }
        if (frame == 560 && !check(QStringLiteral("pre_session_end_cue"), permissions({QStringLiteral("f")}) && active())) return;
        if (frame == 568 && !check(QStringLiteral("session_end_preserves_other_client"), life(QStringLiteral("session_end"))
                && !active() && controller.activeTurnCount() == 1 && controller.pendingPermissionCount() == 0)) return;
        if (frame == 600 && !life(QStringLiteral("start"))) { finish(false, QStringLiteral("new_execution")); return; }
        if (frame == 620 && !check(QStringLiteral("new_execution_can_reuse_request_id"),
                permissions({QStringLiteral("a")}) && active())) return;
        if (frame == 628) {
            controller.resetBusy();
            if (!check(QStringLiteral("reset_cleans_fact_and_cue"), !active() && controller.activeTurnCount() == 0
                    && controller.pendingPermissionCount() == 0)) return;
        }
        const bool sampled = (frame >= 298 && frame <= 428 && frame % 2 == 0)
            || frame == 10 || frame == 80 || frame == 440 || frame == 446
            || frame == 490 || frame == 522 || frame == 568 || frame == 628 || frame == 839;
        const auto image = sampled ? QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0')) : QString();
        if (!window.renderLiveInteractionFrame(dt, directory.filePath(image.isEmpty() ? QStringLiteral("current.png") : image))) {
            finish(false, QStringLiteral("native_frame_failed")); return;
        }
        QJsonObject parameters;
        for (auto it = window.motionParameters().cbegin(); it != window.motionParameters().cend(); ++it)
            parameters.insert(it.key(), it.value());
        frames.append(QJsonObject{{QStringLiteral("frame"), frame}, {QStringLiteral("time"), (frame + 1) * dt},
            {QStringLiteral("file"), image}, {QStringLiteral("event"), action},
            {QStringLiteral("reaction"), window.pointerReviewState()},
            {QStringLiteral("posture"), window.workPostureReviewState()},
            {QStringLiteral("active_turns"), controller.activeTurnCount()},
            {QStringLiteral("pending_permissions"), controller.pendingPermissionCount()},
            {QStringLiteral("parameters"), parameters}});
        if (++frame == count) finish(true);
        else if (!finished) timer.start();
    });
    deadline.start(115000);
    QTimer::singleShot(1000, &timer, [&] { timer.start(); });
    return app.exec();
}
