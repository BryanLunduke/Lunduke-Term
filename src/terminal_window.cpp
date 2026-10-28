// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal_window.hpp"

#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <sys/wait.h>

namespace lterm {

std::string settingsPath() {
    return Glib::build_filename(Glib::get_user_config_dir(), "lunduke-terminal", "settings.ini");
}

bool saveSettings(const ViewOptions& o, const std::string& path, std::string* error) {
    Glib::KeyFile kf;
    try {
        if (Glib::file_test(path, Glib::FILE_TEST_EXISTS))
            kf.load_from_file(path, Glib::KEY_FILE_KEEP_COMMENTS);
        const char* g = "terminal";
        auto drop = [&](const char* k) {
            if (kf.has_group(g) && kf.has_key(g, k)) kf.remove_key(g, k);
        };
        kf.set_string(g, "profile", o.profile);
        if (o.fontFamily.empty()) drop("font");
        else kf.set_string(g, "font", o.fontFamily);
        if (o.fontPx <= 0) drop("font-px");
        else kf.set_integer(g, "font-px", o.fontPx);
        kf.set_string(g, "effects-quality", effectsQualityName(o.effectsQuality));
        kf.set_int64(g, "scrollback-lines", static_cast<gint64>(o.scrollbackLines));
        if (o.tube.frame < 0) drop("frame");
        else kf.set_boolean(g, "frame", o.tube.frame > 0);
        const std::string dir = Glib::path_get_dirname(path);
        if (g_mkdir_with_parents(dir.c_str(), 0700) != 0) {
            if (error) *error = "cannot create " + dir;
            return false;
        }
        kf.save_to_file(path);
        return true;
    } catch (const Glib::Error& e) {
        if (error) *error = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------------------------

TerminalWindow::TerminalWindow(const ViewOptions& opts, const std::string& title)
    : opts_(opts), baseTitle_(title) {
    settingsFile = settingsPath();
    set_title(baseTitle_);
    add_events(Gdk::VISIBILITY_NOTIFY_MASK | Gdk::STRUCTURE_MASK);
    nb_.set_scrollable(true);
    nb_.set_show_border(false);
    nb_.set_can_focus(false);
    nb_.set_show_tabs(false);
    nb_.set_hexpand(true);
    nb_.set_vexpand(true);

    // Search bar (Ctrl+Shift+F): Enter = older match, Shift+Enter = newer, Escape closes.
    entry_.set_placeholder_text("Search scrollback");
    entry_.set_width_chars(28);
    prev_.set_image_from_icon_name("go-up-symbolic");
    prev_.set_tooltip_text("Older match (Enter)");
    next_.set_image_from_icon_name("go-down-symbolic");
    next_.set_tooltip_text("Newer match (Shift+Enter)");
    closeSearch_.set_image_from_icon_name("window-close-symbolic");
    closeSearch_.set_relief(Gtk::RELIEF_NONE);
    closeSearch_.set_tooltip_text("Close (Escape)");
    for (Gtk::Widget* w : std::initializer_list<Gtk::Widget*>{&case_, &prev_, &next_, &closeSearch_}) w->set_can_focus(false);
    count_.set_width_chars(14);
    searchBox_.set_border_width(3);
    searchBox_.pack_start(entry_, false, false);
    searchBox_.pack_start(prev_, false, false);
    searchBox_.pack_start(next_, false, false);
    searchBox_.pack_start(case_, false, false);
    searchBox_.pack_start(count_, false, false);
    searchBox_.pack_end(closeSearch_, false, false);
    searchBox_.set_no_show_all(true);
    for (Gtk::Widget* w : std::initializer_list<Gtk::Widget*>{&entry_, &prev_, &next_, &case_, &count_, &closeSearch_}) w->show();

    entry_.signal_search_changed().connect(sigc::mem_fun(*this, &TerminalWindow::runSearch));
    case_.signal_toggled().connect(sigc::mem_fun(*this, &TerminalWindow::runSearch));
    entry_.signal_activate().connect([this] {
        if (auto* v = currentView()) v->searchStep(true);
    });
    entry_.signal_key_press_event().connect(
        [this](GdkEventKey* ev) {
            if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) && (ev->state & GDK_SHIFT_MASK)) {
                if (auto* v = currentView()) v->searchStep(false);
                return true;
            }
            return false;
        },
        false);
    entry_.signal_stop_search().connect(sigc::mem_fun(*this, &TerminalWindow::hideSearch));
    prev_.signal_clicked().connect([this] {
        if (auto* v = currentView()) v->searchStep(true);
    });
    next_.signal_clicked().connect([this] {
        if (auto* v = currentView()) v->searchStep(false);
    });
    closeSearch_.signal_clicked().connect(sigc::mem_fun(*this, &TerminalWindow::hideSearch));

    vbox_.pack_start(nb_, true, true);
    vbox_.pack_start(searchBox_, false, false);
    add(vbox_);

    nb_.signal_switch_page().connect([this](Gtk::Widget*, guint num) {
        // The search follows the shown tab.
        for (auto& t : tabs_)
            if (nb_.page_num(*t->view) != static_cast<int>(num)) t->view->clearSearch();
        // Deferred: the page is not current yet. mem_fun: dropped if the window goes first.
        Glib::signal_idle().connect_once(sigc::mem_fun(*this, &TerminalWindow::afterSwitch));
    });
    signal_key_press_event().connect(sigc::mem_fun(*this, &TerminalWindow::handleKey), false);

    // Pause the effects animation when the window is inactive, iconified or fully covered, and in
    // every tab that is not shown.
    property_is_active().signal_changed().connect([this] {
        updateVisibility();
        if (is_active()) set_urgency_hint(false);
    });
    signal_window_state_event().connect([this](GdkEventWindowState* e) {
        iconified_ = (e->new_window_state & (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_WITHDRAWN)) != 0;
        updateVisibility();
        return false;
    });
    signal_visibility_notify_event().connect([this](GdkEventVisibility* e) {
        obscured_ = e->state == GDK_VISIBILITY_FULLY_OBSCURED;
        updateVisibility();
        return false;
    });
    signal_map_event().connect([this](GdkEventAny*) {
        mapped_ = true;
        updateVisibility();
        return false;
    });
    signal_unmap_event().connect([this](GdkEventAny*) {
        mapped_ = false;
        updateVisibility();
        return false;
    });
    vbox_.show();
    nb_.show();
}

TerminalWindow::~TerminalWindow() {
    closing_ = true;
    settings_.reset();
    // Views first (they hang up their shells), while the notebook still exists.
    while (!tabs_.empty()) {
        std::unique_ptr<Tab> dead = std::move(tabs_.back());
        tabs_.pop_back();
        nb_.remove_page(*dead->view);
    }
}

int TerminalWindow::indexOf(const TerminalView* v) const {
    for (int i = 0; i < tabCount(); ++i)
        if (tabs_[i]->view.get() == v) return i;
    return -1;
}

// Notebook page -> tab. Tabs are reorderable, so the page number is not the index in tabs_
// (M2 mixed the two up after a drag). A page being removed is no longer in tabs_.
TerminalWindow::Tab* TerminalWindow::tabAtPage(int page) const {
    const Gtk::Widget* w = page >= 0 ? nb_.get_nth_page(page) : nullptr;
    if (!w) return nullptr;
    for (const auto& t : tabs_)
        if (t->view.get() == w) return t.get();
    return nullptr;
}

TerminalView* TerminalWindow::view(int page) {
    Tab* t = tabAtPage(page);
    return t ? t->view.get() : nullptr;
}

TerminalView* TerminalWindow::currentView() { return view(nb_.get_current_page()); }

TerminalWindow::Tab* TerminalWindow::tabById(unsigned long id) const {
    for (const auto& t : tabs_)
        if (t->id == id) return t.get();
    return nullptr;
}

unsigned long TerminalWindow::tabId(const TerminalView* v) const {
    const int i = indexOf(v);
    return i < 0 ? 0 : tabs_[i]->id;
}

void TerminalWindow::closeTabLater(unsigned long id) {
    // mem_fun on the window (a sigc::trackable): the idle callback is dropped if the window
    // goes first. The id, not the view pointer: a closed tab's address may be reused by a new
    // tab before the idle callback runs.
    Glib::signal_idle().connect_once(sigc::bind(sigc::mem_fun(*this, &TerminalWindow::closeTabById), id));
}

void TerminalWindow::closeTabById(unsigned long id) {
    if (Tab* t = tabById(id)) closeTab(t->view.get());
}

TerminalView* TerminalWindow::newTab(bool first) {
    ViewOptions o = opts_;
    if (!first) {
        o.command.clear();  // later tabs run the shell
        // ... in the directory of the current tab's shell, like other terminals.
        if (TerminalView* cur = currentView()) {
            const pid_t pid = cur->childPid();
            if (pid > 0) {
                gchar* cwd = g_file_read_link(("/proc/" + std::to_string(pid) + "/cwd").c_str(), nullptr);
                if (cwd) {
                    o.cwd = cwd;
                    g_free(cwd);
                }
            }
        }
    }
    auto tab = std::make_unique<Tab>();
    tab->id = nextTabId_++;
    const unsigned long id = tab->id;
    tab->view = std::make_unique<TerminalView>(o);
    TerminalView* v = tab->view.get();
    tab->text.set_text(baseTitle_);
    tab->text.set_ellipsize(Pango::ELLIPSIZE_END);
    tab->text.set_max_width_chars(24);
    tab->text.set_width_chars(8);
    tab->close.set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
    tab->close.set_relief(Gtk::RELIEF_NONE);
    tab->close.set_can_focus(false);
    tab->close.set_tooltip_text("Close tab (Ctrl+Shift+W)");
    tab->label.pack_start(tab->text, true, true);
    tab->label.pack_start(tab->close, false, false);
    tab->label.show_all();
    // Deferred: the button lives in the tab that is closed.
    tab->close.signal_clicked().connect([this, id] { closeTabLater(id); });

    v->signalTitle.connect([this, v](const std::string& t) {
        const int i = indexOf(v);
        if (i < 0) return;
        tabs_[i]->title = t;
        tabs_[i]->text.set_text(t.empty() ? baseTitle_ : t);
        tabs_[i]->text.set_tooltip_text(t);
        updateTitle();
    });
    // The child exited (and is not held): close the tab. Deferred, because this is emitted from
    // inside the view (its child watch or key handler).
    v->signalChildExited.connect([this, id] { closeTabLater(id); });
    v->signalExitStatus.connect([this, v](int status) {
        const int i = indexOf(v);
        if (i < 0) return;
        char t[64];
        if (WIFSIGNALED(status)) std::snprintf(t, sizeof t, "[killed: signal %d] ", WTERMSIG(status));
        else std::snprintf(t, sizeof t, "[exited: %d] ", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        tabs_[i]->title = t + (tabs_[i]->title.empty() ? baseTitle_ : tabs_[i]->title);
        tabs_[i]->text.set_text(tabs_[i]->title);
        updateTitle();
    });
    v->signalBell.connect([this] {
        if (!is_active()) set_urgency_hint(true);
    });
    v->signalBackend.connect([](const std::string& b) { std::fprintf(stderr, "lunduke-terminal: %s\n", b.c_str()); });
    v->signalSearchChanged.connect([this, v] {
        if (v == currentView()) updateSearchStatus();
    });

    tabs_.push_back(std::move(tab));
    const int page = nb_.append_page(*v, tabs_.back()->label);
    nb_.set_tab_reorderable(*v, true);
    v->show();
    nb_.set_show_tabs(tabCount() > 1);
    if (!v->start()) {
        std::fprintf(stderr, "lunduke-terminal: could not start the shell\n");
        closeTab(v);
        return nullptr;
    }
    nb_.set_current_page(page);
    updateVisibility();
    v->grabTerminalFocus();
    return v;
}

void TerminalWindow::closeTab(TerminalView* v) {
    const int i = indexOf(v);
    if (i < 0 || closing_) return;
    if (closingTab_) {  // called from inside another close (a signal while a view goes away)
        closeTabLater(tabs_[i]->id);
        return;
    }
    closingTab_ = true;
    // Out of tabs_ first: anything that runs while the page and the view go away (switch-page,
    // the view's signals) no longer finds this tab.
    std::unique_ptr<Tab> dead = std::move(tabs_[i]);
    tabs_.erase(tabs_.begin() + i);
    nb_.remove_page(*dead->view);
    dead.reset();  // the view's destructor hangs up its shell (without waiting)
    closingTab_ = false;
    nb_.set_show_tabs(tabCount() > 1);
    if (tabs_.empty()) {
        hide();  // the last tab closes the window (and ends Gtk::Application::run)
        return;
    }
    updateVisibility();
    updateTitle();
    if (auto* c = currentView()) c->grabTerminalFocus();
}

void TerminalWindow::afterSwitch() {
    updateVisibility();
    updateTitle();
    if (searchShown()) runSearch();
    else if (auto* v = currentView()) v->grabTerminalFocus();
}

void TerminalWindow::updateVisibility() {
    const int cur = nb_.get_current_page();
    const bool shown = mapped_ && !iconified_ && !obscured_;
    for (auto& t : tabs_) {
        const bool current = nb_.page_num(*t->view) == cur;
        t->view->setWindowVisible(shown && current);
        t->view->setWindowActive(is_active() && current);
    }
}

void TerminalWindow::updateTitle() {
    const Tab* t = tabAtPage(nb_.get_current_page());
    if (!t || t->title.empty()) set_title(baseTitle_);
    else set_title(t->title + " - Lunduke Terminal");
}

// ---- search ----

void TerminalWindow::showSearch() {
    searchBox_.show();
    entry_.grab_focus();
    entry_.select_region(0, -1);
    runSearch();
}

void TerminalWindow::hideSearch() {
    searchBox_.hide();
    for (auto& t : tabs_) t->view->clearSearch();
    if (auto* v = currentView()) v->grabTerminalFocus();
}

void TerminalWindow::runSearch() {
    if (!searchShown()) return;
    if (auto* v = currentView()) {
        v->prepareSearch(case_.get_active());  // start the cache even before the first letter
        v->setSearch(entry_.get_text(), case_.get_active());
    }
    updateSearchStatus();
}

void TerminalWindow::updateSearchStatus() {
    TerminalView* v = currentView();
    if (!v || entry_.get_text().empty()) {
        count_.set_text("");
        return;
    }
    if (v->searchPending()) {
        count_.set_text("Searching\u2026");
        return;
    }
    if (v->searchCount() == 0) {
        count_.set_text("No matches");
        return;
    }
    char t[64];
    std::snprintf(t, sizeof t, "%d of %d%s", v->searchIndex() + 1, v->searchCount(), v->searchCount() >= 10000 ? "+" : "");
    count_.set_text(t);
}

// ---- keys ----

bool TerminalWindow::handleKey(GdkEventKey* ev) {
    const guint mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    const guint k = gdk_keyval_to_lower(ev->keyval);
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        switch (k) {
        case GDK_KEY_t: newTab(); return true;
        case GDK_KEY_w:
            if (auto* v = currentView()) closeTab(v);
            return true;
        case GDK_KEY_f: showSearch(); return true;
        case GDK_KEY_s: showSettings(); return true;
        default: break;
        }
    }
    if (mods == GDK_CONTROL_MASK && tabCount() > 1) {
        const int n = tabCount(), cur = nb_.get_current_page();
        if (ev->keyval == GDK_KEY_Page_Up || ev->keyval == GDK_KEY_KP_Page_Up) {
            nb_.set_current_page((cur + n - 1) % n);
            return true;
        }
        if (ev->keyval == GDK_KEY_Page_Down || ev->keyval == GDK_KEY_KP_Page_Down) {
            nb_.set_current_page((cur + 1) % n);
            return true;
        }
    }
    if (searchShown() && ev->keyval == GDK_KEY_Escape && entry_.has_focus()) {
        hideSearch();
        return true;
    }
    return false;
}

// ---- settings ----

void TerminalWindow::showSettings() {
    TerminalView* v = currentView();
    ViewOptions cur = opts_;
    if (v) {
        cur = v->options();
        cur.profile = v->profile().id;  // Ctrl+Shift+P may have changed it
    }
    if (!settings_) settings_ = std::make_unique<SettingsDialog>(*this, cur);
    else settings_->load(cur);
    settings_->show();
    settings_->present();
}

void TerminalWindow::applySettings(const ViewOptions& o) {
    for (auto& t : tabs_) {
        TerminalView& v = *t->view;
        const ViewOptions& cur = v.options();
        if (cur.tube.frame != o.tube.frame) v.setFrame(o.tube.frame);
        if (v.profile().id != o.profile) v.setProfile(o.profile);
        if (cur.fontFamily != o.fontFamily || cur.fontPx != o.fontPx) v.setFont(o.fontFamily, o.fontPx);
        if (cur.effectsQuality != o.effectsQuality) v.setEffectsQuality(o.effectsQuality);
        if (cur.scrollbackLines != o.scrollbackLines) v.setScrollbackLines(o.scrollbackLines);
    }
    const auto command = opts_.command;
    opts_.profile = o.profile;
    opts_.fontFamily = o.fontFamily;
    opts_.fontPx = o.fontPx;
    opts_.effectsQuality = o.effectsQuality;
    opts_.scrollbackLines = o.scrollbackLines;
    opts_.tube.frame = o.tube.frame;
    opts_.command = command;
}

SettingsDialog::SettingsDialog(TerminalWindow& win, const ViewOptions& current)
    : Gtk::Dialog("Lunduke Terminal settings", win, false), win_(win) {
    set_default_size(420, -1);
    add_button("_Close", Gtk::RESPONSE_CLOSE);
    signal_response().connect([this](int) { hide(); });

    for (const auto& p : builtinProfiles()) profile.append(p.id, p.label + (p.effects ? "" : "  (default)"));
    // Monospaced families that are installed, plus the profile's own choice.
    font.append("", "Profile font");
    {
        std::set<std::string> fams;
        auto ctx = create_pango_context();
        for (const auto& f : ctx->list_families())
            if (f->is_monospace()) fams.insert(f->get_name());
        for (const auto& f : fams) font.append(f, f);
    }
    fontPx.set_range(0, 96);
    fontPx.set_increments(1, 4);
    fontPx.set_tooltip_text("Font size in pixels; 0 = the profile's size");
    for (EffectsQuality q : {EffectsQuality::Auto, EffectsQuality::Full, EffectsQuality::Reduced, EffectsQuality::Static})
        quality.append(effectsQualityName(q), effectsQualityName(q));
    quality.set_tooltip_text("auto: reduced on software rendering; static: no animation (0% idle CPU)");
    scrollback.set_range(0, static_cast<double>(kMaxScrollbackLines));
    scrollback.set_increments(1000, 10000);
    scrollback.set_tooltip_text("Lines of history kept per tab");
    frame.append("-1", "Profile default");
    frame.append("1", "On");
    frame.append("0", "Off");
    frame.set_tooltip_text("Monitor bezel (amber, green, green-scanlines, vintage, paper-white)");

    auto* grid = Gtk::manage(new Gtk::Grid);
    grid->set_row_spacing(6);
    grid->set_column_spacing(12);
    grid->set_border_width(12);
    int row = 0;
    auto addRow = [&](const char* text, Gtk::Widget& w) {
        auto* l = Gtk::manage(new Gtk::Label(text));
        l->set_halign(Gtk::ALIGN_END);
        w.set_hexpand(true);
        grid->attach(*l, 0, row, 1, 1);
        grid->attach(w, 1, row, 1, 1);
        ++row;
    };
    addRow("Profile", profile);
    addRow("Font", font);
    addRow("Font size (px)", fontPx);
    addRow("Effects quality", quality);
    addRow("Scrollback lines", scrollback);
    addRow("Bezel (frame)", frame);
    status.set_halign(Gtk::ALIGN_START);
    status.set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    grid->attach(save, 0, row, 1, 1);
    grid->attach(status, 1, row, 1, 1);
    get_content_area()->pack_start(*grid, true, true);
    grid->show_all();

    load(current);
    for (Gtk::ComboBoxText* c : {&profile, &font, &quality, &frame}) c->signal_changed().connect(sigc::mem_fun(*this, &SettingsDialog::changed));
    fontPx.signal_value_changed().connect(sigc::mem_fun(*this, &SettingsDialog::changed));
    scrollback.signal_value_changed().connect(sigc::mem_fun(*this, &SettingsDialog::changed));
    save.signal_clicked().connect([this] {
        std::string err;
        if (saveSettings(options(), win_.settingsFile, &err)) status.set_text("Saved to " + win_.settingsFile);
        else status.set_text("Not saved: " + err);
    });
}

void SettingsDialog::load(const ViewOptions& o) {
    loading_ = true;
    base_ = o;
    profile.set_active_id(o.profile);
    if (!font.set_active_id(o.fontFamily)) {
        font.append(o.fontFamily, o.fontFamily + " (not monospaced or not installed)");
        font.set_active_id(o.fontFamily);
    }
    fontPx.set_value(std::max(0, o.fontPx));
    quality.set_active_id(effectsQualityName(o.effectsQuality));
    scrollback.set_value(static_cast<double>(clampScrollbackLines(o.scrollbackLines)));
    frame.set_active_id(std::to_string(o.tube.frame < 0 ? -1 : (o.tube.frame > 0 ? 1 : 0)));
    status.set_text("");
    updateFrameSensitivity();
    loading_ = false;
}

ViewOptions SettingsDialog::options() const {
    ViewOptions o = base_;
    o.profile = profile.get_active_id();
    o.fontFamily = font.get_active_id();
    o.fontPx = fontPx.get_value_as_int();
    parseEffectsQuality(quality.get_active_id(), o.effectsQuality);
    o.scrollbackLines = static_cast<long long>(scrollback.get_value());
    o.tube.frame = std::stoi(frame.get_active_id().empty() ? std::string("-1") : std::string(frame.get_active_id()));
    return o;
}

void SettingsDialog::updateFrameSensitivity() {
    const Profile* p = findProfile(profile.get_active_id());
    frame.set_sensitive(p && p->bezelOption);
}

void SettingsDialog::changed() {
    if (loading_) return;
    updateFrameSensitivity();
    status.set_text("");
    win_.applySettings(options());
}

}  // namespace lterm
