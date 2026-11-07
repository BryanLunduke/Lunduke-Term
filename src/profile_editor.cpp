// SPDX-License-Identifier: GPL-3.0-or-later
#include "profile_editor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace lterm {

namespace {

Gdk::RGBA rgbaOf(Color8 c) {
    Gdk::RGBA r;
    r.set_rgba(c.r / 255.0, c.g / 255.0, c.b / 255.0, 1.0);
    return r;
}
Gdk::RGBA rgbaOf(Rgb c) {
    Gdk::RGBA r;
    r.set_rgba(c.r, c.g, c.b, 1.0);
    return r;
}
uint8_t byteOf(double v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }
Color8 color8Of(const Gtk::ColorButton& b) {
    const Gdk::RGBA c = b.get_rgba();
    return Color8{byteOf(c.get_red()), byteOf(c.get_green()), byteOf(c.get_blue())};
}
Rgb rgbOf(const Gtk::ColorButton& b) {
    // Through 8 bits, like the profile file (#rrggbb).
    const Color8 c = color8Of(b);
    return Rgb{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
}

void setupScale(Gtk::Scale& s, double lo, double hi, double v) {
    s.set_range(lo, hi);
    s.set_increments(0.01, 0.1);
    s.set_digits(2);
    s.set_value(v);
    s.set_value_pos(Gtk::POS_RIGHT);
    s.set_hexpand(true);
    s.set_size_request(200, -1);
}

void setupSpin(Gtk::SpinButton& s, double lo, double hi, double step, int digits, double v) {
    s.set_range(lo, hi);
    s.set_increments(step, step * 10);
    s.set_digits(static_cast<unsigned>(digits));
    s.set_value(v);
}

struct ToggleSpec {
    const char* key;
    const char* label;
    float EffectParams::*field;
};
const ToggleSpec kToggles[] = {
    {"bloom", "Bloom", &EffectParams::bloom},
    {"burn-in", "Burn-in", &EffectParams::burnIn},
    {"chroma-color", "Chroma color", &EffectParams::chromaColor},
    {"ambient-light", "Ambient light", &EffectParams::ambientLight},
    {"flickering", "Flickering", &EffectParams::flickering},
    {"glowing-line", "Glowing line", &EffectParams::glowingLine},
    {"horizontal-sync", "Horizontal sync", &EffectParams::horizontalSync},
    {"jitter", "Jitter", &EffectParams::jitter},
    {"screen-curvature", "Screen curvature", &EffectParams::screenCurvature},
    {"static-noise", "Static noise", &EffectParams::staticNoise},
    {"rgb-shift", "RGB shift", &EffectParams::rgbShift},
};

Gtk::Grid* newGrid() {
    auto* g = Gtk::manage(new Gtk::Grid);
    g->set_row_spacing(6);
    g->set_column_spacing(12);
    g->set_border_width(12);
    return g;
}

void addRow(Gtk::Grid& g, int& row, const char* text, Gtk::Widget& w) {
    auto* l = Gtk::manage(new Gtk::Label(text));
    l->set_halign(Gtk::ALIGN_END);
    g.attach(*l, 0, row, 1, 1);
    g.attach(w, 1, row, 1, 1);
    ++row;
}

std::string fontsText(const Profile& p) {
    std::string s;
    for (const auto& f : p.fonts) s += (s.empty() ? "" : "; ") + f.family + (f.px > 0 ? ":" + std::to_string(f.px) : "");
    return s;
}

}  // namespace

ProfileEditor::ProfileEditor(Gtk::Window& parent, const Profile& p)
    : Gtk::Dialog("Edit profile: " + p.label, parent, true), base_(p) {
    add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    add_button("_Save", Gtk::RESPONSE_OK);
    set_default_response(Gtk::RESPONSE_OK);
    signal_response().connect([this](int r) {
        if (r == Gtk::RESPONSE_OK && onSave) onSave(result());
        hide();
    });
    const EffectParams& x = p.fx;
    const EffectParams defaults;

    // ---- Text & colors ----
    {
        Gtk::Grid* g = newGrid();
        int row = 0;
        fonts.set_text(fontsText(p));
        fonts.set_hexpand(true);
        fonts.set_tooltip_text("Font families in order, separated by ';'. The first installed one is used. "
                               "\"Family:24\" gives that family its own size (pixel fonts).");
        addRow(*g, row, "Fonts", fonts);
        setupSpin(fontPx, 4, 96, 1, 0, p.fontPx);
        addRow(*g, row, "Font size (px)", fontPx);
        fg.set_rgba(rgbaOf(p.fg));
        bg.set_rgba(rgbaOf(p.bg));
        cursor.set_rgba(rgbaOf(p.cursor));
        addRow(*g, row, "Text color", fg);
        addRow(*g, row, "Background", bg);
        addRow(*g, row, "Cursor", cursor);
        setupSpin(marginFrac, 0, 0.3, 0.01, 2, p.marginFrac);
        marginFrac.set_tooltip_text("Fraction of the window kept free around the text");
        addRow(*g, row, "Margin (fraction)", marginFrac);
        setupSpin(marginMinPx, 0, 200, 1, 0, p.marginMinPx);
        addRow(*g, row, "Margin at least (px)", marginMinPx);
        pages.append_page(*g, "Text");
    }
    // ---- Palette ----
    {
        Gtk::Grid* g = newGrid();
        int row = 0;
        auto addPalette = [&](std::vector<std::unique_ptr<Gtk::ColorButton>>& v, const std::array<Color8, 16>& pal) {
            auto* box = Gtk::manage(new Gtk::Grid);
            box->set_row_spacing(2);
            box->set_column_spacing(2);
            for (int i = 0; i < 16; ++i) {
                v.push_back(std::make_unique<Gtk::ColorButton>(rgbaOf(pal[static_cast<size_t>(i)])));
                v.back()->set_tooltip_text("ANSI color " + std::to_string(i));
                box->attach(*v.back(), i % 8, i / 8, 1, 1);
            }
            return box;
        };
        addRow(*g, row, "ANSI colors 0-15", *addPalette(palette, p.palette));
        setupScale(minFgGrey, 0, 1, p.minFgGrey);
        minFgGrey.set_tooltip_text("Other colors (256-color, truecolor) are lifted to at least this grey; 0 = untouched");
        addRow(*g, row, "Minimum text grey", minFgGrey);
        hasEffectsPalette.set_active(p.hasEffectsPalette);
        g->attach(hasEffectsPalette, 1, row++, 1, 1);
        addRow(*g, row, "With effects", *addPalette(effectsPalette, p.effectsPalette));
        setupScale(effectsMinFgGrey, 0, 1, p.effectsMinFgGrey);
        addRow(*g, row, "Minimum grey (effects)", effectsMinFgGrey);
        pages.append_page(*g, "Palette");
    }
    // ---- Effects ----
    {
        Gtk::Grid* g = newGrid();
        int row = 0;
        effects.set_active(p.effects);
        g->attach(effects, 0, row++, 2, 1);
        setupScale(brightness, 0, 1, x.brightness);
        addRow(*g, row, "Brightness", brightness);
        phosphor.set_rgba(rgbaOf(x.fontColor));
        addRow(*g, row, "Phosphor color", phosphor);
        effectsBackground.set_rgba(rgbaOf(x.backgroundColor));
        addRow(*g, row, "Screen color", effectsBackground);
        for (const auto& s : kToggles) {
            auto t = std::make_unique<Toggle>();
            const float v = x.*(s.field);
            t->on.set_label(s.label);
            t->on.set_active(v > 0);
            // Off: the slider keeps a useful value to switch back on to.
            setupScale(t->value, 0, 1, v > 0 ? v : (defaults.*(s.field) > 0 ? defaults.*(s.field) : 0.5));
            t->on.signal_toggled().connect(sigc::mem_fun(*this, &ProfileEditor::updateSensitivity));
            g->attach(t->on, 0, row, 1, 1);
            g->attach(t->value, 1, row, 1, 1);
            ++row;
            toggles[s.key] = std::move(t);
        }
        scanlines.set_active(x.rasterization == 1);
        scanlines.signal_toggled().connect(sigc::mem_fun(*this, &ProfileEditor::updateSensitivity));
        g->attach(scanlines, 0, row++, 2, 1);
        setupSpin(scanlinePeriod, 1, 16, 0.5, 1, x.scanlinePeriod);
        addRow(*g, row, "Scanline period (px)", scanlinePeriod);
        setupScale(scanlineIntensity, 0, 1, x.rasterizationIntensity);
        addRow(*g, row, "Scanline intensity", scanlineIntensity);
        setupSpin(bloomRadius, 1, 64, 1, 0, x.bloomRadiusHalfRes);
        addRow(*g, row, "Bloom radius", bloomRadius);
        burnInHalfRes.set_active(x.burnInHalfRes);
        g->attach(burnInHalfRes, 0, row++, 2, 1);
        auto* sw = Gtk::manage(new Gtk::ScrolledWindow);
        sw->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
        sw->set_min_content_height(360);
        sw->add(*g);
        pages.append_page(*sw, "Effects");
    }
    // ---- Bezel ----
    {
        Gtk::Grid* g = newGrid();
        int row = 0;
        bezelOption.set_active(p.bezelOption);
            g->attach(bezelOption, 0, row++, 2, 1);
        frame.set_active(x.frame);
        g->attach(frame, 0, row++, 2, 1);
        setupSpin(frameSize, 0, 200, 1, 0, x.frameSize);
        addRow(*g, row, "Size (px)", frameSize);
        frameColor.set_rgba(rgbaOf(x.frameColor));
        addRow(*g, row, "Color", frameColor);
        setupScale(frameShininess, 0, 1, x.frameShininess);
        addRow(*g, row, "Shininess", frameShininess);
        pages.append_page(*g, "Bezel");
    }
    hasEffectsPalette.signal_toggled().connect(sigc::mem_fun(*this, &ProfileEditor::updateSensitivity));
    get_content_area()->pack_start(pages, true, true);
    updateSensitivity();
    get_content_area()->show_all();
}

void ProfileEditor::updateSensitivity() {
    for (auto& [k, t] : toggles) t->value.set_sensitive(t->on.get_active());
    scanlinePeriod.set_sensitive(scanlines.get_active());
    scanlineIntensity.set_sensitive(scanlines.get_active());
    for (auto& b : effectsPalette) b->set_sensitive(hasEffectsPalette.get_active());
    effectsMinFgGrey.set_sensitive(hasEffectsPalette.get_active());
}

Profile ProfileEditor::result() const {
    Profile p = base_;
    EffectParams& x = p.fx;
    p.effects = effects.get_active();
    x.brightness = static_cast<float>(brightness.get_value());
    x.fontColor = rgbOf(phosphor);
    x.backgroundColor = rgbOf(effectsBackground);
    for (const auto& s : kToggles) {
        const Toggle& t = *toggles.at(s.key);
        x.*(s.field) = t.on.get_active() ? static_cast<float>(t.value.get_value()) : 0.0f;
    }
    x.rasterization = scanlines.get_active() ? 1 : 0;
    x.scanlinePeriod = static_cast<float>(scanlinePeriod.get_value());
    x.rasterizationIntensity = static_cast<float>(scanlineIntensity.get_value());
    x.bloomRadiusHalfRes = bloomRadius.get_value_as_int();
    x.burnInHalfRes = burnInHalfRes.get_active();
    std::vector<FontChoice> f;
    std::vector<std::string> items;
    {
        const std::string t = fonts.get_text();
        size_t a = 0;
        while (a <= t.size()) {
            const size_t b = std::min(t.find(';', a), t.size());
            items.push_back(t.substr(a, b - a));
            a = b + 1;
        }
    }
    for (auto& item : items) {
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
        int px = 0;
        const auto colon = item.rfind(':');
        if (colon != std::string::npos && colon + 1 < item.size() &&
            std::all_of(item.begin() + static_cast<long>(colon) + 1, item.end(), [](unsigned char c) { return std::isdigit(c); })) {
            px = std::clamp(std::atoi(item.c_str() + colon + 1), 0, 96);
            item = item.substr(0, colon);
        }
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
        if (!item.empty()) f.push_back(FontChoice{item, px});
    }
    if (!f.empty()) {
        p.fonts = f;
        p.fontFamily.clear();
        for (const auto& c : p.fonts) p.fontFamily += (p.fontFamily.empty() ? "" : ", ") + c.family;
    }
    p.fontPx = fontPx.get_value_as_int();
    p.fg = color8Of(fg);
    p.bg = color8Of(bg);
    p.cursor = color8Of(cursor);
    p.marginFrac = static_cast<float>(marginFrac.get_value());
    p.marginMinPx = marginMinPx.get_value_as_int();
    for (size_t i = 0; i < 16; ++i) {
        p.palette[i] = color8Of(*palette[i]);
        p.effectsPalette[i] = color8Of(*effectsPalette[i]);
    }
    p.minFgGrey = static_cast<float>(minFgGrey.get_value());
    p.hasEffectsPalette = hasEffectsPalette.get_active();
    p.effectsMinFgGrey = static_cast<float>(effectsMinFgGrey.get_value());
    p.bezelOption = bezelOption.get_active();
    x.frame = frame.get_active();
    x.frameSize = static_cast<float>(frameSize.get_value());
    x.frameColor = rgbOf(frameColor);
    x.frameShininess = static_cast<float>(frameShininess.get_value());
    return p;
}

}  // namespace lterm
