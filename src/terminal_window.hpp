// SPDX-License-Identifier: GPL-3.0-or-later
// M2 stage 3: the main window. Tabs (Gtk::Notebook, the tab bar only with two or more tabs),
// a scrollback search bar and the settings dialog. Each tab is a TerminalView; tabs that are not
// shown are told so (setWindowVisible(false)), so their effects never animate in the background.
#pragma once

#include "profile_editor.hpp"
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lterm {

std::string settingsPath();  // $XDG_CONFIG_HOME/lunduke-terminal/settings.ini
// Writes the settings dialog's keys into settings.ini, keeping every other key; atomic (temp file
// + rename). A file that is not a valid key file is replaced (its text kept in <path>.corrupt)
// and *warning says so. False on error.
bool saveSettings(const ViewOptions& o, const std::string& path, std::string* error = nullptr,
                  std::string* warning = nullptr);

// PR #7 review: when settings.ini's [terminal] profile is `deletedId` (a custom profile that was
// just deleted), sets it to the default (amber), keeping every other key. True if it changed.
bool resetDeletedDefaultProfile(const std::string& path, const std::string& deletedId, std::string* error = nullptr);

class SettingsDialog;

class TerminalWindow : public Gtk::Window {
public:
    explicit TerminalWindow(const ViewOptions& opts, const std::string& title = "Lunduke Terminal");
    ~TerminalWindow() override;

    TerminalView* newTab(bool first = false);  // the first tab runs opts.command
    void closeTab(TerminalView* v);
    // Review item 9: what the user closes (tab button, Ctrl+Shift+W, the window) asks first when
    // a tab runs a foreground job. confirmHook (tests) answers instead of the dialog.
    void requestCloseTab(TerminalView* v);
    std::function<bool(const std::string& question)> confirmHook;
    bool deleteEventForTest() { return on_delete_event(nullptr); }
    std::string runningJobs() const;  // "vim, ssh" over every tab, "" if none
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
    Gtk::MessageDialog* confirmDialog() { return confirmDialog_.get(); }  // tests
    // Applies the dialog's options to every tab and to tabs opened later.
    void applySettings(const ViewOptions& o);
    void commitScrollbackLimits();  // Settings closed: apply pending lower limits
    // M6 item 4: a custom profile was saved (oldId == newId), renamed or deleted (newId = the
    // default): tabs that use oldId get newId again, so they show the stored values.
    void profilesChanged(const std::string& oldId, const std::string& newId);
    const ViewOptions& options() const { return opts_; }
    std::string settingsFile;  // where "Save as default" writes (tests point it elsewhere)

    bool handleKey(GdkEventKey* ev);  // window shortcuts; public for the tests

    // M4 item 4: after tabs close, glibc keeps the freed GL (llvmpipe) memory in the main arena
    // (RSS stayed ~480 MB with one tab). closeTab schedules one malloc_trim(0), trimDelayMs after
    // the last close, which hands the free pages back. trims(): how many ran (tests).
    int trims() const { return trims_; }
    long lastTrimKb() const { return lastTrimKb_; }  // RSS drop of the last trim (kB, may be <= 0)
    unsigned trimDelayMs = 1000;

protected:
    bool on_delete_event(GdkEventAny* ev) override;

private:
    void confirm(const std::string& question, const std::string& action, std::function<void()> yes);
    std::unique_ptr<Gtk::MessageDialog> confirmDialog_;
    void dropConfirmDialog();
    void requestCloseTabById(unsigned long id);
    bool closeConfirmed_ = false;
    struct Tab {
        std::unique_ptr<TerminalView> view;
        Gtk::Box label{Gtk::ORIENTATION_HORIZONTAL, 4};
        Gtk::Label text;
        Gtk::Button close;
        std::string title;
        unsigned long id = 0;
    };
    void scheduleTrim();
    int trims_ = 0;
    long lastTrimKb_ = 0;
    sigc::connection trimConn_;
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
    Gtk::Label scrollbackNote;  // review item 4: memory per tab when the scrollback is full
    void updateScrollbackNote();
    // M6 item 4: custom profiles. Built-ins are read-only: Edit, Rename and Delete work on
    // custom profiles only; "Copy to custom profile" starts one from any profile.
    Gtk::Button newProfile{"New..."}, copyProfile{"Copy to custom profile..."}, editProfile{"Edit..."},
        renameProfile{"Rename..."}, deleteProfile{"Delete"};
    Gtk::Label profileNote;
    std::unique_ptr<ProfileEditor> editor;  // the open editor (tests)
    // Tests answer instead of the dialogs: the name ("" = cancel), and the delete question.
    std::function<std::string(const std::string& title, const std::string& suggested)> askNameHook;
    std::function<bool(const std::string& question)> confirmDeleteHook;
    void fillProfiles(const std::string& activeId);
    void createProfile(bool copyCurrent);  // New / Copy to custom profile
    void editCurrentProfile();
    void renameCurrentProfile();
    void deleteCurrentProfile();

private:
    void changed();
    void updateFrameSensitivity();
    // M6 item 3: with no size override the field shows the size the profile (and font) uses;
    // a profile change drops the override and shows the new profile's size.
    void syncFontPx(int overridePx);
    void updateProfileButtons();
    std::string askName(const std::string& title, const std::string& suggested, const std::string& renaming);
    TerminalWindow& win_;
    ViewOptions base_;
    bool loading_ = false;
    int autoPx_ = 0;           // the size shown when there is no override
    std::string lastProfile_;  // profile the controls were last applied with
};

}  // namespace lterm
