/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Differential test for third_party/libvterm-patches: drives libvterm with a deterministic,
 * seeded random stream and writes everything observable to stdout: every screen callback
 * (damage, moverect, movecursor, settermprop, bell, resize, scrollback push/pop/clear, with the
 * pushed cells), and at checkpoints a dump of every cell (all code points, width, attributes,
 * colours), each row's line info, the screen text, the cursor and the scrollback. The same
 * source is linked against two builds of libvterm (stock and patched) and
 * tools/libvterm_diff_run.py checks that the two outputs are byte-identical.
 *
 * Usage: libvterm_diff SEED STEPS MODE
 *   MODE a: stock vs patched. REP never repeats a wide glyph, and the screen is never 1 column
 *           wide (a wide glyph there or REP of one at the margin crash stock 0.3.3: patch 0001).
 *   MODE b: everything, including those (stock + 0001 vs all patches).
 *
 * The stream also avoids the other stock libvterm bugs that the session guards against
 * (docs/FUZZING.md): never more than 16 CSI arguments (F1) or 9 digits (F2), REP only right after
 * a fresh glyph (F7), cursor put back on the screen after a restore (F9), margins reset after a
 * resize (F10), no ESC + C0 inside strings (F15), no alternate screen with reflow (F17), and
 * before a resize row 0's continuation flag cleared and the F3 reflow abort predicted, as
 * VtermSession::resize() does. Two more stock 0.3.3 bugs found while writing this test (both
 * builds; sessions cannot reach them): more than 5 combining marks after one base, or marks
 * that start a later write after the cursor moved, give a glyph of width 0 that REP repeats
 * forever (F7 variants); and a double-width line on a 1-column screen writes through a NULL cell.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vterm.h"

static uint64_t rng_state;
static uint64_t rnd(void) { /* splitmix64 */
  uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static int rn(int n) { return n <= 0 ? 0 : (int)(rnd() % (uint64_t)n); }      /* 0 .. n-1 */
static int rr(int lo, int hi) { return lo + rn(hi - lo + 1); }                 /* lo .. hi */
static int chance(int pct) { return rn(100) < pct; }

static FILE *out;
static VTerm *vt;
static VTermScreen *scr;
static VTermState *st;
static int rows, cols;
static char mode;

/* ---- output ---- */
static void put_color(const VTermColor *c) {
  if (VTERM_COLOR_IS_INDEXED(c)) fprintf(out, "i%x.%x", c->type, c->indexed.idx);
  else fprintf(out, "r%x.%02x%02x%02x", c->type, c->rgb.red, c->rgb.green, c->rgb.blue);
}
static void put_cell(const VTermScreenCell *c) {
  for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c->chars[i]; i++) fprintf(out, "%x,", c->chars[i]);
  const VTermScreenCellAttrs *a = &c->attrs;
  unsigned bits = a->bold | a->underline << 1 | a->italic << 3 | a->blink << 4 | a->reverse << 5 | a->conceal << 6 |
                  a->strike << 7 | a->font << 8 | a->dwl << 12 | a->dhl << 13 | a->small << 15 | a->baseline << 16;
  fprintf(out, "w%d a%x ", c->width, bits);
  put_color(&c->fg);
  fputc(' ', out);
  put_color(&c->bg);
  fputc(';', out);
}
static void put_rect(const char *what, VTermRect r) {
  fprintf(out, "%s %d,%d-%d,%d\n", what, r.start_row, r.start_col, r.end_row, r.end_col);
}

/* ---- scrollback (a stack with a limit, like the session) ---- */
typedef struct { int cols; VTermScreenCell *cells; } SbLine;
static SbLine *sb;
static int sb_len, sb_cap, sb_limit;

static void sb_drop_oldest(void) {
  free(sb[0].cells);
  memmove(sb, sb + 1, sizeof(SbLine) * (size_t)(sb_len - 1));
  sb_len--;
}

/* ---- callbacks ---- */
static int cb_damage(VTermRect r, void *u) { (void)u; put_rect("damage", r); return 1; }
static int cb_moverect(VTermRect d, VTermRect s, void *u) {
  (void)u;
  put_rect("moverect dest", d);
  put_rect("moverect src", s);
  return 1;
}
static int cb_movecursor(VTermPos p, VTermPos o, int vis, void *u) {
  (void)u;
  fprintf(out, "movecursor %d,%d from %d,%d vis %d\n", p.row, p.col, o.row, o.col, vis);
  return 1;
}
static int cb_settermprop(VTermProp prop, VTermValue *val, void *u) {
  (void)u;
  fprintf(out, "termprop %d ", prop);
  switch (vterm_get_prop_type(prop)) {
  case VTERM_VALUETYPE_BOOL: fprintf(out, "b%d", val->boolean); break;
  case VTERM_VALUETYPE_INT: fprintf(out, "n%d", val->number); break;
  case VTERM_VALUETYPE_STRING:
    fprintf(out, "s%d%d %zu:", val->string.initial, val->string.final, (size_t)val->string.len);
    fwrite(val->string.str, 1, val->string.len, out);
    break;
  case VTERM_VALUETYPE_COLOR: put_color(&val->color); break;
  default: fprintf(out, "?");
  }
  fputc('\n', out);
  return 1;
}
static int cb_bell(void *u) { (void)u; fputs("bell\n", out); return 1; }
static int cb_resize(int r, int c, void *u) { (void)u; fprintf(out, "resize-cb %d %d\n", r, c); return 1; }
static int cb_pushline(int ncols, const VTermScreenCell *cells, void *u) {
  (void)u;
  fprintf(out, "pushline %d:", ncols);
  for (int i = 0; i < ncols; i++) put_cell(&cells[i]);
  fputc('\n', out);
  if (sb_limit == 0) return 1;
  if (sb_len == sb_limit) sb_drop_oldest();
  if (sb_len == sb_cap) {
    sb_cap = sb_cap ? sb_cap * 2 : 64;
    sb = realloc(sb, sizeof(SbLine) * (size_t)sb_cap);
  }
  sb[sb_len].cols = ncols;
  sb[sb_len].cells = malloc(sizeof(VTermScreenCell) * (size_t)(ncols ? ncols : 1));
  memcpy(sb[sb_len].cells, cells, sizeof(VTermScreenCell) * (size_t)ncols);
  sb_len++;
  return 1;
}
static int cb_popline(int ncols, VTermScreenCell *cells, void *u) {
  (void)u;
  if (!sb_len) {
    fprintf(out, "popline %d none\n", ncols);
    return 0;
  }
  SbLine *l = &sb[sb_len - 1];
  for (int i = 0; i < ncols; i++) {
    if (i < l->cols) cells[i] = l->cells[i];
    else {
      memset(&cells[i], 0, sizeof cells[i]);
      cells[i].width = 1;
      vterm_state_get_default_colors(st, &cells[i].fg, &cells[i].bg);
    }
  }
  fprintf(out, "popline %d of %d\n", ncols, l->cols);
  free(l->cells);
  sb_len--;
  return 1;
}
static int cb_sbclear(void *u) {
  (void)u;
  fputs("sb_clear\n", out);
  while (sb_len) sb_drop_oldest();
  return 1;
}

static VTermScreenCallbacks callbacks = {
  .damage = cb_damage, .moverect = cb_moverect, .movecursor = cb_movecursor, .settermprop = cb_settermprop,
  .bell = cb_bell, .resize = cb_resize, .sb_pushline = cb_pushline, .sb_popline = cb_popline, .sb_clear = cb_sbclear,
};

/* ---- the dump ---- */
static void dump(const char *why) {
  vterm_screen_flush_damage(scr);
  VTermPos cur;
  vterm_state_get_cursorpos(st, &cur);
  fprintf(out, "== dump %s %dx%d cursor %d,%d\n", why, rows, cols, cur.row, cur.col);
  for (int r = 0; r < rows; r++) {
    const VTermLineInfo *li = vterm_state_get_lineinfo(st, r);
    fprintf(out, "row %d dw%d dh%d c%d |", r, li->doublewidth, li->doubleheight, li->continuation);
    for (int c = 0; c < cols; c++) {
      VTermScreenCell cell;
      memset(&cell, 0, sizeof cell);
      if (!vterm_screen_get_cell(scr, (VTermPos){r, c}, &cell)) fputs("!", out);
      else put_cell(&cell);
    }
    fputc('\n', out);
  }
  static char text[1 << 16];
  size_t n = vterm_screen_get_text(scr, text, sizeof text, (VTermRect){0, rows, 0, cols});
  fprintf(out, "text %zu:", n);
  fwrite(text, 1, n < sizeof text ? n : sizeof text, out);
  fputc('\n', out);
  for (int k = 0; k < 4; k++) {
    VTermPos p = {rn(rows), rn(cols)};
    VTermRect ext = {.start_row = 0, .end_row = 0, .start_col = -1, .end_col = -1};
    int e = vterm_screen_get_attrs_extent(scr, &ext, p, VTERM_ALL_ATTRS_MASK);
    fprintf(out, "eol %d,%d %d extent %d ", p.row, p.col, vterm_screen_is_eol(scr, p), e);
    put_rect("", ext);
  }
  fprintf(out, "scrollback %d\n", sb_len);
}

/* ---- the stream ---- */
static char buf[1 << 16];
static size_t blen;
static int reflow, altscreen_ok, lrmm;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void emit(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + blen, sizeof buf - blen, fmt, ap);
  va_end(ap);
  if (n > 0 && blen + (size_t)n < sizeof buf) blen += (size_t)n;
}
static void emit_cp(uint32_t cp) {
  char *p = buf + blen;
  if (blen + 4 >= sizeof buf) return;
  if (cp < 0x80) *p++ = (char)cp;
  else if (cp < 0x800) { *p++ = (char)(0xC0 | cp >> 6); *p++ = (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { *p++ = (char)(0xE0 | cp >> 12); *p++ = (char)(0x80 | (cp >> 6 & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
  else { *p++ = (char)(0xF0 | cp >> 18); *p++ = (char)(0x80 | (cp >> 12 & 0x3F)); *p++ = (char)(0x80 | (cp >> 6 & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
  blen = (size_t)(p - buf);
}
/* Feeds the buffer in random pieces (the PTY delivers arbitrary chunks). */
static void flush_input(void) {
  size_t off = 0;
  while (off < blen) {
    size_t n = chance(70) ? blen - off : (size_t)rr(1, (int)(blen - off));
    vterm_input_write(vt, buf + off, n);
    off += n;
  }
  blen = 0;
}

static const uint32_t wide_cps[] = {0x4E00, 0x4E2D, 0x6587, 0x5B57, 0xAC00, 0xD55C, 0xFF21, 0xFF41, 0x3042, 0x30AB};
static const uint32_t narrow_cps[] = {0xE9, 0xF1, 0x3B1, 0x416, 0x2500, 0x2588, 0x20AC, 0x1F600, 0x2603, 0xFFFD};
static const uint32_t comb_cps[] = {0x301, 0x308, 0x323, 0x20D7, 0x302, 0x316, 0xFE0F, 0x30A};

static void op_text(int kind) {
  int n = kind == 0 ? rr(1, 3 * cols + 5) : rr(1, 40);
  /* Combining marks only right after a base glyph, at most 5 per glyph (on_text() cuts a glyph
   * at VTERM_MAX_CHARS_PER_CELL code points and makes the rest a glyph of width 0). More than
   * 6 go through op_split_combining(). */
  int after_base = 0, marks = 0;
  for (int i = 0; i < n; i++) {
    int k = rn(100);
    const size_t before = blen;
    if (kind == 0 || k < 45) {
      emit_cp((uint32_t)rr(0x21, 0x7E));
    } else if (k < 60 && (mode == 'b' || cols >= 2)) {
      emit_cp(wide_cps[rn(10)]);
    } else if (k < 75) {
      uint32_t cp = narrow_cps[rn(10)];
      emit_cp(cp);
    } else if (k < 88 && after_base && marks < 5) {
      int m = rr(1, 5 - marks);
      for (int j = 0; j < m; j++) emit_cp(comb_cps[rn(8)]);
      marks += m;
      continue;
    } else if (k < 92) {
      emit_cp(' ');
    } else if (k < 96) {
      static const char ctl[] = {'\t', '\b', '\r', '\a', '\n', 0x0b, 0x0c};
      emit("%c", ctl[rn(7)]);
    } else if (k < 98) {
      emit("%c", (char)rr(0x80, 0xBF));  /* stray UTF-8 continuation: U+FFFD */
    } else {
      emit_cp((uint32_t)rr(0xA1, 0x2FF));
    }
    /* A base glyph was just written (not a control, not a combining mark). */
    after_base = blen > before && (unsigned char)buf[before] >= 0x20;
    marks = 0;
  }
}

/* A glyph, then combining marks in a later write: libvterm appends them to the glyph it printed
 * (any number; the screen keeps VTERM_MAX_CHARS_PER_CELL). */
static void op_split_combining(void) {
  emit_cp(chance(30) && (mode == 'b' || cols >= 2) ? wide_cps[rn(10)] : (uint32_t)rr(0x41, 0x5A));
  flush_input();
  int m = rr(1, 9);
  for (int j = 0; j < m; j++) emit_cp(comb_cps[rn(8)]);
  flush_input();
}

static void op_sgr(void) {
  /* At most 16 arguments in all (F1); 38;2;r;g;b counts 5. */
  int n = rr(1, 4), args = 0;
  emit("\x1b[");
  for (int i = 0; i < n && args <= 11; i++) {
    static const int simple[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 21, 22, 23, 24, 25, 27, 28, 29, 39, 49, 53, 55, 73, 74, 75};
    int k = rn(6);
    if (i) emit(";");
    if (k == 0) { emit("%d", simple[rn(27)]); args += 1; }
    else if (k == 1) { emit("%d", rr(30, 37) + (chance(50) ? 10 : 0)); args += 1; }
    else if (k == 2) { emit("%d", rr(90, 97) + (chance(50) ? 10 : 0)); args += 1; }
    else if (k == 3) { emit("%d;5;%d", chance(50) ? 38 : 48, rn(256)); args += 3; }
    else if (k == 4) { emit("%d;2;%d;%d;%d", chance(50) ? 38 : 48, rn(256), rn(256), rn(256)); args += 5; }
    else { emit("4:%d", rn(4)); args += 2; }
  }
  emit("m");
}

static void after_restore(void) {
  /* F9: libvterm keeps a saved cursor that no longer fits after a shrink. */
  emit("\x1b[%d;%dH", rr(1, rows), rr(1, cols));
}

/* libvterm 0.3.3's reflow aborts ("screen_resize failed to update cursor position") when the
 * cursor is on the last row of a line that no longer fits (F3). Same walk as
 * VtermSession::reflowWouldAbort() (src/vterm_session.cpp); then resize once without reflow. */
static int cont(int r) {
  const VTermLineInfo *li = vterm_state_get_lineinfo(st, r);
  return li && li->continuation;
}
static int popcount(int r) {
  VTermScreenCell cell;
  int c = cols - 1;
  while (c >= 0 && vterm_screen_get_cell(scr, (VTermPos){r, c}, &cell) && cell.chars[0] == 0) --c;
  return c + 1;
}
static int reflow_would_abort(int new_rows, int new_cols) {
  VTermPos cur;
  vterm_state_get_cursorpos(st, &cur);
  int old_row = rows - 1, new_row = new_rows - 1, final_blank = new_rows, found = 0;
  while (old_row >= 0) {
    const int end = old_row;
    while (old_row >= 0 && cont(old_row)) --old_row;
    const int start = old_row;
    if (start < 0) return 0;
    long width = 0;
    for (int r = start; r <= end; ++r) width += (r < rows - 1 && cont(r + 1)) ? cols : popcount(r);
    if (final_blank == new_row + 1 && width == 0) final_blank = new_row;
    const int height = width ? (int)((width + new_cols - 1) / new_cols) : 1;
    int new_start = new_row - height + 1;
    const int spare = new_rows - final_blank;
    if (new_start < 0 && spare >= 0 && !found) {
      const int down = -new_start < spare ? -new_start : spare;
      new_start += down;
      final_blank += down;
    }
    if (new_start < 0) {
      if (start <= cur.row && cur.row < end) found = 1;
      break;
    }
    if (cur.row >= start && cur.row <= end) found = 1;
    old_row = start - 1;
    new_row = new_start - 1;
  }
  if (cur.row <= old_row) found = 1;
  return !found;
}

static void op_resize(void) {
  int nr, nc;
  /* One column only in mode b (F8 and REP of a wide glyph there crash stock: patch 0001), and
   * only with reflow on, which keeps the alternate screen out of the run (see below). */
  if (chance(10)) { nr = rr(1, 3); nc = rr(mode == 'a' || !reflow ? 2 : 1, 4); }
  else { nr = rr(2, 45); nc = rr(2, 110); }
  /* A double-width line on a 1-column screen is 0 cells wide, and libvterm writes through a
   * NULL cell (screen.c:190 in stock, erase_internal() under insert mode with 0001; stock 0.3.3
   * bug, sessions are at least 2 columns). So every row goes back to single width first. */
  if (nc < 2) {
    emit("\x1b[r\x1b[?6l");  /* margins and origin mode off, so the CUPs reach every row */
    for (int r = 1; r <= rows; r++) emit("\x1b[%d;1H\x1b#5", r);
  }
  flush_input();
  /* A wrapped line that scrolled up leaves row 0 marked as a continuation, and the reflow then
   * reads row -1 (heap overflow in resize_buffer(), screen.c:616; F17 on the alternate
   * screen). VtermSession::clearTopContinuation() does this before every resize. */
  const VTermLineInfo *li = vterm_state_get_lineinfo(st, 0);
  if (li && li->continuation) ((VTermLineInfo *)li)->continuation = 0;
  const int fallback = reflow && reflow_would_abort(nr, nc);
  fprintf(out, "-- resize %d %d%s\n", nr, nc, fallback ? " without reflow" : "");
  if (fallback) vterm_screen_enable_reflow(scr, 0);
  vterm_set_size(vt, nr, nc);
  if (fallback) vterm_screen_enable_reflow(scr, 1);
  rows = nr;
  cols = nc;
  /* F10: margins can be left inverted by a shrink; reset them (cursor goes home). */
  emit("\x1b[r");
  if (lrmm) emit("\x1b[s");
  flush_input();
}

static void step(void) {
  int k = rn(1000);
  if (k < 250) op_text(0);
  else if (k < 380) op_text(1);
  else if (k < 470) {  /* line floods: full-screen scrolls */
    int n = rr(1, 3 * rows + 2);
    for (int i = 0; i < n; i++) {
      if (chance(30)) op_text(0);
      emit(chance(50) ? "\r\n" : "\n");
    }
  } else if (k < 530) op_sgr();
  else if (k < 580) emit("\x1b[%d;%dH", rr(0, rows + 2), rr(0, cols + 2));
  else if (k < 620) {  /* DECSTBM */
    int r = rn(4);
    if (r == 0) emit("\x1b[r");
    else if (r == 1) emit("\x1b[1;%dr", rows);  /* explicit full screen */
    else { int t = rr(1, rows), b = rr(t, rows); emit("\x1b[%d;%dr", t, b); }
  } else if (k < 645) {  /* DECLRMM + DECSLRM */
    int r = rn(4);
    if (r == 0) { lrmm = !lrmm; emit(lrmm ? "\x1b[?69h" : "\x1b[?69l"); }
    else if (lrmm) {
      if (r == 1) emit("\x1b[s");
      else if (r == 2) emit("\x1b[1;%ds", cols);
      else { int l = rr(1, cols), rt = rr(l, cols); emit("\x1b[%d;%ds", l, rt); }
    }
  } else if (k < 680) {
    static const char sud[] = {'S', 'T', 'L', 'M'};
    int n = chance(10) ? rr(rows, 999) : rr(0, rows);
    emit("\x1b[%d%c", n, sud[rn(4)]);
  } else if (k < 710) {
    static const char ed[] = {'@', 'P', 'X'};
    emit("\x1b[%d%c", chance(10) ? rr(cols, 999) : rr(0, cols), ed[rn(3)]);
  } else if (k < 740) {
    if (chance(50)) emit("\x1b[%dJ", rn(4)); else emit("\x1b[%dK", rn(3));
  } else if (k < 770) {
    static const char *ind[] = {"\x1b" "D", "\x1b" "M", "\x1b" "E"};
    int n = rr(1, rows + 2);
    for (int i = 0; i < n; i++) emit("%s", ind[rn(3)]);
  } else if (k < 810) {
    /* REP repeats the last glyph by its width: forever when that width is 0 (F7: nothing
     * printed, more than 5 combining marks, or marks that start a later write after the cursor
     * moved). So REP always follows a fresh glyph; in mode a never a wide one (patch 0001). */
    if (mode == 'b' && chance(40)) emit_cp(wide_cps[rn(10)]);
    else emit_cp((uint32_t)rr(0x21, 0x7E));
    if (chance(25)) emit("\x1b[%d;%dH", rr(1, rows), rr(1, cols));
    else if (chance(10)) emit("\r");
    emit("\x1b[%db", chance(15) ? rr(cols, 999) : rr(0, cols));
  } else if (k < 830) {
    if (altscreen_ok) {
      static const char *alt[] = {"\x1b[?1049h", "\x1b[?1049l", "\x1b[?47h", "\x1b[?47l", "\x1b[?1047h", "\x1b[?1047l"};
      int a = rn(6);
      emit("%s", alt[a]);
      if (a == 1) after_restore();
    }
  } else if (k < 850) {
    static const char *sv[] = {"\x1b" "7", "\x1b" "8", "\x1b[?1048h", "\x1b[?1048l"};
    int a = rn(4);
    emit("%s", sv[a]);
    if (a == 1 || a == 3) after_restore();
  } else if (k < 880) {
    static const char *modes[] = {"\x1b[?7h", "\x1b[?7l", "\x1b[?6h", "\x1b[?6l", "\x1b[4h", "\x1b[4l", "\x1b[?25l", "\x1b[?25h", "\x1b[20h", "\x1b[20l", "\x1b[?5h", "\x1b[?5l"};
    emit("%s", modes[rn(12)]);
  } else if (k < 890) {
    static const char *dw[] = {"\x1b#3", "\x1b#4", "\x1b#5", "\x1b#6"};
    emit("%s", cols < 2 ? "\x1b#5" : dw[rn(4)]);  /* see op_resize() */
  } else if (k < 910) {
    int n = rr(1, 8);
    for (int i = 0; i < n; i++) emit("%c", "\t\b\r\x0e\x0f"[rn(5)]);
  } else if (k < 922) op_resize();
  else if (k < 925) {
    flush_input();
    if (chance(50)) { emit("\x1b" "c"); lrmm = 0; }
    else { fputs("-- screen_reset\n", out); vterm_screen_reset(scr, rn(2)); }
  } else if (k < 935) { flush_input(); vterm_screen_flush_damage(scr); }
  else if (k < 945) {
    static const char *cs[] = {"\x1b(0", "\x1b(B", "\x1b)0", "\x1b(A"};
    emit("%s", cs[rn(4)]);
  } else if (k < 950) emit("\x1b#8");  /* DECALN */
  else if (k < 960) {
    emit("\x1b[%d\"q", rn(3));         /* DECSCA */
    if (chance(50)) emit("\x1b[?%dJ", rn(3)); else emit("\x1b[?%dK", rn(3));
  } else if (k < 968) {
    emit("\x1b]%d;", chance(50) ? 0 : 2);
    int n = rr(0, 30);
    for (int i = 0; i < n; i++) emit("%c", (char)rr(0x20, 0x7E));
    emit("\a");
  } else if (k < 980) {
    static const char mv[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'd', 'e', 'a', '`', 'I', 'Z'};
    emit("\x1b[%d%c", rr(0, cols + 2), mv[rn(13)]);
  } else if (k < 985) { flush_input(); dump("step"); }
  else if (k < 990) op_split_combining();
  else emit("\x1b[%d q", rn(7));       /* DECSCUSR */
  if (chance(40)) flush_input();
}

int main(int argc, char **argv) {
  if (argc != 4 || (argv[3][0] != 'a' && argv[3][0] != 'b')) {
    fprintf(stderr, "usage: %s SEED STEPS a|b\n", argv[0]);
    return 2;
  }
  const unsigned long long seed = strtoull(argv[1], NULL, 10);
  const long steps = strtol(argv[2], NULL, 10);
  mode = argv[3][0];
  rng_state = seed * 0x2545F4914F6CDD1Dull + 1;
  out = stdout;
  static char obuf[1 << 20];
  setvbuf(out, obuf, _IOFBF, sizeof obuf);

  rows = chance(20) ? rr(1, 5) : rr(5, 45);
  cols = chance(20) ? rr(mode == 'a' ? 2 : 1, 8) : rr(8, 110);
  reflow = chance(50);
  altscreen_ok = !reflow;  /* F17 */
  int merge = rn(4);
  sb_limit = chance(20) ? 0 : (chance(50) ? rr(1, 50) : 100000);
  fprintf(out, "seed %llu mode %c %dx%d reflow %d altscreen %d merge %d sb %d\n", seed, mode, rows, cols, reflow,
          altscreen_ok, merge, sb_limit);

  vt = vterm_new(rows, cols);
  vterm_set_utf8(vt, 1);
  scr = vterm_obtain_screen(vt);
  st = vterm_obtain_state(vt);
  vterm_screen_set_callbacks(scr, &callbacks, NULL);
  vterm_screen_set_damage_merge(scr, (VTermDamageSize)merge);
  vterm_screen_enable_reflow(scr, reflow);
  if (altscreen_ok) vterm_screen_enable_altscreen(scr, 1);
  vterm_screen_reset(scr, 1);

  for (long i = 0; i < steps; i++) {
    if (i % 64 == 0) fprintf(out, "-- step %ld\n", i);
    step();
  }
  flush_input();
  dump("final");
  for (int i = 0; i < sb_len; i++) {
    fprintf(out, "sb %d %d:", i, sb[i].cols);
    for (int c = 0; c < sb[i].cols; c++) put_cell(&sb[i].cells[c]);
    fputc('\n', out);
  }
  fputs("end\n", out);
  fflush(out);
  while (sb_len) sb_drop_oldest();
  free(sb);
  vterm_free(vt);
  return 0;
}
