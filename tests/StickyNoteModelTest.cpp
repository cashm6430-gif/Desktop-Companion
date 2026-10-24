#include "../src/StickyNoteModel.h"
#include "../src/MemoStore.h"

#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

namespace {
QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
} // namespace

class StickyNoteModelTest final : public QObject {
    Q_OBJECT
private slots:
    void newAndRestoredNotesHaveDifferentPresentation();
    void editsAndDismissalsPersistBeforeChangingState();
    void automaticReceiptsNeverWriteTheStore();
    void automaticReceiptsMergeOnlyAcceptedNearbyTurns();
    void foldingAutomaticPaperKeepsAcceptedAccounting();
    void userPriorityAndDisableDiscardAutomaticHistory();
    void interruptedRiseFoldsWithoutReplay();
    void risenNoteAndCompletionUseTheManualClock();
    void failedPersistencePreservesUserContent();
    void failedFoldStillPreventsAnInvisibleBubble();
    void invalidStoreIsPreservedAndReported();
    void validatesMemoryOnlyNotesAndClockLabels();
    void durationAggregationCannotOverflow();
};

void StickyNoteModelTest::newAndRestoredNotesHaveDifferentPresentation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    StickyNoteModel model(&store);
    QVERIFY(model.restore());
    QCOMPARE(model.origin(), StickyNoteModel::Origin::None);
    QVERIFY(!QFile::exists(path));
    QVERIFY(model.createUser(QStringLiteral("测试便笺"), true));
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Bubble);
    QVERIFY(!model.riseSeen());
    QVERIFY(model.hasUserNote());
    QCOMPARE(store.load().note->collapsed, false);

    StickyNoteModel restored(&store);
    QVERIFY(restored.restore());
    QCOMPARE(restored.text(), model.text());
    QCOMPARE(restored.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(!restored.riseSeen());
    restored.advance(30.0, false, 0.0);
    QCOMPARE(restored.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(restored.reopen(false));
    QCOMPARE(restored.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(restored.reopen(true));
    QCOMPARE(restored.visual(), StickyNoteModel::Visual::Bubble);
}

void StickyNoteModelTest::editsAndDismissalsPersistBeforeChangingState() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoStore store(directory.filePath(QStringLiteral("memo.json")));
    StickyNoteModel model(&store);
    QVERIFY(model.createUser(QStringLiteral("第一条"), false));
    QVERIFY(store.load().note->collapsed);
    QVERIFY(model.reopen(true));
    QVERIFY(!store.load().note->collapsed);
    QVERIFY(model.createUser(QStringLiteral("编辑后的内容"), true));
    QCOMPARE(store.load().note->text, QStringLiteral("编辑后的内容"));
    QVERIFY(model.fold());
    QVERIFY(store.load().note->collapsed);
    QVERIFY(model.complete(false));
    QVERIFY(!store.load().note);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Done);
    QVERIFY(!model.hasUserNote());
    QVERIFY(model.text().isEmpty());
    QVERIFY(model.createUser(QStringLiteral("待丢弃"), false));
    QVERIFY(model.discard());
    QVERIFY(!store.load().note);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
}

void StickyNoteModelTest::automaticReceiptsNeverWriteTheStore() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    StickyNoteModel model(&store);
    QVERIFY(!model.recordDuration(29999, 0, QStringLiteral("10:10")));
    QVERIFY(!model.recordDuration(30000, -1, QStringLiteral("10:10")));
    QVERIFY(model.recordDuration(30000, 1, QStringLiteral("10:10")));
    QCOMPARE(model.origin(), StickyNoteModel::Origin::WorkDuration);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QCOMPARE(model.workCount(), 1);
    QCOMPARE(model.workedMs(), qint64(30000));
    QVERIFY(model.text().contains(QStringLiteral("本回合耗时")));
    QVERIFY(!model.text().contains(QChar(0x2713)));
    QVERIFY(!model.complete(true));
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(model.reopen(true));
    QVERIFY(model.fold());
    QVERIFY(model.discard());
    QVERIFY(!QFile::exists(path));
    QVERIFY(!QFile::exists(path + QStringLiteral(".lock")));
}

void StickyNoteModelTest::automaticReceiptsMergeOnlyAcceptedNearbyTurns() {
    StickyNoteModel model;
    QVERIFY(model.recordDuration(30000, 1000, QStringLiteral("10:10")));
    QVERIFY(!model.recordDuration(900000, 2000, QStringLiteral("10:10"), false));
    QVERIFY(!model.recordDuration(1000, 3000, QStringLiteral("10:10")));
    QVERIFY(model.recordDuration(45000, 61000, QStringLiteral("10:11")));
    QCOMPARE(model.workCount(), 2);
    QCOMPARE(model.workedMs(), qint64(75000));
    QVERIFY(model.text().contains(QStringLiteral("回合耗时合计")));
    QVERIFY(model.text().startsWith(QStringLiteral("10:11")));
    // The window is based on the last accepted receipt, inclusive at 60s.
    QVERIFY(model.recordDuration(60000, 121001, QStringLiteral("10:12")));
    QCOMPARE(model.workCount(), 1);
    QCOMPARE(model.workedMs(), qint64(60000));
    // A backwards clock starts a new batch instead of claiming a merge.
    QVERIFY(model.recordDuration(40000, 100000, QStringLiteral("10:13")));
    QCOMPARE(model.workCount(), 1);
    QCOMPARE(model.workedMs(), qint64(40000));
    QVERIFY(model.recordDuration(35000, 100000, QStringLiteral("10:13")));
    QCOMPARE(model.workCount(), 2);
}

void StickyNoteModelTest::foldingAutomaticPaperKeepsAcceptedAccounting() {
    StickyNoteModel model;
    QVERIFY(model.recordDuration(30000, 1000, QStringLiteral("10:10")));
    QVERIFY(model.reopen(true));
    model.advance(1.1, true, 1.0);
    model.advance(20.0, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QCOMPARE(model.workCount(), 1);
    QVERIFY(model.recordDuration(45000, 30000, QStringLiteral("10:11")));
    QCOMPARE(model.workCount(), 2);
    QCOMPARE(model.workedMs(), qint64(75000));
    QVERIFY(model.interrupt());
    QCOMPARE(model.workCount(), 2);
    QVERIFY(model.discard());
    QVERIFY(model.recordDuration(35000, 30001, QStringLiteral("10:12")));
    QCOMPARE(model.workCount(), 1);
    QCOMPARE(model.workedMs(), qint64(35000));
}

void StickyNoteModelTest::userPriorityAndDisableDiscardAutomaticHistory() {
    StickyNoteModel model;
    QVERIFY(model.recordDuration(30000, 1, QStringLiteral("10:10")));
    model.setAutomaticEnabled(false);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
    QCOMPARE(model.workCount(), 0);
    QVERIFY(!model.recordDuration(90000, 2, QStringLiteral("10:11")));
    model.setAutomaticEnabled(true);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
    QVERIFY(model.recordDuration(30000, 3, QStringLiteral("10:12")));
    QCOMPARE(model.workCount(), 1);
    QVERIFY(model.createUser(QStringLiteral("用户优先"), true));
    QCOMPARE(model.workCount(), 0);
    model.setAutomaticEnabled(false);
    QVERIFY(model.hasUserNote());
    model.setAutomaticEnabled(true);
    QVERIFY(!model.recordDuration(80000, 4, QStringLiteral("10:13")));
    QCOMPARE(model.text(), QStringLiteral("用户优先"));
    QVERIFY(model.complete(true));
    QVERIFY(!model.recordDuration(50000, 5, QStringLiteral("10:14")));
    model.advance(0.8, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
    // Rejected events were not buffered behind the user's completion.
    QVERIFY(model.recordDuration(35000, 6, QStringLiteral("10:15")));
    QCOMPARE(model.workCount(), 1);
    QCOMPARE(model.workedMs(), qint64(35000));
    QVERIFY(model.discard());
    QVERIFY(model.recordDuration(45000, 7, QStringLiteral("10:16")));
    QCOMPARE(model.workCount(), 1);
}

void StickyNoteModelTest::interruptedRiseFoldsWithoutReplay() {
    StickyNoteModel model;
    QVERIFY(model.createUser(QStringLiteral("途中被打断"), true));
    model.advance(0.3, true, 0.0);
    model.advance(0.2, true, 0.5);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Bubble);
    model.advance(0.1, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(!model.riseSeen());
    model.advance(30.0, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QCOMPARE(model.text(), QStringLiteral("途中被打断"));
    QVERIFY(model.reopen(true));
    model.advance(0.1, true, 0.05);
    QVERIFY(model.interrupt());
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
}

void StickyNoteModelTest::risenNoteAndCompletionUseTheManualClock() {
    StickyNoteModel model;
    QVERIFY(model.createUser(QStringLiteral("完成的测试便笺"), true));
    model.advance(1.1, true, 1.0);
    QVERIFY(model.riseSeen());
    model.advance(0.8, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Bubble);
    model.advance(18.0, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Bubble);
    model.advance(0.2, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(model.complete(true));
    model.advance(0.79, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Done);
    model.advance(0.011, false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
}

void StickyNoteModelTest::failedPersistencePreservesUserContent() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    StickyNoteModel model(&store);
    QVERIFY(model.createUser(QStringLiteral("已保存的原文"), false));
    const auto original = readBytes(path);
    QLockFile lock(path + QStringLiteral(".lock"));
    QVERIFY(lock.tryLock());
    QString error;
    QVERIFY(!model.createUser(QStringLiteral("不能写入的改动"), true, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(error, model.error());
    QCOMPARE(model.text(), QStringLiteral("已保存的原文"));
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(!model.reopen(true, &error));
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QVERIFY(!model.complete(true, &error));
    QVERIFY(model.hasUserNote());
    QVERIFY(!model.discard(&error));
    QVERIFY(model.hasUserNote());
    QCOMPARE(readBytes(path), original);
    lock.unlock();
    QVERIFY(model.discard(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(model.error().isEmpty());
}

void StickyNoteModelTest::failedFoldStillPreventsAnInvisibleBubble() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    StickyNoteModel model(&store);
    QVERIFY(model.createUser(QStringLiteral("打断也不丢失"), true));
    const auto original = readBytes(path);
    QLockFile lock(path + QStringLiteral(".lock"));
    QVERIFY(lock.tryLock());
    QString error;
    QVERIFY(!model.interrupt(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Badge);
    QCOMPARE(model.text(), QStringLiteral("打断也不丢失"));
    QCOMPARE(readBytes(path), original);
    StickyNoteModel restored(&store);
    QVERIFY(restored.restore());
    QCOMPARE(restored.visual(), StickyNoteModel::Visual::Badge);
    QCOMPARE(restored.text(), model.text());
}

void StickyNoteModelTest::invalidStoreIsPreservedAndReported() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("memo.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("not-json"), qint64(8));
    file.close();
    MemoStore store(path);
    StickyNoteModel model(&store);
    QString error;
    QVERIFY(!model.restore(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
    QVERIFY(!model.createUser(QStringLiteral("不覆盖原文件"), true, &error));
    QCOMPARE(readBytes(path), QByteArray("not-json"));
    QCOMPARE(model.visual(), StickyNoteModel::Visual::None);
}

void StickyNoteModelTest::validatesMemoryOnlyNotesAndClockLabels() {
    StickyNoteModel model;
    QVERIFY(!model.createUser(QStringLiteral(" \n "), true));
    QVERIFY(!model.createUser(QString(MemoStore::MaxTextCharacters + 1, QChar('x')), true));
    QVERIFY(!model.createUser(QString(1, QChar(0xD800)), true));
    const QString boundary(MemoStore::MaxTextCharacters, QChar(0x9cb8));
    QVERIFY(model.createUser(boundary, false));
    QVERIFY(model.discard());
    QVERIFY(model.recordDuration(30000, 0, QStringLiteral("arbitrary caller metadata")));
    QVERIFY(!model.text().contains(QStringLiteral("arbitrary")));
    QVERIFY(model.recordDuration(30000, 1, QStringLiteral("1:02")));
    QVERIFY(!model.text().contains(QStringLiteral("1:02")));
    QVERIFY(model.recordDuration(30000, 2, QStringLiteral("01:02")));
    QVERIFY(model.text().startsWith(QStringLiteral("01:02")));
    QVERIFY(model.createUser(QStringLiteral("时钟忽略无效输入"), true));
    model.advance(std::numeric_limits<double>::quiet_NaN(), false, 0.0);
    model.advance(-1.0, false, 0.0);
    model.advance(std::numeric_limits<double>::infinity(), false, 0.0);
    QCOMPARE(model.visual(), StickyNoteModel::Visual::Bubble);
    QVERIFY(!model.riseSeen());
}

void StickyNoteModelTest::durationAggregationCannotOverflow() {
    StickyNoteModel model;
    constexpr qint64 maximum = std::numeric_limits<qint64>::max();
    QVERIFY(model.recordDuration(maximum - 1, maximum - 1, QStringLiteral("10:10")));
    QVERIFY(model.recordDuration(30000, maximum, QStringLiteral("10:10")));
    QCOMPARE(model.workedMs(), maximum);
    QCOMPARE(model.workCount(), 2);
}

QTEST_GUILESS_MAIN(StickyNoteModelTest)
#include "StickyNoteModelTest.moc"
