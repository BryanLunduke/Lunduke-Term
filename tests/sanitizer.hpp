// SPDX-License-Identifier: GPL-3.0-or-later
// Which sanitizers this translation unit was compiled with (tests only).
#pragma once

#if defined(__SANITIZE_ADDRESS__)
#define LTERM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LTERM_ASAN 1
#endif
#endif
#ifndef LTERM_ASAN
#define LTERM_ASAN 0
#endif

// meson defines LTERM_SANITIZED_BUILD=1 when b_sanitize is set (build-san). view_test is always
// compiled with -fsanitize=address, even in a release build, so LTERM_ASAN alone cannot tell the
// two builds apart there.
#ifndef LTERM_SANITIZED_BUILD
#define LTERM_SANITIZED_BUILD 0
#endif

inline const char* ltermBuildLabel() {
    if (LTERM_SANITIZED_BUILD) return "ASan+UBSan build";
    if (LTERM_ASAN) return "release build, ASan in this test only";
    return "release build";
}

// Wall-clock limits that ASan+UBSan builds (2-3x slower, often with every core busy) cannot
// keep: the release value, or a looser one in build-san.
inline double ltermTimingLimitMs(double releaseMs, double sanitizedMs) {
    return LTERM_SANITIZED_BUILD ? sanitizedMs : releaseMs;
}
