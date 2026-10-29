// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects_governor.hpp"

#include <algorithm>
#include <cmath>

namespace lterm {

void EffectsGovernor::restart(double nowMs) {
    window0_ = nowMs;
    frameCpu_ = 0;
    frames_ = 0;
}

double EffectsGovernor::probeAbortFrameMs() const {
    return cfg_.probeAbortFrameMs > 0 ? cfg_.probeAbortFrameMs : 2.0 * cfg_.budget * cfg_.maxIntervalMs;
}

void EffectsGovernor::addFrame(double cpuMs) {
    cpuMs = std::max(0.0, cpuMs);
    frameCpu_ += cpuMs;
    ++frames_;
    if (state_ == State::Probing) {
        ++probeFrames_;
        if (cpuMs > probeAbortFrameMs() && !abortProbe_) {
            abortProbe_ = true;
            abortFrame_ = cpuMs;
        }
    }
}

void EffectsGovernor::reset() {
    state_ = State::Normal;
    over_ = probeOk_ = probeFrames_ = 0;
    abortProbe_ = probeAborted_ = false;
    probeDelayMs_ = -1;
    nextProbeMs_ = 0;
}

EffectsGovernor::Action EffectsGovernor::evaluate(double nowMs, double& intervalMs) {
    if (state_ == State::Demoted) return Action::None;
    if (state_ == State::Probing && abortProbe_) {
        // One frame was far over budget: fail the probe now instead of rendering more of them.
        lastFrameMs_ = abortFrame_;
        lastLoad_ = abortFrame_ / std::max(1.0, cfg_.maxIntervalMs);  // its cost even at the slowest tick
        probeAborted_ = true;
        restart(nowMs);
        return Action::Demote;
    }
    if (nowMs - window0_ < cfg_.windowMs) return Action::None;
    if (frames_ == 0) {  // nothing rendered (hidden, paused): no evidence either way
        restart(nowMs);
        return Action::None;
    }
    const double frame = frameCpu_ / frames_;
    restart(nowMs);
    lastFrameMs_ = frame;
    // What the animation alone costs at the current tick: frame cost x ticks per second.
    const double load = frame / std::max(1.0, intervalMs);
    lastLoad_ = load;
    double next = intervalMs;
    // Hysteresis: slow down above the budget, speed up again only below 60 % of it.
    if (load > cfg_.budget) next = std::min(cfg_.maxIntervalMs, std::max(intervalMs * 1.05, frame / cfg_.budget * 1.05));
    else if (load < 0.6 * cfg_.budget) next = std::max(cfg_.baseIntervalMs, intervalMs * 0.85);
    if (load > cfg_.budget && intervalMs >= cfg_.maxIntervalMs) {
        if (++over_ >= cfg_.overWindowsToDemote) return Action::Demote;
    } else {
        over_ = 0;
    }
    if (state_ == State::Probing) {
        // The good windows must be consecutive: an over-budget window starts the count again.
        if (load > cfg_.budget) probeOk_ = 0;
        else if (++probeOk_ >= cfg_.probeWindows) {
            state_ = State::Normal;  // probe passed: the next demotion starts with a short back-off
            probeDelayMs_ = -1;
        }
    }
    if (next != intervalMs && std::abs(next - intervalMs) > 0.05 * intervalMs) {
        intervalMs = next;
        return Action::Interval;
    }
    return Action::None;
}

void EffectsGovernor::demoted(double nowMs) {
    // A failed probe doubles the wait; a demotion from Normal starts at the first delay.
    if (state_ == State::Probing && probeDelayMs_ > 0) probeDelayMs_ = std::min(cfg_.maxProbeMs, probeDelayMs_ * 2);
    else probeDelayMs_ = cfg_.firstProbeMs;
    state_ = State::Demoted;
    over_ = probeOk_ = 0;
    abortProbe_ = false;
    nextProbeMs_ = nowMs + probeDelayMs_;
}

void EffectsGovernor::startProbe(double nowMs) {
    state_ = State::Probing;
    over_ = probeOk_ = probeFrames_ = 0;
    abortProbe_ = probeAborted_ = false;
    restart(nowMs);
}

}  // namespace lterm
