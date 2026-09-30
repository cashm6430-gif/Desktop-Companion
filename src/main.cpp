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
        const bool laptop = clipId == QStringLiteral("busy-laptop");
        const bool grass = clipId == QStringLiteral("grass");
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
        if (laptop) {
            sampler.advance(0.001);
            sampler.forceLaptopBusy();
        } else if (grass) {
            sampler.setPreviewPose(sampler.grassPose(0.0));
            sampler.setState(PetController::State::Grass);
        } else {
            directClip = sampler.library().clip(clipId);
            if (!directClip) return 2;
            sampler.setPreviewPose(directClip->sample(0.0));
        }
        constexpr double step = 1.0 / 15.0;
        const int frameCount = laptop ? 195 : grass ? 120
            : std::max(1, static_cast<int>(std::lround(directClip->duration() / step)));
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
                || !window.renderSequenceFrame(pose, step, QDir(output).filePath(QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'))))) {
                app.exit(1); return;
            }
            if (++frame == frameCount) { app.exit(0); return; }
            if (directClip) return; // The pose above already advanced one frame.
            // Fixed motion time gives a reproducible 15 FPS review even when
            // writing a large PNG takes longer than the desktop frame interval.
            sampler.advance(step);
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
                && window.saveRenderFrame(QDir(output).filePath(QStringLiteral("grass-%1.png").arg(index - 1, 2, 10, QChar('0'))));
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
