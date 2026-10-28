// SPDX-License-Identifier: GPL-3.0-or-later
// Profiles: plain is the default and has no effects; phosphor palettes keep every ANSI colour
// bright enough to read after the monochrome tint (the M0 "dark blue ls directory" problem).
#include "profiles.hpp"

#include <cstdio>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

int main() {
    const auto& all = builtinProfiles();
    CHECK(!all.empty());
    CHECK(all.front().id == "plain");
    CHECK(!all.front().effects);
    for (const char* id : {"plain", "amber", "green", "green-scanlines", "ibm-dos", "ibm-3278"})
        CHECK(findProfile(id) != nullptr);
    CHECK(findProfile("nope") == nullptr);
    CHECK(findProfile("ibm-3278") && !findProfile("ibm-3278")->fx.animated());
    CHECK(findProfile("amber") && findProfile("amber")->fx.animated());

    {
        const Profile& plain = all.front();
        CHECK(plain.hasEffectsPalette);
        for (int i = 1; i < 16; ++i) CHECK(greyOf(plain.effectsPalette[i]) >= (i == 8 ? 0.37f : 0.49f));
    }
    for (const auto& p : all) {
        if (p.id == "plain") continue;
        CHECK(p.effects);
        CHECK(greyOf(p.palette[0]) < 0.05f);  // black stays black
        // Monochrome tubes show only brightness; colour (chroma 1) tubes keep the hue as well.
        const bool colour = p.fx.chromaColor >= 0.99f;
        for (int i = 1; i < 16; ++i) {
            const float min = colour ? (i <= 8 ? 0.29f : 0.44f) : (i == 8 ? 0.37f : 0.49f);
            if (greyOf(p.palette[i]) < min) {
                std::fprintf(stderr, "%s colour %d grey %.3f < %.2f\n", p.id.c_str(), i, greyOf(p.palette[i]), min);
                ++failures;
            }
        }
        // Blue (4) and bright blue (12): the ls directory colours.
        std::printf("%-16s blue grey %.2f, bright blue grey %.2f\n", p.id.c_str(), greyOf(p.palette[4]),
                    greyOf(p.palette[12]));
    }
    // liftToGrey: keeps black, reaches the target, keeps hue order.
    CHECK(greyOf(liftToGrey(Color8{0, 0, 0}, 0.5f)) < 0.01f);
    const Color8 b = liftToGrey(Color8{0x34, 0x65, 0xa4}, 0.5f);
    CHECK(greyOf(b) >= 0.49f);
    CHECK(b.b >= b.r);
    if (failures == 0) std::printf("profiles_test: OK\n");
    return failures ? 1 : 0;
}
