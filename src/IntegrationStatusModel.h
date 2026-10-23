#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <array>
#include <functional>

// A bounded, in-memory receipt summary. It knows neither session identifiers nor
// prompt content, and it does not infer whether a client is online or installed.
class IntegrationStatusModel final : public QObject {
    Q_OBJECT

public:
    using Clock = std::function<QDateTime()>;

    explicit IntegrationStatusModel(QObject* parent = nullptr, Clock now = {});

    void observeEvent(const QString& client, const QString& event);
    void recordReset();
    // Counts and pet state always come from the controller at read time.
    QJsonObject snapshot(bool listening, const QString& listenerError,
                         int activeTurns, const QString& state) const;

signals:
    void changed();

private:
    struct Receipt {
        QString event;
        QDateTime at;
    };

    void record(int clientIndex, const QString& event);

    Clock now_;
    std::array<Receipt, 5> receipts_;
    int latestClient_ = -1;
};
