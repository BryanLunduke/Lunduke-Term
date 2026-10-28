// SPDX-License-Identifier: GPL-3.0-or-later
// Effects governor (M3): keeps the idle CRT animation within a CPU budget on software
// rendering. It is fed the CPU cost of each rendered effects frame (process CPU time spent in
// the effects pass itself, so llvmpipe's or the C++ fallback's worker threads count, but parsing
// terminal output does not) and picks the tick interval. If even the slowest tick is over budget
// it asks to demote auto to static; while demoted it asks for a probe now and then (backing off
// while probes keep failing), so auto comes back once rendering is cheap enough again.
#pragma once

namespace lterm {

struct GovernorConfig {
    double budget = 0.25;          // cores
    double baseIntervalMs = 100;   // the quality's normal tick
    double maxIntervalMs = 1000;   // slowest tick before demoting
    double windowMs = 1000;        // evaluation window
    double firstProbeMs = 10000;   // first probe after a demotion
    double maxProbeMs = 160000;    // probe back-off limit
    int overWindowsToDemote = 2;   // consecutive over-budget windows at the slowest tick
    int probeWindows = 3;          // windows a probe must stay within budget to count as passed
};

class EffectsGovernor {
public:
    enum class State { Normal, Demoted, Probing };
    enum class Action { None, Interval, Demote };

    void configure(const GovernorConfig& c) { cfg_ = c; }
    const GovernorConfig& config() const { return cfg_; }
    // A fresh measurement window (animation (re)started, quality changed); keeps the state.
    void restart(double nowMs);
    void addFrame(double cpuMs);  // one effects frame and what it cost
    // Once per tick. On Interval, intervalMs is the new tick interval.
    Action evaluate(double nowMs, double& intervalMs);
    // Demotion happened (the caller switched to static).
    void demoted(double nowMs);
    bool probeDue(double nowMs) const { return state_ == State::Demoted && nowMs >= nextProbeMs_; }
    void startProbe(double nowMs);  // the caller switched back to the auto level
    void reset();                   // explicit quality choice: forget demotions and back-off
    State state() const { return state_; }
    double probeDelayMs() const { return probeDelayMs_; }
    double lastLoad() const { return lastLoad_; }       // cores, estimated for the current tick
    double lastFrameMs() const { return lastFrameMs_; } // mean frame cost of the last window

private:
    GovernorConfig cfg_;
    State state_ = State::Normal;
    double window0_ = 0, frameCpu_ = 0;
    int frames_ = 0, over_ = 0, probeOk_ = 0;
    double nextProbeMs_ = 0, probeDelayMs_ = -1;
    double lastLoad_ = 0, lastFrameMs_ = 0;
};

}  // namespace lterm
