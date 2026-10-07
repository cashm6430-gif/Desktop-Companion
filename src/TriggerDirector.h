#pragma once

#include <functional>
#include <random>
#include <QString>

// Spontaneous-scene scheduler (trigger system r1). Watches how long the pet
// has been standing idle or working busy and occasionally nominates one of
// the fun scenes that otherwise only exist as tray-menu entries: the box
// peek, the wave hello, the nap, the seated stretch and the rice bowl.
//
// The director only nominates. Every start callback routes through the
// ParameterMotion/PetWindow gates, which stay the final authority: a refused
// nomination is dropped and retried after a backoff, never forced.
//
// Discipline (anti-nagging, the user outranks everything here):
//  - a global minimum spacing between any two spontaneous scenes,
//  - an independent cooldown per scene,
//  - a refusal backoff so a blocked scene is not retried every tick,
//  - four user-facing pace levels (off / sparse / normal / often) that scale
//    both the nomination chance and the cooldowns,
//  - the review/capture paths disable the director outright.
//
// The tick is driven by PetWindow's one-second timer; time lives here as a
// plain running clock so tests can drive it deterministically.
class TriggerDirector {
public:
    enum class Frequency { Off, Sparse, Normal, Often };

    using StartFn = std::function<bool()>;

    // One spontaneous scene: how to start it, when it becomes eligible and
    // how long it stays quiet after a success. `due` is the continuous
    // idle/busy time (seconds) before nominations begin; `cooldown` counts
    // from the last success at the Normal pace.
    struct Scene {
        StartFn start;
        const char* name = "";
        double due = 0.0;
        double cooldown = 0.0;
        double chance = 0.0;  // per-tick probability once due (Normal pace)
        bool busyScene = false;    // due counts continuous busy, not idle
        double lastSuccess = -1e9; // own-clock timestamp of the last success
        double lastAttempt = -1e9; // own-clock timestamp of the last refusal
    };

    TriggerDirector();

    // The five start hooks. PetWindow wires them to the same code paths the
    // tray menu uses, so a spontaneous scene is indistinguishable from a
    // manual one (gating, state handling, logging all shared).
    void setStartBox(StartFn fn) { box_.start = std::move(fn); }
    void setStartWave(StartFn fn) { wave_.start = std::move(fn); }
    void setStartNap(StartFn fn) { nap_.start = std::move(fn); }
    void setStartStretch(StartFn fn) { stretch_.start = std::move(fn); }
    void setStartRice(StartFn fn) { rice_.start = std::move(fn); }

    void setFrequency(Frequency f) { frequency_ = f; }
    Frequency frequency() const { return frequency_; }

    // Deterministic nomination for tests and for reproducible captures.
    void seed(unsigned int s) { rng_.seed(s); }
    // Test hook: multiplies every per-tick chance (values >1 make a hit
    // immediate, 0 disables chance-based hits but not the startup wave).
    void setChanceScale(double scale) { chanceScale_ = scale; }

    // Advance by dt seconds with the current controller state. At most one
    // nomination per tick.
    void tick(double dt, bool idle, bool busy);

    // Trace/test observations.
    int startedCount() const { return startedCount_; }
    QString lastStarted() const { return QString::fromLatin1(lastStarted_ ? lastStarted_ : ""); }
    int attemptCount() const { return attemptCount_; }
    double runningSeconds() const { return clock_; }

private:
    void nominate(Scene& scene, double now);
    bool eligible(const Scene& scene, double now) const;
    double cooldownOf(const Scene& scene) const;
    Scene box_;
    Scene wave_;
    Scene nap_;
    Scene stretch_;
    Scene rice_;

    Frequency frequency_ = Frequency::Normal;
    double clock_ = 0.0;          // running seconds since construction
    double idleSeconds_ = 0.0;    // continuous idle time
    double busySeconds_ = 0.0;    // continuous busy time
    double globalSince_ = -1e9;   // last spontaneous success (any scene)
    bool startupDone_ = false;    // the once-per-launch greeting
    double chanceScale_ = 1.0;
    int startedCount_ = 0;
    int attemptCount_ = 0;
    const char* lastStarted_ = nullptr;
    std::mt19937 rng_{20261007};
};
