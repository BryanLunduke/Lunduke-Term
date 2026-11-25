// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_load.hpp"

#include <glib.h>

#include <cctype>
#include <cstring>
#include <string>

namespace lterm {
namespace {

bool keepLine(const std::string& raw) {
    size_t i = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
    if (i >= raw.size() || raw[i] == '#' || raw[i] == ';') return true;
    if (raw[i] == '[') return raw.find(']', i) != std::string::npos;
    const size_t eq = raw.find('=', i);
    if (eq == std::string::npos || eq == i) return false;
    bool any = false;
    for (size_t k = i; k < eq; ++k) {
        const unsigned char c = static_cast<unsigned char>(raw[k]);
        if (c == ' ' || c == '\t') continue;
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
        any = true;
    }
    return any;
}

std::string filtered(const std::string& text, std::vector<std::string>& warnings) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (keepLine(line)) {
            out += line;
            out += '\n';
        } else {
            warnings.push_back("skipped settings line: " + line);
        }
        i = j + (j < text.size() ? 1 : 0);
        if (j == text.size()) break;
    }
    return out;
}

const char* kGroup = "terminal";

}  // namespace

SettingsValues parseSettingsIni(const std::string& text) {
    SettingsValues o;
    const std::string data = filtered(text, o.warnings);
    GKeyFile* kf = g_key_file_new();
    GError* err = nullptr;
    if (!g_key_file_load_from_data(kf, data.data(), data.size(), G_KEY_FILE_NONE, &err)) {
        if (err) {
            o.warnings.push_back(err->message);
            g_error_free(err);
        }
        g_key_file_free(kf);
        return o;
    }
    auto warn = [&](const char* key, GError* e) {
        o.warnings.push_back(std::string(key) + ": " + (e && e->message ? e->message : "invalid value"));
        if (e) g_error_free(e);
    };
    if (g_key_file_has_key(kf, kGroup, "profile", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "profile", &err)) {
            o.hasProfile = true;
            o.profile = v;
            g_free(v);
        } else warn("profile", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "effects", nullptr)) {
        const gboolean v = g_key_file_get_boolean(kf, kGroup, "effects", &err);
        if (!err) {
            o.hasEffects = true;
            o.effects = v;
        } else warn("effects", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "renderer", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "renderer", &err)) {
            o.hasRenderer = true;
            o.renderer = v;
            g_free(v);
        } else warn("renderer", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "threads", nullptr)) {
        const gint v = g_key_file_get_integer(kf, kGroup, "threads", &err);
        if (!err) {
            o.hasThreads = true;
            o.threads = v;
        } else warn("threads", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "effects-fps", nullptr)) {
        const gint v = g_key_file_get_integer(kf, kGroup, "effects-fps", &err);
        if (!err) {
            o.hasEffectsFps = true;
            o.effectsFps = v;
        } else warn("effects-fps", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "max-fps", nullptr)) {
        const gint v = g_key_file_get_integer(kf, kGroup, "max-fps", &err);
        if (!err) {
            o.hasMaxFps = true;
            o.maxFps = v;
        } else warn("max-fps", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "effects-quality", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "effects-quality", &err)) {
            EffectsQuality q;
            if (parseEffectsQuality(v, q)) {
                o.hasEffectsQuality = true;
                o.effectsQuality = q;
            } else o.warnings.push_back("effects-quality must be auto, full, reduced or static");
            g_free(v);
        } else warn("effects-quality", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "cpu-budget", nullptr)) {
        const gdouble v = g_key_file_get_double(kf, kGroup, "cpu-budget", &err);
        if (!err) {
            o.hasCpuBudget = true;
            o.cpuBudget = v;
        } else warn("cpu-budget", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "pause-unfocused", nullptr)) {
        const gboolean v = g_key_file_get_boolean(kf, kGroup, "pause-unfocused", &err);
        if (!err) {
            o.hasPause = true;
            o.pauseWhenUnfocused = v;
        } else warn("pause-unfocused", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "font", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "font", &err)) {
            o.hasFont = true;
            o.fontFamily = v;
            g_free(v);
        } else warn("font", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "font-px", nullptr)) {
        const gint v = g_key_file_get_integer(kf, kGroup, "font-px", &err);
        if (!err) {
            o.hasFontPx = true;
            o.fontPx = v;
        } else warn("font-px", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "scrollback-lines", nullptr)) {
        const gint64 v = g_key_file_get_int64(kf, kGroup, "scrollback-lines", &err);
        if (!err) {
            o.hasScrollback = true;
            o.scrollbackLines = v;
        } else warn("scrollback-lines", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "login-shell", nullptr)) {
        const gboolean v = g_key_file_get_boolean(kf, kGroup, "login-shell", &err);
        if (!err) {
            o.hasLogin = true;
            o.loginShell = v;
        } else warn("login-shell", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "cwd", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "cwd", &err)) {
            o.hasCwd = true;
            o.cwd = v;
            g_free(v);
        } else warn("cwd", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "on-exit", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "on-exit", &err)) {
            o.hasOnExit = true;
            o.onExit = v;
            g_free(v);
        } else warn("on-exit", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "bell", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "bell", &err)) {
            o.hasBell = true;
            o.bell = v;
            g_free(v);
        } else warn("bell", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "cursor-shape", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "cursor-shape", &err)) {
            o.hasCursorShape = true;
            o.cursorShape = v;
            g_free(v);
        } else warn("cursor-shape", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "cursor-blink", nullptr)) {
        const gboolean v = g_key_file_get_boolean(kf, kGroup, "cursor-blink", &err);
        if (!err) {
            o.hasCursorBlink = true;
            o.cursorBlink = v;
        } else warn("cursor-blink", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "word-chars", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "word-chars", &err)) {
            o.hasWordChars = true;
            o.wordChars = v;
            g_free(v);
        } else warn("word-chars", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "frame", nullptr)) {
        const gboolean v = g_key_file_get_boolean(kf, kGroup, "frame", &err);
        if (!err) {
            o.hasFrame = true;
            o.frame = v ? 1 : 0;
        } else warn("frame", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "frame-size", nullptr)) {
        const gdouble v = g_key_file_get_double(kf, kGroup, "frame-size", &err);
        if (!err) {
            o.hasFrameSize = true;
            o.frameSize = static_cast<float>(v);
        } else warn("frame-size", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "frame-color", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "frame-color", &err)) {
            o.hasFrameColor = true;
            o.frameColor = v;
            g_free(v);
        } else warn("frame-color", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "frame-shininess", nullptr)) {
        const gdouble v = g_key_file_get_double(kf, kGroup, "frame-shininess", &err);
        if (!err) {
            o.hasFrameShininess = true;
            o.frameShininess = static_cast<float>(v);
        } else warn("frame-shininess", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "rgb-shift", nullptr)) {
        const gdouble v = g_key_file_get_double(kf, kGroup, "rgb-shift", &err);
        if (!err) {
            o.hasRgbShift = true;
            o.rgbShift = static_cast<float>(v);
        } else warn("rgb-shift", err);
        err = nullptr;
    }
    if (g_key_file_has_key(kf, kGroup, "scrollbar", nullptr)) {
        if (char* v = g_key_file_get_string(kf, kGroup, "scrollbar", &err)) {
            const std::string s = v;
            g_free(v);
            if (s == "auto" || s == "-1") {
                o.hasScrollbar = true;
                o.scrollbar = -1;
            } else if (s == "on" || s == "true" || s == "1") {
                o.hasScrollbar = true;
                o.scrollbar = 1;
            } else if (s == "off" || s == "false" || s == "0") {
                o.hasScrollbar = true;
                o.scrollbar = 0;
            } else o.warnings.push_back("scrollbar must be auto, on or off");
        } else warn("scrollbar", err);
        err = nullptr;
    }
    // A key this build does not read is kept on disk (Save does not strip unknown
    // keys) but it is not applied, so say so instead of ignoring it.
    if (gchar** keys = g_key_file_get_keys(kf, kGroup, nullptr, nullptr)) {
        static const char* known[] = {
            "profile", "effects", "renderer", "threads", "effects-fps", "max-fps", "effects-quality",
            "cpu-budget", "pause-unfocused", "font", "font-px", "scrollback-lines", "login-shell", "cwd",
            "on-exit", "bell", "cursor-shape", "cursor-blink", "word-chars", "frame", "frame-size",
            "frame-color", "frame-shininess", "rgb-shift", "scrollbar"};
        for (gchar** k = keys; *k; ++k) {
            bool ok = false;
            for (const char* n : known)
                if (std::strcmp(*k, n) == 0) ok = true;
            if (!ok) o.warnings.push_back(std::string("unknown setting ") + *k);
        }
        g_strfreev(keys);
    }
    g_key_file_free(kf);
    return o;
}

}  // namespace lterm
