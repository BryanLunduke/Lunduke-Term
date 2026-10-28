// SPDX-License-Identifier: GPL-3.0-or-later
// M2 stage 3: the main window. Tabs (Gtk::Notebook, the tab bar only with two or more tabs),
// a scrollback search bar and the settings dialog. Each tab is a TerminalView; tabs that are not
// shown are told so (setWindowVisible(false)), so their effects never animate in the background.
#pragma once

#include "terminal_view.hpp"

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

namespace lterm {

std::string settingsPath();  // $XDG_CONFIG_HOME/lunduke-terminal/settings.ini
// Writes the settings dialog's keys into settings.ini, keeping every other key. False on error.
bool saveSettings(const ViewOptions& o, const std::string& path, std::string* error = nullptr);

class SettingsDialog;

class TerminalWindow : public Gtk::Window {
public:
    explicit TerminalWindow(const ViewOptions& opts, const std::string& title = "Lunduke Terminal");
    ~TerminalWindow() override;

    TerminalView* newTab(bool first = false);  // the first tab runs opts.command
    void closeTab(TerminalView* v);
    int tabCount() const { return static_cast<int>(tabs_.size()); }
    TerminalView* currentView();
    TerminalView* view(int page);  // by notebook page (tabs are reorderable)
    // M3 item 1d: tabs have ids; deferred closes (close button, child exit) use the id, so a
    // close that arrives late or twice finds nothing instead of a dangling or reused pointer.
    unsigned long tabId(const TerminalView* v) const;  // 0: not a tab of this window
    void closeTabById(unsigned long id);
    void closeTabLater(unsigned long id);
    void reorderForTest(TerminalView* v, int page) { nb_.reorder_child(*v, page); }
    Gtk::Button* closeButton(const TerminalView* v) {  // tests
        const int i = indexOf(v);
        return i < 0 ? nullptr : &tabs_[i]->close;
    }
    int currentIndex() const { return nb_.get_current_page(); }
    void setCurrent(int i) { nb_.set_current_page(i); }
    bool tabsShown() const { return nb_.get_show_tabs(); }

    void showSearch();
    void hideSearch();
    bool searchShown() const { return searchBox_.get_visible(); }
    Gtk::SearchEntry& searchEntry() { return entry_; }
    Gtk::CheckButton& searchCase() { return case_; }
    std::string searchStatus() const { return count_.get_text(); }

    void showSettings();
    SettingsDialog* settingsDialog() { return settings_.get(); }
    // Applies the dialog's options to every tab and to tabs opened later.
    void applySettings(const ViewOptions& o);
    const ViewOptions& options() const { return opts_; }
    std::string settingsFile;  // where "Save as default" writes (tests point it elsewhere)

    bool handleKey(GdkEventKey* ev);  // window shortcuts; public for the tests

private:
    struct Tab {
        std::unique_ptr<TerminalView> view;
        Gtk::Box label{Gtk::ORIENTATION_HORIZONTAL, 4};
        Gtk::Label text;
        Gtk::Button close;
        std::string title;
        unsigned long id = 0;
    };
    Tab* tabAtPage(int page) const;
    Tab* tabById(unsigned long id) const;
    void afterSwitch();
    void updateVisibility();
    void updateTitle();
    void updateSearchStatus();
    void runSearch();
    int indexOf(const TerminalView* v) const;

    ViewOptions opts_;
    std::string baseTitle_;
    Gtk::Box vbox_{Gtk::ORIENTATION_VERTICAL};
    Gtk::Notebook nb_;
    Gtk::Box searchBox_{Gtk::ORIENTATION_HORIZONTAL, 4};
    Gtk::SearchEntry entry_;
    Gtk::CheckButton case_{"Match case"};
    Gtk::Button prev_, next_, closeSearch_;
    Gtk::Label count_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    std::unique_ptr<SettingsDialog> settings_;
    bool iconified_ = false, obscured_ = false, mapped_ = false;
    bool closing_ = false;
    bool closingTab_ = false;
    unsigned long nextTabId_ = 1;
};

class SettingsDialog : public Gtk::Dialog {
public:
    SettingsDialog(TerminalWindow& win, const ViewOptions& current);
    ViewOptions options() const;  // what the controls say
    void load(const ViewOptions& o);
    // Controls (public for the tests).
    Gtk::ComboBoxText profile, font, quality;
    Gtk::SpinButton fontPx, scrollback;
    Gtk::ComboBoxText frame;  // profile default / on / off
    Gtk::Button save{"Save as default"};
    Gtk::Label status;

private:
    void changed();
    void updateFrameSensitivity();
    TerminalWindow& win_;
    ViewOptions base_;
    bool loading_ = false;
};

}  // namespace lterm
