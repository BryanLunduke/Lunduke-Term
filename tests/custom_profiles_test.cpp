// SPDX-License-Identifier: GPL-3.0-or-later
// M6 item 4: custom profiles on disk. Round trip of every option, corrupt files (skipped, kept,
// no crash), rename / delete, name validation, built-ins read-only, atomic writes.
#include "custom_profiles.hpp"

#include <glib.h>
#include <glib/gstdio.h>

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

static void writeFile(const std::string& path, const std::string& s) {
    g_file_set_contents(path.c_str(), s.data(), static_cast<gssize>(s.size()), nullptr);
}

static bool exists(const std::string& p) { return g_file_test(p.c_str(), G_FILE_TEST_EXISTS); }

static bool same(const Rgb& a, const Rgb& b) {
    // Colours are stored as #rrggbb: equal to 1/510.
    return std::abs(a.r - b.r) < 0.002f && std::abs(a.g - b.g) < 0.002f && std::abs(a.b - b.b) < 0.002f;
}

// Every field of a Profile that the file carries.
static bool sameProfile(const Profile& a, const Profile& b) {
    const EffectParams &x = a.fx, &y = b.fx;
    bool fontsEq = a.fonts.size() == b.fonts.size();
    for (size_t i = 0; fontsEq && i < a.fonts.size(); ++i)
        fontsEq = a.fonts[i].family == b.fonts[i].family && a.fonts[i].px == b.fonts[i].px;
    return a.effects == b.effects && a.fg == b.fg && a.bg == b.bg && a.cursor == b.cursor && a.palette == b.palette &&
           a.minFgGrey == b.minFgGrey && a.hasEffectsPalette == b.hasEffectsPalette &&
           a.effectsPalette == b.effectsPalette && a.effectsMinFgGrey == b.effectsMinFgGrey && fontsEq &&
           a.fontFamily == b.fontFamily && a.fontPx == b.fontPx && a.marginFrac == b.marginFrac &&
           a.marginMinPx == b.marginMinPx && a.bezelOption == b.bezelOption && same(x.fontColor, y.fontColor) &&
           same(x.backgroundColor, y.backgroundColor) && x.bloom == y.bloom && x.burnIn == y.burnIn &&
           x.chromaColor == y.chromaColor && x.brightness == y.brightness && x.ambientLight == y.ambientLight &&
           x.flickering == y.flickering && x.glowingLine == y.glowingLine && x.horizontalSync == y.horizontalSync &&
           x.jitter == y.jitter && x.screenCurvature == y.screenCurvature && x.staticNoise == y.staticNoise &&
           x.rasterization == y.rasterization && x.scanlinePeriod == y.scanlinePeriod &&
           x.rasterizationIntensity == y.rasterizationIntensity && x.bloomRadiusHalfRes == y.bloomRadiusHalfRes &&
           x.burnInHalfRes == y.burnInHalfRes && x.rgbShift == y.rgbShift && x.frame == y.frame &&
           x.frameSize == y.frameSize && same(x.frameColor, y.frameColor) && x.frameShininess == y.frameShininess;
}

int main() {
    const std::string home = "/tmp/lterm-custom-profiles-test-" + std::to_string(getpid());
    g_setenv("XDG_CONFIG_HOME", home.c_str(), TRUE);
    const std::string dir = customProfilesDir();
    CHECK(dir == home + "/lunduke-terminal/profiles");
    CHECK(customProfiles().empty());
    CHECK(loadCustomProfiles(dir).empty());  // no directory yet: no profiles, no crash

    // ---- serialization round trip: every built-in, then a profile with every value changed ----
    for (const auto& b : builtinProfiles()) {
        Profile in;
        CHECK(profileFromIni(profileToIni(b), "copy", in, nullptr));
        if (!sameProfile(b, in)) std::fprintf(stderr, "round trip differs: %s\n", b.id.c_str());
        CHECK(sameProfile(b, in));
        CHECK(in.id == "custom:copy" && in.label == "copy");
    }
    Profile p = *findProfile("amber");
    p.id = customProfileId("My Tube");
    p.effects = false;
    p.fg = Color8{1, 2, 3};
    p.bg = Color8{250, 251, 252};
    p.cursor = Color8{9, 8, 7};
    for (int i = 0; i < 16; ++i) {
        p.palette[i] = Color8{static_cast<uint8_t>(i * 10), static_cast<uint8_t>(i), static_cast<uint8_t>(255 - i)};
        p.effectsPalette[i] = Color8{static_cast<uint8_t>(i), static_cast<uint8_t>(i * 3), 7};
    }
    p.minFgGrey = 0.125f;
    p.hasEffectsPalette = true;
    p.effectsMinFgGrey = 0.375f;
    p.fonts = {{"Terminus (TTF)", 24}, {"DejaVu Sans Mono", 0}};
    p.fontFamily = "Terminus (TTF), DejaVu Sans Mono";
    p.fontPx = 19;
    p.marginFrac = 0.0625f;
    p.marginMinPx = 7;
    p.bezelOption = false;
    EffectParams& x = p.fx;
    x.fontColor = Rgb{0.2f, 0.4f, 0.6f};
    x.backgroundColor = Rgb{0.1f, 0.0f, 0.3f};
    x.bloom = 0.11f;
    x.burnIn = 0.0f;
    x.chromaColor = 0.33f;
    x.brightness = 0.44f;
    x.ambientLight = 0.0f;
    x.flickering = 0.66f;
    x.glowingLine = 0.77f;
    x.horizontalSync = 0.0f;
    x.jitter = 0.99f;
    x.screenCurvature = 0.05f;
    x.staticNoise = 0.0f;
    x.rasterization = 1;
    x.scanlinePeriod = 4.5f;
    x.rasterizationIntensity = 0.25f;
    x.bloomRadiusHalfRes = 9;
    x.burnInHalfRes = false;
    x.rgbShift = 0.3f;
    x.frame = true;
    x.frameSize = 40.0f;
    x.frameColor = Rgb{0.5f, 0.25f, 0.75f};
    x.frameShininess = 0.9f;
    {
        Profile in;
        std::string err;
        CHECK(profileFromIni(profileToIni(p), "My Tube", in, &err));
        CHECK(sameProfile(p, in));
        std::printf("round trip: %zu built-ins and one profile with every option changed\n", builtinProfiles().size());
    }

    // ---- name validation ----
    {
        std::string err;
        for (const char* bad : {"", " ", "   ", "a/b", "a\\b", "..", ".hidden", "x\ny", "tab\there", " lead", "trail ",
                                "plain", "Amber", "AMBER", "Plain (no effects)", "green-scanlines"}) {
            err.clear();
            const bool ok = validateProfileName(bad, &err);
            if (ok) std::fprintf(stderr, "accepted bad name '%s'\n", bad);
            CHECK(!ok && !err.empty());
        }
        CHECK(!validateProfileName(std::string(65, 'a'), &err));
        CHECK(validateProfileName(std::string(64, 'a'), &err));
        for (const char* good : {"My Tube", "amber 2", "Grün", "x:y", "a.b", "Amber (mine)"}) CHECK(validateProfileName(good, &err));
    }

    // ---- save, load (a restart), duplicates ----
    std::string err;
    CHECK(saveCustomProfile(dir, p, &err));
    const std::string file = dir + "/My Tube.ini";
    CHECK(exists(file));
    CHECK(findProfile("custom:My Tube") && findProfile("custom:My Tube")->label == "My Tube");
    CHECK(!validateProfileName("my tube", &err));            // duplicate, case-insensitive
    CHECK(validateProfileName("my tube", &err, p.id));       // its own name when renaming
    {
        Profile dup = p;
        dup.id = customProfileId("MY TUBE");
        CHECK(!saveCustomProfile(dir, dup, &err));
    }
    // Atomic write: no temporary files are left next to it.
    {
        int n = 0;
        GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
        while (d && g_dir_read_name(d)) ++n;
        if (d) g_dir_close(d);
        CHECK(n == 1);
    }
    setCustomProfiles({});  // as after a restart
    CHECK(!findProfile("custom:My Tube"));
    reloadCustomProfiles();
    CHECK(customProfiles().size() == 1);
    CHECK(findProfile("custom:My Tube") && sameProfile(*findProfile("custom:My Tube"), p));
    // Editing: save again replaces the file and the registry entry.
    {
        Profile e = *findProfile("custom:My Tube");
        e.fx.bloom = 0.5f;
        CHECK(saveCustomProfile(dir, e, &err));
        CHECK(customProfiles().size() == 1 && findProfile("custom:My Tube")->fx.bloom == 0.5f);
        setCustomProfiles({});
        reloadCustomProfiles();
        CHECK(findProfile("custom:My Tube") && findProfile("custom:My Tube")->fx.bloom == 0.5f);
    }

    // ---- built-ins are read-only ----
    {
        Profile b = *findProfile("amber");
        CHECK(!saveCustomProfile(dir, b, &err) && err.find("read-only") != std::string::npos);
        CHECK(!renameCustomProfile(dir, "amber", "x", &err));
        CHECK(!deleteCustomProfile(dir, "plain", &err));
        CHECK(findProfile("amber") && findProfile("plain"));
        CHECK(!exists(dir + "/amber.ini"));
        // A file named like a built-in does not shadow it.
        writeFile(dir + "/amber.ini", profileToIni(p));
        std::vector<std::string> w;
        const auto v = loadCustomProfiles(dir, &w);
        CHECK(v.size() == 1 && w.size() == 1);
        CHECK(findProfile("amber")->effects);
        g_unlink((dir + "/amber.ini").c_str());
        // Copy to custom profile: a built-in copied under a new name is a normal custom profile.
        b.id = customProfileId("Amber copy");
        CHECK(saveCustomProfile(dir, b, &err));
        CHECK(findProfile("custom:Amber copy") && findProfile("custom:Amber copy")->label == "Amber copy");
        CHECK(findProfile("amber")->label == "Amber");
    }

    // ---- rename / delete ----
    CHECK(renameCustomProfile(dir, "custom:Amber copy", "Warm", &err));
    CHECK(!exists(dir + "/Amber copy.ini") && exists(dir + "/Warm.ini"));
    CHECK(!findProfile("custom:Amber copy") && findProfile("custom:Warm") && findProfile("custom:Warm")->label == "Warm");
    CHECK(!renameCustomProfile(dir, "custom:Warm", "My Tube", &err));  // taken
    CHECK(!renameCustomProfile(dir, "custom:Warm", "a/b", &err));
    CHECK(!renameCustomProfile(dir, "custom:Nope", "Other", &err));
    CHECK(renameCustomProfile(dir, "custom:Warm", "WARM", &err));  // case only
    CHECK(findProfile("custom:WARM") && exists(dir + "/WARM.ini"));
    setCustomProfiles({});
    reloadCustomProfiles();
    CHECK(customProfiles().size() == 2 && findProfile("custom:WARM") && findProfile("custom:My Tube"));
    CHECK(deleteCustomProfile(dir, "custom:WARM", &err));
    CHECK(!exists(dir + "/WARM.ini") && !findProfile("custom:WARM"));
    CHECK(!deleteCustomProfile(dir, "custom:WARM", &err));
    setCustomProfiles({});
    reloadCustomProfiles();
    CHECK(customProfiles().size() == 1);

    // ---- corrupt files: skipped with a warning, left on disk, others still load ----
    {
        const std::string bad1 = dir + "/Broken.ini", bad2 = dir + "/BadColour.ini", bad3 = dir + "/NoGroup.ini",
                          bad4 = dir + "/Binary.ini", bad5 = dir + "/BadNumber.ini";
        writeFile(bad1, "[profile\nthis is not a key file\n");
        writeFile(bad2, "[profile]\nforeground=#zzzzzz\n");
        writeFile(bad3, "[something]\na=1\n");
        writeFile(bad4, std::string("\0\xff\xfe[\x01", 5));
        writeFile(bad5, "[effects]\nbloom=lots\n[profile]\n");
        writeFile(dir + "/notes.txt", "not a profile");
        std::vector<std::string> w;
        setCustomProfiles({});
        reloadCustomProfiles(&w);
        for (const auto& s : w) std::printf("corrupt: %s\n", s.c_str());
        CHECK(w.size() == 5);
        CHECK(customProfiles().size() == 1 && findProfile("custom:My Tube"));
        for (const auto& f : {bad1, bad2, bad3, bad4, bad5}) CHECK(exists(f));  // kept for the user
        // Out-of-range numbers are clamped, a partial file keeps plain's other values.
        writeFile(dir + "/Partial.ini", "[profile]\nfont-px=500\n[effects]\nbloom=7\njitter=-1\n");
        setCustomProfiles({});
        reloadCustomProfiles();
        const Profile* q = findProfile("custom:Partial");
        CHECK(q && q->fontPx == 96 && q->fx.bloom == 1.0f && q->fx.jitter == 0.0f);
        CHECK(q && q->palette == findProfile("plain")->palette && !q->effects);
        for (const auto& f : {bad1, bad2, bad3, bad4, bad5}) g_unlink(f.c_str());
        g_unlink((dir + "/notes.txt").c_str());
        g_unlink((dir + "/Partial.ini").c_str());
    }
    // ---- PR #7 review: a file skipped as broken is never written over (New, Copy, Rename, Save) ----
    {
        auto read = [](const std::string& path) {
            char* d = nullptr;
            gsize n = 0;
            std::string s = g_file_get_contents(path.c_str(), &d, &n, nullptr) ? std::string(d, n) : std::string("<none>");
            g_free(d);
            return s;
        };
        const std::string badColor = dir + "/badcolor.ini", broken = dir + "/broken.ini";
        const std::string badColorText = "[profile]\nforeground=#nothex\n# keep me\n";
        const std::string brokenText = "[profile\nnot a key file, hand-edited\n";
        writeFile(badColor, badColorText);
        writeFile(broken, brokenText);
        std::vector<std::string> w;
        setCustomProfiles({});
        reloadCustomProfiles(&w);
        CHECK(w.size() == 2);
        CHECK(!findProfile("custom:badcolor") && !findProfile("custom:broken"));
        CHECK(unloadedProfileFile(dir, "badcolor") == "badcolor.ini");
        CHECK(unloadedProfileFile(dir, "BROKEN") == "broken.ini");  // case-insensitive
        CHECK(unloadedProfileFile(dir, "My Tube").empty());         // loaded: no conflict
        CHECK(unloadedProfileFile("", "badcolor").empty());          // no dir: no disk check
        // The name dialog's check (with the directory) explains why.
        CHECK(!validateProfileName("badcolor", &err, "", dir) && err.find("badcolor.ini") != std::string::npos &&
              err.find("could not be loaded") != std::string::npos);
        CHECK(!validateProfileName("Broken", &err, "", dir));
        // New / Copy: saveCustomProfile of a new custom id onto the skipped file's name.
        Profile n = *findProfile("amber");
        n.id = customProfileId("badcolor");
        err.clear();
        CHECK(!saveCustomProfile(dir, n, &err) && err.find("could not be loaded") != std::string::npos);
        n.id = customProfileId("BadColor");  // a case variant would leave two files that clash
        CHECK(!saveCustomProfile(dir, n, &err));
        // Rename a loaded profile onto it.
        CHECK(!renameCustomProfile(dir, "custom:My Tube", "broken", &err) && err.find("broken.ini") != std::string::npos);
        CHECK(findProfile("custom:My Tube") && exists(dir + "/My Tube.ini"));
        // Byte-identical afterwards, and still the only files with those names.
        CHECK(read(badColor) == badColorText);
        CHECK(read(broken) == brokenText);
        CHECK(!exists(dir + "/BadColor.ini"));
        CHECK(!findProfile("custom:badcolor") && !findProfile("custom:BadColor") && !findProfile("custom:broken"));
        // Once the user removes the broken file, the name is free again.
        g_unlink(broken.c_str());
        CHECK(renameCustomProfile(dir, "custom:My Tube", "broken", &err));
        CHECK(renameCustomProfile(dir, "custom:broken", "My Tube", &err));
        g_unlink(badColor.c_str());
        CHECK(validateProfileName("badcolor", &err, "", dir));
    }
    // A directory that cannot be written: save fails with a message, nothing crashes.
    {
        Profile r = p;
        r.id = customProfileId("Elsewhere");
        CHECK(!saveCustomProfile("/proc/lterm-no-such-dir", r, &err) && !err.empty());
        CHECK(!findProfile("custom:Elsewhere"));
    }

    CHECK(deleteCustomProfile(dir, "custom:My Tube", &err));
    g_rmdir(dir.c_str());
    g_rmdir((home + "/lunduke-terminal").c_str());
    g_rmdir(home.c_str());
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("custom profiles: all passed\n");
    return failures ? 1 : 0;
}
