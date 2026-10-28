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

void EffectsGovernor::addFrame(double cpuMs) {
    frameCpu_ += std::max(0.0, cpuMs);
    ++frames_;
}

void EffectsGovernor::reset() {
    state_ = State::Normal;
    over_ = probeOk_ = 0;
    probeDelayMs_ = -1;
    nextProbeMs_ = 0;
}

EffectsGovernor::Action EffectsGovernor::evaluate(double nowMs, double& intervalMs) {
    if (state_ == State::Demoted) return Action::None;
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
    if (state_ == State::Probing && load <= cfg_.budget && ++probeOk_ >= cfg_.probeWindows) {
        state_ = State::Normal;  // probe passed: the next demotion starts with a short back-off
        probeDelayMs_ = -1;
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
    nextProbeMs_ = nowMs + probeDelayMs_;
}

void EffectsGovernor::startProbe(double nowMs) {
    state_ = State::Probing;
    over_ = probeOk_ = 0;
    restart(nowMs);
}

}  // namespace lterm
