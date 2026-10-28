// SPDX-License-Identifier: GPL-3.0-or-later
// M2 item 1: fixes from Ted's review of M1 (session level; the view-level parts are in view_test).
#include "selection.hpp"
#include "vterm_session.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
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

// (a) The OSC title is capped at kMaxTitleBytes, without splitting UTF-8.
static void testTitleCap() {
    VtermSession s(4, 20);
    int titles = 0;
    s.onTitle = [&] { ++titles; };
    std::string big(1 << 20, 'x');
    s.feed("\x1b]0;");
    for (int i = 0; i < 4; ++i) s.feed(big);  // 4 MB in fragments
    s.feed("\a");
    CHECK(titles == 1);
    CHECK(s.title().size() == kMaxTitleBytes);
    // A multi-byte character that straddles the cap is dropped whole.
    s.feed("\x1b]2;" + std::string(kMaxTitleBytes - 1, 'y') + "\xc3\xa9" + "\x1b\\");
    CHECK(s.title() == std::string(kMaxTitleBytes - 1, 'y'));
    s.feed("\x1b]2;short \xc3\xa9\a");
    CHECK(s.title() == "short \xc3\xa9");
    std::string t = "ab\xe2\x82\xac";  // "ab€"
    truncateUtf8(t, 4);
    CHECK(t == "ab");
}

// (b) scrollback-lines is clamped.
static void testScrollbackClamp() {
    CHECK(clampScrollbackLines(-5) == 0);
    CHECK(clampScrollbackLines(0) == 0);
    CHECK(clampScrollbackLines(10000) == 10000);
    CHECK(clampScrollbackLines(LLONG_MAX) == kMaxScrollbackLines);
    CHECK(clampScrollbackLines(static_cast<long long>(kMaxScrollbackLines) + 1) == kMaxScrollbackLines);
    VtermSession s(2, 10);
    s.setScrollbackLimit(static_cast<size_t>(-1));
    CHECK(s.scrollbackLimit() == kMaxScrollbackLines);
    s.setScrollbackLimit(7);
    CHECK(s.scrollbackLimit() == 7);
}

// (c) C1 controls (UTF-8 U+0080-U+009F) and invalid bytes are dropped from pastes.
static void testPasteC1() {
    CHECK(sanitizePaste("a\xc2\x9b" "31mb") == "a31mb");          // U+009B CSI
    CHECK(sanitizePaste("x\xc2\x80y\xc2\x9fz") == "xyz");          // both ends of the C1 range
    CHECK(sanitizePaste("\xc2\xa0\xc2\xa9") == "\xc2\xa0\xc2\xa9"); // U+00A0 NBSP, U+00A9 kept
    CHECK(sanitizePaste("raw\x9b" "1m") == "raw1m");                // 8-bit C1 byte (invalid UTF-8)
    CHECK(sanitizePaste("\xc0\x9b" "x") == "x");                    // overlong ESC/CSI forms
    CHECK(sanitizePaste("\xe0\x80\x9b" "y") == "y");
    CHECK(sanitizePaste("\xed\xa0\x80" "s") == "s");                // surrogate
    CHECK(sanitizePaste("\xe2\x82\xac \xf0\x9f\x98\x80") == "\xe2\x82\xac \xf0\x9f\x98\x80");
    CHECK(sanitizePaste("cut\xe2\x82") == "cut");                   // truncated at the end
    CHECK(sanitizePaste("a\r\nb\nc\x1b[201~\x7f") == "a\rb\rc[201~");
    VtermSession s(4, 20);
    s.feed("\x1b[?2004h");
    s.paste("p\xc2\x9b" "201~q");
    CHECK(s.takeOutput() == "\x1b[200~p201~q\x1b[201~");
}

static int runToExit(VtermSession& s, std::string* out) {
    int status = -1;
    for (int i = 0; i < 1000; ++i) {
        pollfd p{s.masterFd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0 && (p.revents & (POLLHUP | POLLERR))) usleep(10000);
        s.readAvailable();
        if (waitpid(s.childPid(), &status, WNOHANG) == s.childPid()) {
            s.readAvailable();
            s.markExited();
            break;
        }
    }
    if (out) {
        out->clear();
        for (int r = 0; r < s.rows(); ++r) *out += s.rowText(r) + "\n";
    }
    return status;
}

// (e) Inherited fds are closed in the child, including ones above the old 1024 loop bound.
static void testCloseFds() {
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    int high = 5000;
    if (rl.rlim_cur <= static_cast<rlim_t>(high)) {
        rlimit want = rl;
        want.rlim_cur = std::min<rlim_t>(rl.rlim_max, 8192);
        if (setrlimit(RLIMIT_NOFILE, &want) != 0 || want.rlim_cur <= static_cast<rlim_t>(high)) high = 1000;
    }
    int a = open("/dev/null", O_RDONLY);  // no O_CLOEXEC on purpose
    CHECK(a >= 3);
    CHECK(dup2(a, 900) == 900);
    CHECK(dup2(a, high) == high);
    VtermSession s(6, 60);
    const std::string script = "for f in " + std::to_string(a) + " 900 " + std::to_string(high) +
                               "; do [ -e /proc/$$/fd/$f ] && echo OPEN$f; done; echo END";
    CHECK(s.spawn(std::vector<std::string>{"/bin/sh", "-c", script}));
    std::string out;
    int st = runToExit(s, &out);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(out.find("END") != std::string::npos);
    CHECK(out.find("OPEN") == std::string::npos);
    if (out.find("OPEN") != std::string::npos) std::fprintf(stderr, "fds transcript:\n%s", out.c_str());
    close(a);
    close(900);
    close(high);
    int fds[2];
    char c = 0;
    pid_t pid;
    // closeFrom3 itself, in a child.
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        int x = open("/dev/null", O_RDONLY);
        dup2(x, 77);
        closeFrom3(128);
        const bool ok = fcntl(77, F_GETFD) < 0 && fcntl(x, F_GETFD) < 0 && fcntl(fds[0], F_GETFD) < 0 &&
                        fcntl(1, F_GETFD) >= 0;
        ssize_t w = write(1, ok ? "Y" : "N", 1);
        (void)w;
        _exit(0);
    }
    close(fds[1]);
    CHECK(read(fds[0], &c, 1) == 1 && c == 'Y');
    close(fds[0]);
    waitpid(pid, nullptr, 0);
}

// (f) The soft-wrap flag survives multi-line scrolls (CSI S, CSI M) and shrinking resizes, so a
// copied wrapped line gets no extra newline.
static std::string selectAll(VtermSession& s, long from, long to) {
    Selection sel;
    sel.begin(s, SelPos{from, 0}, SelectMode::Line);
    sel.extend(s, SelPos{to, 0});
    return sel.text(s);
}

static void testWrapMultiScroll() {
    {   // CSI 3 S: three rows leave at once; row 0 wraps into row 1.
        VtermSession s(4, 10);
        s.feed("abcdefghijKLM\r\nnext");
        s.feed("\x1b[3S");
        CHECK(s.scrollbackSize() == 3);
        const long l0 = s.firstLine();
        CHECK(s.wrapped(l0));
        CHECK(!s.wrapped(l0 + 1));
        CHECK(selectAll(s, l0, l0 + 2) == "abcdefghijKLM\nnext");
    }
    {   // The sequence split across two reads. (Scrolling the whole screen, n >= rows, makes
        // libvterm erase instead of push, so stay below that.)
        VtermSession s(5, 10);
        s.feed("0123456789ABCDEFGHIJxyz\r\n");
        s.feed("\x1b[");
        s.feed("4S");
        const long l0 = s.firstLine();
        CHECK(s.wrapped(l0) && s.wrapped(l0 + 1) && !s.wrapped(l0 + 2));
        CHECK(selectAll(s, l0, l0 + 2) == "0123456789ABCDEFGHIJxyz");
    }
    {   // CSI M (delete lines) at the top row pushes lines too.
        VtermSession s(4, 10);
        s.feed("abcdefghijKL\r\nz\x1b[H\x1b[2M");
        const long l0 = s.firstLine();
        CHECK(s.scrollbackSize() == 2);
        CHECK(s.wrapped(l0) && !s.wrapped(l0 + 1));
    }
    {   // No false wraps: a full-width line ended by CRLF is not a continuation.
        VtermSession s(4, 10);
        s.feed("0123456789\r\nshort\r\n\x1b[2S");
        const long l0 = s.firstLine();
        CHECK(!s.wrapped(l0) && !s.wrapped(l0 + 1));
    }
    {   // Shrinking the height pushes several rows at once.
        VtermSession s(5, 10);
        s.feed("abcdefghijklmnopqrstUV\r\nend\r\nx");
        s.resize(1, 10);
        const long l0 = s.firstLine();
        CHECK(s.wrapped(l0) && s.wrapped(l0 + 1) && !s.wrapped(l0 + 2));
        CHECK(selectAll(s, l0, l0 + 3) == "abcdefghijklmnopqrstUV\nend");
    }
    {   // Ordinary one-line scrolling is unchanged.
        VtermSession s(2, 10);
        s.feed("abcdefghijKLM\r\n\r\n\r\n");
        const long l0 = s.firstLine();
        CHECK(s.wrapped(l0) && !s.wrapped(l0 + 1));
    }
}

// (g) hangup() reaps the child (no zombie), or hands back the pid when it does not exit.
static bool isZombieOrGone(pid_t pid, bool& gone) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE* f = std::fopen(path, "r");
    gone = !f;
    if (!f) return true;
    char buf[512] = {0};
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* p = std::strrchr(buf, ')');
    return p && p[1] == ' ' && p[2] == 'Z';
}

static void testHangupReaps() {
    pid_t pid;
    {
        VtermSession s(4, 20);
        CHECK(s.spawn(std::vector<std::string>{"/bin/sleep", "30"}));
        pid = s.childPid();
        usleep(100000);
        CHECK(s.hangup(2000) == -1);  // reaped
        CHECK(s.childPid() == -1);
    }
    int st;
    errno = 0;
    CHECK(waitpid(pid, &st, WNOHANG) == -1 && errno == ECHILD);  // nothing left to reap
    bool gone = false;
    CHECK(isZombieOrGone(pid, gone) && gone);
    {   // A child that ignores SIGHUP: hangup() returns its pid for the caller to reap.
        VtermSession s(4, 20);
        CHECK(s.spawn(std::vector<std::string>{"/bin/sh", "-c", "trap '' HUP; exec sleep 30"}));
        usleep(200000);
        pid = s.childPid();
        const pid_t left = s.hangup(50);
        CHECK(left == pid);
        kill(pid, SIGKILL);
        CHECK(waitpid(pid, &st, 0) == pid);
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    testTitleCap();
    testScrollbackClamp();
    testPasteC1();
    testCloseFds();
    testWrapMultiScroll();
    testHangupReaps();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("fixes tests passed\n");
    return 0;
}
