// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal_window.hpp"

#include "custom_profiles.hpp"
#include "grid_painter.hpp"

#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sys/wait.h>

namespace lterm {

std::string settingsPath() {
    return Glib::build_filename(Glib::get_user_config_dir(), "lunduke-terminal", "settings.ini");
}

bool saveSettings(const ViewOptions& o, const std::string& path, std::string* error, std::string* warning) {
    Glib::KeyFile kf;
    if (warning) warning->clear();
    // Keep every other key, unless the file cannot be read as a key file (M2: Save then failed
    // for good). M3: warn, keep the unreadable text as <path>.corrupt and write a fresh file.
    if (Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
        try {
            kf.load_from_file(path, Glib::KEY_FILE_KEEP_COMMENTS);
        } catch (const Glib::Error& e) {
            std::string msg = path + " was not valid (" + e.what() + "); it was replaced";
            try {
                Glib::file_set_contents(path + ".corrupt", Glib::file_get_contents(path));
                msg += ", the old text is in " + path + ".corrupt";
            } catch (const Glib::Error&) {
            }
            if (warning) *warning = msg;
            std::fprintf(stderr, "lunduke-terminal: %s\n", msg.c_str());
            kf = Glib::KeyFile();
        }
    }
    try {
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
        // Atomic: g_file_set_contents() writes a temporary file in the same directory, then
        // renames it over the old one, so a crash never leaves a half-written settings.ini.
        Glib::file_set_contents(path, kf.to_data());
        return true;
    } catch (const Glib::Error& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool resetDeletedDefaultProfile(const std::string& path, const std::string& deletedId, std::string* error) {
    if (!Glib::file_test(path, Glib::FILE_TEST_IS_REGULAR)) return false;
    try {
        Glib::KeyFile kf;
        kf.load_from_file(path, Glib::KEY_FILE_KEEP_COMMENTS);
        if (!kf.has_group("terminal") || !kf.has_key("terminal", "profile") ||
            kf.get_string("terminal", "profile") != deletedId)
            return false;
        kf.set_string("terminal", "profile", kDefaultProfile);
        Glib::file_set_contents(path, kf.to_data());  // atomic, like saveSettings
        return true;
    } catch (const Glib::Error& e) {
        // An unreadable settings.ini is left alone; at start-up an unknown profile falls back to
        // the default anyway.
        if (error) *error = e.what();
        return false;
    }
}

WindowSize defaultWindowSize(const ViewOptions& opts) {
    const Profile* found = findProfile(opts.profile);
    if (!found) found = findProfile(kDefaultProfile);
    Profile profile = found ? *found : Profile{};
    applyTubeOverrides(profile, opts.tube);
    const bool effectsOn = opts.effects < 0 ? profile.effects : opts.effects != 0;
    const FontPick pick = resolveViewFont(profile, opts.fontFamily);
    int px = opts.fontPx > 0 ? opts.fontPx : pick.px;
    if (px < 1) px = profile.fontPx > 0 ? profile.fontPx : 16;
    const std::string family = pick.family.empty() ? std::string("Monospace") : pick.family;
    // Scale 1: set_default_size is in logical pixels, and a later GDK scale keeps the same
    // rows and columns (computeGridLayout).
    GridPainter painter(family, px, 1);
    const int cellW = std::max(1, painter.cellWidth());
    const int cellH = std::max(1, painter.cellHeight());
    // layoutGrid takes the inset from frameGeometry on the finished window, which caps it at a
    // quarter of the shorter side. Start from the profile's frame size and repeat until the
    // inset the window will actually use is the one it was sized for.
    int inset = 0;
    if (effectsOn && profile.fx.frame && profile.fx.frameSize > 0)
        inset = static_cast<int>(std::ceil(profile.fx.frameSize));
    WindowSize s{};
    for (int n = 0; n < 4; ++n) {
        s = windowSizeForGrid(kDefaultCols, kDefaultRows, cellW, cellH, 1, profile.marginMinPx,
                              profile.marginFrac, inset);
        int next = 0;
        if (effectsOn && profile.fx.frame && profile.fx.frameSize > 0) {
            EffectParams fx = profile.fx;
            fx.pixelScale = 1.0f;
            next = static_cast<int>(std::ceil(frameGeometry(fx, s.width, s.height).inset));
        }
        if (next == inset) break;
        inset = next;
    }
    if (s.width < 1) s.width = 640;
    if (s.height < 1) s.height = 384;
    return s;
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
        fullscreen_ = (e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
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
    trimConn_.disconnect();
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

static long rssKb() {
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

void TerminalWindow::scheduleTrim() {
    // Debounced: a burst of closes gives one trim. malloc_trim walks the arenas (a few ms).
    trimConn_.disconnect();
    trimConn_ = Glib::signal_timeout().connect(
        [this] {
            const long before = rssKb();
#if defined(__GLIBC__)
            malloc_trim(0);
#endif
            lastTrimKb_ = before - rssKb();
            ++trims_;
            return false;
        },
        trimDelayMs);
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
    // sigc::mem_fun (trackable): the idle is dropped if the window goes first.
    tab->close.signal_clicked().connect([this, id] {
        Glib::signal_idle().connect_once(sigc::bind(sigc::mem_fun(*this, &TerminalWindow::requestCloseTabById), id));
    });

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
    // PR #9 review: only a changed line is printed, so the font zoom (which rebuilds the painter
    // and re-runs chooseMode()) does not repeat the renderer line on every step. It starts from
    // the line the view had when it was created (emitted before this connect, so not printed,
    // as before).
    v->signalBackend.connect([last = v->backendName()](const std::string& b) mutable {
        if (b == last) return;
        last = b;
        std::fprintf(stderr, "lunduke-terminal: %s\n", b.c_str());
    });
    v->signalSearchChanged.connect([this, v] {
        if (v == currentView()) updateSearchStatus();
    });
    v->signalSettingsRequested.connect([this] { showSettings(); });

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

std::string TerminalWindow::runningJobs() const {
    std::string out;
    for (const auto& t : tabs_) {
        const std::string j = t->view->foregroundJob();
        if (j.empty()) continue;
        if (!out.empty()) out += ", ";
        out += j;
    }
    return out;
}

void TerminalWindow::confirm(const std::string& question, const std::string& action, std::function<void()> yes) {
    if (confirmHook) {
        if (confirmHook(question)) yes();
        return;
    }
    // PR #6 review N4: a second close request while a question is open keeps the first one
    // (it is raised again); replacing it lost the first answer's target.
    if (confirmDialog_ && confirmDialog_->get_visible()) {
        confirmDialog_->present();
        return;
    }
    confirmDialog_ = std::make_unique<Gtk::MessageDialog>(*this, question, false, Gtk::MESSAGE_QUESTION,
                                                          Gtk::BUTTONS_NONE, true);
    confirmDialog_->set_secondary_text("Closing ends it.");
    confirmDialog_->add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    confirmDialog_->add_button(action, Gtk::RESPONSE_ACCEPT);
    confirmDialog_->set_default_response(Gtk::RESPONSE_CANCEL);
    confirmDialog_->signal_response().connect([this, yes](int r) {
        confirmDialog_->hide();
        Glib::signal_idle().connect_once(sigc::mem_fun(*this, &TerminalWindow::dropConfirmDialog));
        if (r == Gtk::RESPONSE_ACCEPT) yes();
    });
    confirmDialog_->show();
}

void TerminalWindow::requestCloseTabById(unsigned long id) {
    if (Tab* t = tabById(id)) requestCloseTab(t->view.get());
}

void TerminalWindow::dropConfirmDialog() {
    if (confirmDialog_ && !confirmDialog_->get_visible()) confirmDialog_.reset();
}

void TerminalWindow::requestCloseTab(TerminalView* v) {
    if (indexOf(v) < 0) return;
    const std::string job = v->foregroundJob();
    if (job.empty()) {
        closeTab(v);
        return;
    }
    const unsigned long id = tabId(v);
    confirm("Close this tab? \"" + job + "\" is still running.", "Close _Tab", [this, id] {
        if (Tab* t = tabById(id)) closeTab(t->view.get());
    });
}

bool TerminalWindow::on_delete_event(GdkEventAny* ev) {
    if (closeConfirmed_) return ev ? Gtk::Window::on_delete_event(ev) : false;
    const std::string jobs = runningJobs();
    if (jobs.empty()) return ev ? Gtk::Window::on_delete_event(ev) : false;
    confirm("Close this window? Still running: " + jobs + ".", "Close _Window", [this] {
        closeConfirmed_ = true;
        if (confirmHook) return;  // tests: the delete event's caller sees "go ahead"
        close();
    });
    if (confirmHook && closeConfirmed_) return false;
    return true;  // keep the window (the dialog decides)
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
    scheduleTrim();
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

void TerminalWindow::toggleFullscreen() {
    // The window manager confirms with a window-state event (fullscreen_); ask for the opposite
    // of what it last reported.
    if (fullscreen_) unfullscreen();
    else fullscreen();
}

bool TerminalWindow::handleKey(GdkEventKey* ev) {
    const guint mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    const guint k = gdk_keyval_to_lower(ev->keyval);
    // Perf step 1: F11 toggles fullscreen (as in GNOME Terminal); it is no longer sent to the
    // program. Shift/Ctrl/Alt/Super+F11 still are (PR #9 review: Super, Hyper and Meta are in
    // this mask; X11 reports Super as MOD4 and GDK adds the virtual SUPER bit).
    const guint fsMods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK | GDK_MOD4_MASK |
                                      GDK_SUPER_MASK | GDK_HYPER_MASK | GDK_META_MASK);
    if (fsMods == 0 && ev->keyval == GDK_KEY_F11) {
        toggleFullscreen();
        return true;
    }
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        switch (k) {
        case GDK_KEY_t: newTab(); return true;
        case GDK_KEY_w:
            if (auto* v = currentView()) requestCloseTab(v);
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

void TerminalWindow::commitScrollbackLimits() {
    for (auto& t : tabs_) t->view->commitScrollbackLimit();
}

void TerminalWindow::profilesChanged(const std::string& oldId, const std::string& newId) {
    for (auto& t : tabs_)
        if (t->view->profile().id == oldId) t->view->setProfile(newId);
    if (opts_.profile == oldId) opts_.profile = newId;
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
    // PR #6 review N2: a lower scrollback limit is applied when the dialog closes (Close, Esc,
    // the window's close button), never on a live spin-button change.
    signal_hide().connect([this] { win_.commitScrollbackLimits(); });

    fillProfiles("");
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
    fontPx.set_tooltip_text("Font size in pixels. Shows the profile's size until you change it; choosing a "
                           "profile switches to that profile's size (0 = the profile's size)");
    for (EffectsQuality q : {EffectsQuality::Auto, EffectsQuality::Full, EffectsQuality::Reduced, EffectsQuality::Static})
        quality.append(effectsQualityName(q), effectsQualityName(q));
    quality.set_tooltip_text("auto: reduced on software rendering; static: no animation (0% idle CPU)");
    scrollback.set_range(0, static_cast<double>(kMaxScrollbackLines));
    scrollback.set_increments(1000, 10000);
    scrollback.set_tooltip_text("Lines of history kept per tab (at most 100,000). Lowering it drops the oldest "
                                "lines when new output arrives.");
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
    {
        auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
        for (Gtk::Button* b : {&newProfile, &copyProfile, &editProfile, &renameProfile, &deleteProfile})
            box->pack_start(*b, false, false);
        newProfile.set_tooltip_text("A new custom profile with the Plain profile's values");
        copyProfile.set_tooltip_text("Built-in profiles are read-only: copy one to change it");
        grid->attach(*box, 1, row++, 1, 1);
        profileNote.set_halign(Gtk::ALIGN_START);
        profileNote.set_xalign(0);
        profileNote.set_line_wrap(true);
        profileNote.set_max_width_chars(60);
        grid->attach(profileNote, 1, row++, 1, 1);
        newProfile.signal_clicked().connect([this] { createProfile(false); });
        copyProfile.signal_clicked().connect([this] { createProfile(true); });
        editProfile.signal_clicked().connect([this] { editCurrentProfile(); });
        renameProfile.signal_clicked().connect([this] { renameCurrentProfile(); });
        deleteProfile.signal_clicked().connect([this] { deleteCurrentProfile(); });
    }
    addRow("Font", font);
    addRow("Font size (px)", fontPx);
    addRow("Effects quality", quality);
    addRow("Scrollback lines", scrollback);
    scrollbackNote.set_halign(Gtk::ALIGN_START);
    scrollbackNote.set_xalign(0);
    scrollbackNote.set_line_wrap(true);
    scrollbackNote.set_max_width_chars(60);
    grid->attach(scrollbackNote, 1, row++, 1, 1);
    addRow("Bezel (frame)", frame);
    status.set_halign(Gtk::ALIGN_START);
    status.set_line_wrap(true);
    status.set_max_width_chars(60);
    status.set_xalign(0);
    save.set_valign(Gtk::ALIGN_START);  // a wrapped warning must not stretch the button
    grid->attach(save, 0, row, 1, 1);
    grid->attach(status, 1, row, 1, 1);
    get_content_area()->pack_start(*grid, true, true);
    grid->show_all();

    load(current);
    for (Gtk::ComboBoxText* c : {&profile, &font, &quality, &frame}) c->signal_changed().connect(sigc::mem_fun(*this, &SettingsDialog::changed));
    fontPx.signal_value_changed().connect(sigc::mem_fun(*this, &SettingsDialog::changed));
    scrollback.signal_value_changed().connect(sigc::mem_fun(*this, &SettingsDialog::changed));
    save.signal_clicked().connect([this] {
        std::string err, warn;
        if (!saveSettings(options(), win_.settingsFile, &err, &warn)) status.set_text("Not saved: " + err);
        else if (!warn.empty()) status.set_text("Warning: " + warn + ". Saved.");
        else status.set_text("Saved to " + win_.settingsFile);
    });
}

void SettingsDialog::load(const ViewOptions& o) {
    loading_ = true;
    base_ = o;
    fillProfiles(o.profile);
    if (!font.set_active_id(o.fontFamily)) {
        font.append(o.fontFamily, o.fontFamily + " (not monospaced or not installed)");
        font.set_active_id(o.fontFamily);
    }
    lastProfile_ = o.profile;
    syncFontPx(std::max(0, o.fontPx));
    quality.set_active_id(effectsQualityName(o.effectsQuality));
    scrollback.set_value(static_cast<double>(clampScrollbackLines(o.scrollbackLines)));
    updateScrollbackNote();
    frame.set_active_id(std::to_string(o.tube.frame < 0 ? -1 : (o.tube.frame > 0 ? 1 : 0)));
    status.set_text("");
    updateFrameSensitivity();
    loading_ = false;
}

ViewOptions SettingsDialog::options() const {
    ViewOptions o = base_;
    o.profile = profile.get_active_id();
    o.fontFamily = font.get_active_id();
    // The profile's own size is no override, so a later profile change keeps following it.
    o.fontPx = fontPx.get_value_as_int() == autoPx_ ? 0 : fontPx.get_value_as_int();
    parseEffectsQuality(quality.get_active_id(), o.effectsQuality);
    o.scrollbackLines = static_cast<long long>(scrollback.get_value());
    o.tube.frame = std::stoi(frame.get_active_id().empty() ? std::string("-1") : std::string(frame.get_active_id()));
    return o;
}

void SettingsDialog::fillProfiles(const std::string& activeId) {
    const bool was = loading_;
    loading_ = true;
    profile.remove_all();
    for (const auto& p : builtinProfiles()) profile.append(p.id, p.label);
    for (const auto& p : customProfiles()) profile.append(p.id, p.label);
    if (!activeId.empty()) profile.set_active_id(activeId);
    loading_ = was;
    updateProfileButtons();
}

void SettingsDialog::updateProfileButtons() {
    const bool custom = isCustomProfileId(profile.get_active_id());
    for (Gtk::Button* b : {&editProfile, &renameProfile, &deleteProfile}) b->set_sensitive(custom);
    copyProfile.set_sensitive(findProfile(profile.get_active_id()) != nullptr);
    profileNote.set_text(custom ? "Custom profile, stored in " + customProfilesDir()
                                : "Built-in profile (read-only). Copy it to make your own.");
}

std::string SettingsDialog::askName(const std::string& title, const std::string& suggested, const std::string& renaming) {
    if (askNameHook) return askNameHook(title, suggested);
    Gtk::Dialog d(title, *this, true);
    d.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    Gtk::Button* ok = d.add_button("_OK", Gtk::RESPONSE_OK);
    d.set_default_response(Gtk::RESPONSE_OK);
    Gtk::Entry entry;
    entry.set_text(suggested);
    entry.set_activates_default(true);
    Gtk::Label why;
    why.set_xalign(0);
    auto check = [&] {
        std::string err;
        const bool good = validateProfileName(entry.get_text(), &err, renaming, customProfilesDir());
        ok->set_sensitive(good);
        why.set_text(good ? "" : err);
    };
    entry.signal_changed().connect(check);
    check();
    auto* box = d.get_content_area();
    box->set_spacing(6);
    box->set_border_width(12);
    box->pack_start(*Gtk::manage(new Gtk::Label("Profile name")), false, false);
    box->pack_start(entry, false, false);
    box->pack_start(why, false, false);
    box->show_all();
    return d.run() == Gtk::RESPONSE_OK ? std::string(entry.get_text()) : std::string();
}

void SettingsDialog::createProfile(bool copyCurrent) {
    const Profile* src = findProfile(copyCurrent ? std::string(profile.get_active_id()) : std::string("plain"));
    if (!src) return;
    const std::string base = copyCurrent ? src->label + " (copy)" : std::string("My profile");
    std::string suggested = base;
    for (int i = 2; !validateProfileName(suggested, nullptr, "", customProfilesDir()) && i < 1000; ++i) suggested = base + " " + std::to_string(i);
    const std::string name = askName(copyCurrent ? "Copy to custom profile" : "New profile", suggested, "");
    if (name.empty()) return;
    Profile p = *src;
    p.id = customProfileId(name);
    p.label = name;
    std::string err;
    if (!saveCustomProfile(customProfilesDir(), p, &err)) {
        status.set_text("Profile not saved: " + err);
        return;
    }
    fillProfiles(p.id);
    changed();  // the new profile is now the active one
    editCurrentProfile();
}

void SettingsDialog::editCurrentProfile() {
    const std::string id = profile.get_active_id();
    const Profile* p = findProfile(id);
    if (!p || !isCustomProfileId(id)) return;  // built-ins are read-only
    editor = std::make_unique<ProfileEditor>(*this, *p);
    editor->onSave = [this, id](const Profile& r) {
        Profile q = r;
        q.id = id;
        std::string err;
        if (!saveCustomProfile(customProfilesDir(), q, &err)) {
            status.set_text("Profile not saved: " + err);
            return;
        }
        status.set_text("Profile saved");
        win_.profilesChanged(id, id);
        syncFontPx(fontPx.get_value_as_int() == autoPx_ ? 0 : fontPx.get_value_as_int());
        updateFrameSensitivity();
    };
    editor->show();
}

void SettingsDialog::renameCurrentProfile() {
    const std::string id = profile.get_active_id();
    if (!isCustomProfileId(id)) return;
    const std::string name = askName("Rename profile", customProfileName(id), id);
    if (name.empty() || name == customProfileName(id)) return;
    std::string err;
    if (!renameCustomProfile(customProfilesDir(), id, name, &err)) {
        status.set_text("Not renamed: " + err);
        return;
    }
    const std::string newId = customProfileId(name);
    win_.profilesChanged(id, newId);
    lastProfile_ = newId;  // the same profile: keep the font size
    fillProfiles(newId);
}

void SettingsDialog::deleteCurrentProfile() {
    const std::string id = profile.get_active_id();
    if (!isCustomProfileId(id)) return;
    const std::string q = "Delete the profile \"" + customProfileName(id) + "\"? Its file is removed.";
    bool yes;
    if (confirmDeleteHook) yes = confirmDeleteHook(q);
    else {
        Gtk::MessageDialog m(*this, q, false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
        m.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        m.add_button("_Delete", Gtk::RESPONSE_OK);
        yes = m.run() == Gtk::RESPONSE_OK;
    }
    if (!yes) return;
    std::string err;
    if (!deleteCustomProfile(customProfilesDir(), id, &err)) {
        status.set_text("Not deleted: " + err);
        return;
    }
    win_.profilesChanged(id, kDefaultProfile);
    fillProfiles(kDefaultProfile);
    changed();
    // PR #7 review: the deleted profile was the saved default: settings.ini goes back to amber.
    std::string resetErr;
    if (resetDeletedDefaultProfile(win_.settingsFile, id, &resetErr))
        status.set_text("Deleted. The saved default profile is " + std::string(kDefaultProfile) + " again.");
    else if (!resetErr.empty())
        status.set_text("Deleted, but " + win_.settingsFile + " could not be updated: " + resetErr);
}

void SettingsDialog::updateFrameSensitivity() {
    const Profile* p = findProfile(profile.get_active_id());
    frame.set_sensitive(p && p->bezelOption);
}

void SettingsDialog::updateScrollbackNote() {
    TerminalView* v = win_.currentView();
    const int cols = v ? std::max(v->session().cols(), 1) : 80;
    const size_t lines = clampScrollbackLines(static_cast<long long>(scrollback.get_value()));
    const double mb = VtermSession::scrollbackBytesFull(lines, cols) / (1024.0 * 1024.0);
    char t[200];
    if (mb >= 100)
        std::snprintf(t, sizeof t, "Warning: up to %.0f MB per tab when full (%d columns). Every tab keeps its own.", mb, cols);
    else
        std::snprintf(t, sizeof t, "Up to %.*f MB per tab when full (%d columns).", mb < 10 ? 1 : 0, mb, cols);
    scrollbackNote.set_text(t);
}

void SettingsDialog::changed() {
    updateScrollbackNote();
    if (loading_) return;
    updateFrameSensitivity();
    updateProfileButtons();
    status.set_text("");
    ViewOptions o = options();
    if (o.profile != lastProfile_) {
        o.fontPx = 0;  // the new profile's size (pixel fonts look right only at their own sizes)
        lastProfile_ = o.profile;
    }
    win_.applySettings(o);
    syncFontPx(o.fontPx);
}

void SettingsDialog::syncFontPx(int overridePx) {
    TerminalView* v = win_.currentView();
    const Profile* p = findProfile(profile.get_active_id());
    autoPx_ = v && p && v->profile().id == p->id && v->autoFontPx() > 0 ? v->autoFontPx() : (p ? profileFont(*p).px : 0);
    const bool was = loading_;
    loading_ = true;
    fontPx.set_value(overridePx > 0 ? overridePx : autoPx_);
    loading_ = was;
}

}  // namespace lterm
