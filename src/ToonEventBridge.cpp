#include "ToonEventBridge.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

#include <cmath>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool integerSequence(const QJsonValue& value, qint64* sequence) {
    const double number = value.toDouble(-1.0);
    // JSON numbers are exactly integral only through 2^53. A connection
    // would have to run for millennia to exhaust this range.
    if (!value.isDouble() || !std::isfinite(number) || number < 1.0
        || number > 9007199254740991.0 || std::floor(number) != number) return false;
    *sequence = static_cast<qint64>(number);
    return true;
}
}

ToonEventBridge::ToonEventBridge(PetController* controller, QObject* parent)
    : QObject(parent), controller_(controller), sessionId_(uuid()) {
    token_ = uuid().remove(QLatin1Char('-')) + uuid().remove(QLatin1Char('-'));
    controller_->setActionFallbackEnabled(false);
    connect(&server_, &QTcpServer::newConnection, this, &ToonEventBridge::acceptConnection);
    handshakeTimer_.setSingleShot(true);
    handshakeTimer_.setInterval(5000);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this] {
        if (peer_) dropConnection(peer_, QStringLiteral("handshake_timeout"));
    });
    actionTimer_.setSingleShot(true);
    actionTimer_.setInterval(kActionTtlMs);
    actionTimer_.setTimerType(Qt::PreciseTimer);
    connect(&actionTimer_, &QTimer::timeout, this, [this] {
        clearForeground(QStringLiteral("timeout"), true);
        publishActivity();
    });
    connect(controller_, &PetController::stateChanged, this, [this] { publishActivity(); });
    connect(controller_, &PetController::eatTriggered, this,
        [this](const QString&, const QPointF& iconPos, const QIcon&) {
            if (!requestAction(QStringLiteral("delete.react"), iconPos))
                controller_->actionFinished();
        });
    connect(controller_, &PetController::allTurnsStopped, this, [this] {
        // Never queue a low-priority reaction behind a deletion. Reconnection
        // likewise restores the current activity, not expired celebrations.
        if (foregroundRequestId_.isEmpty()) requestAction(QStringLiteral("work.finished"));
    });
    activityPoll_.setInterval(1000);
    connect(&activityPoll_, &QTimer::timeout, this, [this] {
        if (lastActiveTurns_ != controller_->activeTurnCount()) publishActivity();
    });
    activityPoll_.start();
}

ToonEventBridge::~ToonEventBridge() { shutdown(); }

bool ToonEventBridge::start() {
    return server_.isListening() || server_.listen(QHostAddress::LocalHost, 0);
}

void ToonEventBridge::record(const QString& event, const QJsonObject& fields) {
    QJsonObject record = fields;
    record.insert(QStringLiteral("event"), event);
    record.insert(QStringLiteral("at_ms"), QDateTime::currentMSecsSinceEpoch());
    emit trace(record);
}

QJsonObject ToonEventBridge::status() const {
    return {{QStringLiteral("render_backend"), QStringLiteral("godot_toon_candidate")},
        {QStringLiteral("protocol_version"), kProtocolVersion},
        {QStringLiteral("renderer_ready"), ready_},
        {QStringLiteral("session_id"), sessionId_},
        {QStringLiteral("connection_id"), connectionId_},
        {QStringLiteral("activity"), controller_->activeTurnCount() > 0
            ? QStringLiteral("busy") : QStringLiteral("idle")},
        {QStringLiteral("active_turns"), controller_->activeTurnCount()},
        {QStringLiteral("foreground_request_id"), foregroundRequestId_},
        {QStringLiteral("foreground_action"), foregroundAction_},
        {QStringLiteral("renderer_details"), rendererDetails_}};
}

void ToonEventBridge::acceptConnection() {
    while (server_.hasPendingConnections()) {
        QTcpSocket* socket = server_.nextPendingConnection();
        if (peer_ || shuttingDown_ || socket->peerAddress() != QHostAddress::LocalHost) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        peer_ = socket;
        // Bound Qt's socket buffer as well as our partial NDJSON line.
        socket->setReadBufferSize(kMaximumLineBytes + 1);
        buffer_.clear();
        connectionId_ = uuid();
        authenticated_ = ready_ = false;
        inboundSeq_ = outboundSeq_ = 0;
        capabilities_.clear();
        rendererDetails_ = {};
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readMessages(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            dropConnection(socket, QStringLiteral("disconnected"));
            socket->deleteLater();
        });
        handshakeTimer_.start();
        record(QStringLiteral("renderer_connected"));
        // A connection may already have received bytes before signal wiring.
        if (socket->bytesAvailable()) readMessages(socket);
    }
}

void ToonEventBridge::readMessages(QTcpSocket* socket) {
    if (peer_ != socket) return;
    while (socket->bytesAvailable() && peer_ == socket) {
        const qsizetype room = kMaximumLineBytes + 1 - buffer_.size();
        if (room <= 0) { dropConnection(socket, QStringLiteral("message_too_large")); return; }
        buffer_ += socket->read(room);
        qsizetype newline = -1;
        while (peer_ == socket && (newline = buffer_.indexOf('\n')) >= 0) {
            if (newline > kMaximumLineBytes) {
                dropConnection(socket, QStringLiteral("message_too_large")); return;
            }
            const QByteArray line = buffer_.left(newline);
            buffer_.remove(0, newline + 1);
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                dropConnection(socket, QStringLiteral("invalid_json")); return;
            }
            handleMessage(socket, document.object());
        }
        if (peer_ == socket && buffer_.size() > kMaximumLineBytes) {
            dropConnection(socket, QStringLiteral("message_too_large")); return;
        }
    }
}

void ToonEventBridge::handleMessage(QTcpSocket* socket, const QJsonObject& message) {
    if (message.value(QStringLiteral("v")).toInt() != kProtocolVersion) {
        dropConnection(socket, QStringLiteral("protocol_version")); return;
    }
    const QString type = message.value(QStringLiteral("type")).toString();
    if (!authenticated_) {
        if (type != QStringLiteral("hello")
            || message.value(QStringLiteral("token")).toString() != token_) {
            dropConnection(socket, QStringLiteral("authentication_failed")); return;
        }
        authenticated_ = true;
        send({{QStringLiteral("type"), QStringLiteral("welcome")}});
        return;
    }
    qint64 sequence = 0;
    if (message.value(QStringLiteral("session_id")).toString() != sessionId_
        || message.value(QStringLiteral("connection_id")).toString() != connectionId_
        || !integerSequence(message.value(QStringLiteral("seq")), &sequence)
        || sequence <= inboundSeq_) {
        record(QStringLiteral("stale_message_rejected"), {{QStringLiteral("type"), type}});
        return;
    }
    inboundSeq_ = sequence;
    if (type == QStringLiteral("ready") && !ready_) {
        const QJsonArray capabilities = message.value(QStringLiteral("capabilities")).toArray();
        if (capabilities.size() > 64) {
            dropConnection(socket, QStringLiteral("invalid_capabilities")); return;
        }
        for (const auto& capability : capabilities) {
            if (!capability.isString() || capability.toString().size() > 64) {
                dropConnection(socket, QStringLiteral("invalid_capabilities")); return;
            }
            capabilities_.insert(capability.toString());
        }
        if (!capabilities_.contains(QStringLiteral("activity.idle"))
            || !capabilities_.contains(QStringLiteral("activity.busy"))) {
            dropConnection(socket, QStringLiteral("activity_not_supported")); return;
        }
        ready_ = true;
        rendererDetails_ = message.value(QStringLiteral("capability_details")).toObject();
        handshakeTimer_.stop();
        record(QStringLiteral("renderer_ready"), {{QStringLiteral("capabilities"), capabilities},
            {QStringLiteral("capability_details"), rendererDetails_}});
        emit rendererReadyChanged(true);
        publishActivity();
    } else if (type == QStringLiteral("action.ack") && ready_) {
        const QString id = message.value(QStringLiteral("request_id")).toString();
        const QString status = message.value(QStringLiteral("status")).toString();
        if (id.isEmpty() || id != foregroundRequestId_) {
            record(QStringLiteral("stale_ack_rejected"), {{QStringLiteral("request_id"), id}});
            return;
        }
        if (status != QStringLiteral("started") && status != QStringLiteral("finished")
            && status != QStringLiteral("interrupted") && status != QStringLiteral("rejected")) {
            record(QStringLiteral("invalid_ack_rejected")); return;
        }
        record(QStringLiteral("action_ack"), {{QStringLiteral("request_id"), id},
            {QStringLiteral("status"), status}});
        if (status != QStringLiteral("started")) {
            clearForeground(status, false);
            publishActivity();
        }
    } else if (type == QStringLiteral("client.quit") && ready_) {
        emit quitRequested();
    }
}

bool ToonEventBridge::send(QJsonObject message) {
    if (!peer_ || !authenticated_) return false;
    if (peer_->bytesToWrite() > 128 * 1024) {
        dropConnection(peer_, QStringLiteral("outbound_backpressure")); return false;
    }
    message.insert(QStringLiteral("v"), kProtocolVersion);
    message.insert(QStringLiteral("session_id"), sessionId_);
    message.insert(QStringLiteral("connection_id"), connectionId_);
    message.insert(QStringLiteral("seq"), ++outboundSeq_);
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaximumLineBytes) return false;
    if (peer_->write(bytes + '\n') < 0) return false;
    record(QStringLiteral("outbound"), {{QStringLiteral("message"), message}});
    return true;
}

void ToonEventBridge::publishActivity() {
    lastActiveTurns_ = controller_->activeTurnCount();
    ++revision_;
    if (!ready_) return;
    send({{QStringLiteral("type"), QStringLiteral("activity.snapshot")},
        {QStringLiteral("revision"), revision_},
        {QStringLiteral("busy"), lastActiveTurns_ > 0},
        {QStringLiteral("active_turns"), lastActiveTurns_},
        {QStringLiteral("foreground_request_id"), foregroundRequestId_}});
}

bool ToonEventBridge::requestAction(const QString& name, const QPointF& iconPos) {
    if (!ready_ || !capabilities_.contains(name)) {
        record(QStringLiteral("action_dropped"), {{QStringLiteral("name"), name},
            {QStringLiteral("reason"), ready_ ? QStringLiteral("unsupported") : QStringLiteral("renderer_unavailable")}});
        return false;
    }
    if (!foregroundRequestId_.isEmpty()) {
        if (name != QStringLiteral("delete.react")) return false;
        clearForeground(QStringLiteral("priority"), true);
    }
    foregroundRequestId_ = QStringLiteral("%1:%2").arg(connectionId_).arg(++requestCounter_);
    foregroundAction_ = name;
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("action.request")},
        {QStringLiteral("request_id"), foregroundRequestId_},
        {QStringLiteral("name"), name}, {QStringLiteral("ttl_ms"), kActionTtlMs}};
    if (!iconPos.isNull() && std::isfinite(iconPos.x()) && std::isfinite(iconPos.y()))
        message.insert(QStringLiteral("icon_position"), QJsonObject{
            {QStringLiteral("x"), iconPos.x()}, {QStringLiteral("y"), iconPos.y()}});
    if (!send(message)) {
        clearForeground(QStringLiteral("send_failed"), false);
        return false;
    }
    actionTimer_.start();
    publishActivity();
    return true;
}

void ToonEventBridge::clearForeground(const QString& reason, bool notifyRenderer) {
    if (foregroundRequestId_.isEmpty()) return;
    const QString id = foregroundRequestId_;
    const QString action = foregroundAction_;
    foregroundRequestId_.clear();
    foregroundAction_.clear();
    actionTimer_.stop();
    if (notifyRenderer && ready_) send({{QStringLiteral("type"), QStringLiteral("action.cancel")},
        {QStringLiteral("request_id"), id}, {QStringLiteral("reason"), reason}});
    record(QStringLiteral("action_cleared"), {{QStringLiteral("request_id"), id},
        {QStringLiteral("name"), action}, {QStringLiteral("reason"), reason}});
    // Only a new deletion can preempt a deletion. Preserve the controller's
    // Delete state until that replacement request finishes.
    if (action == QStringLiteral("delete.react") && reason != QStringLiteral("priority"))
        controller_->actionFinished();
}

void ToonEventBridge::dropConnection(QTcpSocket* socket, const QString& reason) {
    if (peer_ != socket) return;
    const bool wasReady = ready_;
    peer_.clear();
    authenticated_ = ready_ = false;
    handshakeTimer_.stop();
    buffer_.clear();
    capabilities_.clear();
    rendererDetails_ = {};
    clearForeground(reason, false);
    // This also repairs a delete that arrived while the renderer was absent.
    controller_->actionFinished();
    if (socket->state() != QAbstractSocket::UnconnectedState) socket->abort();
    record(QStringLiteral("renderer_disconnected"), {{QStringLiteral("reason"), reason}});
    if (wasReady) emit rendererReadyChanged(false);
}

void ToonEventBridge::turnStarted(const QString& sessionId, const QString& turnId) {
    controller_->turnStarted(sessionId, turnId);
    if (controller_->activeTurnCount() > 0 && foregroundAction_ == QStringLiteral("work.finished"))
        clearForeground(QStringLiteral("activity_changed"), true);
    // Busy-to-busy concurrent starts do not emit PetController::stateChanged.
    publishActivity();
}
void ToonEventBridge::turnStopped(const QString& sessionId, const QString& turnId) {
    controller_->turnStopped(sessionId, turnId);
    publishActivity();
}
void ToonEventBridge::sessionEnded(const QString& sessionId) {
    controller_->sessionEnded(sessionId);
    publishActivity();
}
void ToonEventBridge::desktopItemDeleted(const QString& path, const QPointF& iconPos, const QIcon& icon) {
    controller_->desktopItemDeleted(path, iconPos, icon);
}
void ToonEventBridge::resetBusy() {
    controller_->resetBusy();
    publishActivity();
}
bool ToonEventBridge::wave() { return requestAction(QStringLiteral("wave")); }

void ToonEventBridge::shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    if (ready_) {
        send({{QStringLiteral("type"), QStringLiteral("host.shutdown")}});
        if (peer_) peer_->flush();
    }
    server_.close();
    activityPoll_.stop();
    if (peer_) dropConnection(peer_, QStringLiteral("host_shutdown"));
}
