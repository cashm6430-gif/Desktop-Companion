#include "CodexTurnSource.h"
#include "DesktopDeleteSource.h"
#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    PetController controller;
    PetWindow window(&controller);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, &PetWindow::shutdown);
    window.show();

    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render-interaction")) {
        const QString output = QString::fromLocal8Bit(argv[2]);
        const QString scenario = QString::fromLocal8Bit(argv[3]);
        if (scenario.startsWith(QStringLiteral("grass-"))) {
            const QStringList grassScenes{QStringLiteral("grass-touch"), QStringLiteral("grass-timeout"),
                QStringLiteral("grass-delete-busy"), QStringLiteral("grass-approved")};
            if (!grassScenes.contains(scenario) || !QDir().mkpath(output)) return 2;
            window.prepareLiveInteractionReview();
            const bool approvedEntry = scenario == QStringLiteral("grass-approved");
            window.setInteractionPreviewEnabled(!approvedEntry);
            window.move(-10000, -10000);
            constexpr double step = 1.0 / 15.0;
            constexpr int frameCount = 180;
            int frame = 0;
            bool started = false, missed = false, touched = false, interrupted = false;
            bool newTurn = false, lastStop = false, geometrySaved = false;
            double holdStarted = -1.0;
            QJsonArray trace;
            QTimer timer;
            timer.setInterval(100);
            const auto mouse = [&](QEvent::Type type, const QPointF& point, Qt::MouseButton button,
                                   Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, point + QPointF(window.pos()), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(&window, &event);
            };
            const auto click = [&](const QPointF& point) {
                mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            };
            QObject::connect(&timer, &QTimer::timeout, &app, [&] {
                const double time = frame * step;
                QString event;
                if (!started && time >= 0.2) {
                    started = true;
                    if (approvedEntry) {
                        const QPointF body(window.width() * 0.5, window.height() * 0.5);
                        mouse(QEvent::MouseButtonDblClick, body, Qt::LeftButton, Qt::LeftButton);
                        mouse(QEvent::MouseButtonRelease, body, Qt::LeftButton, Qt::NoButton);
                        if (window.grassInteractionPhase() != QStringLiteral("enter")) { app.exit(2); return; }
                        event = QStringLiteral("default_double_click");
                    } else {
                        if (!window.startGrassInteraction()) { app.exit(2); return; }
                        event = QStringLiteral("grass_start");
                    }
                }
                if (!newTurn && time >= 1.0) {
                    newTurn = true;
                    controller.turnStarted(QStringLiteral("scene"), QStringLiteral("background"));
                    event = QStringLiteral("background_turn_start");
                }
                if (window.grassInteractionPhase() == QStringLiteral("hold")) {
                    if (holdStarted < 0) holdStarted = time;
                    if (!missed) {
                        missed = true;
                        click(QPointF(window.width() * 0.5, window.height() * 0.25));
                        mouse(QEvent::MouseButtonDblClick, QPointF(window.width() * 0.5, window.height() * 0.25),
                              Qt::LeftButton, Qt::LeftButton);
                        mouse(QEvent::MouseButtonRelease, QPointF(window.width() * 0.5, window.height() * 0.25),
                              Qt::LeftButton, Qt::NoButton);
                        event = QStringLiteral("body_click_and_double_click_miss");
                    }
                    const auto tip = window.grassTipHitPath();
                    const QPointF center = tip.boundingRect().center();
                    mouse(QEvent::MouseMove, center + QPointF(window.width() * 0.03, 0),
                          Qt::NoButton, Qt::NoButton);
                    if ((scenario == QStringLiteral("grass-touch") || approvedEntry) && !touched && time >= holdStarted + 0.8) {
                        if (!window.isGrassTipAt(center)) { qWarning() << "Tip center missed" << center; app.exit(2); return; }
                        touched = true;
                        click(center);
                        // A second contact cannot queue a second response.
                        click(center);
                        event = QStringLiteral("tip_click_twice");
                    }
                    if (scenario == QStringLiteral("grass-delete-busy") && !interrupted && time >= holdStarted + 0.8) {
                        interrupted = true;
                        controller.desktopItemDeleted();
                        event = QStringLiteral("delete_interrupt");
                    }
                }
                if (scenario != QStringLiteral("grass-delete-busy") && !lastStop && time >= 5.4) {
                    lastStop = true;
                    controller.turnStopped(QStringLiteral("scene"), QStringLiteral("background"));
                    event = QStringLiteral("background_last_turn_stop");
                }
                if (!window.renderLiveInteractionFrame(step,
                    QDir(output).filePath(QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'))))) {
                    app.exit(1); return;
                }
                QJsonObject parameters;
                for (auto it = window.motionParameters().cbegin(); it != window.motionParameters().cend(); ++it)
                    parameters.insert(it.key(), it.value());
                const QRectF tip = window.grassTipHitPath().boundingRect();
                if (!geometrySaved && window.grassInteractionPhase() == QStringLiteral("hold")) {
                    geometrySaved = true;
                    QImage geometry(QDir(output).filePath(QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'))));
                    QPainter painter(&geometry);
                    painter.setPen(QPen(QColor(220, 50, 50), 2));
                    painter.setBrush(QColor(80, 230, 160, 70));
                    painter.drawPath(window.grassTipHitPath());
                    painter.end();
                    if (!geometry.save(QDir(output).filePath(QStringLiteral("tip-region-diagnostic.png")))) {
                        app.exit(1); return;
                    }
                }
                trace.append(QJsonObject{{QStringLiteral("time"), time}, {QStringLiteral("event"), event},
                    {QStringLiteral("state"), static_cast<int>(controller.state())},
                    {QStringLiteral("active_turns"), controller.activeTurnCount()},
                    {QStringLiteral("preview_enabled"), window.interactionPreviewEnabled()},
                    {QStringLiteral("grass_phase"), window.grassInteractionPhase()},
                    {QStringLiteral("grass_time"), window.grassInteractionTime()},
                    {QStringLiteral("tip_bounds"), QJsonArray{tip.x(), tip.y(), tip.width(), tip.height()}},
                    {QStringLiteral("tip_center_hit"), window.isGrassTipAt(tip.center())},
                    {QStringLiteral("foot_not_tip"), !window.isGrassTipAt(QPointF(window.width() * 0.5, window.height() * 0.95))},
                    {QStringLiteral("parameters"), parameters}});
                if (++frame == frameCount) {
                    QFile evidence(QDir(output).filePath(QStringLiteral("scene.json")));
                    if (!evidence.open(QIODevice::WriteOnly)) { app.exit(1); return; }
                    evidence.write(QJsonDocument(QJsonObject{{QStringLiteral("scenario"), scenario},
                        {QStringLiteral("scope"), QStringLiteral("window_player_and_synthetic_qt_pointer_with_native_model")},
                        {QStringLiteral("frames"), trace}}).toJson());
                    app.exit(0);
                }
            });
            QTimer::singleShot(1000, &timer, [&] { timer.start(); });
            return app.exec();
        }
        const QStringList scenarios{QStringLiteral("turn-ended-standing"), QStringLiteral("turn-ended-laptop"),
            QStringLiteral("turn-ended-interrupt"), QStringLiteral("head-pat"),
            QStringLiteral("head-pat-busy"), QStringLiteral("head-pat-interrupt")};
        if (!scenarios.contains(scenario) || !QDir().mkpath(output)) return 2;
        ParameterMotion sampler;
        QString error;
        if (!sampler.loadMotionLibrary(QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions")), &error)) {
            qWarning() << error;
            return 2;
        }
        const bool glance = scenario.startsWith(QStringLiteral("turn-ended"));
        const bool laptop = scenario == QStringLiteral("turn-ended-laptop") || scenario == QStringLiteral("head-pat-busy");
        const bool interrupted = scenario.endsWith(QStringLiteral("interrupt"));
        sampler.setBusyRandomSeed(20261002);
        if (laptop) sampler.forceLaptopBusy();
        else if (glance) sampler.forceStandingBusy();
        for (int i = 0; i < 90; ++i) sampler.advance(1.0 / 30.0);
        window.setPreviewPose(sampler.values());
        window.move(-10000, -10000);
        constexpr double step = 1.0 / 15.0;
        constexpr int frameCount = 75;
        int frame = 0;
        bool started = false, released = false, cancelled = false;
        QJsonArray trace;
        QTimer timer;
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            const double time = frame * step;
            QString event;
            if (!started && time >= 0.4) {
                started = true;
                if (glance) {
                    sampler.setState(PetController::State::Idle);
                    if (!sampler.playTurnEnded()) { app.exit(2); return; }
                    event = QStringLiteral("last_turn_stopped");
                } else {
                    if (!sampler.beginHeadPat(1.0)) { app.exit(2); return; }
                    event = QStringLiteral("head_pat_begin");
                }
            }
            if (interrupted && !cancelled && time >= 1.2) {
                cancelled = true;
                sampler.setState(glance ? PetController::State::Busy : PetController::State::Delete);
                if (glance) sampler.forceStandingBusy();
                event = glance ? QStringLiteral("new_turn_started") : QStringLiteral("delete_interrupt");
            }
            if (!glance && !interrupted && !released && time >= 2.4) {
                released = true;
                sampler.endHeadPat();
                event = QStringLiteral("head_pat_release");
            }
            if (!glance && started && !released && !cancelled)
                sampler.updateHeadPat(std::cos((time - 0.4) * 2.0));
            if (sampler.consumeActionFinished()) sampler.setState(PetController::State::Idle);
            if (window.renderBackend() != QStringLiteral("cubism_native")
                || !window.renderSequenceFrame(sampler.values(), step,
                    QDir(output).filePath(QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'))),
                    sampler.bubblePulse())) { app.exit(1); return; }
            QJsonObject parameters;
            for (auto it = sampler.values().cbegin(); it != sampler.values().cend(); ++it)
                parameters.insert(it.key(), it.value());
            trace.append(QJsonObject{{QStringLiteral("time"), time}, {QStringLiteral("event"), event},
                {QStringLiteral("interaction"), sampler.interactionId()},
                {QStringLiteral("interaction_time"), sampler.interactionTime()},
                {QStringLiteral("parameters"), parameters},
                {QStringLiteral("head_center_hit"), window.isHeadAt(QPointF(window.width() * 0.5, window.height() * 0.3))},
                {QStringLiteral("foot_not_head"), !window.isHeadAt(QPointF(window.width() * 0.5, window.height() * 0.95))}});
            if (++frame == frameCount) {
                QFile evidence(QDir(output).filePath(QStringLiteral("scene.json")));
                if (!evidence.open(QIODevice::WriteOnly)) { app.exit(1); return; }
                evidence.write(QJsonDocument(QJsonObject{{QStringLiteral("scenario"), scenario},
                    {QStringLiteral("scope"), QStringLiteral("real_interaction_player_and_native_model")},
                    {QStringLiteral("frames"), trace}}).toJson());
                app.exit(0);
                return;
            }
            sampler.advance(step);
        });
        QTimer::singleShot(1000, &timer, [&] { timer.start(); });
        return app.exec();
    }

    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render-pose")) {
        QFile poseFile(QString::fromLocal8Bit(argv[2]));
        if (!poseFile.open(QIODevice::ReadOnly)) return 2;
        const auto object = QJsonDocument::fromJson(poseFile.readAll()).object();
        ParameterMotion::Parameters pose;
        for (auto it = object.begin(); it != object.end(); ++it) pose.insert(it.key(), it.value().toDouble());
        window.setPreviewPose(pose);
        window.move(-10000, -10000);
        const QString path = QString::fromLocal8Bit(argv[3]);
        QTimer::singleShot(1500, &app, [&] { app.exit(window.renderBackend() == QStringLiteral("cubism_native")
                                                    && window.saveRenderFrame(path) ? 0 : 1); });
        return app.exec();
    }

    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render-motion")) {
        const QString output = QString::fromLocal8Bit(argv[2]);
        if (!QDir().mkpath(output)) return 2;
        const QString clipId = argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("grass");
        // Existing activities and the two short reactions use the real player;
        // other authored clips can still be sampled directly for curve review.
        const bool laptop = clipId == QStringLiteral("busy-laptop");
        const bool grass = clipId == QStringLiteral("grass");
        const bool idle = clipId == QStringLiteral("idle");
        const bool busy = clipId == QStringLiteral("busy");
        const bool remove = clipId == QStringLiteral("delete");
        const bool turnEnded = clipId == QStringLiteral("turn-ended");
        const bool headPat = clipId == QStringLiteral("head-pat");
        ParameterMotion sampler;
        QString motionError;
        if (!sampler.loadMotionLibrary(QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions")), &motionError)) {
            qWarning() << "Motion library:" << motionError;
            return 2;
        }
        // Grass and the seated loop run through the real state machine. Any other
        // authored clip is sampled straight from the library, so a new asset can
        // be reviewed without adding a C++ branch.
        const MotionClip* directClip = nullptr;
        const MotionClip* interactionClip = nullptr;
        double patReleaseTime = 0.0;
        if (laptop) {
            sampler.advance(0.001);
            sampler.forceLaptopBusy();
        } else if (busy) {
            // A fixed seed keeps the standing variant reproducible frame by frame.
            sampler.setBusyRandomSeed(20260930);
            sampler.advance(0.001);
            sampler.forceStandingBusy();
        } else if (idle) {
            sampler.advance(0.001);
            sampler.setState(PetController::State::Idle);
        } else if (remove) {
            sampler.advance(0.001);
            sampler.setState(PetController::State::Delete);
        } else if (grass) {
            sampler.setPreviewPose(sampler.grassPose(0.0));
            sampler.setState(PetController::State::Grass);
        } else if (turnEnded || headPat) {
            interactionClip = sampler.library().clip(clipId);
            if (!interactionClip) return 2;
            if (turnEnded) {
                sampler.forceStandingBusy();
                for (int i = 0; i < 30; ++i) sampler.advance(1.0 / 30.0);
                sampler.setState(PetController::State::Idle);
                if (!sampler.playTurnEnded()) return 2;
            } else {
                sampler.advance(0.001);
                if (!sampler.beginHeadPat(1.0)) return 2;
                QFile authored(QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions/head-pat.motion.json")));
                if (!authored.open(QIODevice::ReadOnly)) return 2;
                patReleaseTime = QJsonDocument::fromJson(authored.readAll()).object()
                    .value(QStringLiteral("interaction")).toObject().value(QStringLiteral("holdEnd")).toDouble();
                if (!(patReleaseTime > 0.0)) return 2;
            }
        } else {
            directClip = sampler.library().clip(clipId);
            if (!directClip) return 2;
            sampler.setPreviewPose(directClip->sample(0.0));
        }
        constexpr double step = 1.0 / 15.0;
        const int frameCount = laptop ? 195
            : grass ? 120
            : busy ? 195
            : idle ? 120
            : remove ? std::max(1, static_cast<int>(std::lround(
                  sampler.actionDuration(PetController::State::Delete) / step)))
            : std::max(1, static_cast<int>(std::lround(
                (interactionClip ? interactionClip : directClip)->duration() / step)));
        window.setPreviewPose(sampler.values());
        window.move(-10000, -10000);
        int frame = 0;
        QTimer captureTimer;
        captureTimer.setInterval(100);
        QObject::connect(&captureTimer, &QTimer::timeout, &app, [&] {
            double time = frame * step;
            if (directClip && directClip->isLoop()) time = std::fmod(time, directClip->duration());
            const ParameterMotion::Parameters pose = directClip ? directClip->sample(time) : sampler.values();
            if (window.renderBackend() != QStringLiteral("cubism_native")
                || !window.renderSequenceFrame(pose, step,
                       QDir(output).filePath(QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'))),
                       sampler.bubblePulse())) {
                app.exit(1); return;
            }
            if (++frame == frameCount) { app.exit(0); return; }
            if (directClip) return; // The pose above already advanced one frame.
            // Fixed motion time gives a reproducible 15 FPS review even when
            // writing a large PNG takes longer than the desktop frame interval.
            sampler.advance(step);
            if (headPat && frame * step >= patReleaseTime) sampler.endHeadPat();
            if (laptop && frame == 150) sampler.setState(PetController::State::Idle);
        });
        QTimer::singleShot(1000, &captureTimer, [&] { captureTimer.start(); });
        return app.exec();
    }

    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--review-motion")) {
        const QString output = QString::fromLocal8Bit(argv[2]);
        if (!QDir().mkpath(output)) return 2;
        // An optional pose manifest lets artists review eye/limb sweeps in one
        // Native render session with the same keyframe capture path.
        QFile motionFile(argc >= 4 ? QString::fromLocal8Bit(argv[3])
            : QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions/grass.motion.json")));
        if (!motionFile.open(QIODevice::ReadOnly)) return 2;
        // The capture prefix names the clip so several motions can share one
        // review directory. It stays optional and defaults to grass so the
        // earlier commands and their artifacts keep the same file names.
        const QString prefix = argc >= 5 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("grass");
        const auto frames = QJsonDocument::fromJson(motionFile.readAll()).object().value(QStringLiteral("keyframes")).toArray();
        if (frames.isEmpty()) return 2;
        window.move(-10000, -10000);
        window.setPreviewPose({});
        int index = 0;
        bool savedAll = true;
        QTimer reviewTimer;
        reviewTimer.setInterval(240);
        QObject::connect(&reviewTimer, &QTimer::timeout, &app, [&] {
            if (index > 0) savedAll &= window.renderBackend() == QStringLiteral("cubism_native")
                && window.saveRenderFrame(QDir(output).filePath(
                    QStringLiteral("%1-%2.png").arg(prefix, QString::number(index - 1).rightJustified(2, QChar('0')))));
            if (index == frames.size()) { app.exit(savedAll ? 0 : 1); return; }
            ParameterMotion::Parameters pose;
            const auto parameters = frames[index++].toObject().value(QStringLiteral("parameters")).toObject();
            for (auto it = parameters.begin(); it != parameters.end(); ++it) pose.insert(it.key(), it.value().toDouble());
            if (!pose.contains(QStringLiteral("ParamEyeLOpen"))) pose.insert(QStringLiteral("ParamEyeLOpen"), 1.0);
            if (!pose.contains(QStringLiteral("ParamEyeROpen"))) pose.insert(QStringLiteral("ParamEyeROpen"), 1.0);
            window.setPreviewPose(pose);
        });
        QTimer::singleShot(1000, &reviewTimer, [&] { reviewTimer.start(); });
        return app.exec();
    }

    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--dump-parameters")) {
        // MOC3 is the only place a parameter's real limits live, and the Native
        // backend is the only thing that reads it. Authors validate their curves
        // against this dump instead of a hand-copied range table that drifts.
        const QString path = QString::fromLocal8Bit(argv[2]);
        window.move(-10000, -10000);
        QTimer::singleShot(1500, &app, [&] {
            const QJsonObject ranges = window.modelParameterRanges();
            QFile out(path);
            const bool saved = !ranges.isEmpty() && out.open(QIODevice::WriteOnly)
                && out.write(QJsonDocument(QJsonObject{
                       {QStringLiteral("render_backend"), window.renderBackend()},
                       {QStringLiteral("parameter_count"), ranges.size()},
                       {QStringLiteral("parameters"), ranges}}).toJson()) > 0;
            app.exit(saved ? 0 : 1);
        });
        return app.exec();
    }

    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--dump-motion")) {
        // Sample a clip through MotionClip itself so the approval report never
        // re-implements curve evaluation in Python, and dump the parameter set
        // the runtime always writes on its own. A library-less sampler takes the
        // procedural fallback path and touches no clip, so its values are exactly
        // the base skeleton an interrupted overlay decays back to.
        const QString clipId = QString::fromLocal8Bit(argv[2]);
        const QString path = QString::fromLocal8Bit(argv[3]);
        ParameterMotion sampler;
        QString motionError;
        if (!sampler.loadMotionLibrary(QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions")), &motionError)) {
            qWarning() << "Motion library:" << motionError;
            return 2;
        }
        const MotionClip* clip = sampler.library().clip(clipId);
        if (!clip || clip->duration() <= 0) return 2;
        ParameterMotion base;
        base.advance(0.05);
        constexpr double step = 1.0 / 60.0;
        const int count = std::max(1, static_cast<int>(std::lround(clip->duration() / step)));
        QJsonArray samples;
        for (int frame = 0; frame <= count; ++frame) {
            const double time = frame * step;
            const auto values = clip->sample(time);
            QJsonObject parameters;
            for (auto it = values.cbegin(); it != values.cend(); ++it)
                parameters.insert(it.key(), it.value());
            samples.append(QJsonObject{{QStringLiteral("t"), time},
                                       {QStringLiteral("parameters"), parameters}});
        }
        QJsonArray runtime;
        for (auto it = base.values().cbegin(); it != base.values().cend(); ++it)
            runtime.append(it.key());
        QFile out(path);
        const bool saved = out.open(QIODevice::WriteOnly)
            && out.write(QJsonDocument(QJsonObject{
                   {QStringLiteral("clip"), clipId},
                   {QStringLiteral("duration"), clip->duration()},
                   {QStringLiteral("loop"), clip->isLoop()},
                   {QStringLiteral("loop_start"), clip->loopStart()},
                   {QStringLiteral("loop_end"), clip->loopEnd()},
                   {QStringLiteral("additive"), clip->isAdditive()},
                   {QStringLiteral("step"), step},
                   {QStringLiteral("runtime_parameters"), runtime},
                   {QStringLiteral("samples"), samples}}).toJson()) > 0;
        return saved ? 0 : 2;
    }

    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render-smoke")) {
        const QString imagePath = QString::fromLocal8Bit(argv[2]);
        if (argc >= 4 && QString::fromLocal8Bit(argv[3]) == QStringLiteral("grass")) controller.playGrass();
        QTimer::singleShot(1500, &app, [&] {
            const bool saved = window.saveRenderFrame(imagePath);
            QFile report(imagePath + QStringLiteral(".json"));
            if (report.open(QIODevice::WriteOnly))
                report.write(QJsonDocument(QJsonObject{
                    {QStringLiteral("render_backend"), window.renderBackend()},
                    {QStringLiteral("render_error"), window.renderError()},
                    {QStringLiteral("render_samples"), window.renderSampleCount()},
                    {QStringLiteral("state"), controller.state() == PetController::State::Grass ? QStringLiteral("grass") : QStringLiteral("idle")},
                    {QStringLiteral("frame_saved"), saved}
                }).toJson());
            app.exit(saved ? 0 : 1);
        });
        return app.exec();
    }

    DesktopDeleteSource desktopSource;
    QObject::connect(&desktopSource, &DesktopDeleteSource::desktopItemDeleted,
                     &controller, &PetController::desktopItemDeleted);
    desktopSource.start(reinterpret_cast<HWND>(window.winId()));

    // Drag-and-drop feeding: dropping a file on the character eats it too.
    QObject::connect(&window, &PetWindow::filesDropped,
                     &controller, &PetController::fileDropped);

    CodexTurnSource codexSource;
    QObject::connect(&codexSource, &CodexTurnSource::turnStarted,
                     &controller, &PetController::turnStarted);
    QObject::connect(&codexSource, &CodexTurnSource::turnStopped,
                     &controller, &PetController::turnStopped);
    QObject::connect(&codexSource, &CodexTurnSource::sessionEnded,
                     &controller, &PetController::sessionEnded);
    codexSource.setStatusProvider([&controller, &desktopSource, &window] {
        QString state;
        switch (controller.state()) {
        case PetController::State::Idle: state = QStringLiteral("idle"); break;
        case PetController::State::Busy: state = QStringLiteral("busy"); break;
        case PetController::State::Delete: state = QStringLiteral("delete"); break;
        case PetController::State::Grass: state = QStringLiteral("grass"); break;
        }
        QJsonObject parameters;
        for (auto it = window.motionParameters().cbegin(); it != window.motionParameters().cend(); ++it)
            parameters.insert(it.key(), it.value());
        return QJsonObject{{QStringLiteral("state"), state},
                           {QStringLiteral("render_backend"), window.renderBackend()},
                           {QStringLiteral("render_error"), window.renderError()},
                           {QStringLiteral("cubism_parameters"), parameters},
                           {QStringLiteral("active_turns"), controller.activeTurnCount()},
                           {QStringLiteral("desktop_watching"), desktopSource.isWatching()},
                           {QStringLiteral("desktop_error"), desktopSource.error()},
                           {QStringLiteral("desktop_paths"), QJsonArray::fromStringList(desktopSource.desktopPaths())}};
    });
    codexSource.start();
    return app.exec();
}
