#include "CodexTurnSource.h"
#include "DesktopDeleteSource.h"
#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    PetController controller;
    PetWindow window(&controller);
    window.show();

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
    codexSource.setStatusProvider([&controller, &desktopSource] {
        QString state;
        switch (controller.state()) {
        case PetController::State::Idle: state = QStringLiteral("idle"); break;
        case PetController::State::Busy: state = QStringLiteral("busy"); break;
        case PetController::State::Delete: state = QStringLiteral("delete"); break;
        case PetController::State::Grass: state = QStringLiteral("grass"); break;
        }
        return QJsonObject{{QStringLiteral("state"), state},
                           {QStringLiteral("active_turns"), controller.activeTurnCount()},
                           {QStringLiteral("desktop_watching"), desktopSource.isWatching()},
                           {QStringLiteral("desktop_error"), desktopSource.error()},
                           {QStringLiteral("desktop_paths"), QJsonArray::fromStringList(desktopSource.desktopPaths())}};
    });
    codexSource.start();
    return app.exec();
}
