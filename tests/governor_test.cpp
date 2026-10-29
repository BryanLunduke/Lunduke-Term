// SPDX-License-Identifier: GPL-3.0-or-later
// M3 item 1b: the effects governor only sees effects-frame costs, slows the tick within its
// budget, demotes when 1 fps is still too expensive, and probes back with back-off.
#include "effects_governor.hpp"

#include <cstdio>

using namespace lterm;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

// Run the governor for `ms` of simulated time at its current tick, each frame costing `cost`.
struct Sim {
    EffectsGovernor g;
    double now = 0, interval = 100;
    int demotions = 0;
    void run(double ms, double cost) {
        const double end = now + ms;
        while (now < end) {
            now += interval;
            if (g.state() == EffectsGovernor::State::Demoted) {
                if (g.probeDue(now)) {
                    g.startProbe(now);
                    interval = g.config().baseIntervalMs;
                }
                continue;  // static: nothing animates
            }
            g.addFrame(cost);
            double next = interval;
            const auto a = g.evaluate(now, next);
            if (a == EffectsGovernor::Action::Demote) {
                ++demotions;
                g.demoted(now);
            } else if (a == EffectsGovernor::Action::Interval) {
                interval = next;
            }
        }
    }
};

static Sim make() {
    Sim s;
    GovernorConfig c;
    c.budget = 0.25;
    c.baseIntervalMs = 100;
    c.firstProbeMs = 10000;
    c.maxProbeMs = 80000;
    s.g.configure(c);
    s.g.restart(0);
    return s;
}

int main() {
    {  // cheap frames: stays at the base rate
        Sim s = make();
        s.run(20000, 5);  // 5 ms / 100 ms = 5 % of a core
        CHECK(s.interval == 100);
        CHECK(s.demotions == 0);
    }
    {  // moderately expensive: slows to keep within budget, recovers when cheap again
        Sim s = make();
        s.run(20000, 60);  // 60 ms frames: needs >= 240 ms ticks
        CHECK(s.interval >= 240 && s.interval <= 1000);
        CHECK(s.g.lastLoad() <= 0.26);
        CHECK(s.demotions == 0);
        s.run(30000, 5);
        CHECK(s.interval == 100);
        std::printf("slow-down then recover: interval %.0f ms\n", s.interval);
    }
    {  // too expensive even at 1 fps: demotes, probes after 10 s, backs off, recovers
        Sim s = make();
        s.run(6000, 400);  // 400 ms frames: 40 % of a core at 1 fps
        CHECK(s.demotions == 1);
        CHECK(s.g.state() == EffectsGovernor::State::Demoted);
        CHECK(s.g.probeDelayMs() == 10000);
        s.run(6000, 400);  // still within the first wait (demoted at ~3 s)
        CHECK(s.g.state() == EffectsGovernor::State::Demoted);
        s.run(10000, 400);  // probe ~10 s after demotion, fails again
        CHECK(s.demotions == 2);
        CHECK(s.g.probeDelayMs() == 20000);
        s.run(100000, 400);  // keeps failing: 40 s, 80 s (capped)
        CHECK(s.g.probeDelayMs() == 80000);
        const int before = s.demotions;
        s.run(90000, 5);  // load dropped: the next probe passes and auto stays up
        CHECK(s.g.state() == EffectsGovernor::State::Normal);
        CHECK(s.demotions == before);
        CHECK(s.interval == 100);
        s.run(5000, 400);  // a new demotion starts with the short wait again
        CHECK(s.g.probeDelayMs() == 10000);
        std::printf("demote/probe/recover: %d demotions\n", s.demotions);
    }
    {  // one expensive window is not enough to demote (hysteresis)
        Sim s = make();
        s.interval = 1000;
        s.g.restart(0);
        s.run(1000, 400);
        s.run(3000, 100);  // 10 % at 1 fps
        CHECK(s.demotions == 0);
    }
    {  // no frames (paused / hidden) says nothing
        EffectsGovernor g;
        GovernorConfig c;
        g.configure(c);
        g.restart(0);
        double iv = 100;
        CHECK(g.evaluate(5000, iv) == EffectsGovernor::Action::None);
        CHECK(iv == 100);
    }
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("governor: all passed\n");
    return failures ? 1 : 0;
}
