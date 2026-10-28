// SPDX-License-Identifier: GPL-3.0-or-later
// Font fallback (M1 item 6): a profile whose fonts are not installed must end up on an installed
// *monospace* family, never on the proportional sans that fontconfig silently substitutes.
// Runs against private fontconfig configurations (DejaVu only; no monospace at all), so the
// result does not depend on which font packages the build machine has.
#include "font_resolve.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static const char* kDejaVu = "/usr/share/fonts/truetype/dejavu";

static std::string writeConf(const std::string& dir, const std::string& name, const std::string& extra) {
    const std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return {};
    std::fprintf(f,
                 "<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"fonts.dtd\"><fontconfig>\n"
                 "<dir>%s</dir><cachedir>%s/cache-%s</cachedir>\n"
                 "<alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>\n"
                 "<alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>\n"
                 "%s</fontconfig>\n",
                 kDejaVu, dir.c_str(), name.c_str(), extra.c_str());
    std::fclose(f);
    return path;
}

static void useConf(const std::string& path) {
    setenv("FONTCONFIG_FILE", path.c_str(), 1);
    FcInitReinitialize();
    // Pango caches its fontmap; drop it so it sees the new configuration.
    pango_cairo_font_map_set_default(nullptr);
}

// Width of `text` laid out with the given family (what the painter would get from Pango).
static int textWidth(const std::string& family, const char* text) {
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* cr = cairo_create(s);
    PangoLayout* l = pango_cairo_create_layout(cr);
    PangoFontDescription* d = pango_font_description_from_string(family.c_str());
    pango_font_description_set_family(d, family.c_str());
    pango_font_description_set_absolute_size(d, 16 * PANGO_SCALE);
    pango_layout_set_font_description(l, d);
    pango_layout_set_text(l, text, -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    pango_font_description_free(d);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return w;
}

int main() {
    if (access((std::string(kDejaVu) + "/DejaVuSansMono.ttf").c_str(), R_OK) != 0 ||
        access((std::string(kDejaVu) + "/DejaVuSans.ttf").c_str(), R_OK) != 0) {
        std::fprintf(stderr, "SKIP: fonts-dejavu-core not installed\n");
        return 77;
    }
    char tmpl[] = "/tmp/lterm-fonts-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 1;

    // splitFamilies
    {
        auto v = splitFamilies(" A, B ,,C ");
        CHECK(v.size() == 3 && v[0] == "A" && v[1] == "B" && v[2] == "C");
        CHECK(splitFamilies("").empty());
    }

    // --- DejaVu only: none of the retro fonts exist -------------------------------------
    useConf(writeConf(dir, "dejavu.conf", ""));
    CHECK(familyInstalled("DejaVu Sans Mono"));
    CHECK(familyInstalled("dejavu sans mono"));  // case-insensitive, like fontconfig
    CHECK(familyInstalled("Monospace"));         // generic alias
    CHECK(!familyInstalled("Px IBM VGA8"));
    CHECK(!familyInstalled("Terminus (TTF)"));
    CHECK(!familyInstalled("IBM 3270"));
    CHECK(!familyInstalled(""));

    {
        FontPick p = pickFont({{"Px IBM VGA8", 32}, {"Terminus (TTF)", 0}, {"DejaVu Sans Mono", 0}});
        CHECK(p.family == "DejaVu Sans Mono");
        CHECK(p.px == 0);  // the fallback's own size, not the pixel font's 32 px
        CHECK(p.fallback);
        CHECK(p.monospace);
        FontPick q = pickFont("No Such Font, Neither This");
        CHECK(q.family == "Monospace" && q.fallback && q.monospace);
        FontPick r = pickFont("DejaVu Sans Mono, Monospace", 14);
        CHECK(r.family == "DejaVu Sans Mono" && !r.fallback && r.px == 14);
    }

    // What raw Pango does with a missing family: proportional substitution (the bug we avoid).
    {
        const int narrow = textWidth("Px IBM VGA8", "iiiiiiii"), wide = textWidth("Px IBM VGA8", "MMMMMMMM");
        CHECK(narrow > 0 && wide > narrow);
        const FontPick p = pickFont("Px IBM VGA8, Terminus, Monospace");
        CHECK(textWidth(p.family, "iiiiiiii") == textWidth(p.family, "MMMMMMMM"));
    }

    // Every built-in profile resolves to an installed monospace family with a sane cell.
    for (const auto& prof : builtinProfiles()) {
        CHECK(!prof.fonts.empty());
        const FontPick p = profileFont(prof);
        CHECK(familyInstalled(p.family));
        CHECK(p.monospace);
        CHECK(p.px > 0);
        GridPainter gp(p.family, p.px);
        CHECK(gp.cellWidth() > 0 && gp.cellHeight() > gp.cellWidth() / 2);
        CHECK(gp.cellWidth() < p.px * 2);
    }
    {
        const Profile* dos = findProfile("ibm-dos");
        CHECK(dos && !dos->fonts.empty() && dos->fonts.front().family == "Px IBM VGA8" && dos->fonts.front().px == 32);
        const Profile* m3278 = findProfile("ibm-3278");
        CHECK(m3278 && !m3278->fonts.empty() && m3278->fonts.front().family == "IBM 3270");
        CHECK(profileFont(*dos).fallback);
    }

    // --- No monospace font at all: the pick is flagged so the terminal can warn -------------
    useConf(writeConf(dir, "nomono.conf",
                      std::string("<selectfont><rejectfont><glob>") + kDejaVu +
                          "/DejaVuSansMono*</glob></rejectfont></selectfont>\n"));
    CHECK(!familyInstalled("DejaVu Sans Mono"));
    {
        const FontPick p = profileFont(*findProfile("amber"));
        CHECK(p.family == "Monospace");
        CHECK(p.fallback);
        CHECK(!p.monospace);
    }

    std::string cmd = std::string("rm -rf '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "note: could not remove %s\n", dir);
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("fonts: all checks passed\n");
    return failures ? 1 : 0;
}
