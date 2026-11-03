// SPDX-License-Identifier: GPL-3.0-or-later
#include "custom_profiles.hpp"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace lterm {

namespace {

const char* kGP = "profile";
const char* kGE = "effects";

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hexOf(Color8 c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}

std::string hexOf(Rgb c) {
    auto u = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return hexOf(Color8{u(c.r), u(c.g), u(c.b)});
}

bool parseColor8(const std::string& s, Color8& out) {
    Rgb r;
    if (!parseHexColor(s, r)) return false;
    out = Color8{static_cast<uint8_t>(std::lround(r.r * 255)), static_cast<uint8_t>(std::lround(r.g * 255)),
                 static_cast<uint8_t>(std::lround(r.b * 255))};
    return true;
}

struct KeyFile {
    GKeyFile* kf = g_key_file_new();
    ~KeyFile() { g_key_file_free(kf); }
};

std::string paletteString(const std::array<Color8, 16>& p) {
    std::string s;
    for (size_t i = 0; i < p.size(); ++i) s += (i ? ";" : "") + hexOf(p[i]);
    return s;
}

// Reader that turns any bad value into an error (the file is then corrupt).
struct Reader {
    GKeyFile* kf;
    std::string err;
    bool has(const char* g, const char* k) const { return g_key_file_has_key(kf, g, k, nullptr); }
    std::string str(const char* g, const char* k) const {
        char* v = g_key_file_get_string(kf, g, k, nullptr);
        std::string s = v ? v : "";
        g_free(v);
        return s;
    }
    void boolean(const char* g, const char* k, bool& out) {
        if (!has(g, k)) return;
        GError* e = nullptr;
        const gboolean v = g_key_file_get_boolean(kf, g, k, &e);
        if (e) {
            fail(g, k, e->message);
            g_error_free(e);
            return;
        }
        out = v;
    }
    void number(const char* g, const char* k, float& out, float lo, float hi) {
        if (!has(g, k)) return;
        GError* e = nullptr;
        const double v = g_key_file_get_double(kf, g, k, &e);
        if (e) {
            fail(g, k, e->message);
            g_error_free(e);
            return;
        }
        if (!std::isfinite(v)) return fail(g, k, "not a number");
        out = static_cast<float>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
    }
    void integer(const char* g, const char* k, int& out, int lo, int hi) {
        if (!has(g, k)) return;
        GError* e = nullptr;
        const gint64 v = g_key_file_get_int64(kf, g, k, &e);
        if (e) {
            fail(g, k, e->message);
            g_error_free(e);
            return;
        }
        out = static_cast<int>(std::clamp<gint64>(v, lo, hi));
    }
    void color(const char* g, const char* k, Color8& out) {
        if (!has(g, k)) return;
        if (!parseColor8(str(g, k), out)) fail(g, k, "bad colour (want #rrggbb)");
    }
    void color(const char* g, const char* k, Rgb& out) {
        if (!has(g, k)) return;
        if (!parseHexColor(str(g, k), out)) fail(g, k, "bad colour (want #rrggbb)");
    }
    void palette(const char* g, const char* k, std::array<Color8, 16>& out) {
        if (!has(g, k)) return;
        gsize n = 0;
        char** v = g_key_file_get_string_list(kf, g, k, &n, nullptr);
        if (!v || n != 16) {
            g_strfreev(v);
            return fail(g, k, "want 16 colours");
        }
        for (gsize i = 0; i < n; ++i)
            if (!parseColor8(v[i], out[i])) {
                g_strfreev(v);
                return fail(g, k, "bad colour (want #rrggbb)");
            }
        g_strfreev(v);
    }
    void fail(const char* g, const char* k, const char* why) {
        if (err.empty()) err = std::string("[") + g + "] " + k + ": " + why;
    }
};

// Base for keys a file does not have.
Profile baseProfile() {
    const Profile* p = findProfile("plain");
    return p ? *p : Profile{};
}

std::string profilePath(const std::string& dir, const std::string& name) {
    char* p = g_build_filename(dir.c_str(), (name + ".ini").c_str(), nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

bool ensureDir(const std::string& dir, std::string* err) {
    if (g_mkdir_with_parents(dir.c_str(), 0700) != 0) {
        if (err) *err = dir + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool writeAtomic(const std::string& path, const std::string& data, std::string* err) {
    GError* e = nullptr;
    // g_file_set_contents writes a temporary file in the same directory and renames it over
    // `path`, so a crash leaves either the old or the new file, never a partial one.
    if (!g_file_set_contents(path.c_str(), data.data(), static_cast<gssize>(data.size()), &e)) {
        if (err) *err = e ? e->message : "write failed";
        if (e) g_error_free(e);
        return false;
    }
    return true;
}

}  // namespace

bool isCustomProfileId(const std::string& id) {
    return id.rfind(kCustomProfilePrefix, 0) == 0 && id.size() > std::strlen(kCustomProfilePrefix);
}

std::string customProfileId(const std::string& name) { return kCustomProfilePrefix + name; }

std::string customProfileName(const std::string& id) {
    return isCustomProfileId(id) ? id.substr(std::strlen(kCustomProfilePrefix)) : std::string();
}

std::string customProfilesDir() {
    char* p = g_build_filename(g_get_user_config_dir(), "lunduke-terminal", "profiles", nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

std::string unloadedProfileFile(const std::string& dir, const std::string& name, const std::string& renaming) {
    if (dir.empty()) return {};
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return {};
    const std::string l = lower(name), own = customProfileName(renaming);
    std::string found;
    while (const char* f = g_dir_read_name(d)) {
        const std::string s = f;
        if (s.size() <= 4 || s.compare(s.size() - 4, 4, ".ini") != 0) continue;
        const std::string stem = s.substr(0, s.size() - 4);
        if (lower(stem) != l || (!own.empty() && stem == own)) continue;
        // A loaded profile's own file is no conflict here (the registry check covers it).
        const bool loaded = std::any_of(customProfiles().begin(), customProfiles().end(),
                                        [&](const Profile& c) { return customProfileName(c.id) == stem; });
        if (!loaded) {
            found = s;
            break;
        }
    }
    g_dir_close(d);
    return found;
}

bool validateProfileName(const std::string& name, std::string* err, const std::string& renaming, const std::string& dir) {
    auto bad = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    if (name.empty()) return bad("The name is empty");
    if (name.size() > static_cast<size_t>(kMaxProfileNameLength)) return bad("The name is longer than 64 bytes");
    if (!g_utf8_validate(name.c_str(), static_cast<gssize>(name.size()), nullptr)) return bad("The name is not valid UTF-8");
    bool blank = true;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return bad("The name contains a control character");
        if (c == '/' || c == '\\') return bad("The name contains a path character ('/' or '\\')");
        if (!std::isspace(c)) blank = false;
    }
    if (blank) return bad("The name is empty");
    if (name[0] == '.') return bad("The name starts with '.'");
    if (std::isspace(static_cast<unsigned char>(name.front())) || std::isspace(static_cast<unsigned char>(name.back())))
        return bad("The name starts or ends with a space");
    const std::string l = lower(name);
    for (const auto& p : builtinProfiles())
        if (lower(p.id) == l || lower(p.label) == l) return bad("A built-in profile has this name");
    for (const auto& p : customProfiles())
        if (p.id != renaming && lower(customProfileName(p.id)) == l) return bad("A custom profile has this name");
    // PR #7 review: a file that was skipped as broken (bad colour, not a key file, ...) is not in
    // the registry, but it is on disk: writing NAME.ini would silently replace it.
    const std::string file = unloadedProfileFile(dir, name, renaming);
    if (!file.empty()) {
        if (err) *err = "A profile file named \"" + file + "\" already exists in " + dir +
                        " but could not be loaded; fix or remove it by hand, or pick another name";
        return false;
    }
    return true;
}

std::string profileToIni(const Profile& p) {
    KeyFile k;
    GKeyFile* kf = k.kf;
    auto setF = [&](const char* g, const char* key, double v) { g_key_file_set_double(kf, g, key, v); };
    auto setS = [&](const char* g, const char* key, const std::string& v) { g_key_file_set_string(kf, g, key, v.c_str()); };
    g_key_file_set_comment(kf, nullptr, nullptr, " Lunduke Terminal custom profile (Settings > Profile). The file name is its name.", nullptr);
    g_key_file_set_integer(kf, kGP, "format", 1);
    g_key_file_set_boolean(kf, kGP, "effects", p.effects);
    setS(kGP, "foreground", hexOf(p.fg));
    setS(kGP, "background", hexOf(p.bg));
    setS(kGP, "cursor", hexOf(p.cursor));
    setS(kGP, "palette", paletteString(p.palette));
    setF(kGP, "min-fg-grey", p.minFgGrey);
    g_key_file_set_boolean(kf, kGP, "effects-palette-on", p.hasEffectsPalette);
    setS(kGP, "effects-palette", paletteString(p.effectsPalette));
    setF(kGP, "effects-min-fg-grey", p.effectsMinFgGrey);
    std::string fonts;
    for (const auto& f : p.fonts) fonts += (fonts.empty() ? "" : ";") + f.family + (f.px > 0 ? ":" + std::to_string(f.px) : "");
    setS(kGP, "fonts", fonts);
    g_key_file_set_integer(kf, kGP, "font-px", p.fontPx);
    setF(kGP, "margin-fraction", p.marginFrac);
    g_key_file_set_integer(kf, kGP, "margin-min-px", p.marginMinPx);
    g_key_file_set_boolean(kf, kGP, "bezel-option", p.bezelOption);
    const EffectParams& x = p.fx;
    setS(kGE, "font-color", hexOf(x.fontColor));
    setS(kGE, "background-color", hexOf(x.backgroundColor));
    setF(kGE, "bloom", x.bloom);
    setF(kGE, "burn-in", x.burnIn);
    setF(kGE, "chroma-color", x.chromaColor);
    setF(kGE, "brightness", x.brightness);
    setF(kGE, "ambient-light", x.ambientLight);
    setF(kGE, "flickering", x.flickering);
    setF(kGE, "glowing-line", x.glowingLine);
    setF(kGE, "horizontal-sync", x.horizontalSync);
    setF(kGE, "jitter", x.jitter);
    setF(kGE, "screen-curvature", x.screenCurvature);
    setF(kGE, "static-noise", x.staticNoise);
    g_key_file_set_boolean(kf, kGE, "scanlines", x.rasterization == 1);
    setF(kGE, "scanline-period", x.scanlinePeriod);
    setF(kGE, "scanline-intensity", x.rasterizationIntensity);
    g_key_file_set_integer(kf, kGE, "bloom-radius", x.bloomRadiusHalfRes);
    g_key_file_set_boolean(kf, kGE, "burn-in-half-res", x.burnInHalfRes);
    setF(kGE, "rgb-shift", x.rgbShift);
    g_key_file_set_boolean(kf, kGE, "frame", x.frame);
    setF(kGE, "frame-size", x.frameSize);
    setS(kGE, "frame-color", hexOf(x.frameColor));
    setF(kGE, "frame-shininess", x.frameShininess);
    gsize n = 0;
    char* data = g_key_file_to_data(kf, &n, nullptr);
    std::string s(data ? data : "", n);
    g_free(data);
    return s;
}

bool profileFromIni(const std::string& data, const std::string& name, Profile& out, std::string* err) {
    KeyFile k;
    GError* e = nullptr;
    if (!g_key_file_load_from_data(k.kf, data.data(), data.size(), G_KEY_FILE_NONE, &e)) {
        if (err) *err = e ? e->message : "not a key file";
        if (e) g_error_free(e);
        return false;
    }
    if (!g_key_file_has_group(k.kf, kGP)) {
        if (err) *err = "no [profile] group";
        return false;
    }
    Profile p = baseProfile();
    p.id = customProfileId(name);
    p.label = name;
    Reader r{k.kf, {}};
    r.boolean(kGP, "effects", p.effects);
    r.color(kGP, "foreground", p.fg);
    r.color(kGP, "background", p.bg);
    r.color(kGP, "cursor", p.cursor);
    r.palette(kGP, "palette", p.palette);
    r.number(kGP, "min-fg-grey", p.minFgGrey, 0, 1);
    r.boolean(kGP, "effects-palette-on", p.hasEffectsPalette);
    r.palette(kGP, "effects-palette", p.effectsPalette);
    r.number(kGP, "effects-min-fg-grey", p.effectsMinFgGrey, 0, 1);
    if (r.has(kGP, "fonts")) {
        std::vector<FontChoice> fonts;
        gsize n = 0;
        char** v = g_key_file_get_string_list(k.kf, kGP, "fonts", &n, nullptr);
        for (gsize i = 0; v && i < n; ++i) {
            std::string f = v[i];
            int px = 0;
            const auto colon = f.rfind(':');
            if (colon != std::string::npos) {
                const std::string num = f.substr(colon + 1);
                if (!num.empty() && std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    px = std::clamp(std::atoi(num.c_str()), 0, 96);
                    f = f.substr(0, colon);
                }
            }
            while (!f.empty() && std::isspace(static_cast<unsigned char>(f.back()))) f.pop_back();
            while (!f.empty() && std::isspace(static_cast<unsigned char>(f.front()))) f.erase(f.begin());
            if (!f.empty()) fonts.push_back(FontChoice{f, px});
        }
        g_strfreev(v);
        if (!fonts.empty()) {
            p.fonts = fonts;
            p.fontFamily.clear();
            for (const auto& f : p.fonts) p.fontFamily += (p.fontFamily.empty() ? "" : ", ") + f.family;
        }
    }
    r.integer(kGP, "font-px", p.fontPx, 4, 96);
    r.number(kGP, "margin-fraction", p.marginFrac, 0, 0.3f);
    r.integer(kGP, "margin-min-px", p.marginMinPx, 0, 200);
    r.boolean(kGP, "bezel-option", p.bezelOption);
    EffectParams& x = p.fx;
    r.color(kGE, "font-color", x.fontColor);
    r.color(kGE, "background-color", x.backgroundColor);
    r.number(kGE, "bloom", x.bloom, 0, 1);
    r.number(kGE, "burn-in", x.burnIn, 0, 1);
    r.number(kGE, "chroma-color", x.chromaColor, 0, 1);
    r.number(kGE, "brightness", x.brightness, 0, 1);
    r.number(kGE, "ambient-light", x.ambientLight, 0, 1);
    r.number(kGE, "flickering", x.flickering, 0, 1);
    r.number(kGE, "glowing-line", x.glowingLine, 0, 1);
    r.number(kGE, "horizontal-sync", x.horizontalSync, 0, 1);
    r.number(kGE, "jitter", x.jitter, 0, 1);
    r.number(kGE, "screen-curvature", x.screenCurvature, 0, 1);
    r.number(kGE, "static-noise", x.staticNoise, 0, 1);
    bool scan = x.rasterization == 1;
    r.boolean(kGE, "scanlines", scan);
    x.rasterization = scan ? 1 : 0;
    r.number(kGE, "scanline-period", x.scanlinePeriod, 1, 16);
    r.number(kGE, "scanline-intensity", x.rasterizationIntensity, 0, 1);
    r.integer(kGE, "bloom-radius", x.bloomRadiusHalfRes, 1, 64);
    r.boolean(kGE, "burn-in-half-res", x.burnInHalfRes);
    r.number(kGE, "rgb-shift", x.rgbShift, 0, 1);
    r.boolean(kGE, "frame", x.frame);
    r.number(kGE, "frame-size", x.frameSize, 0, 200);
    r.color(kGE, "frame-color", x.frameColor);
    r.number(kGE, "frame-shininess", x.frameShininess, 0, 1);
    if (!r.err.empty()) {
        if (err) *err = r.err;
        return false;
    }
    out = p;
    return true;
}

std::vector<Profile> loadCustomProfiles(const std::string& dir, std::vector<std::string>* warnings) {
    std::vector<Profile> v;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return v;
    std::vector<std::string> names;
    while (const char* f = g_dir_read_name(d)) {
        const std::string s = f;
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".ini") == 0) names.push_back(s.substr(0, s.size() - 4));
    }
    g_dir_close(d);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const std::string path = profilePath(dir, name);
        std::string why;
        char* data = nullptr;
        gsize n = 0;
        GError* e = nullptr;
        Profile p;
        if (!g_file_get_contents(path.c_str(), &data, &n, &e)) {
            why = e ? e->message : "unreadable";
            if (e) g_error_free(e);
        } else if (!profileFromIni(std::string(data, n), name, p, &why)) {
            // kept on disk as it is: the user may want to fix it by hand
        } else {
            // A file name that is not a valid profile name (e.g. "plain.ini", a leading dot) is skipped.
            std::string nameErr;
            const std::vector<Profile> before = customProfiles();
            setCustomProfiles(v);  // duplicates are checked against the ones loaded so far
            const bool ok = validateProfileName(name, &nameErr);
            setCustomProfiles(before);
            if (ok) v.push_back(p);
            else why = nameErr;
        }
        g_free(data);
        if (!why.empty() && warnings) warnings->push_back(path + ": " + why + " (skipped)");
    }
    return v;
}

void reloadCustomProfiles(std::vector<std::string>* warnings) {
    setCustomProfiles(loadCustomProfiles(customProfilesDir(), warnings));
}

bool saveCustomProfile(const std::string& dir, const Profile& p, std::string* err) {
    const std::string name = customProfileName(p.id);
    if (name.empty()) {
        if (err) *err = "Built-in profiles are read-only (use Copy to custom profile)";
        return false;
    }
    const bool exists = std::any_of(customProfiles().begin(), customProfiles().end(),
                                    [&](const Profile& c) { return c.id == p.id; });
    if (!validateProfileName(name, err, exists ? p.id : "", dir)) return false;
    if (!ensureDir(dir, err) || !writeAtomic(profilePath(dir, name), profileToIni(p), err)) return false;
    std::vector<Profile> v = customProfiles();
    Profile stored = p;
    stored.label = name;
    auto it = std::find_if(v.begin(), v.end(), [&](const Profile& c) { return c.id == p.id; });
    if (it != v.end()) *it = stored;
    else v.push_back(stored);
    std::sort(v.begin(), v.end(), [](const Profile& a, const Profile& b) { return a.id < b.id; });
    setCustomProfiles(std::move(v));
    return true;
}

bool renameCustomProfile(const std::string& dir, const std::string& id, const std::string& newName, std::string* err) {
    std::vector<Profile> v = customProfiles();
    auto it = std::find_if(v.begin(), v.end(), [&](const Profile& c) { return c.id == id; });
    if (!isCustomProfileId(id) || it == v.end()) {
        if (err) *err = isCustomProfileId(id) ? "No such custom profile" : "Built-in profiles are read-only";
        return false;
    }
    if (!validateProfileName(newName, err, id, dir)) return false;
    const std::string from = profilePath(dir, customProfileName(id)), to = profilePath(dir, newName);
    if (from != to) {
        // Write the new file first (atomic), then remove the old one: an interruption leaves
        // both, never neither.
        Profile renamed = *it;
        renamed.id = customProfileId(newName);
        if (!ensureDir(dir, err) || !writeAtomic(to, profileToIni(renamed), err)) return false;
        if (g_unlink(from.c_str()) != 0 && errno != ENOENT) {
            if (err) *err = from + ": " + std::strerror(errno);
            g_unlink(to.c_str());
            return false;
        }
    }
    it->id = customProfileId(newName);
    it->label = newName;
    std::sort(v.begin(), v.end(), [](const Profile& a, const Profile& b) { return a.id < b.id; });
    setCustomProfiles(std::move(v));
    return true;
}

bool deleteCustomProfile(const std::string& dir, const std::string& id, std::string* err) {
    std::vector<Profile> v = customProfiles();
    auto it = std::find_if(v.begin(), v.end(), [&](const Profile& c) { return c.id == id; });
    if (!isCustomProfileId(id) || it == v.end()) {
        if (err) *err = isCustomProfileId(id) ? "No such custom profile" : "Built-in profiles are read-only";
        return false;
    }
    const std::string path = profilePath(dir, customProfileName(id));
    if (g_unlink(path.c_str()) != 0 && errno != ENOENT) {
        if (err) *err = path + ": " + std::strerror(errno);
        return false;
    }
    v.erase(it);
    setCustomProfiles(std::move(v));
    return true;
}

}  // namespace lterm
