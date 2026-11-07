// SPDX-License-Identifier: GPL-3.0-or-later
// Profiles: amber is the default (M5), plain has no effects and stays selectable; phosphor palettes keep every ANSI colour
// bright enough to read after the monochrome tint (the M0 "dark blue ls directory" problem).
#include "profiles.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

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
    // M5 (review item 12, editor's decision): the retro look is the default.
    CHECK(std::string(kDefaultProfile) == "amber");
    CHECK(findProfile(kDefaultProfile) && findProfile(kDefaultProfile)->effects);
    CHECK(findProfile("plain") && !findProfile("plain")->effects);
    CHECK(!all.front().effects);
    for (const char* id : {"plain", "amber", "green", "green-scanlines", "ibm-dos", "ibm-3278"})
        CHECK(findProfile(id) != nullptr);
    CHECK(findProfile("nope") == nullptr);
    CHECK(findProfile("ibm-3278") && !findProfile("ibm-3278")->fx.animated());
    CHECK(findProfile("amber") && findProfile("amber")->fx.animated());
    // Desktop font: same pixel size as xfce4-terminal's Monospace 12 (12 pt = 16 px at 96 dpi).
    CHECK(findProfile("amber")->fontPx == 16);
    CHECK(findProfile("plain")->fontPx == 16);
    // User-visible spelling is American. The ids stay "ibm-dos" / "futuristic", which is what
    // settings.ini and --profile store, so a saved profile still loads.
    CHECK(findProfile("ibm-dos") && findProfile("ibm-dos")->label == "IBM DOS (color)");
    CHECK(findProfile("futuristic") && findProfile("futuristic")->label == "Futuristic (color)");
    for (const auto& p : all) {
        CHECK(p.label.find("colour") == std::string::npos);
        CHECK(p.label.find("Colour") == std::string::npos);
        CHECK(p.id.find("colour") == std::string::npos);
    }
    if (const Profile* dos = findProfile("ibm-dos")) {
        // Slightly less bloom than CRT 1.2.0's 0.2969 / radius 20. Scanlines stay off; the RGB
        // shift stays CRT's 0.3524 so the DOS color fringes remain.
        CHECK(dos->fx.bloom > 0.15f && dos->fx.bloom < 0.2969f);
        CHECK(dos->fx.bloomRadiusHalfRes >= 12 && dos->fx.bloomRadiusHalfRes < 20);
        CHECK(dos->fx.rasterization == 0);
        CHECK(std::fabs(dos->fx.rgbShift - 0.3524f) < 1e-4f);
        CHECK(dos->id == "ibm-dos");
    }

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

    // M2 stage 3: the CRT-1.2.0-style profiles exist, carry CRT's tube values and their own palette.
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    auto rgbIs = [](const Rgb& c, uint32_t v) {
        auto q = [](float f) { return static_cast<int>(std::lround(f * 255.0f)); };
        return q(c.r) == int((v >> 16) & 0xff) && q(c.g) == int((v >> 8) & 0xff) && q(c.b) == int(v & 0xff);
    };
    for (const char* id : {"vintage", "futuristic", "paper-white"}) CHECK(findProfile(id) != nullptr);
    if (const Profile* v = findProfile("vintage")) {
        CHECK(rgbIs(v->fx.fontColor, 0x00ff3e));
        CHECK(near(v->fx.ambientLight, 0.5f) && near(v->fx.bloom, 0.4983f) && near(v->fx.burnIn, 0.4983f));
        CHECK(near(v->fx.flickering, 0.9f) && near(v->fx.horizontalSync, 0.42f) && near(v->fx.jitter, 0.4f));
        CHECK(near(v->fx.chromaColor, 0.0f) && v->fx.rasterization == 1 && near(v->fx.rgbShift, 0.2969f));
        CHECK(near(v->fx.screenCurvature, 0.5f) && near(v->fx.staticNoise, 0.2969f) && near(v->fx.glowingLine, 0.3f));
        CHECK(v->bezelOption && v->fx.animated());
    }
    if (const Profile* f = findProfile("futuristic")) {
        CHECK(rgbIs(f->fx.fontColor, 0x729fcf));
        CHECK(near(f->fx.chromaColor, 1.0f) && near(f->fx.ambientLight, 0.0f) && near(f->fx.bloom, 0.5017f));
        CHECK(near(f->fx.burnIn, 0.0955f) && near(f->fx.flickering, 0.2f) && near(f->fx.glowingLine, 0.1476f));
        CHECK(near(f->fx.jitter, 0.099f) && near(f->fx.screenCurvature, 0.0f) && f->fx.rasterization == 0);
        CHECK(!f->bezelOption);
    }
    if (const Profile* w = findProfile("paper-white")) {
        CHECK(near(w->fx.chromaColor, 0.0f) && w->fx.rasterization == 1 && w->bezelOption);
        // white phosphor: no channel dominates
        CHECK(std::min({w->fx.fontColor.r, w->fx.fontColor.g, w->fx.fontColor.b}) > 0.8f);
    }
    // Every profile has its own palette (no two effect profiles share one), except the documented
    // amber family (green, green-scanlines, ibm-3278 and the M5 font profiles pixel-8bit,
    // pixel-hires-mono, pixel-16bit, pixel-40col and modern reuse amber's phosphor Tango palette).
    {
        std::set<std::string> seen;
        for (const auto& p : all) {
            if (p.id == "plain" || p.id == "green" || p.id == "green-scanlines" || p.id == "ibm-3278" ||
                p.id.rfind("pixel-", 0) == 0 || p.id == "modern")  // M5 font profiles: amber's palette
                continue;
            std::string key;
            for (const auto& c : p.palette) key += std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + ";";
            if (!seen.insert(key).second) {
                std::fprintf(stderr, "%s shares its palette with another profile\n", p.id.c_str());
                ++failures;
            }
        }
    }
    // Licence rule (docs/FONTS.md, docs/PACKAGING.md): no Apple II, Commodore PET or C64 fonts, and
    // no "c64" profile.
    for (const auto& p : all) {
        std::string id = p.id;
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::tolower(c); });
        CHECK(id.find("c64") == std::string::npos && id.find("commodore") == std::string::npos &&
              id.find("apple") == std::string::npos && id.find("pet") == std::string::npos);
        for (const auto& f : p.fonts) {
            std::string fam = f.family;
            std::transform(fam.begin(), fam.end(), fam.begin(), [](unsigned char c) { return std::tolower(c); });
            // "c64" catches Style64's C64 Pro / C64 Pro Mono / C64 Elite Mono. The one allowed family
            // with the mark is Debian fonts-sixtyfour's exact "C-64" (OFL-1.1, editor decision M5),
            // and only in pixel-40col.
            if (fam == "c-64") {
                if (f.family != "C-64" || p.id != "pixel-40col") {
                    std::fprintf(stderr, "%s uses '%s' (only pixel-40col may use C-64)\n", p.id.c_str(), f.family.c_str());
                    ++failures;
                }
                continue;
            }
            for (const char* bad : {"petme", "commodore", "c64", "c-64", "printchar", "prnumber", "apple", "atariclassic"})
                if (fam.find(bad) != std::string::npos) {
                    std::fprintf(stderr, "%s uses forbidden font family '%s'\n", p.id.c_str(), f.family.c_str());
                    ++failures;
                }
        }
    }
    CHECK(findProfile("c64") == nullptr);
    // M5 item 2: neutral profile names (no Atari/Amiga/Commodore/C64 marks in id or label);
    // fonts-sixtyfour ("C-64") is in pixel-40col (editor decision, docs/FONTS.md).
    for (const char* id : {"pixel-8bit", "pixel-hires-mono", "pixel-16bit", "pixel-40col", "modern"})
        CHECK(findProfile(id) != nullptr);
    for (const auto& p : all) {
        std::string s = p.id + " " + p.label;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        CHECK(s.find("atari") == std::string::npos && s.find("amiga") == std::string::npos &&
              s.find("commodore") == std::string::npos && s.find("c64") == std::string::npos &&
              s.find("c-64") == std::string::npos);
    }
    // The forbidden fonts stay out: no profile names PetMe, C64 Pro (Mono), Apple II or AtariClassic.
    for (const auto& p : all)
        for (const auto& f : p.fonts)
            for (const char* bad : {"PetMe", "C64 Pro Mono", "C64 Pro", "PrintChar21", "PRNumber3", "AtariClassic"})
                CHECK(f.family.find(bad) == std::string::npos);
    if (failures == 0) std::printf("profiles_test: OK\n");
    return failures ? 1 : 0;
}
