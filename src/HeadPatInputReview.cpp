#include "HeadPatInputReview.h"

#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QTimer>

#include <cmath>
#include <functional>

namespace {
constexpr double kStep = 0.1;
const QString kHeadPat = QStringLiteral("head-pat");

QJsonObject captureContext(const PetWindow& window) {
    const QString overridePath = qApp->property("desktopCompanionReviewModelPath").toString();
    const QString modelPath = overridePath.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(
              QStringLiteral("assets/live2d/whale-girl/whale-girl-layered-draft.model3.json"))
        : overridePath;
    return {{QStringLiteral("model_manifest_path"), QFileInfo(modelPath).absoluteFilePath()},
        {QStringLiteral("isolated_model_override"), !overridePath.isEmpty()},
        {QStringLiteral("desk_work_mode"), qApp->property("desktopCompanionDeskWorkMode").toBool()},
        {QStringLiteral("render_backend"), window.renderBackend()},
        {QStringLiteral("render_error"), window.renderError()},
        {QStringLiteral("adoption"), QStringLiteral("not_requested")}};
}

bool findHeadPoint(const PetWindow& window, QPointF* point) {
    // Prefer the middle of the face/hair, with room for the complete stroke.
    double bestDistance = 1e9;
    bool found = false;
    const QPointF preferred(window.width() * 0.5, window.height() * 0.3);
    for (int y = window.height() / 10; y < window.height() * 7 / 10; y += 3) {
        for (int x = window.width() / 4; x < window.width() * 3 / 4; x += 3) {
            const QPointF candidate(x, y);
            if (!window.isHeadAt(candidate)
                || !window.isHeadAt(candidate + QPointF(16, 0))
                || !window.isHeadAt(candidate - QPointF(16, 0))) continue;
            const QPointF delta = candidate - preferred;
            const double distance = delta.x() * delta.x() + delta.y() * delta.y();
            if (distance < bestDistance) {
                bestDistance = distance;
                *point = candidate;
                found = true;
            }
        }
    }
    return found;
}

bool findBodyPoint(const PetWindow& window, QPointF* point) {
    for (const double y : {0.72, 0.8, 0.65, 0.9}) {
        const QPointF candidate(window.width() * 0.5, window.height() * y);
        if (!window.isHeadAt(candidate)) {
            *point = candidate;
            return true;
        }
    }
    return false;
}
} // namespace

int runHeadPatInputReview(QApplication& app, PetController& controller,
                         PetWindow& window, const QString& output,
                         const QString& scenario) {
    const QStringList scenes{
        QStringLiteral("head-pat-input-left"), QStringLiteral("head-pat-input-right"),
        QStringLiteral("head-pat-input-busy-left"), QStringLiteral("head-pat-input-busy-right"),
        QStringLiteral("head-pat-input-ungrab"), QStringLiteral("head-pat-input-deactivate"),
        QStringLiteral("head-pat-input-hide"), QStringLiteral("head-pat-input-delete"),
        QStringLiteral("head-pat-input-grass"), QStringLiteral("head-pat-input-wave"),
        QStringLiteral("head-pat-input-drag")};
    if (!scenes.contains(scenario) || !QDir().mkpath(output)) return 2;
    const QDir directory(QFileInfo(output).absoluteFilePath());
    const bool busy = scenario.contains(QStringLiteral("-busy-"));
    const bool wave = scenario.endsWith(QStringLiteral("-wave"));
    const bool drag = scenario.endsWith(QStringLiteral("-drag"));
    const bool stroke = scenario.endsWith(QStringLiteral("-left"))
        || scenario.endsWith(QStringLiteral("-right"));
    const bool cancellation = !stroke && !wave && !drag;
    const double direction = scenario.endsWith(QStringLiteral("-left")) ? -1.0 : 1.0;

    window.prepareLiveInteractionReview(280);
    window.setInteractionPreviewEnabled(false);
    window.move(-10000, -10000);
    window.show();

    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(100);
    QTimer deadline;
    deadline.setSingleShot(true);
    QElapsedTimer clock;
    clock.start();
    QJsonArray frames;
    QJsonObject checks;
    QPointF pressPoint;
    double simulatedTime = 0.0;
    double patAge = 0.0;
    double releasedAt = -1.0;
    qint64 pressedWallMs = -1;
    int frame = 0;
    bool finished = false, busyStarted = false, pressed = false;
    bool released = false, sawPat = false, sawAutoRelease = false, autoEnded = false;
    bool sawWave = false, sawDrag = false;
    QString failure;

    const auto mouse = [&](QEvent::Type type, const QPointF& local,
                           Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, local, local + QPointF(window.pos()), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&window, &event);
    };
    const auto release = [&] {
        mouse(QEvent::MouseButtonRelease, pressPoint, Qt::LeftButton, Qt::NoButton);
        released = true;
        releasedAt = simulatedTime;
    };
    const auto finish = [&](int code, const QString& reason = {}) {
        if (finished) return;
        finished = true;
        timer.stop();
        deadline.stop();
        if (!reason.isEmpty()) failure = reason;
        QFile evidence(directory.filePath(QStringLiteral("scene.json")));
        if (!evidence.open(QIODevice::WriteOnly)) { app.exit(1); return; }
        evidence.write(QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), QStringLiteral("window_player_and_synthetic_qt_pointer_with_native_model")},
            {QStringLiteral("capture"), captureContext(window)},
            {QStringLiteral("passed"), code == 0}, {QStringLiteral("failure"), failure},
            {QStringLiteral("wall_ms"), clock.elapsed()},
            {QStringLiteral("checks"), checks},
            {QStringLiteral("final_pointer"), window.pointerReviewState()},
            {QStringLiteral("frames"), frames}}).toJson());
        evidence.close();
        app.exit(code);
    };
    const auto capture = [&](const QString& event) {
        if (finished) return false;
        if (frame >= 120) { finish(2, QStringLiteral("frame_limit")); return false; }
        const QString path = directory.filePath(QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0')));
        if (!window.renderLiveInteractionFrame(kStep, path)) {
            finish(2, QStringLiteral("native_frame_capture_failed"));
            return false;
        }
        simulatedTime += kStep;
        QJsonObject parameters;
        const auto values = window.motionParameters();
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!std::isfinite(it.value())) {
                finish(2, QStringLiteral("non_finite_parameter:") + it.key());
                return false;
            }
            parameters.insert(it.key(), it.value());
        }
        frames.append(QJsonObject{{QStringLiteral("frame"), frame++},
            {QStringLiteral("time"), simulatedTime}, {QStringLiteral("dt"), kStep},
            {QStringLiteral("wall_ms"), clock.elapsed()}, {QStringLiteral("event"), event},
            {QStringLiteral("pointer"), window.pointerReviewState()},
            {QStringLiteral("parameters"), parameters},
            {QStringLiteral("state"), static_cast<int>(controller.state())},
            {QStringLiteral("active_turns"), controller.activeTurnCount()},
            {QStringLiteral("pressed_head_hit"), pressed && window.isHeadAt(pressPoint)},
            {QStringLiteral("press_point"), QJsonArray{pressPoint.x(), pressPoint.y()}}});
        return true;
    };

    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        finish(2, QStringLiteral("wall_time_limit_or_required_transition_missing"));
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        QString event;
        if (window.renderBackend() != QStringLiteral("cubism_native")) {
            finish(2, QStringLiteral("native_backend_required"));
            return;
        }
        if (!window.pointerReviewState().value(QStringLiteral("head_pat_approved")).toBool()
            || window.interactionPreviewEnabled()) {
            finish(2, QStringLiteral("approved_default_entry_required"));
            return;
        }
        if (busy && !busyStarted) {
            controller.turnStarted(QStringLiteral("head-pat-input-review"), QStringLiteral("busy"));
            window.forceBusyLaptopForReview();
            busyStarted = true;
            event = QStringLiteral("real_turn_start_force_seated_review_variant");
        }
        const double settleTime = busy ? 2.2 : 0.6;
        if (!pressed && simulatedTime >= settleTime - 1e-6) {
            if (busy) {
                const auto pose = window.motionParameters();
                const bool seated = controller.state() == PetController::State::Busy
                    && controller.activeTurnCount() == 1
                    && pose.value(QStringLiteral("ParamSitPose")) > 0.98
                    && pose.value(QStringLiteral("ParamDeskVisible")) > 0.98
                    && pose.value(QStringLiteral("ParamLaptopVisible")) > 0.98;
                checks.insert(QStringLiteral("seated_before_stroke"), seated);
                if (!seated) { finish(2, QStringLiteral("busy_pose_not_settled_after_two_seconds")); return; }
            }
            if (!(cancellation ? findBodyPoint(window, &pressPoint) : findHeadPoint(window, &pressPoint))) {
                finish(2, QStringLiteral("required_input_hit_region_not_found"));
                return;
            }
            const bool hit = window.isHeadAt(pressPoint);
            checks.insert(QStringLiteral("press_head_hit"), hit);
            if (hit == cancellation) { finish(2, QStringLiteral("wrong_press_hit_region")); return; }
            pressed = true;
            pressedWallMs = clock.elapsed();
            mouse(QEvent::MouseButtonPress, pressPoint, Qt::LeftButton, Qt::LeftButton);
            const auto pending = window.pointerReviewState();
            if (pending.value(QStringLiteral("gesture")).toString() != QStringLiteral("pending")
                || !pending.value(QStringLiteral("long_press_pending")).toBool()) {
                finish(2, QStringLiteral("press_did_not_enter_pending_with_long_press"));
                return;
            }
            if (stroke) {
                // Recognition and both manual hold ticks share a callback, so
                // a slow GL capture cannot let the wall-clock greeting win.
                mouse(QEvent::MouseMove, pressPoint + QPointF(direction * 16, 0), Qt::NoButton, Qt::LeftButton);
                mouse(QEvent::MouseMove, pressPoint + QPointF(direction * 12, 0), Qt::NoButton, Qt::LeftButton);
                if (!capture(QStringLiteral("head_press_stroke_out_and_return"))
                    || !capture(QStringLiteral("stroke_hold_threshold"))) return;
                const auto state = window.pointerReviewState();
                sawPat = state.value(QStringLiteral("reaction")).toString() == kHeadPat
                    && state.value(QStringLiteral("gesture")).toString() == QStringLiteral("pat")
                    && !state.value(QStringLiteral("long_press_pending")).toBool();
                checks.insert(QStringLiteral("real_pointer_started_head_pat"), sawPat);
                if (!sawPat) { finish(2, QStringLiteral("default_head_pat_not_triggered")); return; }
                patAge = kStep;
                return;
            }
            if (drag) {
                mouse(QEvent::MouseMove, pressPoint + QPointF(24, 0), Qt::NoButton, Qt::LeftButton);
                const auto state = window.pointerReviewState();
                sawDrag = state.value(QStringLiteral("gesture")).toString() == QStringLiteral("drag")
                    && state.value(QStringLiteral("dragging")).toBool()
                    && state.value(QStringLiteral("drag_motion_active")).toBool()
                    && !state.value(QStringLiteral("long_press_pending")).toBool();
                checks.insert(QStringLiteral("quick_head_drag_initialized"), sawDrag);
                if (!sawDrag) { finish(2, QStringLiteral("quick_head_drag_not_initialized")); return; }
                event = QStringLiteral("head_press_move_beyond_18px");
            } else if (wave) {
                event = QStringLiteral("stationary_head_hold_for_wall_clock_greeting");
            } else {
                if (scenario.endsWith(QStringLiteral("-ungrab"))) {
                    QEvent cancellationEvent(QEvent::UngrabMouse);
                    QApplication::sendEvent(&window, &cancellationEvent);
                } else if (scenario.endsWith(QStringLiteral("-deactivate"))) {
                    QEvent cancellationEvent(QEvent::WindowDeactivate);
                    QApplication::sendEvent(&window, &cancellationEvent);
                } else if (scenario.endsWith(QStringLiteral("-hide"))) {
                    window.hide();
                    window.show();
                    window.prepareLiveInteractionReview(280);
                } else if (scenario.endsWith(QStringLiteral("-delete"))) {
                    controller.desktopItemDeleted(); // animation event only, no file operation
                    if (controller.state() != PetController::State::Delete) {
                        finish(2, QStringLiteral("delete_did_not_take_stage")); return;
                    }
                    controller.actionFinished();
                } else {
                    controller.playGrass();
                    if (controller.state() != PetController::State::Grass) {
                        finish(2, QStringLiteral("grass_did_not_take_stage")); return;
                    }
                    controller.actionFinished();
                }
                event = QStringLiteral("body_pending_cancel_then_return_idle");
                const auto state = window.pointerReviewState();
                const bool cleared = state.value(QStringLiteral("gesture")).toString() == QStringLiteral("none")
                    && !state.value(QStringLiteral("long_press_pending")).toBool()
                    && controller.state() == PetController::State::Idle;
                checks.insert(QStringLiteral("pending_cancelled_before_deadline"), cleared);
                if (!cleared) { finish(2, QStringLiteral("cancel_left_pending_input")); return; }
            }
        } else if (pressed && stroke && !released
                   && ((!busy && patAge >= 2.4 - 1e-6) || (busy && autoEnded))) {
            release();
            event = busy ? QStringLiteral("release_pointer_after_automatic_busy_recovery")
                         : QStringLiteral("release_after_two_hold_cycles");
            checks.insert(QStringLiteral("pointer_released"), true);
        } else if (pressed && drag && !released && clock.elapsed() - pressedWallMs >= 300) {
            release();
            event = QStringLiteral("drag_release_without_settings_persistence");
        }

        if (!capture(event)) return;
        if (!pressed) return;
        const auto state = window.pointerReviewState();
        const QString reaction = state.value(QStringLiteral("reaction")).toString();
        if (stroke) {
            if (!released) patAge += kStep;
            if (state.value(QStringLiteral("wave_active")).toBool()) {
                finish(2, QStringLiteral("greeting_stole_head_pat")); return;
            }
            if (busy) {
                const auto pose = window.motionParameters();
                if (controller.state() != PetController::State::Busy || controller.activeTurnCount() != 1
                    || pose.value(QStringLiteral("ParamSitPose")) < 0.98
                    || pose.value(QStringLiteral("ParamDeskVisible")) < 0.98
                    || pose.value(QStringLiteral("ParamLaptopVisible")) < 0.98) {
                    finish(2, QStringLiteral("busy_head_pat_lost_seated_work_pose")); return;
                }
                if (!released && reaction == kHeadPat && patAge >= 1.2
                    && state.value(QStringLiteral("reaction_time")).toDouble() >= 1.6) sawAutoRelease = true;
                if (!released && sawPat && reaction.isEmpty()) {
                    autoEnded = true;
                    checks.insert(QStringLiteral("busy_automatic_short_release"), sawAutoRelease);
                    if (!sawAutoRelease) { finish(2, QStringLiteral("busy_pat_ended_without_release_tail")); return; }
                }
            }
            if (released && reaction.isEmpty() && simulatedTime - releasedAt >= 0.5) {
                const bool cleared = state.value(QStringLiteral("gesture")).toString() == QStringLiteral("none")
                    && !state.value(QStringLiteral("long_press_pending")).toBool();
                checks.insert(QStringLiteral("recovered_after_release"), cleared);
                finish(cleared ? 0 : 2, cleared ? QString() : QStringLiteral("release_left_pointer_pending"));
            }
        } else if (cancellation) {
            const bool clean = state.value(QStringLiteral("gesture")).toString() == QStringLiteral("none")
                && !state.value(QStringLiteral("long_press_pending")).toBool()
                && !state.value(QStringLiteral("wave_active")).toBool()
                && reaction.isEmpty();
            if (!clean) { finish(2, QStringLiteral("cancelled_press_replayed_after_return_to_idle")); return; }
            if (clock.elapsed() - pressedWallMs >= 1000) {
                checks.insert(QStringLiteral("no_wave_past_600ms_wall_deadline"), true);
                finish(0);
            }
        } else if (wave) {
            if (state.value(QStringLiteral("wave_active")).toBool()) {
                sawWave = true;
                checks.insert(QStringLiteral("stationary_head_long_press_waved"), true);
            }
            if (!reaction.isEmpty()) { finish(2, QStringLiteral("stationary_hold_misclassified_as_pat")); return; }
            if (sawWave && !state.value(QStringLiteral("wave_active")).toBool()) {
                checks.insert(QStringLiteral("wave_recovered"), true);
                finish(0);
            }
        } else if (drag) {
            if (!reaction.isEmpty() || state.value(QStringLiteral("wave_active")).toBool()) {
                finish(2, QStringLiteral("drag_misclassified_as_head_pat_or_wave")); return;
            }
            if (released && simulatedTime - releasedAt >= 1.0
                && !state.value(QStringLiteral("drag_motion_active")).toBool()) {
                checks.insert(QStringLiteral("drag_released_and_recovered"), true);
                finish(0);
            }
        }
    });
    deadline.start(7800);
    QTimer::singleShot(800, &timer, [&] { timer.start(); });
    return app.exec();
}
