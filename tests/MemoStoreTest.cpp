#include "../src/MemoStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest/QTest>

namespace {
bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
} // namespace

class MemoStoreTest final : public QObject {
    Q_OBJECT
private slots:
    void missingDoesNotCreateAFile();
    void roundTripsUserTextAndCollapsedState();
    void clearPersistsAnEmptyVersionedRecord();
    void rejectsMalformedAndUnknownDocuments_data();
    void rejectsMalformedAndUnknownDocuments();
    void boundariesAndInvalidInput();
    void exactFileSizeLimit();
    void lockedWriteFailsWithoutChangingTheRecord();
    void reportsInvalidLocations();
};

void MemoStoreTest::missingDoesNotCreateAFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nested/memo.json"));
    const auto result = MemoStore(path).load();
    QCOMPARE(result.status, MemoStore::Status::Missing);
    QVERIFY(result.ok());
    QVERIFY(!result.note);
    QVERIFY(!QFile::exists(path));
    QVERIFY(!QDir(dir.filePath(QStringLiteral("nested"))).exists());
}

void MemoStoreTest::roundTripsUserTextAndCollapsedState() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nested/memo.json"));
    MemoStore store(path);
    const QString text = QString::fromUtf8("  记得喝水 🐳\n第二行\t\"保留排版\"  ");
    QString error = QStringLiteral("old error");
    QVERIFY2(store.save({text, false}, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    auto result = MemoStore(path).load();
    QCOMPARE(result.status, MemoStore::Status::Ready);
    QVERIFY(result.note);
    QCOMPARE(result.note->text, text);
    QVERIFY(!result.note->collapsed);
    const auto bytes = readBytes(path);
    QVERIFY(!bytes.startsWith("\xef\xbb\xbf"));
    QVERIFY(bytes.contains(QString::fromUtf8("记得喝水").toUtf8()));
    const auto object = QJsonDocument::fromJson(bytes).object();
    QCOMPARE(object.keys(), QStringList({QStringLiteral("note"), QStringLiteral("version")}));
    QCOMPARE(object.value(QStringLiteral("note")).toObject().keys(),
             QStringList({QStringLiteral("collapsed"), QStringLiteral("text")}));

    QVERIFY(store.save({text, true}));
    result = MemoStore(path).load();
    QVERIFY(result.note);
    QVERIFY(result.note->collapsed);
    QCOMPARE(result.note->text, text);
    QVERIFY(!QFile::exists(path + QStringLiteral(".lock")));
}

void MemoStoreTest::clearPersistsAnEmptyVersionedRecord() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    QVERIFY(store.save({QStringLiteral("a small reminder"), true}));
    QString error;
    QVERIFY2(store.clear(&error), qPrintable(error));
    const auto result = store.load();
    QCOMPARE(result.status, MemoStore::Status::Ready);
    QVERIFY(!result.note);
    QCOMPARE(QJsonDocument::fromJson(readBytes(path)).object(),
             (QJsonObject{{QStringLiteral("version"), 1},
                          {QStringLiteral("note"), QJsonValue(QJsonValue::Null)}}));
    QVERIFY(store.clear());
    QVERIFY(store.save({QStringLiteral("new note"), false}));
    QVERIFY(store.load().note);
}

void MemoStoreTest::rejectsMalformedAndUnknownDocuments_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<MemoStore::Status>("status");
    const auto invalid = MemoStore::Status::InvalidData;
    QTest::newRow("empty") << QByteArray{} << invalid;
    QTest::newRow("partial") << QByteArray("{\"version\":1,") << invalid;
    QTest::newRow("array") << QByteArray("[]") << invalid;
    QTest::newRow("unknown-version") << QByteArray("{\"version\":2,\"note\":null}")
        << MemoStore::Status::UnsupportedVersion;
    QTest::newRow("string-version") << QByteArray("{\"version\":\"1\",\"note\":null}") << invalid;
    QTest::newRow("fraction-version") << QByteArray("{\"version\":1.5,\"note\":null}") << invalid;
    QTest::newRow("missing-note") << QByteArray("{\"version\":1}") << invalid;
    QTest::newRow("extra-root") << QByteArray("{\"version\":1,\"note\":null,\"prompt\":\"x\"}") << invalid;
    QTest::newRow("extra-note") << QByteArray("{\"version\":1,\"note\":{\"text\":\"x\",\"collapsed\":false,\"time\":1}}") << invalid;
    QTest::newRow("missing-collapsed") << QByteArray("{\"version\":1,\"note\":{\"text\":\"x\"}}") << invalid;
    QTest::newRow("non-bool-collapsed") << QByteArray("{\"version\":1,\"note\":{\"text\":\"x\",\"collapsed\":0}}") << invalid;
    QTest::newRow("blank-text") << QByteArray("{\"version\":1,\"note\":{\"text\":\" \\n\",\"collapsed\":true}}") << invalid;
    QTest::newRow("wrong-text") << QByteArray("{\"version\":1,\"note\":{\"text\":7,\"collapsed\":false}}") << invalid;
    QTest::newRow("unpaired-surrogate") << QByteArray("{\"version\":1,\"note\":{\"text\":\"\\ud800\",\"collapsed\":false}}") << invalid;
    QTest::newRow("bad-utf8") << QByteArray("{\"version\":1,\"note\":{\"text\":\"")
        .append(char(0xc3)).append("\",\"collapsed\":false}}") << invalid;
    QTest::newRow("oversized") << QByteArray(MemoStore::MaxFileBytes + 1, ' ') << invalid;
}

void MemoStoreTest::rejectsMalformedAndUnknownDocuments() {
    QFETCH(QByteArray, bytes);
    QFETCH(MemoStore::Status, status);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("memo.json"));
    QVERIFY(writeBytes(path, bytes));
    MemoStore store(path);
    const auto result = store.load();
    QCOMPARE(result.status, status);
    QVERIFY(!result.ok());
    QVERIFY(!result.note);
    QVERIFY(!result.error.isEmpty());
    QString error;
    QVERIFY(!store.save({QStringLiteral("replacement"), false}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(path), bytes);
    QVERIFY(!store.clear(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(path), bytes);
}

void MemoStoreTest::boundariesAndInvalidInput() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    QString text(MemoStore::MaxTextCharacters, QChar(0x9cb8));
    QVERIFY(store.save({text, true}));
    QVERIFY(store.load().note);
    QCOMPARE(store.load().note->text, text);
    const auto original = readBytes(path);
    text.append(QChar(0x9cb8));
    QString error;
    QVERIFY(!store.save({text, false}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(path), original);
    QVERIFY(!store.save({QStringLiteral(" \n\t"), false}));
    QVERIFY(!store.save({QString(QChar(0xd800)), false}));
    QVERIFY(!store.save({QString(QChar(0xdc00)), false}));
    QCOMPARE(readBytes(path), original);

    const QJsonObject oversized{{QStringLiteral("version"), 1},
        {QStringLiteral("note"), QJsonObject{{QStringLiteral("text"), text},
                                            {QStringLiteral("collapsed"), false}}}};
    QVERIFY(writeBytes(path, QJsonDocument(oversized).toJson(QJsonDocument::Compact)));
    QCOMPARE(store.load().status, MemoStore::Status::InvalidData);
}

void MemoStoreTest::exactFileSizeLimit() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("memo.json"));
    QByteArray bytes("{\"version\":1,\"note\":null}");
    bytes.append(QByteArray(MemoStore::MaxFileBytes - bytes.size(), ' '));
    QVERIFY(writeBytes(path, bytes));
    MemoStore store(path);
    QCOMPARE(store.load().status, MemoStore::Status::Ready);
    QVERIFY(!store.load().note);
    bytes.append(' ');
    QVERIFY(writeBytes(path, bytes));
    QCOMPARE(store.load().status, MemoStore::Status::InvalidData);
    QVERIFY(!store.clear());
    QCOMPARE(readBytes(path), bytes);
}

void MemoStoreTest::lockedWriteFailsWithoutChangingTheRecord() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("memo.json"));
    MemoStore store(path);
    QVERIFY(store.save({QStringLiteral("keep this"), false}));
    const auto original = readBytes(path);
    QLockFile lock(path + QStringLiteral(".lock"));
    QVERIFY(lock.tryLock());
    QString error;
    QVERIFY(!store.save({QStringLiteral("replacement"), true}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(path), original);
    QVERIFY(!store.clear(&error));
    QCOMPARE(readBytes(path), original);
    lock.unlock();
    QVERIFY(store.clear(&error));
    QVERIFY(error.isEmpty());
}

void MemoStoreTest::reportsInvalidLocations() {
    MemoStore noPath(QString{});
    QCOMPARE(noPath.load().status, MemoStore::Status::IoError);
    QString error;
    QVERIFY(!noPath.save({QStringLiteral("x"), false}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!noPath.clear());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MemoStore directory(dir.path());
    QCOMPARE(directory.load().status, MemoStore::Status::IoError);
    QVERIFY(!directory.save({QStringLiteral("x"), false}));
    QVERIFY(!directory.clear());
    const QString parentFile = dir.filePath(QStringLiteral("not-a-directory"));
    QVERIFY(writeBytes(parentFile, "keep"));
    MemoStore child(parentFile + QStringLiteral("/memo.json"));
    QVERIFY(!child.save({QStringLiteral("x"), false}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(parentFile), QByteArray("keep"));
}

QTEST_GUILESS_MAIN(MemoStoreTest)
#include "MemoStoreTest.moc"
