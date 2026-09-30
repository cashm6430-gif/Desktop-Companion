#include "CodexTurnSource.h"
#include "DesktopDeleteSource.h"
#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    PetController controller;
    PetWindow window(&controller);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &window, &PetWindow::shutdown);
    window.show();

    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render-smoke")) {
        const QString imagePath = QString::fromLocal8Bit(argv[2]);
        QTimer::singleShot(1500, &app, [&] {
            const bool saved = window.saveRenderFrame(imagePath);
            QFile report(imagePath + QStringLiteral(".json"));
            if (report.open(QIODevice::WriteOnly))
                report.write(QJsonDocument(QJsonObject{
                    {QStringLiteral("render_backend"), window.renderBackend()},
                    {QStringLiteral("render_error"), window.renderError()},
                    {QStringLiteral("render_samples"), window.renderSampleCount()},
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
