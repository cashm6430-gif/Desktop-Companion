#include "DeskPeekReview.h"

#include "MotionClip.h"
#include "PetWindow.h"
#include "motion/DeskPeekPose.h"
#include "motion/MotionMath.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QSaveFile>
#include <QTimer>
#include <cmath>

namespace {
QJsonObject parametersJson(const ParameterMotion::Parameters& values) {
    QJsonObject result;
    for (auto it = values.cbegin(); it != values.cend(); ++it) result.insert(it.key(), it.value());
    return result;
}

}

int runDeskPeekReview(QApplication& app, PetWindow& window, const QString& output,
                      const QString& scenario, const QString& clipPath) {
    const bool drag = scenario == QStringLiteral("desk-peek-standing-drag");
    const bool remove = scenario == QStringLiteral("desk-peek-laptop-delete");
    const bool seated = remove || scenario == QStringLiteral("desk-peek-laptop-left")
        || scenario == QStringLiteral("desk-peek-laptop-right");
    const bool left = scenario.endsWith(QStringLiteral("-left"));
    if (!drag && !seated && scenario != QStringLiteral("desk-peek-standing-left")
        && scenario != QStringLiteral("desk-peek-standing-right")) return 2;
    if (!app.property("desktopCompanionDeskWorkMode").toBool() || clipPath.isEmpty()) return 2;

    const QDir destination(output);
    MotionClip clip;
    ParameterMotion base;
    base.setDeskWorkMode(true);
    QString error;
    if (!clip.loadJson(clipPath, &error) || clip.approval() != QStringLiteral("pending")
        || clip.isLoop() || !clip.isAdditive() || clip.duration() > 6.0
        || !base.loadMotionLibrary(QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions")), &error)) {
        qWarning() << "Desk peek review:" << error;
        return 2;
    }
    base.setProceduralBlink(false); // one authored blink, reproducible for review
    if (seated) base.forceLaptopBusy();
    for (int i = 0; i < 120; ++i) base.advance(1.0 / 30.0);
    if (seated && base.deskWorkPhase() != QStringLiteral("work")) return 2;
    window.setPreviewPose(base.values(), 420);

    constexpr double step = 1.0 / 15.0;
    constexpr int start = 6;
    constexpr int interrupt = 25;
    constexpr double handoff = 0.13;
    const int frames = remove
        ? interrupt + static_cast<int>(std::ceil((base.actionDuration(PetController::State::Delete) + 1.2) / step))
        : start + static_cast<int>(std::ceil((clip.duration() + 0.65) / step));
    const bool interrupted = drag || remove;
    const double side = left ? -1.0 : 1.0;
    int frame = 0;
    QJsonArray trace;
    QTimer timer;
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        QString event;
        if (frame == start) event = QStringLiteral("peek");
        if (interrupted && frame == interrupt) {
            if (drag) {
                base.beginDragMotion();
                base.updateDragMotion(110.0, 0.0);
                event = QStringLiteral("drag_begin");
            } else {
                base.setState(PetController::State::Delete);
                event = QStringLiteral("delete_begin");
            }
        }
        if (drag && frame == interrupt + 8) {
            base.endDragMotion();
            event = QStringLiteral("drag_end");
        }
        if (remove && base.consumeActionFinished()) {
            base.setState(PetController::State::Busy);
            event = QStringLiteral("resume_busy");
        }
        const double local = (frame - start) * step;
        const bool yielding = interrupted && frame >= interrupt;
        // Hold the last rendered offset as it fades away; no new peek keys
        // can run after the handoff, even when the foreground action finishes.
        const double sampled = yielding ? (interrupt - start - 1) * step : local;
        const double weight = local < 0.0 || local >= clip.duration() ? 0.0
            : yielding ? 1.0 - motion::smooth((frame - interrupt + 1) * step / handoff) : 1.0;
        base.advance(step);
        auto pose = base.values();
        if (weight > 0.0) motion::applyDeskPeekPose(pose, clip.sample(sampled), side, weight, seated);
        const QString file = QStringLiteral("frame-%1.png").arg(frame, 3, 10, QLatin1Char('0'));
        if (window.renderBackend() != QStringLiteral("cubism_native")
            || !window.renderSequenceFrame(pose, step, destination.filePath(file), base.bubblePulse())) {
            app.exit(1);
            return;
        }
        trace.append(QJsonObject{{QStringLiteral("time"), frame * step}, {QStringLiteral("file"), file},
            {QStringLiteral("event"), event}, {QStringLiteral("weight"), weight},
            {QStringLiteral("desk_phase"), base.deskWorkPhase()},
            {QStringLiteral("base"), parametersJson(base.values())},
            {QStringLiteral("applied"), parametersJson(pose)}});
        if (++frame < frames) return;
        QFile receipt(destination.filePath(QStringLiteral("scene.json")));
        const QJsonDocument report(QJsonObject{{QStringLiteral("scenario"), scenario},
            {QStringLiteral("approval"), clip.approval()}, {QStringLiteral("revision"), clip.revision()},
            {QStringLiteral("render_backend"), window.renderBackend()}, {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("isolated_settings"), app.property("desktopCompanionReviewSettingsIsolated").toBool()},
            {QStringLiteral("scope"), QStringLiteral("candidate_layer_on_real_motion_player_native_render")},
            {QStringLiteral("start_frame"), start}, {QStringLiteral("step"), step},
            {QStringLiteral("frames"), trace}});
        app.exit(receipt.open(QIODevice::WriteOnly) && receipt.write(report.toJson()) > 0 ? 0 : 1);
    });
    QTimer::singleShot(1000, &timer, [&] { timer.start(); });
    return app.exec();
}

int runDeskPeekAdoptionReview(QApplication& app, PetController& controller, PetWindow& window,
                              const QString& output) {
    if (!app.property("desktopCompanionReviewSettingsIsolated").toBool()) return 2;
    window.prepareLiveInteractionReview(420);
    const QDir dir(output);
    QTimer::singleShot(1500, &app, [&] {
        auto& motion = window.motion_;
        auto& director = window.director_;
        director.setFrequency(TriggerDirector::Frequency::Normal);
        director.setChanceScale(1000.0);
        window.setInteractionPreviewEnabled(false);
        // Inject only clock and pointer direction. All nominations use the
        // real timer callback, window gate and approved player.
        const auto tick = [&](int count, double x) {
            window.deskPeekCursorX_ = x;
            while (count-- > 0) window.tickSpontaneousScenes();
        };
        const auto quiet = [&](bool on) { app.setProperty("desktopCompanionReviewSuppressAutomatic", on); };
        const auto clearIdle = [&] { quiet(true); tick(1, 0); quiet(false); };
        const auto value = [&](const char* id) { return motion.values().value(QString::fromLatin1(id)); };
        QJsonObject checks;
        const auto check = [&](const char* id, bool passed) { checks.insert(QString::fromLatin1(id), passed); };
        check("native_backend", window.renderBackend() == QStringLiteral("cubism_native"));
        quiet(true);
        tick(450, -2.0);
        check("quiet_drops_nomination", director.attemptCount() == 0 && !motion.deskPeekActive());
        quiet(false);
        tick(359, -2.0);
        check("fresh_idle_after_quiet", !motion.deskPeekActive() && director.startedCount() == 0);
        check("memo_visible", window.setUserMemo(QStringLiteral("fixture note")));
        tick(1, -2.0);
        check("visible_memo_owns_stage", !motion.deskPeekActive() && director.attemptCount() == 1);
        check("fold_note", window.foldMemo());
        tick(1, -2.0);
        check("rejected_nomination_not_queued", director.attemptCount() == 1 && !motion.deskPeekActive());
        clearIdle();
        tick(360, 0.05);
        check("ambiguous_direction_refused", director.attemptCount() == 2 && !motion.deskPeekActive());
        clearIdle();
        director.setFrequency(TriggerDirector::Frequency::Off);
        tick(365, -2.0);
        check("frequency_off", director.attemptCount() == 2 && !motion.deskPeekActive());
        director.setFrequency(TriggerDirector::Frequency::Normal);
        clearIdle();

        tick(360, -2.0);
        check("approved_normal_entry", motion.deskPeekActive() && director.startedCount() == 1
              && director.lastStarted() == QStringLiteral("desk-peek") && !window.interactionPreviewEnabled());
        QJsonArray frames;
        constexpr double dt = 1.0 / 30.0;
        bool rendered = true;
        bool protectedPose = true;
        double minimumEye = 1.0;
        const auto sample = [&](const QString& side, int i) {
            if (i % 2 == 0) {
                const QString file = QStringLiteral("%1-%2.png").arg(side).arg(i / 2, 3, 10, QLatin1Char('0'));
                rendered &= window.renderLiveInteractionFrame(dt, dir.filePath(file));
            } else window.advanceLiveFrame(dt);
            frames.append(QJsonObject{{QStringLiteral("side"), side}, {QStringLiteral("frame"), i},
                {QStringLiteral("active"), motion.deskPeekActive()},
                {QStringLiteral("eye_x"), value("ParamEyeBallX")}, {QStringLiteral("eye_open"), value("ParamEyeLOpen")},
                {QStringLiteral("head_x"), value("ParamAngleX")}, {QStringLiteral("body_z"), value("ParamBodyAngleZ")},
                {QStringLiteral("state"), int(controller.state())}});
        };
        for (int i = 0; i < 112; ++i) {
            // A direction sample on the other side cannot reverse the current animation.
            if (i == 18) window.deskPeekCursorX_ = 2.0;
            sample(QStringLiteral("left"), i);
            protectedPose &= value("ParamSitPose") == 0.0 && value("ParamLaptopVisible") == 0.0
                && value("ParamDeskVisible") == 0.0 && value("ParamArmLA") == 0.0 && value("ParamArmRA") == 0.0;
            if (i == 34) check("left_kept_after_mouse_change", value("ParamBodyAngleZ") < -2.5);
            if (motion.deskPeekActive()) minimumEye = std::min(minimumEye, value("ParamEyeLOpen"));
        }
        check("slow_blink_closes", minimumEye < 0.05);
        check("returns_to_idle", !motion.deskPeekActive() && value("ParamBodyAngleZ") == 0.0
              && controller.state() == PetController::State::Idle && protectedPose);

        // Clear continuous idle without rewinding time or the per-scene cooldown.
        quiet(true); tick(899, 2.0); quiet(false);
        tick(360, 2.0);
        check("right_side_eligible_after_cooldown", motion.deskPeekActive() && director.startedCount() == 2);
        for (int i = 0; i < 76; ++i) {
            if (i == 35) {
                check("right_peak", value("ParamBodyAngleZ") > 2.5);
                controller.turnStarted(QStringLiteral("desk-peek-review"), QStringLiteral("fixture"));
                check("agent_preempts_peek", !motion.deskPeekActive()
                      && controller.state() == PetController::State::Busy);
            }
            sample(QStringLiteral("right"), i);
        }
        check("busy_no_peek", !motion.playDeskPeek(2.0) && !motion.deskPeekActive());
        controller.turnStopped(QStringLiteral("desk-peek-review"), QStringLiteral("fixture"));
        window.advanceLiveFrame(dt);
        check("no_replay_when_turn_ends", !motion.deskPeekActive());
        check("native_frames_saved", rendered);
        bool passed = true;
        for (auto it = checks.constBegin(); it != checks.constEnd(); ++it) passed &= it.value().toBool();
        const QJsonObject report{{QStringLiteral("scenario"), QStringLiteral("desk-peek-adoption")},
            {QStringLiteral("passed"), passed}, {QStringLiteral("approval"), QStringLiteral("approved")},
            {QStringLiteral("scope"), QStringLiteral("normal_scheduler_window_and_native_motion; isolated settings and fixture input")},
            {QStringLiteral("checks"), checks}, {QStringLiteral("frames"), frames}};
        QSaveFile receipt(dir.filePath(QStringLiteral("scene.json")));
        const auto bytes = QJsonDocument(report).toJson();
        const bool saved = receipt.open(QIODevice::WriteOnly) && receipt.write(bytes) == bytes.size() && receipt.commit();
        app.exit(saved && passed ? 0 : 1);
    });
    return app.exec();
}

int runDeskPeekManualReview(QApplication& app, PetController& controller, PetWindow& window,
                            const QString& output) {
    if (!app.property("desktopCompanionReviewSettingsIsolated").toBool()) return 2;
    window.prepareLiveInteractionReview(280);
    window.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
    const QDir dir(output);
    QTimer::singleShot(1000, &app, [&] {
        using State = PetController::State;
        using Frequency = TriggerDirector::Frequency;
        auto& director = window.director_;
        auto& motion = window.motion_;
        QJsonObject checks;
        QJsonObject peaks;
        const auto check = [&](const char* id, bool passed) { checks.insert(QString::fromLatin1(id), passed); };
        const auto finish = [&] {
            bool passed = true;
            for (const auto& result : checks) passed &= result.toBool();
            const QJsonObject report{{QStringLiteral("scenario"), QStringLiteral("desk-peek-manual")},
                {QStringLiteral("passed"), passed},
                {QStringLiteral("scope"), QStringLiteral("isolated_native_window_with_qt_menu_actions_and_fixture_events")},
                {QStringLiteral("checks"), checks}, {QStringLiteral("peaks"), peaks}};
            QSaveFile receipt(dir.filePath(QStringLiteral("scene.json")));
            const auto bytes = QJsonDocument(report).toJson();
            const bool saved = receipt.open(QIODevice::WriteOnly) && receipt.write(bytes) == bytes.size()
                && receipt.commit();
            app.exit(saved && passed ? 0 : 1);
        };
        auto* interactions = window.trayMenu_.findChild<QMenu*>(QStringLiteral("interactionMenu"));
        auto* previews = window.trayMenu_.findChild<QMenu*>(QStringLiteral("developerPreviewMenu"));
        auto* menu = window.findChild<QMenu*>(QStringLiteral("deskPeekMenu"));
        QAction* left = nullptr;
        QAction* right = nullptr;
        if (menu) {
            for (auto* action : menu->actions()) {
                if (action->objectName() == QStringLiteral("deskPeekLeft")) left = action;
                if (action->objectName() == QStringLiteral("deskPeekRight")) right = action;
            }
        }
        check("native_window", window.isVisible() && window.renderBackend() == QStringLiteral("cubism_native"));
        check("has_two_manual_actions", left && right);
        check("ordinary_tray_interaction", interactions && menu
              && window.trayMenu_.actions().contains(interactions->menuAction())
              && interactions->actions().contains(menu->menuAction()));
        check("no_developer_preview_entry", previews && menu
              && !previews->actions().contains(menu->menuAction())
              && !previews->findChild<QMenu*>(QStringLiteral("deskPeekPreviewMenu")));
        if (!interactions || !left || !right) { finish(); return; }
        // Drive the real menu and action signals without popping a menu on the
        // user's desktop. The start is deferred until after the menu dismisses.
        const auto actionsAvailable = [&](bool available) {
            return QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection)
                && left->isEnabled() == available && right->isEnabled() == available;
        };
        const auto drainAction = [] {
            QEventLoop loop;
            QTimer::singleShot(1, &loop, &QEventLoop::quit);
            loop.exec();
        };
        const auto trigger = [&](QAction* action) { action->trigger(); drainAction(); };
        const auto quiet = [&](bool on) { app.setProperty("desktopCompanionReviewSuppressAutomatic", on); };
        const auto settle = [&] {
            for (int i = 0; i < 112; ++i) window.advanceLiveFrame(1.0 / 30.0);
        };
        const auto capturePeak = [&](const QString& side, double oppositeCursor) {
            for (int i = 0; i < 34; ++i) {
                window.deskPeekCursorX_ = oppositeCursor;
                window.advanceLiveFrame(1.0 / 30.0);
            }
            const double bodyZ = motion.values().value(QStringLiteral("ParamBodyAngleZ"));
            const QString file = side + QStringLiteral("-peak.png");
            const bool saved = window.saveRenderFrame(dir.filePath(file));
            peaks.insert(side, QJsonObject{{QStringLiteral("body_z"), bodyZ}, {QStringLiteral("file"), file},
                {QStringLiteral("saved"), saved}});
            return saved && bodyZ * oppositeCursor < -5.0;
        };
        const int attempts = director.attemptCount();
        const int starts = director.startedCount();
        const double clock = director.runningSeconds();

        director.setFrequency(Frequency::Off);
        check("left_and_right_enabled_in_standing_idle", actionsAvailable(true));
        window.deskPeekCursorX_ = 2.0;
        // Navigate the normal tray hierarchy without showing a popup on the user's desktop.
        auto& tray = window.trayMenu_;
        tray.setAttribute(Qt::WA_DontShowOnScreen);
        interactions->setAttribute(Qt::WA_DontShowOnScreen);
        menu->setAttribute(Qt::WA_DontShowOnScreen);
        const auto openSubmenu = [](QMenu& parent, QMenu& child) {
            parent.setActiveAction(child.menuAction());
            if (!child.isVisible()) {
                QKeyEvent rightArrow(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
                QApplication::sendEvent(&parent, &rightArrow);
            }
            return parent.isVisible() && child.isVisible();
        };
        tray.popup(window.pos());
        const bool opened = openSubmenu(tray, *interactions) && openSubmenu(*interactions, *menu);
        check("qt_tray_opens_nested_interaction", opened);
        menu->setActiveAction(left);
        const bool popupSelected = opened && menu->activeAction() == left;
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(menu, &enter);
        check("qt_selection_dismisses_tray_hierarchy", popupSelected && !menu->isVisible()
              && !interactions->isVisible() && !tray.isVisible());
        if (menu->isVisible()) menu->hide();
        if (interactions->isVisible()) interactions->hide();
        if (tray.isVisible()) tray.hide();
        drainAction();
        check("left_starts_with_automatic_and_preview_off", motion.deskPeekActive()
              && !window.interactionPreviewEnabled());
        check("left_peak_ignores_right_cursor", capturePeak(QStringLiteral("left"), 2.0));
        settle();
        check("left_returns_to_standing_idle", !motion.deskPeekInFlight() && controller.state() == State::Idle);

        director.setFrequency(Frequency::Normal);
        quiet(true);
        check("directions_remain_available_while_quiet", actionsAvailable(true));
        window.deskPeekCursorX_ = -2.0;
        trigger(right);
        check("right_starts_while_quiet", motion.deskPeekActive());
        check("right_peak_ignores_left_cursor", capturePeak(QStringLiteral("right"), -2.0));
        settle();
        check("right_returns_to_standing_idle", !motion.deskPeekInFlight() && controller.state() == State::Idle);
        check("manual_playback_never_records_an_attempt", director.attemptCount() == attempts
              && director.startedCount() == starts && director.runningSeconds() == clock);

        const QString session = QStringLiteral("desk-peek-manual-review");
        const QString turn = QStringLiteral("fixture");
        check("stale_selection_was_enabled", actionsAvailable(true));
        left->trigger();
        controller.turnStarted(session, turn);
        drainAction();
        check("selection_rechecks_after_work_begins", controller.state() == State::Busy && !motion.deskPeekInFlight());
        check("busy_disables_both_directions", actionsAvailable(false));
        controller.sessionEnded(session);
        window.pointerGesture_.press(QPointF(120, 160), false);
        window.pointerGesture_.move(QPointF(145, 160));
        check("drag_disables_both_directions", window.pointerGesture_.mode() == PetPointerGesture::Mode::Drag
              && actionsAvailable(false));
        window.pointerGesture_.release();
        check("expanded_memo_disables_both_directions", window.setUserMemo(QStringLiteral("fixture note"))
              && window.memoReviewState().value(QStringLiteral("visual")).toString() == QStringLiteral("bubble")
              && actionsAvailable(false));
        check("folded_memo_releases_manual_gate", window.foldMemo() && actionsAvailable(true));
        check("fixture_memo_removed", window.discardMemo());

        // A due auto tick advances the clock, but must not record a refusal
        // while the user owns the stage. The next eligible tick can still play.
        window.tickSpontaneousScenes(); // quiet consumes the launch greeting
        quiet(false);
        director.setChanceScale(1000.0);
        for (int i = 0; i < 359; ++i) window.tickSpontaneousScenes();
        check("automatic_not_due_at_359_seconds", director.attemptCount() == attempts && !motion.deskPeekInFlight());
        trigger(left);
        const double beforeDueTick = director.runningSeconds();
        window.deskPeekCursorX_ = 2.0;
        window.tickSpontaneousScenes();
        check("due_tick_holds_nomination_without_pausing_clock", director.runningSeconds() == beforeDueTick + 1.0
              && director.attemptCount() == attempts && director.startedCount() == starts && motion.deskPeekActive());
        settle();
        window.deskPeekCursorX_ = 2.0;
        window.tickSpontaneousScenes();
        check("automatic_plays_without_manual_cooldown_or_backoff", director.attemptCount() == attempts + 1
              && director.startedCount() == starts + 1 && director.lastStarted() == QStringLiteral("desk-peek")
              && motion.deskPeekActive());
        settle();

        const bool enabled = actionsAvailable(true);
        trigger(right);
        check("manual_can_play_during_automatic_cooldown", enabled && motion.deskPeekActive());
        controller.turnStarted(session, turn);
        check("new_work_preempts_manual_peek", controller.state() == State::Busy && !motion.deskPeekActive());
        settle();
        controller.sessionEnded(session);
        check("work_end_does_not_replay_manual_peek", controller.state() == State::Idle && !motion.deskPeekInFlight()
              && director.attemptCount() == attempts + 1 && director.startedCount() == starts + 1);
        finish();
    });
    return app.exec();
}
