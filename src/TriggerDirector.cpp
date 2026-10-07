#include "TriggerDirector.h"

#include <algorithm>

namespace {
// Discipline constants (seconds, Normal pace). The chance values are per
// one-second tick once a scene is due: 0.02 reads as "usually within a
// minute or two after the due time", never on a fixed schedule.
constexpr double kGlobalInterval = 300.0;   // between any two spontaneous scenes
constexpr double kRefusalBackoff = 90.0;    // after a gate refused a nomination
constexpr double kStartupDelay = 0.8;       // the once-per-launch greeting
constexpr double kStartupGiveUp = 10.0;     // stop trying after this

constexpr double kSparseCooldownScale = 2.0;
constexpr double kSparseChanceScale = 0.5;
constexpr double kOftenCooldownScale = 0.5;
constexpr double kOftenChanceScale = 2.0;
}  // namespace

TriggerDirector::TriggerDirector() {
    // Standing-idle scenes: she has been left alone at her spot.
    box_ = {nullptr, "box", 480.0, 1200.0, 0.02, false, -1e9, -1e9};     // 8 min due, 20 min cooldown
    wave_ = {nullptr, "wave", 720.0, 1800.0, 0.015, false, -1e9, -1e9};  // 12 min due, 30 min cooldown
    nap_ = {nullptr, "nap", 1800.0, 3600.0, 0.03, false, -1e9, -1e9};    // 30 min due, 60 min cooldown
    // Working-busy scenes: the desk break choreography.
    stretch_ = {nullptr, "stretch", 1500.0, 1500.0, 0.02, true, -1e9, -1e9}; // 25 min / 25 min
    rice_ = {nullptr, "rice", 2700.0, 2700.0, 0.02, true, -1e9, -1e9};       // 45 min / 45 min
}

double TriggerDirector::cooldownOf(const Scene& scene) const {
    switch (frequency_) {
    case Frequency::Sparse: return scene.cooldown * kSparseCooldownScale;
    case Frequency::Often: return scene.cooldown * kOftenCooldownScale;
    case Frequency::Off:
    case Frequency::Normal: break;
    }
    return scene.cooldown;
}

bool TriggerDirector::eligible(const Scene& scene, double now) const {
    if (!scene.start) return false;
    const double held = scene.busyScene ? busySeconds_ : idleSeconds_;
    if (held < scene.due) return false;
    if (now - globalSince_ < kGlobalInterval) return false;
    if (now - scene.lastSuccess < cooldownOf(scene)) return false;
    if (now - scene.lastAttempt < kRefusalBackoff) return false;
    return true;
}

void TriggerDirector::nominate(Scene& scene, double now) {
    ++attemptCount_;
    scene.lastAttempt = now;
    if (scene.start()) {
        ++startedCount_;
        lastStarted_ = scene.name;
        globalSince_ = now;
        scene.lastSuccess = now;
        scene.lastAttempt = -1e9;  // a success clears the backoff
    }
}

void TriggerDirector::tick(double dt, bool idle, bool busy) {
    clock_ += dt;
    idleSeconds_ = idle ? idleSeconds_ + dt : 0.0;
    busySeconds_ = busy ? busySeconds_ + dt : 0.0;
    if (frequency_ == Frequency::Off) return;

    // The once-per-launch greeting: wave shortly after the window appears.
    // It obeys nothing but the frequency switch (a fresh launch is never
    // inside a cooldown), and it gives up rather than nagging.
    if (!startupDone_) {
        if (clock_ >= kStartupDelay) {
            if (wave_.start) {
                ++attemptCount_;
                if (wave_.start()) {
                    ++startedCount_;
                    lastStarted_ = wave_.name;
                    globalSince_ = clock_;
                    startupDone_ = true;
                }
            }
            if (clock_ >= kStartupGiveUp) startupDone_ = true;
        }
        return;  // nothing else competes with the greeting
    }

    // Chance gate: one roll per eligible scene, first hit wins. Priority is
    // fixed (desk breaks first, then nap, then the standing scenes) which
    // keeps nominations testable; the randomness lives in whether a scene is
    // hit at all, not in which one wins.
    const double chanceScale = chanceScale_
        * (frequency_ == Frequency::Sparse ? kSparseChanceScale
                                           : frequency_ == Frequency::Often
                                               ? kOftenChanceScale : 1.0);
    const auto hit = [&](const Scene& scene) {
        if (chanceScale <= 0.0) return false;
        if (chanceScale >= 1.0) return true;
        return std::uniform_real_distribution<>(0.0, 1.0)(rng_) < scene.chance * chanceScale;
    };

    if (busy) {
        if (eligible(stretch_, clock_) && hit(stretch_)) { nominate(stretch_, clock_); return; }
        if (eligible(rice_, clock_) && hit(rice_)) { nominate(rice_, clock_); return; }
        return;
    }
    if (idle) {
        if (eligible(nap_, clock_) && hit(nap_)) { nominate(nap_, clock_); return; }
        if (eligible(box_, clock_) && hit(box_)) { nominate(box_, clock_); return; }
        if (eligible(wave_, clock_) && hit(wave_)) { nominate(wave_, clock_); return; }
    }
}
