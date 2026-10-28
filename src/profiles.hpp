// SPDX-License-Identifier: GPL-3.0-or-later
// Built-in looks. "plain" (the default) has no effects at all; the others take their effect
// values from the cool-retro-term 1.2.0 profiles of the same name and add a palette tuned so
// every ANSI colour stays readable after the monochrome phosphor tint.
#pragma once

#include "effects.hpp"
#include "grid.hpp"

#include <array>
#include <string>
#include <vector>

namespace lterm {

struct Profile {
    std::string id;           // command line / settings name
    std::string label;        // human readable
    bool effects = false;     // effects on by default for this profile
    EffectParams fx;          // used when effects are on
    Color8 fg{208, 208, 208}, bg{0, 0, 0}, cursor{208, 208, 208};
    std::array<Color8, 16> palette{};  // ANSI 0-15 as painted into the source image
    // Colours outside the 16-colour palette (256-colour cube, truecolor) are lifted to at
    // least this grey level (rgb2grey) so they survive the phosphor tint; 0 = untouched.
    float minFgGrey = 0.0f;
    // Palette and minimum grey used while effects are on, when they differ from the above
    // (plain: Tango normally, a phosphor palette under the amber tube).
    bool hasEffectsPalette = false;
    std::array<Color8, 16> effectsPalette{};
    float effectsMinFgGrey = 0.0f;
    std::string fontFamily;   // Pango family list, first installed one wins
    int fontPx = 16;
    // Fraction of the window kept free around the text (the barrel warp hides the corners).
    float marginFrac = 0.0f;
    int marginMinPx = 4;
};

const std::vector<Profile>& builtinProfiles();
// nullptr when unknown.
const Profile* findProfile(const std::string& id);

// Raises c towards white until rgb2grey(c) >= minGrey (keeps the hue for chroma profiles).
// Colours that are (almost) black are left alone so "black on colour" stays black.
Color8 liftToGrey(Color8 c, float minGrey);
float greyOf(Color8 c);

}  // namespace lterm
