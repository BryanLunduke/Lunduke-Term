// SPDX-License-Identifier: GPL-3.0-or-later
#include "profiles.hpp"

#include <algorithm>
#include <cctype>
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

// Palettes of the M2 CRT-style profiles (each profile has its own; values are ours).
// "Vintage": warm, slightly washed-out phosphor-era colours (only brightness survives the tint).
constexpr std::array<Color8, 16> kVintage{{
    {0x00, 0x00, 0x00}, {0xb0, 0x3a, 0x2e}, {0x5f, 0x9e, 0x3c}, {0xc8, 0x9b, 0x3c},
    {0x3d, 0x6a, 0x9e}, {0x8e, 0x5a, 0x8e}, {0x4a, 0x9a, 0x8f}, {0xc9, 0xc2, 0xb0},
    {0x6e, 0x68, 0x5e}, {0xe0, 0x6c, 0x5a}, {0x9c, 0xd0, 0x6e}, {0xf0, 0xd0, 0x78},
    {0x7a, 0xa6, 0xd8}, {0xc4, 0x90, 0xc4}, {0x86, 0xd0, 0xc4}, {0xf4, 0xef, 0xe0},
}};
// "Futuristic": cool, saturated colours around the Tango sky blue #729fcf (chroma 1 keeps hues).
constexpr std::array<Color8, 16> kFuturistic{{
    {0x00, 0x00, 0x00}, {0xe0, 0x44, 0x6a}, {0x3c, 0xd6, 0x9c}, {0xe8, 0xc8, 0x5a},
    {0x4a, 0x7c, 0xd8}, {0xa8, 0x6c, 0xe0}, {0x2e, 0xc4, 0xd8}, {0xb8, 0xcc, 0xe4},
    {0x5a, 0x6a, 0x80}, {0xff, 0x6e, 0x90}, {0x6c, 0xf2, 0xbe}, {0xff, 0xe4, 0x88},
    {0x7c, 0xa8, 0xff}, {0xcc, 0x98, 0xff}, {0x6c, 0xe8, 0xf8}, {0xf0, 0xf6, 0xff},
}};
// "Paper white" (P4 white phosphor): neutral greys with a faint cool cast.
constexpr std::array<Color8, 16> kPaperWhite{{
    {0x00, 0x00, 0x00}, {0xb4, 0x4c, 0x4c}, {0x5c, 0xa0, 0x5c}, {0xb4, 0xa0, 0x50},
    {0x50, 0x70, 0xb0}, {0x96, 0x64, 0xa0}, {0x50, 0xa0, 0xa8}, {0xd0, 0xd4, 0xdc},
    {0x70, 0x74, 0x7c}, {0xe4, 0x78, 0x78}, {0x8c, 0xd4, 0x8c}, {0xe8, 0xd8, 0x88},
    {0x88, 0xa8, 0xe8}, {0xc8, 0x98, 0xd4}, {0x88, 0xd8, 0xe0}, {0xf8, 0xfa, 0xff},
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
    // The bezel is an option on the monochrome tubes: dark warm plastic, a satin finish.
    amber.bezelOption = true;
    amber.fx.frameSize = 28.0f;
    amber.fx.frameColor = hex(0x2a2622);
    amber.fx.frameShininess = 0.5f;
    v.push_back(amber);
    v[0].fx = amber.fx;  // plain + effects toggled on = amber effects with the plain palette
    {
        Profile p = amber;  // "Monochrome Green": same values, green phosphor, no chroma
        p.id = "green";
        p.fx.frameColor = hex(0x22262a);  // cooler grey next to the green phosphor
        p.label = "Green";
        p.fx.fontColor = hex(0x0ccc68);
        p.fx.chromaColor = 0.0f;
        v.push_back(p);
    }
    {
        Profile p = amber;  // "Green Scanlines"-style: green with scanlines on
        p.id = "green-scanlines";
        p.fx.frameColor = hex(0x22262a);
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
        p.fx.rgbShift = 0.3524f;  // CRT 1.2.0 "IBM Dos" rbgShift
        p.bezelOption = false;
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
        p.bezelOption = false;
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
    {
        // cool-retro-term 1.2.0 "Vintage" tube values. CRT uses the PetMe (Commodore PET) font
        // there, which is not free (see docs/FONTS.md); Terminus instead. Not supported: contrast
        // 0.7473 (no contrast control) and margin 0.5.
        Profile p = amber;
        p.id = "vintage";
        p.label = "Vintage";
        p.fx.ambientLight = 0.5f;
        p.fx.bloom = 0.4983f;
        p.fx.brightness = 0.5014f;
        p.fx.burnIn = 0.4983f;
        p.fx.chromaColor = 0.0f;
        p.fx.flickering = 0.9f;
        p.fx.fontColor = hex(0x00ff3e);
        p.fx.glowingLine = 0.3f;
        p.fx.horizontalSync = 0.42f;
        p.fx.jitter = 0.4f;
        p.fx.rasterization = 1;
        p.fx.rgbShift = 0.2969f;
        p.fx.screenCurvature = 0.5f;
        p.fx.staticNoise = 0.2969f;
        p.fx.frameColor = hex(0x3b3328);  // old beige-brown plastic
        p.palette = phosphorPalette(kVintage);
        p.fg = Color8{0xf4, 0xef, 0xe0};
        p.cursor = p.fg;
        setFonts(p, {}, kTerminus);
        v.push_back(p);
    }
    {
        // cool-retro-term 1.2.0 "Futuristic": colour, flat glass, no scanlines. Not supported:
        // saturationColor 0.4983, contrast 0.85, windowOpacity 0.7 (no translucent windows).
        Profile p = amber;
        p.id = "futuristic";
        p.label = "Futuristic (colour)";
        p.bezelOption = false;  // flat glass, frameMargin 0
        p.fx.ambientLight = 0.0f;
        p.fx.bloom = 0.5017f;
        p.fx.brightness = 0.5014f;
        p.fx.burnIn = 0.0955f;
        p.fx.chromaColor = 1.0f;
        p.fx.flickering = 0.2f;
        p.fx.fontColor = hex(0x729fcf);
        p.fx.glowingLine = 0.1476f;
        p.fx.horizontalSync = 0.0f;
        p.fx.jitter = 0.099f;
        p.fx.rasterization = 0;
        p.fx.rgbShift = 0.0f;
        p.fx.screenCurvature = 0.0f;
        p.fx.staticNoise = 0.0955f;
        p.palette = legibleColourPalette(kFuturistic);
        p.fg = Color8{0xb8, 0xcc, 0xe4};
        p.cursor = Color8{0x6c, 0xe8, 0xf8};
        p.minFgGrey = 0.30f;
        p.marginFrac = 0.01f;  // CRT margin 0.1
        setFonts(p, {}, kTerminus);
        v.push_back(p);
    }
    {
        // A white (P4) monochrome tube, a variation of CRT's monochrome profiles: amber's tube
        // with a cool white phosphor, fine scanlines, less burn-in and noise.
        Profile p = amber;
        p.id = "paper-white";
        p.label = "Paper white";
        p.fx.fontColor = hex(0xdce6ff);
        p.fx.chromaColor = 0.0f;
        p.fx.bloom = 0.35f;
        p.fx.burnIn = 0.15f;
        p.fx.flickering = 0.05f;
        p.fx.horizontalSync = 0.0f;
        p.fx.jitter = 0.1f;
        p.fx.staticNoise = 0.06f;
        p.fx.screenCurvature = 0.2f;
        p.fx.rasterization = 1;
        p.fx.rasterizationIntensity = 0.5f;
        p.fx.frameColor = hex(0xc8c4bc);  // light grey terminal case
        p.fx.frameShininess = 0.3f;
        p.palette = phosphorPalette(kPaperWhite);
        p.fg = Color8{0xf8, 0xfa, 0xff};
        p.cursor = p.fg;
        setFonts(p, {}, kTerminus);
        v.push_back(p);
    }
    return v;
}

}  // namespace

bool parseHexColor(const std::string& s, Rgb& out) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6) return false;
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    out = hex(static_cast<uint32_t>(std::stoul(h, nullptr, 16)));
    return true;
}

std::string applyTubeOverrides(Profile& p, const TubeOverrides& o) {
    std::string msg;
    if (o.frame >= 0) {
        if (p.bezelOption) p.fx.frame = o.frame > 0;
        else if (o.frame > 0) msg += "profile '" + p.id + "' has no bezel option; --frame ignored. ";
    }
    if (o.frameSize >= 0) p.fx.frameSize = std::min(200.0f, o.frameSize);
    if (!o.frameColor.empty() && !parseHexColor(o.frameColor, p.fx.frameColor))
        msg += "frame colour must be #rrggbb. ";
    if (o.frameShininess >= 0) p.fx.frameShininess = std::min(1.0f, o.frameShininess);
    if (o.rgbShift >= 0) p.fx.rgbShift = std::min(1.0f, o.rgbShift);
    return msg;
}

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
