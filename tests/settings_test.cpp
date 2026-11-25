// SPDX-License-Identifier: GPL-3.0-or-later
// One bad settings.ini line or value must not discard the keys around it.
#include "settings_load.hpp"

#include <cstdio>
#include <string>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                                                        \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            ++failures;                                                                                    \
        }                                                                                                  \
    } while (0)

static bool warned(const SettingsValues& s, const char* bit) {
    for (const auto& w : s.warnings)
        if (w.find(bit) != std::string::npos) return true;
    return false;
}

int main() {
    const char* text =
        "[terminal]\n"
        "this is not a key\n"
        "threads = no\n"
        "profile = ibm-dos\n"
        "font-px = 18\n"
        "effects = true\n"
        "effects-quality = static\n"
        "bell = urgent\n"
        "not-a-real-key = 1\n";
    const SettingsValues s = parseSettingsIni(text);
    CHECK(warned(s, "skipped settings line"));
    CHECK(warned(s, "threads"));
    CHECK(s.hasProfile && s.profile == "ibm-dos");
    CHECK(s.hasFontPx && s.fontPx == 18);
    CHECK(s.hasEffects && s.effects);
    CHECK(s.hasEffectsQuality && s.effectsQuality == EffectsQuality::Static);
    CHECK(s.hasBell && s.bell == "urgent");
    CHECK(!s.hasThreads);
    CHECK(warned(s, "unknown setting not-a-real-key"));
    std::printf("settings: %zu warning(s), profile %s, font-px %d\n", s.warnings.size(), s.profile.c_str(),
                s.fontPx);
    if (failures) {
        for (const auto& w : s.warnings) std::fprintf(stderr, "  warn: %s\n", w.c_str());
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("settings_test: OK\n");
    return 0;
}
