// SPDX-License-Identifier: GPL-3.0-or-later
#include "profiles.hpp"

#include <algorithm>
#include <cmath>

namespace lterm {

float greyOf(Color8 c) { return rgb2grey(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f); }

Color8 liftToGrey(Color8 c, float minGrey) {
    if (minGrey <= 0.0f) return c;
    if (std::max({c.r, c.g, c.b}) < 16) return c;  // intentional black
    const float g = greyOf(c);
    if (g >= minGrey) return c;
    const float white = rgb2grey(1.0f, 1.0f, 1.0f);  // 0.97: the weights do not sum to 1
    const float t = std::min(1.0f, (minGrey - g) / (white - g));
    auto mixc = [t](uint8_t v) {
        return static_cast<uint8_t>(std::lround(v + (255.0f - v) * t));
    };
    return Color8{mixc(c.r), mixc(c.g), mixc(c.b)};
}

namespace {

// Tango (GNOME Terminal's default): readable on black, well known.
constexpr std::array<Color8, 16> kTango{{
    {0x2e, 0x34, 0x36}, {0xcc, 0x00, 0x00}, {0x4e, 0x9a, 0x06}, {0xc4, 0xa0, 0x00},
    {0x34, 0x65, 0xa4}, {0x75, 0x50, 0x7b}, {0x06, 0x98, 0x9a}, {0xd3, 0xd7, 0xcf},
    {0x55, 0x57, 0x53}, {0xef, 0x29, 0x29}, {0x8a, 0xe2, 0x34}, {0xfc, 0xe9, 0x4f},
    {0x72, 0x9f, 0xcf}, {0xad, 0x7f, 0xa8}, {0x34, 0xe2, 0xe2}, {0xee, 0xee, 0xec},
}};

// IBM CGA/VGA text-mode colours (for the full-colour "IBM Dos" look).
constexpr std::array<Color8, 16> kCga{{
    {0x00, 0x00, 0x00}, {0xaa, 0x00, 0x00}, {0x00, 0xaa, 0x00}, {0xaa, 0x55, 0x00},
    {0x00, 0x00, 0xaa}, {0xaa, 0x00, 0xaa}, {0x00, 0xaa, 0xaa}, {0xaa, 0xaa, 0xaa},
    {0x55, 0x55, 0x55}, {0xff, 0x55, 0x55}, {0x55, 0xff, 0x55}, {0xff, 0xff, 0x55},
    {0x55, 0x55, 0xff}, {0xff, 0x55, 0xff}, {0x55, 0xff, 0xff}, {0xff, 0xff, 0xff},
}};

// Monochrome phosphor palette: brightness is all that survives the tint, so give every colour
// a readable grey level and keep bright colours brighter than normal ones. Colour 0 (black)
// stays black, colour 8 ("bright black", used for dim text) is lifted less.
std::array<Color8, 16> phosphorPalette(const std::array<Color8, 16>& base) {
    std::array<Color8, 16> p = base;
    p[0] = Color8{0, 0, 0};
    for (int i = 1; i < 16; ++i) {
        const float minGrey = i == 8 ? 0.38f : (i < 8 ? 0.50f : 0.68f);
        p[i] = liftToGrey(base[i], minGrey);
    }
    return p;
}

// Colour (chroma) palette: hues survive, so only lift the darkest ones (CGA blue #0000aa is
// almost invisible on black, especially through bloom and the tube's ambient light).
std::array<Color8, 16> legibleColourPalette(const std::array<Color8, 16>& base) {
    std::array<Color8, 16> p = base;
    for (int i = 1; i < 16; ++i) p[i] = liftToGrey(base[i], i < 8 || i == 8 ? 0.30f : 0.45f);
    return p;
}

Rgb hex(uint32_t v) {
    return Rgb{((v >> 16) & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f, (v & 0xff) / 255.0f};
}

// fonts-terminus (TTF, scalable) first, then fonts-terminus-otb (bitmap strikes), then
// fonts-dejavu-core, then whatever fontconfig calls monospace.
const std::vector<FontChoice> kTerminus = {
    {"Terminus (TTF)", 0}, {"Terminus", 0}, {"DejaVu Sans Mono", 0}, {"Monospace", 0}};

void setFonts(Profile& p, std::vector<FontChoice> head, const std::vector<FontChoice>& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    p.fonts = head;
    p.fontFamily.clear();
    for (const auto& f : p.fonts) p.fontFamily += (p.fontFamily.empty() ? "" : ", ") + f.family;
}

std::vector<Profile> makeProfiles() {
    std::vector<Profile> v;
    {
        Profile p;
        p.id = "plain";
        p.label = "Plain (no effects)";
        p.effects = false;
        p.palette = kTango;
        p.fg = Color8{0xd3, 0xd7, 0xcf};
        p.cursor = p.fg;
        setFonts(p, {{"DejaVu Sans Mono", 0}}, {{"Monospace", 0}});
        p.fontPx = 16;
        p.marginFrac = 0.0f;
        p.marginMinPx = 4;
        // Effects stay off unless asked for; toggling them on (Ctrl+Shift+E / --effects) uses the
        // amber tube values on plain's own colours.
        p.fx = EffectParams{};
        p.fx.rasterization = 0;
        p.hasEffectsPalette = true;
        p.effectsPalette = phosphorPalette(kTango);
        p.effectsMinFgGrey = 0.45f;
        v.push_back(p);
    }
    // cool-retro-term 1.2.0 "Default Amber".
    Profile amber;
    amber.id = "amber";
    amber.label = "Amber";
    amber.effects = true;
    amber.fx = EffectParams{};
    amber.fx.rasterization = 0;
    amber.palette = phosphorPalette(kTango);
    amber.fg = Color8{0xee, 0xee, 0xec};
    amber.cursor = amber.fg;
    amber.minFgGrey = 0.45f;
    setFonts(amber, {}, kTerminus);
    amber.fontPx = 22;
    amber.marginFrac = 0.04f;
    amber.marginMinPx = 12;
    v.push_back(amber);
    v[0].fx = amber.fx;  // plain + effects toggled on = amber effects with the plain palette
    {
        Profile p = amber;  // "Monochrome Green": same values, green phosphor, no chroma
        p.id = "green";
        p.label = "Green";
        p.fx.fontColor = hex(0x0ccc68);
        p.fx.chromaColor = 0.0f;
        v.push_back(p);
    }
    {
        Profile p = amber;  // "Green Scanlines"-style: green with scanlines on
        p.id = "green-scanlines";
        p.label = "Green scanlines";
        p.fx.fontColor = hex(0x0ccc68);
        p.fx.chromaColor = 0.0f;
        p.fx.rasterization = 1;
        v.push_back(p);
    }
    {
        Profile p = amber;  // "IBM Dos": full colour (chroma 1), lighter effects
        p.id = "ibm-dos";
        p.label = "IBM DOS (colour)";
        p.fx.fontColor = hex(0xffffff);
        p.fx.chromaColor = 1.0f;
        p.fx.ambientLight = 0.151f;
        p.fx.bloom = 0.2969f;
        p.fx.burnIn = 0.0469f;
        p.fx.flickering = 0.0955f;
        p.fx.glowingLine = 0.1545f;
        p.fx.horizontalSync = 0.0f;
        p.fx.jitter = 0.1545f;
        p.fx.screenCurvature = 0.4f;
        p.fx.staticNoise = 0.0503f;
        p.palette = legibleColourPalette(kCga);
        p.fg = Color8{0xaa, 0xaa, 0xaa};
        p.cursor = p.fg;
        p.minFgGrey = 0.30f;
        // fonts-pc names it "Px IBM VGA8"; the int10h originals ("Px437"/"PxPlus") work if the
        // user installed them. 32 px = exactly 2x the 8x16 VGA glyphs (crisp, ~80x25 on 1280x800).
        setFonts(p, {{"Px IBM VGA8", 32}, {"Px437 IBM VGA8", 32}, {"PxPlus IBM VGA8", 32}}, kTerminus);
        v.push_back(p);
    }
    {
        Profile p = amber;  // "IBM 3278": green, no animated effects at all (only burn-in)
        p.id = "ibm-3278";
        p.label = "IBM 3278 (static)";
        p.fx.fontColor = hex(0x0ccc68);
        p.fx.chromaColor = 0.0f;
        p.fx.ambientLight = 0.1f;
        p.fx.bloom = 0.2969f;
        p.fx.burnIn = 0.6f;
        p.fx.flickering = 0.0f;
        p.fx.glowingLine = 0.0f;
        p.fx.horizontalSync = 0.0f;
        p.fx.jitter = 0.0f;
        p.fx.screenCurvature = 0.2f;
        p.fx.staticNoise = 0.0f;
        setFonts(p, {{"IBM 3270", 0}, {"3270", 0}}, kTerminus);  // fonts-3270
        v.push_back(p);
    }
    return v;
}

}  // namespace

const std::vector<Profile>& builtinProfiles() {
    static const std::vector<Profile> v = makeProfiles();
    return v;
}

FontPick profileFont(const Profile& p) {
    FontPick f = pickFont(p.fonts);
    if (f.px <= 0) f.px = p.fontPx;
    return f;
}

const Profile* findProfile(const std::string& id) {
    for (const auto& p : builtinProfiles())
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace lterm
