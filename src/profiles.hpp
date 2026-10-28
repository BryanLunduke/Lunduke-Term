// SPDX-License-Identifier: GPL-3.0-or-later
// Built-in looks. "plain" (the default) has no effects at all; the others take their effect
// values from the cool-retro-term 1.2.0 profiles of the same name and add a palette tuned so
// every ANSI colour stays readable after the monochrome phosphor tint.
#pragma once

#include "effects.hpp"
#include "font_resolve.hpp"
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
    // Font choices in order; the first *installed* one wins (pickFont / profileFont), each may
    // carry its own pixel size (pixel fonts look right only at integer multiples). See
    // docs/FONTS.md for the Debian package that provides each family.
    std::vector<FontChoice> fonts;
    std::string fontFamily;   // the same list joined with ", " (for display / Pango)
    int fontPx = 16;          // size for choices with px == 0
    // Fraction of the window kept free around the text (the barrel warp hides the corners).
    float marginFrac = 0.0f;
    int marginMinPx = 4;
    // M2: the bezel (fx.frame) can be switched on for this profile (--frame / settings frame).
    bool bezelOption = false;
};

// Command-line / settings overrides of a profile's tube (negative / empty = keep the profile's).
struct TubeOverrides {
    int frame = -1;             // 0 off, 1 on (only for profiles with bezelOption)
    float frameSize = -1;       // logical px
    std::string frameColor;     // #rrggbb
    float frameShininess = -1;  // 0..1
    float rgbShift = -1;        // CRT rbgShift slider value, 0..1
};
// Applies `o` to p.fx; returns a message for an override that was ignored or invalid ("" = ok).
std::string applyTubeOverrides(Profile& p, const TubeOverrides& o);
bool parseHexColor(const std::string& s, Rgb& out);

const std::vector<Profile>& builtinProfiles();
// The installed font this profile will use (falls back along `fonts`, then to Monospace).
FontPick profileFont(const Profile& p);
// nullptr when unknown.
const Profile* findProfile(const std::string& id);

// Raises c towards white until rgb2grey(c) >= minGrey (keeps the hue for chroma profiles).
// Colours that are (almost) black are left alone so "black on colour" stays black.
Color8 liftToGrey(Color8 c, float minGrey);
float greyOf(Color8 c);

}  // namespace lterm
