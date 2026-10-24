#include "StickyNoteModel.h"

#include "MemoStore.h"

#include <QTime>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
bool validUserText(const QString& text) {
    if (text.trimmed().isEmpty() || text.size() > MemoStore::MaxTextCharacters) return false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

qint64 saturatingAdd(qint64 first, qint64 second) {
    return first > std::numeric_limits<qint64>::max() - second
        ? std::numeric_limits<qint64>::max() : first + second;
}
} // namespace

StickyNoteModel::StickyNoteModel(MemoStore* store) : store_(store) {}

void StickyNoteModel::beginOperation(QString* error) {
    error_.clear();
    if (error) error->clear();
}

bool StickyNoteModel::fail(const QString& message, QString* error) {
    error_ = message;
    if (error) *error = message;
    return false;
}

bool StickyNoteModel::saveUser(const QString& text, bool collapsed, QString* error) {
    QString message;
    if (store_ && !store_->save({text, collapsed}, &message)) return fail(message, error);
    return true;
}

bool StickyNoteModel::clearUser(QString* error) {
    QString message;
    if (store_ && !store_->clear(&message)) return fail(message, error);
    return true;
}

void StickyNoteModel::clearAutomaticAggregate() {
    workCount_ = 0;
    workedMs_ = 0;
    lastReceiptMs_ = -1;
}

void StickyNoteModel::clearVisual() {
    text_.clear();
    origin_ = Origin::None;
    visual_ = Visual::None;
    riseSeen_ = false;
    visualAge_ = 0.0;
    clearAutomaticAggregate();
}

bool StickyNoteModel::restore(QString* error) {
    beginOperation(error);
    if (!store_) return true;
    const auto loaded = store_->load();
    if (!loaded.ok()) return fail(loaded.error, error);
    clearVisual();
    if (loaded.note) {
        text_ = loaded.note->text;
        origin_ = Origin::User;
        visual_ = Visual::Badge;
    }
    return true;
}

bool StickyNoteModel::createUser(const QString& text, bool canAnimate, QString* error) {
    beginOperation(error);
    if (!validUserText(text))
        return fail(QStringLiteral("便笺需包含文字，且不能超过 2000 个字符。"), error);
    if (!saveUser(text, !canAnimate, error)) return false;
    clearAutomaticAggregate();
    text_ = text;
    origin_ = Origin::User;
    visual_ = canAnimate ? Visual::Bubble : Visual::Badge;
    riseSeen_ = false;
    visualAge_ = 0.0;
    return true;
}

bool StickyNoteModel::fold(QString* error) {
    beginOperation(error);
    if (visual_ != Visual::Bubble && visual_ != Visual::Badge) return true;
    // Folding must always prevent an interrupted rise from becoming an
    // invisible Bubble. A failed disk write leaves the previous file intact;
    // restore() folds that previous note too.
    const bool saved = origin_ != Origin::User || saveUser(text_, true, error);
    visual_ = Visual::Badge;
    riseSeen_ = false;
    visualAge_ = 0.0;
    return saved;
}

bool StickyNoteModel::reopen(bool canAnimate, QString* error) {
    beginOperation(error);
    if (visual_ != Visual::Badge || !canAnimate) return true;
    if (origin_ == Origin::User && !saveUser(text_, false, error)) return false;
    visual_ = Visual::Bubble;
    riseSeen_ = false;
    visualAge_ = 0.0;
    return true;
}

bool StickyNoteModel::complete(bool canCelebrate, QString* error) {
    beginOperation(error);
    // The window decides whether the existing face reaction can run. The
    // paper completion fade is safe even while another action owns the face.
    Q_UNUSED(canCelebrate);
    if (origin_ != Origin::User) return false;
    if (!clearUser(error)) return false;
    clearVisual();
    visual_ = Visual::Done;
    return true;
}

bool StickyNoteModel::discard(QString* error) {
    beginOperation(error);
    if (origin_ == Origin::User && !clearUser(error)) return false;
    clearVisual();
    return true;
}

bool StickyNoteModel::interrupt(QString* error) {
    return fold(error);
}

void StickyNoteModel::setAutomaticEnabled(bool enabled) {
    automaticEnabled_ = enabled;
    if (!enabled) {
        if (origin_ == Origin::WorkDuration) clearVisual();
        else clearAutomaticAggregate();
    }
}

bool StickyNoteModel::recordDuration(qint64 workedMs, qint64 receiptMonotonicMs,
                                     const QString& localHHmm, bool allowed) {
    if (workedMs < 30000 || receiptMonotonicMs < 0 || !automaticEnabled_ || !allowed
        || origin_ == Origin::User || visual_ == Visual::Done) return false;

    const bool merge = origin_ == Origin::WorkDuration && workCount_ > 0
        && lastReceiptMs_ >= 0 && receiptMonotonicMs >= lastReceiptMs_
        && receiptMonotonicMs - lastReceiptMs_ <= 60000;
    if (merge) {
        if (workCount_ < std::numeric_limits<int>::max()) ++workCount_;
        workedMs_ = saturatingAdd(workedMs_, workedMs);
    } else {
        workCount_ = 1;
        workedMs_ = workedMs;
    }
    lastReceiptMs_ = receiptMonotonicMs;
    const qint64 seconds = workedMs_ / 1000;
    const QString duration = QStringLiteral("%1 分 %2 秒")
        .arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
    const QString receipt = workCount_ > 1
        ? QStringLiteral("%1 回合 · 回合耗时合计 %2").arg(workCount_).arg(duration)
        : QStringLiteral("本回合耗时 %1").arg(duration);
    // Only retain the expected clock label, never arbitrary caller metadata.
    const QTime time = QTime::fromString(localHHmm, QStringLiteral("HH:mm"));
    text_ = time.isValid() && time.toString(QStringLiteral("HH:mm")) == localHHmm
        ? localHHmm + QStringLiteral(" · ") + receipt : receipt;
    origin_ = Origin::WorkDuration;
    visual_ = Visual::Badge;
    riseSeen_ = false;
    visualAge_ = 0.0;
    return true;
}

void StickyNoteModel::advance(double seconds, bool memoActive, double rise) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    visualAge_ = std::min(21.0, visualAge_ + seconds);
    if (visual_ == Visual::Done) {
        if (visualAge_ >= 0.8) clearVisual();
        return;
    }
    if (visual_ != Visual::Bubble) return;
    if (std::isfinite(rise) && rise >= 0.99) riseSeen_ = true;
    if ((!memoActive && !riseSeen_) || (!memoActive && visualAge_ >= 20.0)) fold();
}
