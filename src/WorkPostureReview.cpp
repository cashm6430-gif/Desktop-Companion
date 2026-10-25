#include "WorkPostureReview.h"

#include "PetController.h"
#include "PetWindow.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QTimer>

#include <cmath>

namespace {
constexpr double kStep = 0.05;
constexpr int kFrames = 800;
constexpr int kStart = 20;
constexpr int kStanding = 300;
constexpr int kLaptopAgain = 420;
constexpr int kStop = 580;
const QSet<int> kSnapshots{0, 1, 19, 20, 100, 199, 218, 219, 230, 240, 250,
    260, 280, 299, 300, 305, 310, 315, 320, 330, 340, 360, 399, 419, 420,
    430, 440, 450, 460, 480, 520, 579, 580, 590, 600, 610, 620, 640, 680,
    740, 799};

QJsonObject menuState(QMenu* menu) {
    QJsonObject result;
    for (auto* action : menu->actions())
        result.insert(action->data().toString(), action->isChecked());
    return result;
}

bool saveMenu(QMenu* menu, const QString& path) {
    menu->setAttribute(Qt::WA_DontShowOnScreen);
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
    menu->ensurePolished();
    menu->resize(menu->sizeHint());
    QImage image(menu->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    menu->render(&image);
    return image.save(path);
}

int runRestoreReview(QApplication& app, PetController& controller,
                     PetWindow& window, const QDir& directory,
                     const QString& scenario) {
    const bool invalid = scenario == QStringLiteral("work-posture-invalid");
    window.prepareLiveInteractionReview(560);
    window.move(-10000, -10000);
    QTimer::singleShot(1000, &app, [&app, &controller, &window, directory, scenario, invalid] {
        const QString name = QStringLiteral("frame-000.png");
        const bool rendered = window.renderLiveInteractionFrame(kStep, directory.filePath(name));
        const auto state = window.workPostureReviewState();
        const QString expected = invalid ? QStringLiteral("automatic") : QStringLiteral("laptop");
        auto* menu = window.findChild<QMenu*>(QStringLiteral("workPostureMenu"));
        const auto menuValues = menu ? menuState(menu) : QJsonObject{};
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
            QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
        settings.sync();
        QJsonObject checks{
            {QStringLiteral("native_backend"), rendered && window.renderBackend() == QStringLiteral("cubism_native")},
            {QStringLiteral("fixture_settings_copied"), !app.property("desktopCompanionWorkPostureSourceSettings").toString().isEmpty()},
            {QStringLiteral("restored_preference"), state.value(QStringLiteral("preference")).toString() == expected},
            {QStringLiteral("restart_does_not_invent_work"), controller.activeTurnCount() == 0
                && controller.state() == PetController::State::Idle
                && !state.value(QStringLiteral("laptop_selected")).toBool()},
            {QStringLiteral("menu_tracks_restored_preference"), menuValues.value(expected).toBool()},
            {QStringLiteral("fixture_setting_preserved"), settings.status() == QSettings::NoError
                && settings.value(QStringLiteral("workPosture")).toString()
                    == (invalid ? QStringLiteral("future-or-corrupt-value") : QStringLiteral("laptop"))}};
        bool passed = true;
        for (const auto value : checks) passed &= value.toBool();
        QJsonObject parameters;
        const auto values = window.motionParameters();
        for (auto it = values.cbegin(); it != values.cend(); ++it) parameters.insert(it.key(), it.value());
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), QStringLiteral("native_work_posture_fresh_process_restore")},
            {QStringLiteral("passed"), passed},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("settings_file"), settings.fileName()},
            {QStringLiteral("source_settings_file"), app.property("desktopCompanionWorkPostureSourceSettings").toString()},
            {QStringLiteral("checks"), checks}, {QStringLiteral("posture"), state},
            {QStringLiteral("menu"), menuValues}, {QStringLiteral("parameters"), parameters},
            {QStringLiteral("frame"), name},
            {QStringLiteral("privacy"), QJsonObject{
                {QStringLiteral("fixture_only"), true}, {QStringLiteral("isolated_settings"), true},
                {QStringLiteral("default_endpoint_used"), false}, {QStringLiteral("real_hooks_used"), false},
                {QStringLiteral("desktop_files_changed"), false}}}}).toJson();
        QSaveFile file(directory.filePath(QStringLiteral("scene.json")));
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            app.exit(2); return;
        }
        app.exit(passed ? 0 : 1);
    });
    return app.exec();
}
}

int runWorkPostureReview(QApplication& app, PetController& controller,
                        PetWindow& window, const QString& output,
                        const QString& scenario) {
    const QDir directory(QFileInfo(output).absoluteFilePath());
    if (!directory.exists()
        || !app.property("desktopCompanionReviewSettingsIsolated").toBool()) return 2;
    if (scenario == QStringLiteral("work-posture-restore") || scenario == QStringLiteral("work-posture-invalid"))
        return runRestoreReview(app, controller, window, directory, scenario);
    if (scenario != QStringLiteral("work-posture-preference")) return 2;
    auto* menu = window.findChild<QMenu*>(QStringLiteral("workPostureMenu"));
    auto* laptop = window.findChild<QAction*>(QStringLiteral("workPostureLaptop"));
    auto* standing = window.findChild<QAction*>(QStringLiteral("workPostureStanding"));
    if (!menu || !laptop || !standing || menu->actions().size() != 3) return 2;
    window.prepareLiveInteractionReview(560);
    window.setInteractionPreviewEnabled(false);
    window.setGrassDragPreviewEnabled(false);
    window.move(-10000, -10000);
    window.show();

    QTimer timer, deadline;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(1);
    deadline.setSingleShot(true);
    QElapsedTimer wall;
    wall.start();
    QJsonArray frames;
    QJsonObject checks, persistence;
    int frame = 0;
    bool finished = false;
    double previousBusy = 0.0, previousContinuous = 0.0;
    const auto finish = [&](int code, const QString& reason = {}) {
        if (finished) return;
        finished = true;
        timer.stop(); deadline.stop();
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), QStringLiteral("work-posture-preference")},
            {QStringLiteral("scope"), QStringLiteral("native_work_posture_real_qaction_and_fixture_turn")},
            {QStringLiteral("passed"), code == 0}, {QStringLiteral("failure"), reason},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("wall_ms"), wall.elapsed()},
            {QStringLiteral("frame_count"), frames.size()},
            {QStringLiteral("checks"), checks}, {QStringLiteral("persistence"), persistence},
            {QStringLiteral("privacy"), QJsonObject{
                {QStringLiteral("fixture_only"), true},
                {QStringLiteral("isolated_settings"), true},
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
    const auto seated = [&] {
        const auto& values = window.motionParameters();
        for (const auto* id : {"ParamSitPose", "ParamBusyLaptop", "ParamDeskVisible", "ParamLaptopVisible"})
            if (values.value(QString::fromLatin1(id)) < 0.98) return false;
        return window.workPostureReviewState().value(QStringLiteral("desk_phase")).toString()
            == QStringLiteral("work");
    };
    const auto recovered = [&] {
        const auto& values = window.motionParameters();
        for (const auto* id : {"ParamSitPose", "ParamBusyLaptop", "ParamDeskVisible", "ParamLaptopVisible"})
            if (std::abs(values.value(QString::fromLatin1(id))) > 0.002) return false;
        return window.workPostureReviewState().value(QStringLiteral("desk_phase")).toString()
            == QStringLiteral("hidden");
    };
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        finish(2, QStringLiteral("wall_time_limit"));
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        if (!check(QStringLiteral("native_backend"), window.renderBackend() == QStringLiteral("cubism_native"))) return;
        QString event;
        if (frame == 0 && !check(QStringLiteral("default_automatic"),
                window.workPosturePreference() == ParameterMotion::WorkPosturePreference::Automatic)) return;
        if (frame == 1) {
            laptop->trigger();
            event = QStringLiteral("tray_laptop_idle");
            if (!check(QStringLiteral("idle_menu_only_saves_preference"),
                    controller.activeTurnCount() == 0 && controller.state() == PetController::State::Idle
                    && window.workPosturePreference() == ParameterMotion::WorkPosturePreference::Laptop)
                || !check(QStringLiteral("menu_idle_snapshot"),
                    saveMenu(menu, directory.filePath(QStringLiteral("menu-idle-laptop.png"))))) return;
        }
        if (frame == kStart) {
            controller.turnStarted(QStringLiteral("posture-review-fixture"), QStringLiteral("work"));
            event = QStringLiteral("fixture_turn_started");
        }
        if (frame == kStanding) {
            if (!check(QStringLiteral("first_laptop_settled"), seated())) return;
            standing->trigger();
            event = QStringLiteral("tray_standing_while_seated");
            const auto state = menuState(menu);
            if (!check(QStringLiteral("menu_shows_preference_before_pose_recovers"),
                    state.value(QStringLiteral("standing")).toBool()
                    && !state.value(QStringLiteral("laptop")).toBool() && seated())
                || !check(QStringLiteral("menu_pending_snapshot"),
                    saveMenu(menu, directory.filePath(QStringLiteral("menu-pending-standing.png"))))) return;
        }
        if (frame == kLaptopAgain) {
            if (!check(QStringLiteral("standing_settled_without_reset"), recovered())) return;
            laptop->trigger();
            event = QStringLiteral("tray_laptop_again");
        }
        if (frame == kStop) {
            if (!check(QStringLiteral("second_laptop_settled"), seated())) return;
            controller.turnStopped(QStringLiteral("posture-review-fixture"), QStringLiteral("work"));
            event = QStringLiteral("fixture_turn_stopped");
        }
        const bool sampled = kSnapshots.contains(frame)
            || (frame >= 280 && frame <= 400 && frame % 2 == 0);
        const auto name = sampled
            ? QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0')) : QString();
        const auto capturePath = name.isEmpty() ? QStringLiteral("current.png") : name;
        if (!window.renderLiveInteractionFrame(kStep, directory.filePath(capturePath))) {
            finish(2, QStringLiteral("native_frame_capture_failed")); return;
        }
        const auto posture = window.workPostureReviewState();
        const auto menuValues = menuState(menu);
        const auto preference = posture.value(QStringLiteral("preference")).toString();
        int checked = 0;
        for (auto value : menuValues) checked += value.toBool() ? 1 : 0;
        if (!check(QStringLiteral("menu_exclusive_and_tracks_preference"),
                checked == 1 && menuValues.value(preference).toBool())) return;
        QJsonObject parameters;
        const auto values = window.motionParameters();
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!std::isfinite(it.value())) { finish(2, QStringLiteral("non_finite_parameter")); return; }
            parameters.insert(it.key(), it.value());
        }
        const bool busy = frame >= kStart && frame < kStop;
        if (!check(QStringLiteral("only_fixture_turn_controls_busy"),
                controller.activeTurnCount() == (busy ? 1 : 0)
                && controller.state() == (busy ? PetController::State::Busy : PetController::State::Idle))) return;
        if (busy) {
            const double busyTime = posture.value(QStringLiteral("busy_time")).toDouble();
            const double continuous = posture.value(QStringLiteral("continuous_busy_time")).toDouble();
            if (!check(QStringLiteral("preference_preserves_work_clocks"),
                    busyTime >= previousBusy && continuous >= previousContinuous
                    && busyTime <= previousBusy + kStep + 0.00001
                    && continuous <= previousContinuous + kStep + 0.00001)) return;
            previousBusy = busyTime; previousContinuous = continuous;
            if (continuous < 10.0 - 0.000001 && !check(QStringLiteral("ten_second_sit_guard"),
                    std::abs(values.value(QStringLiteral("ParamSitPose"))) < 0.002
                    && std::abs(values.value(QStringLiteral("ParamDeskVisible"))) < 0.002
                    && std::abs(values.value(QStringLiteral("ParamLaptopVisible"))) < 0.002)) return;
        } else if (frame < kStart && !check(QStringLiteral("idle_never_enters_work_pose"), recovered())) return;
        frames.append(QJsonObject{{QStringLiteral("frame"), frame}, {QStringLiteral("file"), name},
            {QStringLiteral("time"), (frame + 1) * kStep}, {QStringLiteral("dt"), kStep},
            {QStringLiteral("event"), event}, {QStringLiteral("posture"), posture},
            {QStringLiteral("menu"), menuValues}, {QStringLiteral("parameters"), parameters}});
        if (++frame == kFrames) {
            if (!check(QStringLiteral("stop_recovers_idle_with_saved_preference"), recovered()
                    && window.workPosturePreference() == ParameterMotion::WorkPosturePreference::Laptop)) return;
            QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("DesktopCompanion"), QStringLiteral("WhaleGirl"));
            settings.sync();
            if (!check(QStringLiteral("stable_string_saved"), settings.status() == QSettings::NoError
                    && settings.value(QStringLiteral("workPosture")).toString() == QStringLiteral("laptop"))) return;
            // Each Qt/Cubism process owns exactly one PetWindow: the Native
            // Framework is global and a second canvas cannot be torn down
            // while the first one still owns a model. The wrapper starts two
            // fresh processes against copies of this actual saved INI.
            persistence.insert(QStringLiteral("settings_file"), settings.fileName());
            persistence.insert(QStringLiteral("saved_value"), settings.value(QStringLiteral("workPosture")).toString());
            finish(0);
        }
    });
    deadline.start(100000);
    QTimer::singleShot(1000, &app, [&] { if (!finished) timer.start(); });
    return app.exec();
}
