#include "GrassDragInputReview.h"

#include "PetController.h"
#include "PetWindow.h"

#include <QAction>
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

#include <algorithm>
#include <cmath>

namespace {
constexpr double kStep = 1.0 / 15.0;
constexpr int kFrames = 156;
QPointF point(const QJsonValue& value) {
    const auto a = value.toArray();
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}
QJsonArray pair(const QPointF& p) { return {p.x(), p.y()}; }
bool findTip(const PetWindow& window, QPointF& result) {
    const auto path = window.grassTipHitPath();
    const auto bounds = path.boundingRect();
    if (path.contains(bounds.center())) { result = bounds.center(); return true; }
    for (double y = bounds.top(); y <= bounds.bottom(); y += 1.0)
        for (double x = bounds.left(); x <= bounds.right(); x += 1.0)
            if (path.contains(QPointF(x, y))) { result = {x, y}; return true; }
    return false;
}
} // namespace

int runGrassDragInputReview(QApplication& app, PetController& controller,
                           PetWindow& window, const QString& output,
                           const QString& scenario) {
    const QString kind = scenario.mid(QStringLiteral("grass-drag-").size());
    const QStringList kinds{QStringLiteral("right"), QStringLiteral("left"),
        QStringLiteral("up"), QStringLiteral("down"), QStringLiteral("limit"),
        QStringLiteral("timeout"), QStringLiteral("delete"), QStringLiteral("ungrab"),
        QStringLiteral("deactivate"), QStringLiteral("hide"), QStringLiteral("disable"),
        QStringLiteral("tap"), QStringLiteral("default"), QStringLiteral("approved")};
    const QDir directory(QFileInfo(output).absoluteFilePath());
    if (!scenario.startsWith(QStringLiteral("grass-drag-")) || !kinds.contains(kind)
        || !directory.exists()) return 2;
    const bool adopted = window.grassDragReviewState().value(QStringLiteral("approved")).toBool();
    // The old draft action disappears on adoption. Its historic cancellation
    // evidence remains readable; current preview-off ownership is reviewed by
    // the dedicated approved scene instead.
    if ((adopted && kind == QStringLiteral("disable"))
        || (!adopted && kind == QStringLiteral("approved"))) return 2;
    const bool ordinary = !adopted && kind == QStringLiteral("default");
    const bool tap = kind == QStringLiteral("tap");
    const bool timeout = kind == QStringLiteral("timeout");
    const bool interruption = QStringList{QStringLiteral("delete"), QStringLiteral("ungrab"),
        QStringLiteral("deactivate"), QStringLiteral("hide"), QStringLiteral("disable")}.contains(kind);

    window.prepareLiveInteractionReview(560);
    window.setInteractionPreviewEnabled(false);
    window.setGrassDragPreviewEnabled(!adopted && !ordinary);
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
    int frame = 0;
    bool finished = false, pressed = false, released = false, interrupted = false, previewOffChecked = false;
    bool sawHold = false, sawActive = false, sawMoved = false, sawRespond = false;
    bool sawTimeout = false, sawSettling = false, sawBusyResume = false;
    double holdStart = -1, clearAfter = -1;
    QPointF pressPoint, mousePoint, normal;
    double span = 0;
    QString failure;

    const auto finish = [&](int code, const QString& reason = {}) {
        if (finished) return;
        finished = true;
        timer.stop(); deadline.stop();
        if (!reason.isEmpty()) failure = reason;
        QSaveFile file(directory.filePath(QStringLiteral("scene.json")));
        file.setDirectWriteFallback(false);
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), adopted ? QStringLiteral("native_soft_grass_approved_synthetic_qt_input")
                                              : QStringLiteral("native_soft_grass_candidate_synthetic_qt_input")},
            {QStringLiteral("adoption"), adopted ? QStringLiteral("approved") : QStringLiteral("pending")},
            {QStringLiteral("passed"), code == 0}, {QStringLiteral("failure"), failure},
            {QStringLiteral("wall_ms"), wall.elapsed()},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("frame_count"), frames.size()},
            {QStringLiteral("checks"), checks},
            {QStringLiteral("privacy"), QJsonObject{
                {QStringLiteral("fixture_only"), true},
                {QStringLiteral("isolated_settings"), app.property("desktopCompanionReviewSettingsIsolated").toBool()},
                {QStringLiteral("default_endpoint_used"), false},
                {QStringLiteral("real_hooks_used"), false},
                {QStringLiteral("desktop_files_changed"), false}}},
            {QStringLiteral("final_drag"), window.grassDragReviewState()},
            {QStringLiteral("frames"), frames}}).toJson();
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
    const auto mouse = [&](QEvent::Type type, const QPointF& local,
                           Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, local, local + QPointF(window.pos()), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&window, &event);
    };
    const auto release = [&] {
        mouse(QEvent::MouseButtonRelease, mousePoint, Qt::LeftButton, Qt::NoButton);
        released = true;
    };

    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        finish(2, QStringLiteral("wall_time_limit"));
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        const double now = frame * kStep;
        QString event;
        if (!check(QStringLiteral("native_backend"), window.renderBackend() == QStringLiteral("cubism_native"))
            || !check(QStringLiteral("isolated_settings"), app.property("desktopCompanionReviewSettingsIsolated").toBool())) return;
        if (frame == 0) {
            const auto initial = window.grassDragReviewState();
            if (!check(adopted ? QStringLiteral("approved_no_preview_entry") : QStringLiteral("candidate_opt_in_only"),
                    initial.value(QStringLiteral("enabled")).toBool() == !ordinary
                    && !window.interactionPreviewEnabled()
                    && (!adopted || (initial.value(QStringLiteral("approved")).toBool()
                        && !initial.value(QStringLiteral("preview_enabled")).toBool())))) return;
        }
        if (frame == 3) {
            if (kind == QStringLiteral("approved")) {
                const QPointF body(window.width() * 0.5, window.height() * 0.5);
                mouse(QEvent::MouseButtonDblClick, body, Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, body, Qt::LeftButton, Qt::NoButton);
                if (!check(QStringLiteral("grass_started"), window.grassInteractionPhase() == QStringLiteral("enter"))) return;
                event = QStringLiteral("default_double_click");
            } else {
                if (!check(QStringLiteral("grass_started"), window.startGrassInteraction())) return;
                event = QStringLiteral("start_approved_grass");
            }
        }
        if (frame == 15) {
            controller.turnStarted(QStringLiteral("grass-drag-fixture"), QStringLiteral("work"));
            if (!check(QStringLiteral("background_start_keeps_grass"),
                    controller.activeTurnCount() == 1 && controller.state() == PetController::State::Grass)) return;
            event = QStringLiteral("real_background_turn_start");
        }
        const QString phase = window.grassInteractionPhase();
        if (phase == QStringLiteral("hold")) {
            sawHold = true;
            if (holdStart < 0) holdStart = now;
        }
        const double holdAge = holdStart < 0 ? -1 : now - holdStart;
        if (!pressed && phase == QStringLiteral("hold") && holdAge >= 0.15 - 1e-6) {
            if (!check(QStringLiteral("actual_tip_hit_found"), findTip(window, pressPoint)
                    && window.isGrassTipAt(pressPoint))) return;
            mousePoint = pressPoint;
            mouse(QEvent::MouseButtonPress, pressPoint, Qt::LeftButton, Qt::LeftButton);
            pressed = true;
            const auto drag = window.grassDragReviewState();
            if (ordinary) {
                if (!check(QStringLiteral("default_press_responds"),
                        window.grassInteractionPhase() == QStringLiteral("respond")
                        && !drag.value(QStringLiteral("active")).toBool())) return;
            } else {
                normal = point(drag.value(QStringLiteral("normal")));
                span = drag.value(QStringLiteral("span")).toDouble();
                if (!check(QStringLiteral("candidate_press_captures"),
                        drag.value(QStringLiteral("pressed")).toBool()
                        && drag.value(QStringLiteral("active")).toBool()
                        && window.grassInteractionPhase() == QStringLiteral("hold")
                        && span >= 12 && std::isfinite(span)
                        && std::abs(std::hypot(normal.x(), normal.y()) - 1) < 1e-5)) return;
            }
            event = ordinary ? QStringLiteral("default_tip_press") : QStringLiteral("candidate_tip_press");
        }
        if (pressed && !released && !interrupted && !ordinary && !tap
            && phase == QStringLiteral("hold")) {
            const double progress = std::clamp((holdAge - 0.15) / 0.35, 0.0, 1.0);
            QPointF delta = normal * span;
            if (kind == QStringLiteral("left")) delta = -delta;
            if (kind == QStringLiteral("up")) delta = {0, -span};
            if (kind == QStringLiteral("down")) delta = {0, span};
            if (kind == QStringLiteral("limit")) delta *= 4;
            mousePoint = pressPoint + delta * progress;
            mouse(QEvent::MouseMove, mousePoint, Qt::NoButton, Qt::LeftButton);
            if (event.isEmpty()) event = QStringLiteral("qt_tip_pull");
        }
        if (kind == QStringLiteral("approved") && pressed && !previewOffChecked && holdAge >= 0.7 - 1e-6) {
            const double force = window.grassDragReviewState().value(QStringLiteral("offset")).toDouble();
            window.setInteractionPreviewEnabled(false);
            window.setGrassDragPreviewEnabled(false);
            const auto drag = window.grassDragReviewState();
            if (!check(QStringLiteral("approved_preview_off_keeps_drag"), force > 0.3
                    && drag.value(QStringLiteral("active")).toBool()
                    && drag.value(QStringLiteral("pressed")).toBool()
                    && !drag.value(QStringLiteral("preview_enabled")).toBool()
                    && !window.interactionPreviewEnabled()
                    && std::abs(drag.value(QStringLiteral("offset")).toDouble() - force) < 1e-9)) return;
            previewOffChecked = true;
            event = QStringLiteral("approved_preview_off_keeps_drag");
        }
        if (pressed && !released && !interrupted && interruption && holdAge >= 0.7 - 1e-6) {
            if (kind == QStringLiteral("delete")) controller.desktopItemDeleted();
            else if (kind == QStringLiteral("hide")) { window.hide(); window.show(); }
            else if (kind == QStringLiteral("disable")) {
                const auto actions = window.findChildren<QAction*>(QStringLiteral("grassDragPreviewAction"));
                if (!check(QStringLiteral("draft_action_available"), actions.size() == 1
                        && actions.front()->isChecked())) return;
                actions.front()->trigger();
            } else {
                QEvent cancel(kind == QStringLiteral("ungrab") ? QEvent::UngrabMouse : QEvent::WindowDeactivate);
                QApplication::sendEvent(&window, &cancel);
            }
            interrupted = true;
            clearAfter = now;
            event = QStringLiteral("cancel_") + kind;
        }
        if (pressed && !released && !interrupted && (ordinary || tap || !timeout)
            && holdAge >= (tap || ordinary ? 0.35 : 1.0) - 1e-6) {
            release();
            event = tap || ordinary ? QStringLiteral("qt_tip_tap_release") : QStringLiteral("qt_tip_drag_release");
            if (tap && !check(QStringLiteral("candidate_tap_responds"),
                    window.grassInteractionPhase() == QStringLiteral("respond"))) return;
            if (!tap && !ordinary && !check(QStringLiteral("drag_release_stays_hold"),
                    window.grassInteractionPhase() == QStringLiteral("hold")
                    && window.grassDragReviewState().value(QStringLiteral("settling")).toBool())) return;
        }
        if (pressed && !released && ((timeout && holdAge >= 3.2) || (interrupted && now > clearAfter))) {
            release();
            event = QStringLiteral("late_release_after_cancel");
        }
        if (frame == 147) {
            controller.turnStopped(QStringLiteral("grass-drag-fixture"), QStringLiteral("work"));
            event = QStringLiteral("real_background_turn_stop");
        }

        const QString name = QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'));
        if (!window.renderLiveInteractionFrame(kStep, directory.filePath(name))) {
            finish(2, QStringLiteral("native_frame_capture_failed")); return;
        }
        const auto drag = window.grassDragReviewState();
        const auto pointer = window.pointerReviewState();
        const auto afterPhase = window.grassInteractionPhase();
        sawActive |= drag.value(QStringLiteral("active")).toBool();
        sawMoved |= drag.value(QStringLiteral("moved")).toBool();
        sawSettling |= drag.value(QStringLiteral("settling")).toBool();
        sawRespond |= afterPhase == QStringLiteral("respond");
        sawTimeout |= afterPhase == QStringLiteral("timeout");
        sawBusyResume |= now > 6 && now < 9.8 && controller.state() == PetController::State::Busy;
        if (!check(QStringLiteral("grass_does_not_drag_window"),
                !pointer.value(QStringLiteral("dragging")).toBool()
                && pointer.value(QStringLiteral("gesture")).toString() == QStringLiteral("none"))) return;
        if (interrupted || (pressed && afterPhase != QStringLiteral("hold"))) {
            if (!check(QStringLiteral("tracking_clears_after_owner_change"),
                    (!interrupted || !drag.value(QStringLiteral("pressed")).toBool())
                    && !drag.value(QStringLiteral("active")).toBool()
                    && std::abs(drag.value(QStringLiteral("offset")).toDouble()) < 1e-8)) return;
        }
        QJsonObject parameters;
        const auto values = window.motionParameters();
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!std::isfinite(it.value())) { finish(2, QStringLiteral("non_finite_parameter")); return; }
            parameters.insert(it.key(), it.value());
        }
        frames.append(QJsonObject{{QStringLiteral("frame"), frame}, {QStringLiteral("file"), name},
            {QStringLiteral("time"), (frame + 1) * kStep}, {QStringLiteral("dt"), kStep},
            {QStringLiteral("event"), event}, {QStringLiteral("state"), static_cast<int>(controller.state())},
            {QStringLiteral("active_turns"), controller.activeTurnCount()},
            {QStringLiteral("grass_phase"), afterPhase},
            {QStringLiteral("grass_time"), window.grassInteractionTime()},
            {QStringLiteral("parameters"), parameters}, {QStringLiteral("pointer"), pointer},
            {QStringLiteral("drag"), drag}, {QStringLiteral("press_point"), pair(pressPoint)},
            {QStringLiteral("mouse_point"), pair(mousePoint)}});
        if (++frame == kFrames) {
            const auto final = window.grassDragReviewState();
            if (!check(QStringLiteral("hold_and_press_seen"), sawHold && pressed)
                || !check(QStringLiteral("real_turn_finished_idle"), controller.activeTurnCount() == 0
                    && controller.state() == PetController::State::Idle)
                || !check(QStringLiteral("final_tracking_clear"), !final.value(QStringLiteral("pressed")).toBool()
                    && !final.value(QStringLiteral("active")).toBool() && !final.value(QStringLiteral("settling")).toBool())
                || !check(QStringLiteral("candidate_or_default_entry"), ordinary ? !sawActive : sawActive)
                || !check(QStringLiteral("only_taps_respond"), sawRespond == (ordinary || tap))
                || !check(QStringLiteral("drag_threshold_or_tap"), ordinary || tap ? !sawMoved : sawMoved)
                || !check(QStringLiteral("release_rebounds"), ordinary || tap || interruption || timeout || sawSettling)
                || !check(QStringLiteral("timeout_branch"), interruption ? true : sawTimeout == !(ordinary || tap))
                || !check(QStringLiteral("background_resumes"), kind == QStringLiteral("delete") || sawBusyResume)) return;
            finish(0);
        }
    });
    deadline.start(55000);
    QTimer::singleShot(1000, &app, [&] { if (!finished) timer.start(); });
    return app.exec();
}
