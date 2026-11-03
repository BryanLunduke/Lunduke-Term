// SPDX-License-Identifier: GPL-3.0-or-later
// M6 item 4: the custom profile editor (Settings > Profile > Edit). Every option of a Profile:
// each CRT effect can be switched on/off and adjusted, plus colours, palettes, fonts, margins
// and the bezel. The dialog edits a copy; Save hands the result back (the caller writes it).
#pragma once

#include "profiles.hpp"

#include <gtkmm.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lterm {

class ProfileEditor : public Gtk::Dialog {
public:
    ProfileEditor(Gtk::Window& parent, const Profile& p);
    Profile result() const;  // the profile as the controls say
    std::function<void(const Profile&)> onSave;  // Save pressed (the dialog then hides)

    // An effect that is switched off (value 0) keeps its slider value while the dialog is
    // open, so switching it on again restores it.
    struct Toggle {
        Gtk::CheckButton on;
        Gtk::Scale value{Gtk::ORIENTATION_HORIZONTAL};
    };
    // Controls (public for the tests). Toggle keys: the key names of the profile file
    // (bloom, burn-in, chroma-color, ambient-light, flickering, glowing-line, horizontal-sync,
    // jitter, screen-curvature, static-noise, rgb-shift).
    std::map<std::string, std::unique_ptr<Toggle>> toggles;
    Gtk::CheckButton effects{"Effects on when this profile is chosen"};
    Gtk::Scale brightness{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::ColorButton phosphor, effectsBackground;
    Gtk::CheckButton scanlines{"Scanlines"};
    Gtk::SpinButton scanlinePeriod, bloomRadius;
    Gtk::Scale scanlineIntensity{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::CheckButton burnInHalfRes{"Burn-in at half resolution (less memory)"};
    Gtk::Entry fonts;  // "Family:px; Family" (first installed wins; px optional)
    Gtk::SpinButton fontPx, marginMinPx, marginFrac;
    Gtk::ColorButton fg, bg, cursor;
    std::vector<std::unique_ptr<Gtk::ColorButton>> palette, effectsPalette;
    Gtk::Scale minFgGrey{Gtk::ORIENTATION_HORIZONTAL}, effectsMinFgGrey{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::CheckButton hasEffectsPalette{"Use a separate palette while effects are on"};
    Gtk::CheckButton bezelOption{"Bezel can be switched on (Settings > Bezel)"};
    Gtk::CheckButton frame{"Bezel on by default"};
    Gtk::SpinButton frameSize;
    Gtk::ColorButton frameColor;
    Gtk::Scale frameShininess{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Notebook pages;

private:
    void updateSensitivity();
    Profile base_;
};

}  // namespace lterm
