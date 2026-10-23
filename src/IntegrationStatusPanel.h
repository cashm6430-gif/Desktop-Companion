#pragma once

#include <QDialog>
#include <QJsonObject>
#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class QShowEvent;
class QHideEvent;
class QCloseEvent;

// The provider returns an intentionally small, display-safe event summary.
// The panel never owns the hook listener or changes the agent process.
class IntegrationStatusPanel final : public QDialog {
    Q_OBJECT
public:
    using SnapshotProvider = std::function<QJsonObject()>;
    explicit IntegrationStatusPanel(SnapshotProvider provider,
                                    std::function<void()> resetBusy,
                                    QWidget* parent = nullptr);

public slots:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    SnapshotProvider provider_;
    std::function<void()> resetBusy_;
    QTimer* refreshTimer_ = nullptr;
    QLabel* listener_ = nullptr;
    QLabel* listenerError_ = nullptr;
    QLabel* activeTurns_ = nullptr;
    QLabel* petState_ = nullptr;
    QLabel* lastEvent_ = nullptr;
    QTableWidget* clients_ = nullptr;
    QPushButton* resetButton_ = nullptr;
};
