#include "IntegrationStatusPanel.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFont>
#include <QGridLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
const QStringList clientIds = {QStringLiteral("codex"), QStringLiteral("workbuddy"),
                               QStringLiteral("claude"), QStringLiteral("unknown"),
                               QStringLiteral("manual")};
const QStringList clientNames = {QStringLiteral("Codex"), QStringLiteral("WorkBuddy"),
                                 QStringLiteral("Claude"), QStringLiteral("未知来源"),
                                 QStringLiteral("手动操作")};

int clientRow(const QString& id) {
    const auto row = clientIds.indexOf(id);
    return row < 0 ? 3 : int(row);
}

QString eventText(const QString& event) {
    if (event == QStringLiteral("start")) return QStringLiteral("收到开始");
    if (event == QStringLiteral("stop")) return QStringLiteral("收到停止");
    if (event == QStringLiteral("session_end")) return QStringLiteral("收到会话结束");
    if (event == QStringLiteral("reset") || event == QStringLiteral("manual_reset"))
        return QStringLiteral("手动复位");
    return QStringLiteral("收到事件");
}

QString receivedTime(const QString& iso) {
    auto date = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(iso, Qt::ISODate);
    return date.isValid() ? date.toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss"))
                          : QStringLiteral("时间未知");
}

QString stateText(const QString& state) {
    if (state == QStringLiteral("busy")) return QStringLiteral("忙碌");
    if (state == QStringLiteral("delete")) return QStringLiteral("删除互动");
    if (state == QStringLiteral("grass")) return QStringLiteral("狗尾巴草互动");
    if (state == QStringLiteral("idle")) return QStringLiteral("空闲");
    return QStringLiteral("暂无状态");
}

QLabel* plainLabel(const QString& text, QWidget* parent, const QString& name = {}) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(name);
    label->setWordWrap(true);
    return label;
}
}

IntegrationStatusPanel::IntegrationStatusPanel(SnapshotProvider provider,
                                             std::function<void()> resetBusy,
                                             QWidget* parent)
    : QDialog(parent), provider_(std::move(provider)), resetBusy_(std::move(resetBusy)) {
    setObjectName(QStringLiteral("integrationStatusPanel"));
    setWindowTitle(QStringLiteral("联动状态"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto panelFont = font();
    if (panelFont.pointSizeF() < 10.0) panelFont.setPointSizeF(10.0);
    setFont(panelFont);
    setStyleSheet(QStringLiteral(
        "QDialog { background:#ffffff; color:#2b3a4b; }"
        "QLabel { color:#2b3a4b; }"
        "QLabel#statusTitle { color:#205d9d; font-size:15pt; font-weight:600; }"
        "QLabel#statusNote, QLabel#resetExplanation { color:#637487; }"
        "QLabel#listenerError { color:#ad4f34; }"
        "QTableWidget { background:#ffffff; alternate-background-color:#f4f8fd;"
        " color:#2b3a4b; border:1px solid #dce5ef; gridline-color:#e8eef6; }"
        "QHeaderView::section { background:#edf4fc; color:#315878; border:0;"
        " padding:7px; font-weight:600; }"
        "QPushButton { background:#edf4fc; color:#205d9d; border:1px solid #cfdeed;"
        " border-radius:6px; padding:7px 14px; }"
        "QPushButton:hover { background:#dfedfc; }"
        "QPushButton:disabled { background:#f4f6f8; color:#94a1ad; border-color:#e5e9ef; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    layout->addWidget(plainLabel(QStringLiteral("鲸鱼娘 · 联动状态"), this,
                                QStringLiteral("statusTitle")));
    auto* summary = new QGridLayout;
    summary->setHorizontalSpacing(20);
    summary->setVerticalSpacing(8);
    summary->addWidget(plainLabel(QStringLiteral("hook 接收器"), this), 0, 0);
    listener_ = plainLabel({}, this, QStringLiteral("listenerStatus"));
    summary->addWidget(listener_, 0, 1);
    summary->addWidget(plainLabel(QStringLiteral("活跃回合"), this), 1, 0);
    activeTurns_ = plainLabel({}, this, QStringLiteral("activeTurns"));
    summary->addWidget(activeTurns_, 1, 1);
    summary->addWidget(plainLabel(QStringLiteral("桌宠状态"), this), 2, 0);
    petState_ = plainLabel({}, this, QStringLiteral("petState"));
    summary->addWidget(petState_, 2, 1);
    summary->setColumnStretch(1, 1);
    layout->addLayout(summary);
    listenerError_ = plainLabel(QStringLiteral("接收器启动失败；请检查是否已有桌宠实例运行。"), this,
                                QStringLiteral("listenerError"));
    layout->addWidget(listenerError_);

    clients_ = new QTableWidget(clientIds.size(), 3, this);
    clients_->setObjectName(QStringLiteral("clientsTable"));
    clients_->setHorizontalHeaderLabels({QStringLiteral("来源"), QStringLiteral("最近事件"),
                                         QStringLiteral("收到时间（本地）")});
    clients_->verticalHeader()->hide();
    clients_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    clients_->setSelectionMode(QAbstractItemView::NoSelection);
    clients_->setFocusPolicy(Qt::NoFocus);
    clients_->setAlternatingRowColors(true);
    clients_->setWordWrap(true);
    clients_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    clients_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    clients_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    for (int row = 0; row < clientIds.size(); ++row) {
        for (int column = 0; column < 3; ++column) {
            clients_->setItem(row, column, new QTableWidgetItem);
        }
        clients_->item(row, 0)->setText(clientNames[row]);
    }
    layout->addWidget(clients_);
    lastEvent_ = plainLabel({}, this, QStringLiteral("lastEvent"));
    layout->addWidget(lastEvent_);
    layout->addWidget(plainLabel(
        QStringLiteral("hook 按事件短连接；最近收到不代表客户端仍在线。旧 hooks 可能显示未知来源。"),
        this, QStringLiteral("statusNote")));

    resetButton_ = new QPushButton(QStringLiteral("结束忙碌（复位）"), this);
    resetButton_->setObjectName(QStringLiteral("resetBusyButton"));
    layout->addWidget(resetButton_, 0, Qt::AlignLeft);
    layout->addWidget(plainLabel(QStringLiteral("仅清除桌宠本地的忙碌状态，不会停止 agent。"),
                                this, QStringLiteral("resetExplanation")));
    connect(resetButton_, &QPushButton::clicked, this, [this] {
        if (resetBusy_) resetBusy_();
        refresh();
    });

    auto* buttons = new QDialogButtonBox(this);
    auto* close = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    close->setObjectName(QStringLiteral("closeStatusButton"));
    close->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setObjectName(QStringLiteral("statusRefreshTimer"));
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, &IntegrationStatusPanel::refresh);
    refresh();
    resize(sizeHint().expandedTo(QSize(510, 0)));
}

void IntegrationStatusPanel::refresh() {
    const auto snapshot = provider_ ? provider_() : QJsonObject{};
    const auto listening = snapshot.value(QStringLiteral("listening")).toBool();
    listener_->setText(listening ? QStringLiteral("监听中") : QStringLiteral("未监听"));
    listenerError_->setVisible(!listening &&
                              !snapshot.value(QStringLiteral("listener_error")).toString().isEmpty());
    const auto count = std::max(0, snapshot.value(QStringLiteral("active_turns")).toInt());
    activeTurns_->setText(QString::number(count));
    resetButton_->setEnabled(count > 0 && bool(resetBusy_));
    petState_->setText(stateText(snapshot.value(QStringLiteral("pet_state")).toString()));

    for (int row = 0; row < clientIds.size(); ++row) {
        clients_->item(row, 1)->setText(QStringLiteral("尚未收到"));
        clients_->item(row, 2)->setText(QStringLiteral("—"));
    }
    for (const auto& value : snapshot.value(QStringLiteral("clients")).toArray()) {
        const auto client = value.toObject();
        const auto event = client.value(QStringLiteral("last_event")).toString();
        const auto at = client.value(QStringLiteral("last_seen")).toString();
        if (event.isEmpty() && at.isEmpty()) continue;
        const auto row = clientRow(client.value(QStringLiteral("id")).toString());
        clients_->item(row, 1)->setText(eventText(event));
        clients_->item(row, 2)->setText(receivedTime(at));
    }
    clients_->resizeRowsToContents();
    int tableHeight = clients_->horizontalHeader()->height() + 2 * clients_->frameWidth();
    for (int row = 0; row < clients_->rowCount(); ++row) tableHeight += clients_->rowHeight(row);
    clients_->setMinimumHeight(tableHeight);

    const auto last = snapshot.value(QStringLiteral("last_event")).toObject();
    if (last.isEmpty()) {
        lastEvent_->setText(QStringLiteral("最近事件：尚未收到"));
    } else {
        lastEvent_->setText(QStringLiteral("最近事件：%1 · %2\n%3（本地时间）")
            .arg(clientNames[clientRow(last.value(QStringLiteral("client")).toString())],
                 eventText(last.value(QStringLiteral("event")).toString()),
                 receivedTime(last.value(QStringLiteral("at")).toString())));
    }
}

void IntegrationStatusPanel::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refresh();
    refreshTimer_->start();
}

void IntegrationStatusPanel::hideEvent(QHideEvent* event) {
    refreshTimer_->stop();
    QDialog::hideEvent(event);
}

void IntegrationStatusPanel::closeEvent(QCloseEvent* event) {
    refreshTimer_->stop();
    QDialog::closeEvent(event);
}
