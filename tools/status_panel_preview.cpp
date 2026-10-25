// Isolated QWidget/IPC acceptance capture. No hooks, desktop watcher, settings,
// pet assets, or default listener endpoint are used.
//
//     IntegrationStatusPanelPreview <new-output-directory>
#include "../src/CodexTurnSource.h"
#include "../src/IntegrationStatusModel.h"
#include "../src/IntegrationStatusPanel.h"
#include "../src/PetController.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalSocket>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QDebug>

namespace {
QString stateName(const PetController& controller) {
    switch (controller.state()) {
    case PetController::State::Idle: return QStringLiteral("idle");
    case PetController::State::Busy: return QStringLiteral("busy");
    case PetController::State::Delete: return QStringLiteral("delete");
    case PetController::State::Grass: return QStringLiteral("grass");
    }
    return QStringLiteral("unknown");
}

void connectProjection(CodexTurnSource& source, PetController& controller,
                       IntegrationStatusModel& model) {
    // Identical event ordering to the normal application's listener wiring.
    QObject::connect(&source, &CodexTurnSource::turnStarted,
                     &controller, &PetController::turnStarted);
    QObject::connect(&source, &CodexTurnSource::turnStopped,
                     &controller, &PetController::turnStopped);
    QObject::connect(&source, &CodexTurnSource::sessionEnded,
                     &controller, &PetController::sessionEnded);
    QObject::connect(&source, &CodexTurnSource::eventReceived,
                     &model, &IntegrationStatusModel::observeEvent);
}

bool request(const QString& endpoint, const QJsonObject& payload, QByteArray* response = nullptr) {
    QLocalSocket socket;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool complete = false;
    bool failed = false;
    QByteArray bytes;
    const auto line = QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n';
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        failed = true;
        loop.quit();
    });
    QObject::connect(&socket, &QLocalSocket::connected, &loop, [&] {
        if (socket.write(line) != line.size()) {
            failed = true;
            loop.quit();
            return;
        }
        socket.flush();
    });
    QObject::connect(&socket, &QLocalSocket::readyRead, &loop, [&] { bytes += socket.readAll(); });
    QObject::connect(&socket, &QLocalSocket::disconnected, &loop, [&] {
        if (socket.bytesAvailable() > 0) bytes += socket.readAll();
        complete = true;
        loop.quit();
    });
    QObject::connect(&socket, &QLocalSocket::errorOccurred, &loop,
                     [&](QLocalSocket::LocalSocketError error) {
        if (error != QLocalSocket::PeerClosedError) {
            failed = true;
            loop.quit();
        }
    });
    QTimer::singleShot(0, &loop, [&] { socket.connectToServer(endpoint); });
    deadline.start(2000);
    loop.exec();
    if (response) *response = bytes;
    return complete && !failed;
}

QJsonObject event(const QString& action, const QString& client, const QString& session,
                  const QString& turn = {}) {
    QJsonObject result{{QStringLiteral("event"), action},
                       {QStringLiteral("client"), client},
                       {QStringLiteral("session_id"), session}};
    if (!turn.isEmpty()) result.insert(QStringLiteral("turn_id"), turn);
    return result;
}

bool capture(IntegrationStatusPanel& panel, const QString& path) {
    panel.refresh();
    panel.setAttribute(Qt::WA_DontShowOnScreen, true);
    panel.show();
    QEventLoop settle;
    QTimer::singleShot(40, &settle, &QEventLoop::quit);
    settle.exec();
    return panel.grab().save(path);
}

bool writeJson(const QString& path, const QJsonObject& object) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size();
}
} // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_WIN
        // The app deploys qwindows.dll. Widgets stay off-screen via
        // WA_DontShowOnScreen without requiring another platform plugin.
        qputenv("QT_QPA_PLATFORM", "windows");
#else
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }
    QApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        qCritical() << "Usage: IntegrationStatusPanelPreview <new-output-directory>";
        return 2;
    }
    const auto outputPath = QFileInfo(app.arguments()[1]).absoluteFilePath();
    if (QFileInfo::exists(outputPath)) {
        qCritical() << "Refusing an existing output path:" << outputPath;
        return 2;
    }
    QDir output(outputPath);
    if (!output.mkpath(QStringLiteral("."))) {
        qCritical() << "Could not create output directory:" << outputPath;
        return 2;
    }
    const auto fail = [](const char* message) {
        qCritical().noquote() << message;
        return 1;
    };

    // UUID namespace is private to this process; never resolve serverName().
    const QString endpoint = QStringLiteral("pet-status-preview-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    CodexTurnSource source;
    PetController controller;
    IntegrationStatusModel model;
    connectProjection(source, controller, model);
    const auto status = [&] {
        return model.snapshot(source.isListening(), source.error(),
                              controller.activeTurnCount(), stateName(controller));
    };
    source.setStatusProvider([&] {
        return QJsonObject{{QStringLiteral("state"), stateName(controller)},
                           {QStringLiteral("active_turns"), controller.activeTurnCount()},
                           {QStringLiteral("integration"), status()}};
    });
    if (!source.start(endpoint)) return fail("Private preview listener could not start.");

    int resetCalls = 0;
    IntegrationStatusPanel panel(status, [&] {
        ++resetCalls;
        controller.resetBusy();
        model.recordReset();
    });
    QJsonArray trace;
    const auto record = [&](const QString& step) {
        trace.append(QJsonObject{{QStringLiteral("step"), step},
                                  {QStringLiteral("snapshot"), status()}});
    };
    record(QStringLiteral("initial"));
    if (!request(endpoint, event(QStringLiteral("start"), QStringLiteral("codex"),
                                 QStringLiteral("preview-a"), QStringLiteral("turn-a"))) ||
        controller.activeTurnCount() != 1)
        return fail("First real IPC start did not produce exactly one active turn.");
    record(QStringLiteral("codex_start"));
    if (!request(endpoint, event(QStringLiteral("start"), QStringLiteral("opencode"),
                                 QStringLiteral("preview-b"), QStringLiteral("turn-b"))) ||
        controller.activeTurnCount() != 2)
        return fail("Concurrent real IPC starts did not produce exactly two active turns.");
    record(QStringLiteral("opencode_start"));
    const auto latestBeforeStatus = status().value(QStringLiteral("last_event"));
    QByteArray response;
    if (!request(endpoint, {{QStringLiteral("event"), QStringLiteral("status")}}, &response))
        return fail("Isolated status IPC query did not finish.");
    const auto returned = QJsonDocument::fromJson(response).object();
    if (returned.value(QStringLiteral("integration")).toObject() != status() ||
        status().value(QStringLiteral("last_event")) != latestBeforeStatus)
        return fail("Status IPC query changed event history or disagreed with the live model.");
    if (!capture(panel, output.filePath(QStringLiteral("panel-listening.png"))))
        return fail("Could not capture the actual listening QWidget.");
    const auto* activeLabel = panel.findChild<QLabel*>(QStringLiteral("activeTurns"));
    if (!activeLabel || activeLabel->text() != QStringLiteral("2"))
        return fail("Panel active-turn label did not show the real count of two.");

    if (!request(endpoint, event(QStringLiteral("stop"), QStringLiteral("codex"),
                                 QStringLiteral("preview-a"), QStringLiteral("turn-a"))) ||
        controller.activeTurnCount() != 1)
        return fail("Real stop did not leave the other client's active turn intact.");
    record(QStringLiteral("codex_stop"));
    if (!request(endpoint, event(QStringLiteral("session_end"), QStringLiteral("opencode"),
                                 QStringLiteral("preview-b"))) || controller.activeTurnCount() != 0)
        return fail("Real session_end did not clear its own final active turn.");
    record(QStringLiteral("opencode_session_end"));
    if (!request(endpoint, event(QStringLiteral("start"), QStringLiteral("workbuddy"),
                                 QStringLiteral("preview-c"), QStringLiteral("turn-c"))) ||
        controller.activeTurnCount() != 1)
        return fail("Real IPC start before reset did not restore Busy.");
    record(QStringLiteral("workbuddy_start_before_reset"));
    panel.refresh();
    auto* resetButton = panel.findChild<QPushButton*>(QStringLiteral("resetBusyButton"));
    if (!resetButton || !resetButton->isEnabled()) return fail("Real reset button is unavailable.");
    resetButton->click();
    record(QStringLiteral("panel_button_reset"));
    const auto latest = status().value(QStringLiteral("last_event")).toObject();
    if (resetCalls != 1 || controller.activeTurnCount() != 0 ||
        controller.state() != PetController::State::Idle || resetButton->isEnabled() ||
        latest.value(QStringLiteral("client")).toString() != QStringLiteral("manual") ||
        latest.value(QStringLiteral("event")).toString() != QStringLiteral("reset"))
        return fail("Actual panel reset did not clear local Busy and record manual/reset.");
    if (!capture(panel, output.filePath(QStringLiteral("panel-reset.png"))))
        return fail("Could not capture the actual reset QWidget.");
    panel.hide();

    // A deliberately invalid, isolated endpoint deterministically exercises a
    // real listen failure on Windows too (same-name named pipes may coexist).
    CodexTurnSource failedSource;
    PetController failedController;
    IntegrationStatusModel failedModel;
    connectProjection(failedSource, failedController, failedModel);
    if (failedSource.start(endpoint + QString(1024, QLatin1Char('x'))) ||
        failedSource.isListening() || failedSource.error().isEmpty())
        return fail("Invalid isolated endpoint did not produce a real listen failure.");
    const auto failedStatus = [&] {
        return failedModel.snapshot(failedSource.isListening(), failedSource.error(),
                                    failedController.activeTurnCount(), stateName(failedController));
    };
    IntegrationStatusPanel failedPanel(failedStatus, {});
    if (!capture(failedPanel, output.filePath(QStringLiteral("panel-listener-failed.png"))))
        return fail("Could not capture the actual failed-listener QWidget.");
    failedPanel.hide();
    trace.append(QJsonObject{{QStringLiteral("step"), QStringLiteral("listener_failed")},
                             {QStringLiteral("snapshot"), failedStatus()}});

    const QJsonObject report{
        {QStringLiteral("schema"), 1}, {QStringLiteral("passed"), true},
        {QStringLiteral("scope"), QStringLiteral("isolated real IPC/controller/model and QWidget button")},
        {QStringLiteral("physical_tray_routing_tested"), false},
        {QStringLiteral("default_endpoint_used"), false},
        {QStringLiteral("failure_trigger"), QStringLiteral("invalid isolated endpoint length")},
        {QStringLiteral("status_query_preserved_history"), true},
        {QStringLiteral("reset_button_calls"), resetCalls},
        {QStringLiteral("images"), QJsonArray{QStringLiteral("panel-listening.png"),
            QStringLiteral("panel-reset.png"), QStringLiteral("panel-listener-failed.png")}},
        {QStringLiteral("trace"), trace}};
    if (!writeJson(output.filePath(QStringLiteral("status-panel-check.json")), report))
        return fail("Could not save the isolated acceptance trace.");
    qInfo().noquote() << "PASS: isolated IPC counts 1 -> 2 -> 1 -> 0, actual reset button, and listener failure.";
    return 0;
}
