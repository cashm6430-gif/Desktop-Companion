#pragma once

#include <QDateTime>
#include <QString>
#include <functional>

// One gate for unsolicited reactions. It does not change the controller's
// real activity state and does not gate actions explicitly requested by users.
class QuietPolicy final {
public:
    using WallClock = std::function<QDateTime()>;
    using MonotonicClock = std::function<qint64()>;

    explicit QuietPolicy(WallClock wallClock = {}, MonotonicClock monotonicClock = {});

    bool suppressAutomatic() const;
    // Stable identifiers, in precedence order: snoozed, night, fullscreen, empty.
    QString reason() const;

    void snoozeFor(qint64 ms);
    // Only ends the user's timed pause. Night/fullscreen remain independent.
    void resume();
    qint64 snoozeRemainingMs() const;

    void setNightEnabled(bool enabled) { nightEnabled_ = enabled; }
    bool nightEnabled() const { return nightEnabled_; }
    void setFullscreenPresent(bool present) { fullscreenPresent_ = present; }

private:
    WallClock wallClock_;
    MonotonicClock monotonicClock_;
    qint64 snoozeStartedAt_ = 0;
    qint64 snoozeDurationMs_ = 0;
    bool nightEnabled_ = true;
    bool fullscreenPresent_ = false;
};
