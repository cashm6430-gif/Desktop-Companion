#include "../src/IntegrationStatusPanel.h"

#include <QDateTime>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QtTest/QTest>

namespace {
QJsonObject idleSnapshot() {
    return {{QStringLiteral("listening"), true}, {QStringLiteral("active_turns"), 0},
            {QStringLiteral("pet_state"), QStringLiteral("idle")},
            {QStringLiteral("clients"), QJsonArray{}}};
}

QString renderedText(const IntegrationStatusPanel& panel) {
    QStringList result;
    for (auto* label : panel.findChildren<QLabel*>()) result.append(label->text());
    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("clientsTable"));
    for (int row = 0; row < table->rowCount(); ++row)
        for (int column = 0; column < table->columnCount(); ++column)
            result.append(table->item(row, column)->text());
    return result.join(QLatin1Char('\n'));
}
}

class IntegrationStatusPanelTest final : public QObject {
    Q_OBJECT
private slots:
    void emptyReceiptDoesNotClaimClientPresence();
    void listenerFailureIsReadableAndDoesNotExposeRawError();
    void concurrentTurnsRefreshAndReset();
    void knownEventsAndLocalReceiptTimes();
    void hiddenAndClosedPanelStopPollingAndReopen();
};

void IntegrationStatusPanelTest::emptyReceiptDoesNotClaimClientPresence() {
    IntegrationStatusPanel panel(idleSnapshot, [] {});
    QCOMPARE(panel.isModal(), false);
    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("clientsTable"));
    QCOMPARE(table->rowCount(), 6);
    for (int row = 0; row < table->rowCount(); ++row) {
        QCOMPARE(table->item(row, 1)->text(), QStringLiteral("尚未收到"));
        QCOMPARE(table->item(row, 2)->text(), QStringLiteral("—"));
    }
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("listenerStatus"))->text(),
             QStringLiteral("监听中"));
    QVERIFY(!panel.findChild<QPushButton*>(QStringLiteral("resetBusyButton"))->isEnabled());
    const auto text = renderedText(panel);
    QVERIFY(text.contains(QStringLiteral("不代表客户端仍在线")));
    QVERIFY(!text.contains(QStringLiteral("已连接")));
    QVERIFY(!text.contains(QStringLiteral("客户端在线")));
    QVERIFY(!text.contains(QStringLiteral("任务成功")));
}

void IntegrationStatusPanelTest::listenerFailureIsReadableAndDoesNotExposeRawError() {
    auto snapshot = idleSnapshot();
    snapshot.insert(QStringLiteral("listening"), false);
    snapshot.insert(QStringLiteral("listener_error"), QStringLiteral("secret C:/private/pipe_name"));
    IntegrationStatusPanel panel([&] { return snapshot; }, [] {});
    panel.show();
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("listenerStatus"))->text(),
             QStringLiteral("未监听"));
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("listenerError"))->isVisible());
    QVERIFY(!renderedText(panel).contains(QStringLiteral("C:/private")));
    snapshot.insert(QStringLiteral("listening"), true);
    panel.refresh();
    QVERIFY(!panel.findChild<QLabel*>(QStringLiteral("listenerError"))->isVisible());
}

void IntegrationStatusPanelTest::concurrentTurnsRefreshAndReset() {
    auto snapshot = idleSnapshot();
    snapshot.insert(QStringLiteral("active_turns"), 2);
    snapshot.insert(QStringLiteral("pet_state"), QStringLiteral("busy"));
    int resetCount = 0;
    IntegrationStatusPanel panel([&] { return snapshot; }, [&] {
        ++resetCount;
        snapshot.insert(QStringLiteral("active_turns"), 0);
        snapshot.insert(QStringLiteral("pet_state"), QStringLiteral("idle"));
        snapshot.insert(QStringLiteral("last_event"), QJsonObject{
            {QStringLiteral("client"), QStringLiteral("manual")},
            {QStringLiteral("event"), QStringLiteral("reset")},
            {QStringLiteral("at"), QStringLiteral("2026-10-23T10:00:00.000Z")}});
    });
    auto* turns = panel.findChild<QLabel*>(QStringLiteral("activeTurns"));
    auto* state = panel.findChild<QLabel*>(QStringLiteral("petState"));
    auto* reset = panel.findChild<QPushButton*>(QStringLiteral("resetBusyButton"));
    QCOMPARE(turns->text(), QStringLiteral("2"));
    QCOMPARE(state->text(), QStringLiteral("忙碌"));
    QVERIFY(reset->isEnabled());
    snapshot.insert(QStringLiteral("active_turns"), 1);
    panel.refresh();
    QCOMPARE(turns->text(), QStringLiteral("1"));
    QCOMPARE(state->text(), QStringLiteral("忙碌"));
    QVERIFY(reset->isEnabled());
    reset->click();
    QCOMPARE(resetCount, 1);
    QCOMPARE(turns->text(), QStringLiteral("0"));
    QCOMPARE(state->text(), QStringLiteral("空闲"));
    QVERIFY(!reset->isEnabled());
    reset->click();
    QCOMPARE(resetCount, 1);
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("lastEvent"))->text()
                .contains(QStringLiteral("手动复位")));
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("resetExplanation"))->text()
                .contains(QStringLiteral("不会停止 agent")));
}

void IntegrationStatusPanelTest::knownEventsAndLocalReceiptTimes() {
    auto snapshot = idleSnapshot();
    const QString at = QStringLiteral("2026-10-23T10:20:30.400Z");
    snapshot.insert(QStringLiteral("clients"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("codex")},
                    {QStringLiteral("last_event"), QStringLiteral("start")},
                    {QStringLiteral("last_seen"), at}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("claude")},
                    {QStringLiteral("last_event"), QStringLiteral("stop")},
                    {QStringLiteral("last_seen"), at}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("opencode")},
                    {QStringLiteral("last_event"), QStringLiteral("start")},
                    {QStringLiteral("last_seen"), at}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("unknown")},
                    {QStringLiteral("last_event"), QStringLiteral("session_end")},
                    {QStringLiteral("last_seen"), at}}});
    snapshot.insert(QStringLiteral("last_event"), QJsonObject{
        {QStringLiteral("client"), QStringLiteral("unknown")},
        {QStringLiteral("event"), QStringLiteral("session_end")},
        {QStringLiteral("at"), at}});
    IntegrationStatusPanel panel([&] { return snapshot; }, [] {});
    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("clientsTable"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("收到开始"));
    QCOMPARE(table->item(2, 1)->text(), QStringLiteral("收到停止"));
    QCOMPARE(table->item(3, 0)->text(), QStringLiteral("OpenCode"));
    QCOMPARE(table->item(3, 1)->text(), QStringLiteral("收到开始"));
    QCOMPARE(table->item(4, 1)->text(), QStringLiteral("收到会话结束"));
    QCOMPARE(table->item(0, 2)->text(), QDateTime::fromString(at, Qt::ISODateWithMs)
                                          .toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss")));
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("lastEvent"))->text()
                .contains(QStringLiteral("未知来源 · 收到会话结束")));
    snapshot.insert(QStringLiteral("last_event"), QJsonObject{
        {QStringLiteral("client"), QStringLiteral("opencode")},
        {QStringLiteral("event"), QStringLiteral("stop")},
        {QStringLiteral("at"), at}});
    panel.refresh();
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("lastEvent"))->text()
                .contains(QStringLiteral("OpenCode · 收到停止")));
    QVERIFY(!renderedText(panel).contains(QStringLiteral("成功")));
}

void IntegrationStatusPanelTest::hiddenAndClosedPanelStopPollingAndReopen() {
    int providerCalls = 0;
    IntegrationStatusPanel panel([&] { ++providerCalls; return idleSnapshot(); }, [] {});
    auto* timer = panel.findChild<QTimer*>(QStringLiteral("statusRefreshTimer"));
    QVERIFY(!timer->isActive());
    const auto beforeShow = providerCalls;
    panel.show();
    QVERIFY(timer->isActive());
    QCOMPARE(timer->interval(), 1000);
    QVERIFY(providerCalls > beforeShow);
    const auto initialShownCalls = providerCalls;
    QTRY_VERIFY_WITH_TIMEOUT(providerCalls > initialShownCalls, 1600);
    panel.hide();
    QVERIFY(!timer->isActive());
    const auto hiddenCalls = providerCalls;
    QTest::qWait(1100);
    QCOMPARE(providerCalls, hiddenCalls);
    panel.show();
    QVERIFY(timer->isActive());
    QVERIFY(providerCalls > hiddenCalls);
    panel.findChild<QPushButton*>(QStringLiteral("closeStatusButton"))->click();
    QVERIFY(!panel.isVisible());
    QVERIFY(!timer->isActive());
    const auto closedCalls = providerCalls;
    QTest::qWait(1100);
    QCOMPARE(providerCalls, closedCalls);
    panel.show();
    QVERIFY(timer->isActive());
    QVERIFY(providerCalls > closedCalls);
}

QTEST_MAIN(IntegrationStatusPanelTest)
#include "IntegrationStatusPanelTest.moc"
