#include "VisibilityReview.h"

#include "PetWindow.h"

#include <QApplication>
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>

int runVisibilityReview(QApplication& app, PetController& controller, PetWindow& window,
                        const QString& output, qint64& nowMs) {
    if (!app.property("desktopCompanionReviewSettingsIsolated").toBool()) return 2;
    window.prepareLiveInteractionReview(280);
    window.setWorkPosturePreference(ParameterMotion::WorkPosturePreference::Standing);
    window.setAutomaticWorkNotesEnabled(true);
    using State = PetController::State;
    const QString session = QStringLiteral("visibility-fixture");
    const QString finished = QStringLiteral("finished-hidden");
    const QString running = QStringLiteral("still-running");
    const QString pending = QStringLiteral("seen-hidden");
    QJsonObject checks;
    const auto check = [&](const char* key, bool passed) { checks.insert(QString::fromLatin1(key), passed); };
    const auto reaction = [&] { return window.motion_.interactionId(); };
    const auto memoVisual = [&] { return window.memoReviewState().value(QStringLiteral("visual")).toString(); };
    const auto* flight = window.findChild<QWidget*>(QStringLiteral("deleteIconFlight"));
    const auto deletion = [&] {
        QPixmap icon(48, 48);
        icon.fill(Qt::cyan);
        controller.desktopItemDeleted({}, window.pos() + QPoint(-120, 80), QIcon(icon));
    };
    QTimer::singleShot(1000, &app, [&] {
        check("visible_native_window", window.isVisible()
              && window.renderBackend() == QStringLiteral("cubism_native"));
        window.frameTimer_.start();
        window.toggleVisibility();
        check("hidden_frame_timer_stops", !window.isVisible() && !window.frameTimer_.isActive());
        controller.turnStarted(session, finished);
        check("hidden_start_updates_busy_without_greeting", controller.state() == State::Busy
              && window.motion_.state() == State::Busy && reaction().isEmpty());
        nowMs += 31'000;
        controller.turnStopped(session, finished);
        check("hidden_stop_updates_idle_without_return_or_note", controller.state() == State::Idle
              && reaction().isEmpty() && memoVisual() == QStringLiteral("none"));

        controller.turnStarted(session, running);
        for (int i = 0; i < 120; ++i) window.motion_.advance(1.0 / 30.0);
        controller.applyPermissionSnapshot(session, running, {pending});
        check("hidden_permission_is_counted_without_cue", controller.pendingPermissionCount() == 1
              && controller.activeTurnCount() == 1 && reaction().isEmpty());
        deletion();
        check("hidden_delete_has_no_action_or_flight", controller.state() == State::Busy
              && flight && flight->isHidden());
        // Leave the negative control able to finish the later stages even
        // when the regression is present and has started a foreground action.
        if (controller.state() == State::Delete) controller.actionFinished();
        check("hidden_manual_note_is_saved_folded", window.setUserMemo(QStringLiteral("visibility fixture note"))
              && memoVisual() == QStringLiteral("badge"));
        check("fixture_note_removed", window.discardMemo());

        window.toggleVisibility();
        check("show_frame_timer_resumes", window.frameTimer_.isActive());
        window.frameTimer_.stop();
        controller.applyPermissionSnapshot(session, running, {pending});
        check("show_keeps_busy_and_does_not_replay_permission", window.isVisible()
              && controller.state() == State::Busy && window.motion_.state() == State::Busy
              && controller.pendingPermissionCount() == 1 && reaction().isEmpty());
        for (int i = 0; i < 120; ++i) window.advanceLiveFrame(1.0 / 30.0);
        controller.applyPermissionSnapshot(session, running, {pending, QStringLiteral("seen-visible")});
        check("fresh_visible_permission_plays", reaction() == QStringLiteral("permission-cue"));
        controller.applyPermissionSnapshot(session, running, {});
        // The delete de-duplication window is real even during fixture playback.
        QTimer::singleShot(500, &app, [&] {
            deletion();
            check("visible_delete_starts_action_and_flight", controller.state() == State::Delete
                  && flight && flight->isVisible());
            window.toggleVisibility();
            check("hide_clears_delete_and_flight_but_not_work", controller.state() == State::Busy
                  && controller.activeTurnCount() == 1 && flight && flight->isHidden());
            window.toggleVisibility();
            check("show_does_not_restore_flight", window.motion_.state() == State::Busy
                  && flight && flight->isHidden());
            if (controller.state() == State::Delete) controller.actionFinished();
            controller.sessionEnded(session);

            auto& director = window.director_;
            director.setFrequency(TriggerDirector::Frequency::Normal);
            director.setChanceScale(1000.0);
            app.setProperty("desktopCompanionReviewSuppressAutomatic", true);
            window.tickSpontaneousScenes();
            app.setProperty("desktopCompanionReviewSuppressAutomatic", false);
            for (int i = 0; i < 359; ++i) window.tickSpontaneousScenes();
            const int attempts = director.attemptCount();
            window.toggleVisibility();
            window.toggleVisibility();
            window.deskPeekCursorX_ = -2.0;
            window.tickSpontaneousScenes();
            check("brief_hide_resets_idle_even_between_ticks", director.attemptCount() == attempts
                  && !window.motion_.deskPeekActive());
            for (int i = 0; i < 359; ++i) window.tickSpontaneousScenes();
            check("fresh_visible_idle_can_start_peek", controller.state() == State::Idle
                  && director.attemptCount() == attempts + 1 && window.motion_.deskPeekActive());

            bool passed = true;
            for (const auto& value : checks) passed &= value.toBool();
            const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("scenario"), QStringLiteral("visibility")},
                {QStringLiteral("scope"), QStringLiteral("isolated Qt window and simulated controller events")},
                {QStringLiteral("passed"), passed}, {QStringLiteral("checks"), checks}}).toJson();
            QSaveFile receipt(QDir(output).filePath(QStringLiteral("scene.json")));
            const bool saved = receipt.open(QIODevice::WriteOnly) && receipt.write(bytes) == bytes.size() && receipt.commit();
            app.exit(saved && passed ? 0 : 1);
        });
    });
    return app.exec();
}
