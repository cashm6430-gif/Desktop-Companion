#include "IntegrationStatusModel.h"

#include <QJsonArray>

#include <algorithm>
#include <utility>

namespace {
const std::array<QString, 5> clientIds{
    QStringLiteral("codex"), QStringLiteral("workbuddy"), QStringLiteral("claude"),
    QStringLiteral("unknown"), QStringLiteral("manual")};

int clientIndex(const QString& client) {
    const auto found = std::find(clientIds.cbegin(), clientIds.cend(), client);
    return found == clientIds.cend() ? 3 : int(found - clientIds.cbegin());
}

QString canonicalEvent(const QString& event) {
    if (event == QStringLiteral("start")) return QStringLiteral("start");
    if (event == QStringLiteral("stop")) return QStringLiteral("stop");
    if (event == QStringLiteral("session_end")) return QStringLiteral("session_end");
    return {};
}

QString publicState(const QString& state) {
    if (state == QStringLiteral("idle") || state == QStringLiteral("busy") ||
        state == QStringLiteral("delete") || state == QStringLiteral("grass"))
        return state;
    return QStringLiteral("unknown");
}

QString utcTimestamp(const QDateTime& at) {
    return at.toUTC().toString(Qt::ISODateWithMs);
}
} // namespace

IntegrationStatusModel::IntegrationStatusModel(QObject* parent, Clock now)
    : QObject(parent),
      now_(now ? std::move(now) : Clock([] { return QDateTime::currentDateTimeUtc(); })) {}

void IntegrationStatusModel::observeEvent(const QString& client, const QString& event) {
    const auto accepted = canonicalEvent(event);
    if (accepted.isEmpty()) return;
    record(clientIndex(client), accepted);
}

void IntegrationStatusModel::recordReset() {
    record(4, QStringLiteral("reset"));
}

void IntegrationStatusModel::record(int index, const QString& event) {
    receipts_[index] = {event, now_().toUTC()};
    // Latest means last received, even if the wall clock moved backwards.
    latestClient_ = index;
    emit changed();
}

QJsonObject IntegrationStatusModel::snapshot(bool listening, const QString& listenerError,
                                           int activeTurns, const QString& state) const {
    QJsonArray clients;
    for (int i = 0; i < int(receipts_.size()); ++i) {
        const auto& receipt = receipts_[i];
        clients.append(QJsonObject{{QStringLiteral("id"), clientIds[i]},
                                   {QStringLiteral("last_event"), receipt.event},
                                   {QStringLiteral("last_seen"), utcTimestamp(receipt.at)}});
    }

    QJsonObject latest;
    if (latestClient_ >= 0) {
        const auto& receipt = receipts_[latestClient_];
        latest = {{QStringLiteral("client"), clientIds[latestClient_]},
                  {QStringLiteral("event"), receipt.event},
                  {QStringLiteral("at"), utcTimestamp(receipt.at)}};
    }

    return {{QStringLiteral("listening"), listening},
            {QStringLiteral("listener_error"), listenerError},
            {QStringLiteral("active_turns"), std::max(0, activeTurns)},
            {QStringLiteral("pet_state"), publicState(state)},
            {QStringLiteral("last_event"), latest},
            {QStringLiteral("clients"), clients}};
}
