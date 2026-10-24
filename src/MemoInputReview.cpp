#include "MemoInputReview.h"

#include "MemoStore.h"
#include "PetController.h"
#include "PetWindow.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSaveFile>
#include <QTimer>

#include <cmath>

namespace {
constexpr double kStep = 0.1;
constexpr int kFrames = 108;

QJsonObject safeSnapshot(const PetWindow& window) {
    // Never serialize arbitrary future fields from a diagnostic snapshot: a
    // private user note must not accidentally become review evidence.
    const auto source = window.memoReviewState();
    QJsonObject safe;
    const QStringList fields{QStringLiteral("origin"), QStringLiteral("visual"),
        QStringLiteral("text_length"), QStringLiteral("text_sha256"),
        QStringLiteral("rise_seen"), QStringLiteral("motion_active"),
        QStringLiteral("work_count"), QStringLiteral("worked_ms"),
        QStringLiteral("automatic_enabled"), QStringLiteral("store_ok"),
        QStringLiteral("age_seconds")};
    for (const auto& field : fields) {
        if (source.contains(field)) safe.insert(field, source.value(field));
    }
    const auto rect = source.value(QStringLiteral("badge_rect")).toObject();
    QJsonObject safeRect;
    for (const auto& key : {QStringLiteral("x"), QStringLiteral("y"),
                            QStringLiteral("width"), QStringLiteral("height")})
        safeRect.insert(key, rect.value(key));
    safe.insert(QStringLiteral("badge_rect"), safeRect);
    return safe;
}
} // namespace

int runMemoInputReview(QApplication& app, PetController& controller,
                       PetWindow& window, const QString& output,
                       qint64& reviewNowMs) {
    const QDir directory(QFileInfo(output).absoluteFilePath());
    if (!directory.exists()) return 1;
    const QString fixture = QStringLiteral("先喝口水，再继续");
    const QString fixtureHash = QString::fromLatin1(
        QCryptographicHash::hash(fixture.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString storePath = app.property("desktopCompanionReviewMemoPath").toString();
    const QString relativeStore = directory.relativeFilePath(QFileInfo(storePath).absoluteFilePath());
    const bool isolatedStore = !storePath.isEmpty() && !QDir::isAbsolutePath(relativeStore)
        && relativeStore != QStringLiteral("..")
        && !relativeStore.startsWith(QStringLiteral("../"));
    MemoStore store(isolatedStore ? storePath : QString{});

    window.prepareLiveInteractionReview(280);
    window.setInteractionPreviewEnabled(false);
    window.move(-10000, -10000);
    window.show();

    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    QElapsedTimer wall;
    wall.start();
    int frame = 0;
    bool finished = false;
    QJsonArray trace;
    QJsonObject checks;
    QString failure;

    const auto finish = [&](int code, const QString& reason = {}) {
        if (finished) return;
        finished = true;
        timer.stop();
        deadline.stop();
        if (!reason.isEmpty()) failure = reason;
        QSaveFile evidence(directory.filePath(QStringLiteral("scene.json")));
        evidence.setDirectWriteFallback(false);
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), QStringLiteral("memo-recovery")},
            {QStringLiteral("scope"), QStringLiteral("native_window_memo_actions_synthetic_qt_badge_click_and_real_controller_events")},
            {QStringLiteral("passed"), code == 0},
            {QStringLiteral("failure"), failure},
            {QStringLiteral("wall_ms"), wall.elapsed()},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("frame_count"), trace.size()},
            {QStringLiteral("privacy"), QJsonObject{
                {QStringLiteral("fixture_only"), true},
                {QStringLiteral("default_endpoint_used"), false},
                {QStringLiteral("real_hooks_used"), false},
                {QStringLiteral("desktop_files_changed"), false},
                {QStringLiteral("raw_text_in_trace"), false},
                {QStringLiteral("isolated_settings"),
                    app.property("desktopCompanionReviewSettingsIsolated").toBool()},
                {QStringLiteral("isolated_store"), isolatedStore}}},
            {QStringLiteral("checks"), checks},
            {QStringLiteral("final_memo"), safeSnapshot(window)},
            {QStringLiteral("frames"), trace}}).toJson();
        if (!evidence.open(QIODevice::WriteOnly) || evidence.write(bytes) != bytes.size()
            || !evidence.commit()) {
            app.exit(1);
            return;
        }
        app.exit(code);
    };
    const auto check = [&](const QString& name, bool passed) {
        checks.insert(name, passed);
        if (!passed) finish(1, name);
        return passed;
    };
    const auto snapshotIs = [&](const QString& origin, const QString& visual) {
        const auto state = window.memoReviewState();
        return state.value(QStringLiteral("origin")).toString() == origin
            && state.value(QStringLiteral("visual")).toString() == visual
            && state.value(QStringLiteral("store_ok")).toBool();
    };
    const auto userTextIntact = [&] {
        const auto state = window.memoReviewState();
        return state.value(QStringLiteral("origin")).toString() == QStringLiteral("user")
            && state.value(QStringLiteral("text_length")).toInt() == fixture.size()
            && state.value(QStringLiteral("text_sha256")).toString() == fixtureHash;
    };
    const auto storedUser = [&](bool collapsed) {
        const auto loaded = store.load();
        return loaded.ok() && loaded.note && loaded.note->text == fixture
            && loaded.note->collapsed == collapsed;
    };
    const auto storedEmpty = [&] {
        const auto loaded = store.load();
        return loaded.ok() && !loaded.note;
    };
    const auto triggerAction = [&](const QString& name) {
        const auto actions = window.findChildren<QAction*>(name);
        if (actions.size() != 1 || !actions.front()->isEnabled())
            return check(QStringLiteral("action_available_") + name, false);
        actions.front()->trigger();
        return true;
    };
    const auto workTurn = [&](int number) {
        const QString session = QStringLiteral("memo-fixture-session");
        const QString turn = QStringLiteral("fixture-%1").arg(number);
        controller.turnStarted(session, turn);
        if (!check(QStringLiteral("turn_%1_started").arg(number),
                   controller.activeTurnCount() == 1)) return false;
        reviewNowMs += 35000;
        controller.turnStopped(session, turn);
        return check(QStringLiteral("turn_%1_stopped").arg(number),
                     controller.activeTurnCount() == 0);
    };
    const auto workSummary = [&](int count, qint64 workedMs) {
        const auto state = window.memoReviewState();
        return snapshotIs(QStringLiteral("work_duration"), QStringLiteral("badge"))
            && state.value(QStringLiteral("work_count")).toInt() == count
            && state.value(QStringLiteral("worked_ms")).toVariant().toLongLong() == workedMs;
    };
    const auto clickBadge = [&] {
        const auto rect = window.memoReviewState().value(QStringLiteral("badge_rect")).toObject();
        const QRectF badge(rect.value(QStringLiteral("x")).toDouble(),
                           rect.value(QStringLiteral("y")).toDouble(),
                           rect.value(QStringLiteral("width")).toDouble(),
                           rect.value(QStringLiteral("height")).toDouble());
        if (!check(QStringLiteral("badge_hit_rect_valid"), badge.isValid()
                   && QRectF(window.rect()).contains(badge.center()))) return false;
        const QPointF point = badge.center();
        QMouseEvent press(QEvent::MouseButtonPress, point, point + QPointF(window.pos()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, point + QPointF(window.pos()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &release);
        return true;
    };

    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        finish(1, QStringLiteral("wall_time_limit"));
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        if (!check(QStringLiteral("native_backend"),
                   window.renderBackend() == QStringLiteral("cubism_native"))) return;
        QString event;
        switch (frame) {
        case 0:
            if (!check(QStringLiteral("fixture_store_isolated"), isolatedStore)
                || !check(QStringLiteral("fixture_settings_isolated"),
                    app.property("desktopCompanionReviewSettingsIsolated").toBool())
                || !check(QStringLiteral("recovered_user_badge_without_gaze"),
                    snapshotIs(QStringLiteral("user"), QStringLiteral("badge"))
                    && userTextIntact() && storedUser(false)
                    && !window.memoReviewState().value(QStringLiteral("motion_active")).toBool())
                || !check(QStringLiteral("automatic_enabled_initially"),
                    window.memoReviewState().value(QStringLiteral("automatic_enabled")).toBool())) return;
            event = QStringLiteral("recovered_collapsed_user_note");
            break;
        case 1:
            if (!clickBadge()
                || !check(QStringLiteral("synthetic_badge_click_reopens"),
                    snapshotIs(QStringLiteral("user"), QStringLiteral("bubble"))
                    && storedUser(false)
                    && window.memoReviewState().value(QStringLiteral("motion_active")).toBool())) return;
            event = QStringLiteral("qt_badge_click");
            break;
        case 20:
            if (!check(QStringLiteral("full_rise_and_gaze_completed"),
                    window.memoReviewState().value(QStringLiteral("rise_seen")).toBool()
                    && !window.memoReviewState().value(QStringLiteral("motion_active")).toBool()
                    && snapshotIs(QStringLiteral("user"), QStringLiteral("bubble")))
                || !triggerAction(QStringLiteral("memoFoldAction"))
                || !check(QStringLiteral("fold_action_persists_collapsed"),
                    snapshotIs(QStringLiteral("user"), QStringLiteral("badge")) && storedUser(true))) return;
            event = QStringLiteral("tray_fold_user_note");
            break;
        case 21:
            if (!check(QStringLiteral("new_user_note_starts_early_rise"),
                    window.setUserMemo(fixture)
                    && snapshotIs(QStringLiteral("user"), QStringLiteral("bubble"))
                    && window.memoReviewState().value(QStringLiteral("motion_active")).toBool()
                    && !window.memoReviewState().value(QStringLiteral("rise_seen")).toBool())) return;
            event = QStringLiteral("set_known_fixture_user_note");
            break;
        case 22:
            controller.desktopItemDeleted(); // animation event only; no path or file operation
            if (!check(QStringLiteral("early_delete_folds_user_note"),
                    controller.state() == PetController::State::Delete
                    && snapshotIs(QStringLiteral("user"), QStringLiteral("badge"))
                    && !window.memoReviewState().value(QStringLiteral("motion_active")).toBool()
                    && storedUser(true))) return;
            event = QStringLiteral("animation_only_delete_interrupts_early_rise");
            break;
        case 27:
            controller.actionFinished();
            if (!check(QStringLiteral("delete_return_keeps_folded_no_replay"),
                    controller.state() == PetController::State::Idle
                    && snapshotIs(QStringLiteral("user"), QStringLiteral("badge"))
                    && !window.memoReviewState().value(QStringLiteral("motion_active")).toBool())) return;
            event = QStringLiteral("delete_action_finished");
            break;
        case 36:
            if (!triggerAction(QStringLiteral("memoCompleteAction"))
                || !check(QStringLiteral("explicit_completion_clears_store_and_fades"),
                    window.memoReviewState().value(QStringLiteral("visual")).toString() == QStringLiteral("done")
                    && window.memoReviewState().value(QStringLiteral("store_ok")).toBool()
                    && storedEmpty())) return;
            event = QStringLiteral("tray_complete_user_note");
            break;
        case 46:
            if (!check(QStringLiteral("done_paper_disappears"),
                    snapshotIs(QStringLiteral("none"), QStringLiteral("none")))
                || !workTurn(1)
                || !check(QStringLiteral("first_accepted_work_note"), workSummary(1, 35000) && storedEmpty())) return;
            event = QStringLiteral("real_controller_first_35_second_turn");
            break;
        case 50:
            if (!workTurn(2)
                || !check(QStringLiteral("second_work_note_merges"), workSummary(2, 70000) && storedEmpty())) return;
            event = QStringLiteral("real_controller_second_turn_merges");
            break;
        case 54:
            if (!triggerAction(QStringLiteral("automaticWorkNotesAction"))
                || !check(QStringLiteral("disabled_work_notes_clear_current_summary"),
                    !window.memoReviewState().value(QStringLiteral("automatic_enabled")).toBool()
                    && snapshotIs(QStringLiteral("none"), QStringLiteral("none")))) return;
            event = QStringLiteral("tray_disable_automatic_work_notes");
            break;
        case 55:
            if (!workTurn(3)
                || !check(QStringLiteral("disabled_turn_is_discarded"),
                    snapshotIs(QStringLiteral("none"), QStringLiteral("none"))
                    && window.memoReviewState().value(QStringLiteral("work_count")).toInt() == 0)) return;
            event = QStringLiteral("real_turn_while_automatic_disabled");
            break;
        case 56:
            if (!triggerAction(QStringLiteral("automaticWorkNotesAction"))
                || !check(QStringLiteral("reenable_does_not_replay"),
                    window.memoReviewState().value(QStringLiteral("automatic_enabled")).toBool()
                    && snapshotIs(QStringLiteral("none"), QStringLiteral("none")))) return;
            event = QStringLiteral("tray_reenable_without_replay");
            break;
        case 60:
            if (!workTurn(4)
                || !check(QStringLiteral("new_turn_after_reenable_starts_fresh"), workSummary(1, 35000))) return;
            event = QStringLiteral("new_real_turn_after_reenable");
            break;
        case 75:
            if (!check(QStringLiteral("user_note_replaces_automatic_summary"),
                    window.setUserMemo(fixture) && userTextIntact() && storedUser(false))) return;
            event = QStringLiteral("set_user_fixture_over_automatic_summary");
            break;
        case 76:
            if (!workTurn(5)
                || !check(QStringLiteral("real_work_never_overwrites_user_note"),
                    userTextIntact() && storedUser(false)
                    && window.memoReviewState().value(QStringLiteral("work_count")).toInt() == 0)) return;
            event = QStringLiteral("real_work_turn_preserves_user_note");
            break;
        case 95:
            if (!triggerAction(QStringLiteral("memoFoldAction"))
                || !check(QStringLiteral("final_fold_persists_user_note"),
                    snapshotIs(QStringLiteral("user"), QStringLiteral("badge")) && storedUser(true))) return;
            event = QStringLiteral("tray_fold_before_reopen");
            break;
        case 96:
            if (!triggerAction(QStringLiteral("memoReopenAction"))
                || !check(QStringLiteral("tray_reopen_persists_expanded"),
                    snapshotIs(QStringLiteral("user"), QStringLiteral("bubble"))
                    && storedUser(false)
                    && window.memoReviewState().value(QStringLiteral("motion_active")).toBool())) return;
            event = QStringLiteral("tray_reopen_user_note");
            break;
        case 97:
            if (!triggerAction(QStringLiteral("memoDiscardAction"))
                || !check(QStringLiteral("tray_discard_clears_note_and_gaze"),
                    snapshotIs(QStringLiteral("none"), QStringLiteral("none")) && storedEmpty()
                    && !window.memoReviewState().value(QStringLiteral("motion_active")).toBool())) return;
            event = QStringLiteral("tray_discard_user_note");
            break;
        default:
            break;
        }
        if (frame >= 28 && frame < 36
            && !check(QStringLiteral("no_delayed_rise_after_delete"),
                snapshotIs(QStringLiteral("user"), QStringLiteral("badge"))
                && !window.memoReviewState().value(QStringLiteral("motion_active")).toBool())) return;
        if (frame >= 57 && frame < 60
            && !check(QStringLiteral("no_delayed_replay_after_reenable"),
                snapshotIs(QStringLiteral("none"), QStringLiteral("none")))) return;
        if (frame >= 77 && frame < 95
            && !check(QStringLiteral("user_note_keeps_priority"), userTextIntact())) return;
        if (frame >= 98 && !check(QStringLiteral("discard_does_not_replay"),
                snapshotIs(QStringLiteral("none"), QStringLiteral("none")))) return;

        const QString name = QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'));
        if (!window.renderLiveInteractionFrame(kStep, directory.filePath(name))) {
            finish(1, QStringLiteral("native_frame_capture_failed"));
            return;
        }
        QJsonObject parameters;
        const auto values = window.motionParameters();
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!std::isfinite(it.value())) {
                finish(1, QStringLiteral("non_finite_native_parameter"));
                return;
            }
            parameters.insert(it.key(), it.value());
        }
        trace.append(QJsonObject{{QStringLiteral("frame"), frame},
            {QStringLiteral("file"), name}, {QStringLiteral("time"), (frame + 1) * kStep},
            {QStringLiteral("dt"), kStep}, {QStringLiteral("event"), event},
            {QStringLiteral("memo"), safeSnapshot(window)},
            {QStringLiteral("parameters"), parameters},
            {QStringLiteral("controller_state"), static_cast<int>(controller.state())},
            {QStringLiteral("active_turns"), controller.activeTurnCount()}});
        if (++frame >= kFrames) {
            if (!check(QStringLiteral("frame_budget"), frame >= 80 && frame <= 120)
                || !check(QStringLiteral("all_real_turns_stopped"), controller.activeTurnCount() == 0)) return;
            finish(0);
        }
    });

    deadline.start(45000);
    QTimer::singleShot(1000, &app, [&] { if (!finished) timer.start(); });
    return app.exec();
}
