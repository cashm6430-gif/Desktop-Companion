#include "QuietPolicy.h"

#include <chrono>
#include <utility>

QuietPolicy::QuietPolicy(WallClock wallClock, MonotonicClock monotonicClock)
    : wallClock_(wallClock ? std::move(wallClock)
                           : WallClock([] { return QDateTime::currentDateTime(); })),
      monotonicClock_(monotonicClock ? std::move(monotonicClock) : MonotonicClock([] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
      })) {}

bool QuietPolicy::suppressAutomatic() const {
    return !reason().isEmpty();
}

QString QuietPolicy::reason() const {
    if (snoozeRemainingMs() > 0) return QStringLiteral("snoozed");
    if (nightEnabled_) {
        const auto time = wallClock_().toLocalTime().time();
        // The endpoint at 06:00 is open; 23:00 belongs to quiet hours.
        if (time.isValid() && (time.hour() >= 23 || time.hour() < 6))
            return QStringLiteral("night");
    }
    if (fullscreenPresent_) return QStringLiteral("fullscreen");
    return {};
}

void QuietPolicy::snoozeFor(qint64 ms) {
    if (ms <= 0) {
        resume();
        return;
    }
    snoozeStartedAt_ = monotonicClock_();
    snoozeDurationMs_ = ms;
}

void QuietPolicy::resume() {
    snoozeDurationMs_ = 0;
}

qint64 QuietPolicy::snoozeRemainingMs() const {
    if (snoozeDurationMs_ <= 0) return 0;
    const auto now = monotonicClock_();
    if (now <= snoozeStartedAt_) return snoozeDurationMs_;
    // Unsigned subtraction also covers the full signed clock range without
    // overflowing if a supplied clock starts below zero.
    const auto elapsed = quint64(now) - quint64(snoozeStartedAt_);
    if (elapsed >= quint64(snoozeDurationMs_)) return 0;
    return snoozeDurationMs_ - qint64(elapsed);
}
