// SPDX-License-Identifier: GPL-3.0-or-later
// libvterm + PTY session: owns the VTerm, the child shell and the master fd.
#pragma once

#include "grid.hpp"

#include <vterm.h>

#include <array>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace lterm {

class VtermSession {
public:
    VtermSession(int rows, int cols);
    ~VtermSession();
    VtermSession(const VtermSession&) = delete;
    VtermSession& operator=(const VtermSession&) = delete;

    // Starts argv (or $SHELL / /bin/sh when empty) on a new PTY. Returns false on failure.
    bool spawn(const std::vector<std::string>& argv);
    int masterFd() const { return fd_; }
    pid_t childPid() const { return pid_; }

    // Reads whatever is available on the PTY and feeds libvterm. Returns false on EOF/HUP.
    bool readAvailable();
    // Feeds bytes directly (used by tests; no PTY needed).
    void feed(const char* bytes, size_t len);

    void resize(int rows, int cols);  // vterm_set_size + TIOCSWINSZ
    int rows() const { return rows_; }
    int cols() const { return cols_; }

    // Keyboard input (GDK keyval mapping lives in the view).
    void sendKey(VTermKey key, VTermModifier mod);
    void sendChar(uint32_t codepoint, VTermModifier mod);
    void sendBytes(const char* bytes, size_t len);

    void fillGrid(Grid& g) const;
    // Colours used by fillGrid(): default fg/bg, ANSI 0-15, and a minimum grey level applied to
    // every other foreground colour (see Profile).
    void setColors(Color8 fg, Color8 bg, const std::array<Color8, 16>& palette, float minFgGrey);
    std::string rowText(int row) const;

    bool takeDamage() {
        bool d = damaged_;
        damaged_ = false;
        return d;
    }
    const std::string& title() const { return title_; }
    std::function<void()> onTitle;
    std::function<void()> onBell;

private:
    Color8 mapColor(VTermColor c, bool foreground) const;
    static int cbDamage(VTermRect, void* user);
    static int cbMoveCursor(VTermPos pos, VTermPos, int visible, void* user);
    static int cbSetTermProp(VTermProp prop, VTermValue* val, void* user);
    static int cbBell(void* user);
    static void cbOutput(const char* s, size_t len, void* user);

    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
    int rows_, cols_;
    int fd_ = -1;
    pid_t pid_ = -1;
    bool damaged_ = true;
    VTermPos cursor_{0, 0};
    bool cursorVisible_ = true;
    std::string title_, titleBuf_;
    std::string pendingOut_;
    Color8 fg_{208, 208, 208}, bg_{0, 0, 0};
    std::array<Color8, 16> palette_{};
    bool havePalette_ = false;
    float minFgGrey_ = 0.0f;
};

}  // namespace lterm
