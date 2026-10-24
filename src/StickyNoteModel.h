#pragma once

#include <QString>

class MemoStore;

// One user note or an ephemeral duration receipt. No model parameters or UI
// events live here, and automatic receipts never enter the persistent store.
class StickyNoteModel final {
public:
    enum class Origin { None, User, WorkDuration };
    enum class Visual { None, Bubble, Badge, Done };

    explicit StickyNoteModel(MemoStore* store = nullptr);

    const QString& text() const { return text_; }
    Origin origin() const { return origin_; }
    Visual visual() const { return visual_; }
    bool riseSeen() const { return riseSeen_; }
    bool hasUserNote() const { return origin_ == Origin::User; }
    bool automaticEnabled() const { return automaticEnabled_; }
    double visualAge() const { return visualAge_; }
    const QString& error() const { return error_; }
    int workCount() const { return workCount_; }
    qint64 workedMs() const { return workedMs_; }

    // Restored notes always start folded, independent of their saved state.
    bool restore(QString* error = nullptr);
    bool createUser(const QString& text, bool canAnimate, QString* error = nullptr);
    bool fold(QString* error = nullptr);
    bool reopen(bool canAnimate, QString* error = nullptr);
    bool complete(bool canCelebrate, QString* error = nullptr);
    bool discard(QString* error = nullptr);
    bool interrupt(QString* error = nullptr);
    void setAutomaticEnabled(bool enabled);
    bool recordDuration(qint64 workedMs, qint64 receiptMonotonicMs,
                        const QString& localHHmm, bool allowed = true);
    void advance(double seconds, bool memoActive, double rise);

private:
    bool saveUser(const QString& text, bool collapsed, QString* error);
    bool clearUser(QString* error);
    bool fail(const QString& message, QString* error);
    void beginOperation(QString* error);
    void clearAutomaticAggregate();
    void clearVisual();

    MemoStore* store_ = nullptr; // caller owns it
    QString text_;
    QString error_;
    Origin origin_ = Origin::None;
    Visual visual_ = Visual::None;
    bool riseSeen_ = false;
    bool automaticEnabled_ = true;
    double visualAge_ = 0.0;
    int workCount_ = 0;
    qint64 workedMs_ = 0;
    qint64 lastReceiptMs_ = -1;
};
