#include "MemoStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStringConverter>

#include <utility>

namespace {
bool validText(const QString& text) {
    if (text.size() > MemoStore::MaxTextCharacters || text.trimmed().isEmpty()) return false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

MemoStore::LoadResult failed(MemoStore::Status status, const QString& error) {
    return {status, std::nullopt, error};
}

bool failWrite(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
} // namespace

MemoStore::MemoStore(QString filePath)
    : path_(filePath.isEmpty() ? QString{} : QFileInfo(filePath).absoluteFilePath()) {}

MemoStore::LoadResult MemoStore::load() const {
    if (path_.isEmpty())
        return failed(Status::IoError, QStringLiteral("便笺存储位置不可用。"));
    const QFileInfo info(path_);
    if (info.isSymLink() || (info.exists() && !info.isFile()))
        return failed(Status::IoError, QStringLiteral("便笺存储位置不是普通文件。"));
    if (!info.exists()) return {};
    if (info.size() > MaxFileBytes)
        return failed(Status::InvalidData, QStringLiteral("便笺文件超出大小限制，已保留原文件。"));

    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return failed(Status::IoError, QStringLiteral("无法读取便笺文件，已保留原文件。"));
    const QByteArray bytes = file.read(MaxFileBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return failed(Status::IoError, QStringLiteral("无法读取便笺文件，已保留原文件。"));
    if (bytes.size() > MaxFileBytes)
        return failed(Status::InvalidData, QStringLiteral("便笺文件超出大小限制，已保留原文件。"));
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(bytes);
    Q_UNUSED(decoded);
    if (decoder.hasError())
        return failed(Status::InvalidData, QStringLiteral("便笺文件编码损坏，已保留原文件。"));

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failed(Status::InvalidData, QStringLiteral("便笺文件格式损坏，已保留原文件。"));
    const auto object = document.object();
    const auto version = object.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() != 1.0) {
        if (version.isDouble() && version.toDouble() >= 0.0
            && version.toDouble() == static_cast<double>(version.toInteger(-1)))
            return failed(Status::UnsupportedVersion,
                          QStringLiteral("便笺文件版本不受支持，已保留原文件。"));
        return failed(Status::InvalidData, QStringLiteral("便笺文件格式损坏，已保留原文件。"));
    }
    if (object.size() != 2 || !object.contains(QStringLiteral("note")))
        return failed(Status::InvalidData, QStringLiteral("便笺文件格式损坏，已保留原文件。"));
    const auto value = object.value(QStringLiteral("note"));
    if (value.isNull()) return {Status::Ready, std::nullopt, {}};
    if (!value.isObject())
        return failed(Status::InvalidData, QStringLiteral("便笺文件格式损坏，已保留原文件。"));
    const auto note = value.toObject();
    if (note.size() != 2 || !note.value(QStringLiteral("text")).isString()
        || !note.value(QStringLiteral("collapsed")).isBool()
        || !validText(note.value(QStringLiteral("text")).toString()))
        return failed(Status::InvalidData, QStringLiteral("便笺文件内容无效，已保留原文件。"));
    return {Status::Ready, Note{note.value(QStringLiteral("text")).toString(),
                               note.value(QStringLiteral("collapsed")).toBool()}, {}};
}

bool MemoStore::save(const Note& note, QString* error) const {
    if (error) error->clear();
    if (!validText(note.text))
        return failWrite(error, QStringLiteral("便笺需包含文字，且不能超过 2000 个字符。"));
    return write(note, error);
}

bool MemoStore::clear(QString* error) const {
    if (error) error->clear();
    return write(std::nullopt, error);
}

bool MemoStore::write(const std::optional<Note>& note, QString* error) const {
    if (path_.isEmpty())
        return failWrite(error, QStringLiteral("便笺存储位置不可用。"));
    const auto beforeLock = load();
    if (!beforeLock.ok()) return failWrite(error, beforeLock.error);
    if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
        return failWrite(error, QStringLiteral("无法创建便笺存储目录。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock())
        return failWrite(error, QStringLiteral("便笺文件正被使用，请稍后重试。"));
    // Check again after taking the writer lock: another instance may have
    // replaced the document between our first read and acquiring the lock.
    const auto existing = load();
    if (!existing.ok()) return failWrite(error, existing.error);

    QJsonObject object{{QStringLiteral("version"), 1}};
    if (note) {
        object.insert(QStringLiteral("note"),
                      QJsonObject{{QStringLiteral("text"), note->text},
                                  {QStringLiteral("collapsed"), note->collapsed}});
    } else {
        object.insert(QStringLiteral("note"), QJsonValue(QJsonValue::Null));
    }
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(path_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
        || !file.commit())
        return failWrite(error, QStringLiteral("无法保存便笺，已保留原文件。"));
    return true;
}
