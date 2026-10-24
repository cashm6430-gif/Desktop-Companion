#pragma once

#include <QString>
#include <optional>

// A single user-authored note. The caller supplies the location so review and
// test processes never need to touch the live profile. No prompts or event
// history belong in this file.
class MemoStore final {
public:
    static constexpr int MaxTextCharacters = 2000;
    static constexpr qint64 MaxFileBytes = 32768;

    struct Note {
        QString text;
        bool collapsed = false;
    };

    enum class Status { Missing, Ready, InvalidData, UnsupportedVersion, IoError };
    struct LoadResult {
        Status status = Status::Missing;
        std::optional<Note> note;
        QString error;
        bool ok() const { return status == Status::Missing || status == Status::Ready; }
    };

    explicit MemoStore(QString filePath);
    LoadResult load() const;
    bool save(const Note& note, QString* error = nullptr) const;
    // Retain a versioned empty document rather than deleting/recreating the
    // file. Like save(), this refuses to overwrite unreadable/unknown content.
    bool clear(QString* error = nullptr) const;

private:
    bool write(const std::optional<Note>& note, QString* error) const;
    QString path_;
};
