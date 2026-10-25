#include "DragInputReview.h"

#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSaveFile>
#include <QTimer>

#include <cmath>

namespace {
constexpr double kStep = 0.05;
constexpr int kFrames = 120;
QJsonArray point(const QPointF& p) { return {p.x(), p.y()}; }
}

int runDragInputReview(QApplication& app, PetController& controller,
                       PetWindow& window, const QString& output,
                       const QString& scenario) {
    const QString kind = scenario.mid(QStringLiteral("drag-input-").size());
    const QStringList kinds{QStringLiteral("idle"), QStringLiteral("busy"),
        QStringLiteral("delete"), QStringLiteral("grass"), QStringLiteral("ungrab"), QStringLiteral("release")};
    const QDir directory(QFileInfo(output).absoluteFilePath());
    if (!scenario.startsWith(QStringLiteral("drag-input-")) || !kinds.contains(kind)
        || !directory.exists()) return 2;
    const bool busy = kind == QStringLiteral("busy");
    const bool movingRelease = kind == QStringLiteral("release");
    const int releaseFrame = movingRelease ? 13 : 40;
    const bool interrupted = kind == QStringLiteral("delete") || kind == QStringLiteral("grass");
    window.prepareLiveInteractionReview(560);
    window.setInteractionPreviewEnabled(false);
    window.setGrassDragPreviewEnabled(false);
    window.move(-10000, -10000);
    window.show();

    QTimer timer, deadline;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(10);
    deadline.setSingleShot(true);
    QElapsedTimer wall;
    wall.start();
    QJsonArray frames;
    QJsonObject checks;
    QJsonObject pausedParameters;
    QJsonArray releaseEyesBefore, releaseEyesAfter;
    int frame = 0;
    bool finished = false, sawDrag = false;
    QPointF pressLocal, pressGlobal, currentGlobal;
    QPoint windowOrigin;
    QString failure;
    const auto finish = [&](int code, const QString& reason = {}) {
        if (finished) return;
        finished = true;
        timer.stop(); deadline.stop();
        if (!reason.isEmpty()) failure = reason;
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), QStringLiteral("native_drag_stop_and_release_synthetic_qt_input")},
            {QStringLiteral("passed"), code == 0}, {QStringLiteral("failure"), failure},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("wall_ms"), wall.elapsed()},
            {QStringLiteral("frame_count"), frames.size()},
            {QStringLiteral("checks"), checks},
            {QStringLiteral("expected_actual_path_px"), 160},
            {QStringLiteral("paused_parameters"), pausedParameters},
            {QStringLiteral("release_eyes_before"), releaseEyesBefore},
            {QStringLiteral("release_eyes_after"), releaseEyesAfter},
            {QStringLiteral("final_drag"), window.dragReviewState()},
            {QStringLiteral("privacy"), QJsonObject{
                {QStringLiteral("fixture_only"), true},
                {QStringLiteral("isolated_settings"), app.property("desktopCompanionReviewSettingsIsolated").toBool()},
                {QStringLiteral("default_endpoint_used"), false},
                {QStringLiteral("real_hooks_used"), false},
                {QStringLiteral("desktop_files_changed"), false}}},
            {QStringLiteral("frames"), frames}}).toJson();
        QSaveFile file(directory.filePath(QStringLiteral("scene.json")));
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            app.exit(2); return;
        }
        app.exit(code);
    };
    const auto check = [&](const QString& key, bool ok) {
        checks.insert(key, ok);
        if (!ok) finish(1, key);
        return ok;
    };
    const auto mouse = [&](QEvent::Type type, const QPointF& global,
                           Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, global - QPointF(window.pos()), global, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&window, &event);
    };
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        finish(2, QStringLiteral("wall_time_limit"));
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        QString event;
        if (!check(QStringLiteral("native_backend"), window.renderBackend() == QStringLiteral("cubism_native"))
            || !check(QStringLiteral("isolated_settings"), app.property("desktopCompanionReviewSettingsIsolated").toBool())) return;
        if (frame == 3) {
            pressLocal = QPointF(window.width() * 0.5, window.height() * 0.9);
            if (!check(QStringLiteral("body_press_not_head"), !window.isHeadAt(pressLocal))) return;
            windowOrigin = window.pos();
            currentGlobal = pressGlobal = pressLocal + QPointF(windowOrigin);
            mouse(QEvent::MouseButtonPress, currentGlobal, Qt::LeftButton, Qt::LeftButton);
            if (!check(QStringLiteral("body_press_pending"),
                    window.dragReviewState().value(QStringLiteral("gesture")).toString() == QStringLiteral("pending"))) return;
            event = QStringLiteral("body_press");
        }
        if (frame >= 5 && frame <= 12) {
            currentGlobal = pressGlobal + QPointF((frame - 4) * 20, 0);
            mouse(QEvent::MouseMove, currentGlobal, Qt::NoButton, Qt::LeftButton);
            event = QStringLiteral("actual_mouse_move_20px");
        }
        if (frame == releaseFrame) {
            if (movingRelease) {
                const auto& values = window.motionParameters();
                const double eyeX = values.value(QStringLiteral("ParamEyeBallX"));
                releaseEyesBefore = {eyeX, values.value(QStringLiteral("ParamEyeBallY"))};
                if (!check(QStringLiteral("release_starts_with_eye_offset"), std::abs(eyeX) > 0.03)) return;
            }
            if (kind == QStringLiteral("delete")) {
                controller.desktopItemDeleted();
                event = QStringLiteral("delete_interrupt");
            } else if (kind == QStringLiteral("grass")) {
                controller.playGrass();
                event = QStringLiteral("grass_interrupt");
            } else if (kind == QStringLiteral("ungrab")) {
                QEvent cancel(QEvent::UngrabMouse);
                QApplication::sendEvent(&window, &cancel);
                event = QStringLiteral("ungrab");
            } else {
                mouse(QEvent::MouseButtonRelease, currentGlobal, Qt::LeftButton, Qt::NoButton);
                event = QStringLiteral("mouse_release");
            }
        }
        if (frame == releaseFrame + 1) {
            // No fresh press: a stale move must not restart a window drag.
            currentGlobal += QPointF(20, 0);
            mouse(QEvent::MouseMove, currentGlobal, Qt::NoButton, Qt::LeftButton);
            event = QStringLiteral("stale_move_after_release");
        }
        if (frame == 44 && kind == QStringLiteral("ungrab")) {
            QEvent duplicate(QEvent::UngrabMouse);
            QApplication::sendEvent(&window, &duplicate);
            event = QStringLiteral("duplicate_ungrab");
        }
        const QString name = QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'));
        if (!window.renderLiveInteractionFrame(kStep, directory.filePath(name))) {
            finish(2, QStringLiteral("native_frame_capture_failed")); return;
        }
        const auto drag = window.dragReviewState();
        const bool following = drag.value(QStringLiteral("following")).toBool();
        sawDrag |= following;
        if (!movingRelease && frame >= 12 && frame < releaseFrame) {
            if (!check(QStringLiteral("actual_path_stays_160_while_still"),
                    std::abs(drag.value(QStringLiteral("travelled")).toDouble() - 160) < 1e-6)
                || !check(QStringLiteral("still_pointer_keeps_drag_owner"), following
                    && drag.value(QStringLiteral("dragging")).toBool()
                    && drag.value(QStringLiteral("gesture")).toString() == QStringLiteral("drag"))
                || !check(QStringLiteral("no_false_long_drag_curiosity"),
                    !drag.value(QStringLiteral("curious_triggered")).toBool())) return;
        }
        if (!movingRelease && frame == 39 && !check(QStringLiteral("still_input_expires"),
                drag.value(QStringLiteral("input_age")).toDouble() >= 1.3)) return;
        if (frame >= releaseFrame) {
            if (!check(QStringLiteral("released_pointer_stays_clear"), !following
                    && !drag.value(QStringLiteral("dragging")).toBool()
                    && drag.value(QStringLiteral("gesture")).toString() == QStringLiteral("none"))) return;
            if (interrupted && !check(QStringLiteral("foreground_cancels_drag_immediately"),
                    !drag.value(QStringLiteral("drag_motion_active")).toBool())) return;
        }
        if (frame >= releaseFrame + 52 && !check(QStringLiteral("settle_finishes_without_replay"),
                !drag.value(QStringLiteral("drag_motion_active")).toBool())) return;
        QJsonObject parameters;
        const auto values = window.motionParameters();
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!std::isfinite(it.value())) { finish(2, QStringLiteral("non_finite_parameter")); return; }
            parameters.insert(it.key(), it.value());
        }
        if (busy) {
            bool seated = controller.activeTurnCount() == 1 && controller.state() == PetController::State::Busy;
            for (const auto* axis : {"ParamSitPose", "ParamBusyLaptop", "ParamDeskVisible", "ParamLaptopVisible"})
                seated = seated && parameters.value(QString::fromLatin1(axis)).toDouble() >= 0.98;
            if (!check(QStringLiteral("busy_pose_and_real_turn_preserved"), seated)) return;
        }
        if (!movingRelease && frame == 39) pausedParameters = parameters;
        if (movingRelease && frame == releaseFrame)
            releaseEyesAfter = {parameters.value(QStringLiteral("ParamEyeBallX")),
                                parameters.value(QStringLiteral("ParamEyeBallY"))};
        frames.append(QJsonObject{{QStringLiteral("frame"), frame}, {QStringLiteral("file"), name},
            {QStringLiteral("time"), (frame + 1) * kStep}, {QStringLiteral("dt"), kStep},
            {QStringLiteral("event"), event}, {QStringLiteral("state"), int(controller.state())},
            {QStringLiteral("active_turns"), controller.activeTurnCount()},
            {QStringLiteral("parameters"), parameters}, {QStringLiteral("drag"), drag},
            {QStringLiteral("window_position"), point(QPointF(window.pos()))},
            {QStringLiteral("pointer_global"), point(currentGlobal)}});
        if (++frame == kFrames) {
            if (!check(QStringLiteral("drag_seen"), sawDrag)
                || !check(QStringLiteral("actual_window_displacement_160"), window.pos() == windowOrigin + QPoint(160, 0))
                || !check(QStringLiteral("final_pointer_and_motion_clear"),
                    !drag.value(QStringLiteral("drag_motion_active")).toBool()
                    && !following && !drag.value(QStringLiteral("dragging")).toBool())) return;
            finish(0);
        }
    });
    deadline.start(55000);
    QTimer::singleShot(1000, &app, [&] {
        if (finished) return;
        if (busy) {
            controller.turnStarted(QStringLiteral("drag-review-fixture"), QStringLiteral("work"));
            window.forceBusyLaptopForReview();
            for (int i = 0; i < 50; ++i)
                if (!window.renderLiveInteractionFrame(0.1, directory.filePath(QStringLiteral("warmup.png")))) {
                    finish(2, QStringLiteral("busy_warmup_failed")); return;
                }
        }
        timer.start();
    });
    return app.exec();
}
