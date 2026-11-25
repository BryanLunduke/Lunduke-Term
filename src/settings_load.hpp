// SPDX-License-Identifier: GPL-3.0-or-later
// settings.ini reader. One bad line or one value of the wrong type is skipped
// with a warning; the rest of the file is still applied.
#pragma once

#include "effects.hpp"
#include "profiles.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lterm {

struct SettingsValues {
    bool hasProfile = false;
    std::string profile;
    bool hasEffects = false;
    bool effects = false;
    bool hasRenderer = false;
    std::string renderer;
    bool hasThreads = false;
    int threads = 0;
    bool hasEffectsFps = false;
    int effectsFps = 0;
    bool hasMaxFps = false;
    int maxFps = 0;
    bool hasEffectsQuality = false;
    EffectsQuality effectsQuality = EffectsQuality::Auto;
    bool hasCpuBudget = false;
    double cpuBudget = 0;
    bool hasPause = false;
    bool pauseWhenUnfocused = true;
    bool hasFont = false;
    std::string fontFamily;
    bool hasFontPx = false;
    int fontPx = 0;
    bool hasScrollback = false;
    int64_t scrollbackLines = 0;
    bool hasLogin = false;
    bool loginShell = false;
    bool hasCwd = false;
    std::string cwd;
    bool hasOnExit = false;
    std::string onExit;
    bool hasBell = false;
    std::string bell;
    bool hasCursorShape = false;
    std::string cursorShape;
    bool hasCursorBlink = false;
    bool cursorBlink = false;
    bool hasWordChars = false;
    std::string wordChars;
    bool hasFrame = false;
    int frame = 0;
    bool hasFrameSize = false;
    float frameSize = 0;
    bool hasFrameColor = false;
    std::string frameColor;
    bool hasFrameShininess = false;
    float frameShininess = 0;
    bool hasRgbShift = false;
    float rgbShift = 0;
    bool hasScrollbar = false;
    int scrollbar = -1;  // -1 auto, 0 off, 1 on
    bool hasConfirmPaste = false;
    bool confirmPaste = true;
    std::vector<std::string> warnings;
};

// `text` is the contents of settings.ini. Warnings name the key or line that was skipped.
SettingsValues parseSettingsIni(const std::string& text);

}  // namespace lterm
