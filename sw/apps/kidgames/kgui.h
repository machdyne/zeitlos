#ifndef KGUI_H
#define KGUI_H

/*
 * kidgames -- the runtime layer. This is what src/ui.c was in the
 * original: everything the ten games are allowed to touch.
 *
 * The original's rule was "never call ncurses outside ui.c, so
 * terminal quirks stay in one place". The same rule applies here for a
 * stronger reason: the quirks are worse. Content-relative versus
 * absolute screen coordinates, a blitter and a rasterizer that do not
 * order against each other, a visible region that must be reloaded
 * before every draw, and two display modes with different origins.
 * Every one of those is a documented way to ship a broken panel
 * (docs/widgets.md, docs/window_manager.md, sw/common/tests/zrender.h).
 * A game file should see none of it.
 *
 * So: games draw in a 320x240 coordinate space with (0,0) at the top
 * left of the playfield, and this file is the only thing that knows
 * where that actually is.
 *
 * -- THE PUMP --
 *
 * The single most important function here is kg_getkey(). It replaces
 * the original's ui_getkey(), and it is the reason the ten games port
 * with their control flow intact: it BLOCKS until a key arrives,
 * exactly as ui_getkey() did.
 *
 * What it does while blocking is the Zeitlos part. It drains the
 * mailbox, applies Z_WM_SET_CLIP, answers Z_WM_REDRAW through the
 * repaint callback, acks it, turns pointer presses into keys where the
 * on-screen keyboard caught them, and sleeps with z_proc_wait() rather
 * than spinning -- an app that spins takes a full scheduler share from
 * whatever is in the foreground (docs/app_runtime.md, "Idling").
 *
 * The callback is not optional, for the same reason zdialog.c's is
 * not: a window that misses a Z_WM_SET_CLIP keeps drawing against a
 * region that is no longer true, and a window that has never had one
 * draws nothing at all, silently. Every screen that can be on display
 * while kg_getkey() blocks must register how to redraw itself.
 *
 * -- TWO MODES, ONE PICTURE --
 *
 * Game mode is a 320x240 camera over the 640x480 framebuffer
 * (docs/game_mode.md) and is the default, because pixel-doubled text
 * is readable from a sofa and 1:1 5x8 text is not. F2 drops to a
 * window of exactly the same content size (kg.h).
 *
 * Game mode here does NOT page-flip, which is worth saying out loud
 * because every other full-screen app in this tree does.
 * z_game_flip() exists for things that redraw the world every frame; a
 * quiz screen redraws one field when a letter is typed. Flipping would
 * mean the incremental draw landed on the page nobody is looking at,
 * so every partial update would have to become a full repaint --
 * slower, and a rewrite of ten games' worth of drawing for a tearing
 * risk that a static screen does not have. So this app draws straight
 * into page 0 in both modes, and the drawing code is genuinely
 * identical between them.
 *
 * Pages 1-3 stay free, which is where staged sprite art goes when the
 * blitter has no one-pass masked sprite -- see kgart.h.
 */

#include "kg.h"

/* -- lifecycle ------------------------------------------------------ */

/* Creates the window, probes for game mode and enters it if present.
 * Returns false if the window could not be created, which is fatal --
 * there is nowhere to draw. */
bool kg_init(void);
void kg_shutdown(void);

/* True while the 320x240 pixel-doubled viewport is in use. */
bool kg_in_game_mode(void);

/*
 * This app's one window.
 *
 * Exposed for the host harness, which has to stand a window up itself
 * (z_render_open, sw/common/tests/zrender.h) because kg_init() would
 * try to talk to a window manager that is not there. Nothing in the
 * app proper should need it -- if a game does, the drawing it wants is
 * missing from this header.
 *
 * void *, not z_win_t *, because z_win_t is a typedef of an anonymous
 * struct and cannot be forward declared -- and this header deliberately
 * does not include zwin.h, since a game that can see zwin.h can draw
 * with the wrong coordinate convention.
 */
void *kg_window(void);

/* F2. Silently does nothing if the bitstream has no game mode, having
 * already said so once on the console at startup. */
void kg_toggle_mode(void);

/* -- repaint callback -----------------------------------------------
 *
 * Set by whatever owns the screen right now, cleared by passing NULL.
 * The pump calls it on Z_WM_REDRAW and after leaving game mode.
 *
 * Save and restore it around anything that takes over the screen --
 * kg_message() does, so a message box does not leave the game beneath
 * it unable to repaint. */
typedef void (*kg_repaint_fn)(void *user);

void kg_set_repaint(kg_repaint_fn fn, void *user);
kg_repaint_fn kg_get_repaint(void **user_out);

/* Repaint now, through whatever callback is installed. */
void kg_repaint(void);

/* -- drawing --------------------------------------------------------
 *
 * All coordinates are playfield-relative: (0,0) is the top left of the
 * 320x240 area, in BOTH modes. Everything clips to the playfield, so a
 * rectangle that runs off the edge is cut rather than landing on the
 * desktop or on another window.
 */

void kg_clear(void);
void kg_fill(int x, int y, int w, int h, int color);

/* Ordered dither, 0 (black) to Z_SHADE_MAX (solid). Screen-aligned in
 * hardware, so adjacent shaded fills join seamlessly. Never the only
 * thing distinguishing two states -- see kg.h. */
void kg_shade(int x, int y, int w, int h, int level);

/*
 * An 8x8 repeating pattern, `pat` being eight rows of eight bits.
 *
 * Anchored to the SCREEN's 8-pixel grid in hardware, not to the
 * rectangle -- so two patterned fills side by side join seamlessly and
 * a patterned area does not shift when it is redrawn at a different
 * offset. That is the same screen-alignment property kg_shade() has,
 * and for the same reason.
 *
 * Used for card backs: a surface that carries no text, where a texture
 * says "there is something here you cannot see yet" more directly than
 * an empty box does. kg.h's second rule applies -- nothing patterned
 * may have text drawn on it.
 */
void kg_pattern(int x, int y, int w, int h, const uint8_t *pat);

/*
 * A 1bpp bitmap from main memory, drawn at (x,y). `stride` is the
 * source's row pitch in BYTES, in the framebuffer's own bit order --
 * which is what gen_art.py emits, so art from kgart.h goes straight
 * in with no repacking.
 *
 * The rectangle is CLEARED first and the bitmap COPIED over it, rather
 * than going through z_fb_hw_blit_sprite()'s data-plus-mask path.
 * Two reasons, and the second is the one that decides it:
 *
 *  - every piece in this app is drawn onto a background this app has
 *    just cleared, so there is nothing to mask against; and
 *
 *  - a masked sprite is TWO passes on a blitter without the one-pass
 *    cookie (zgfx.h), which leaves the sprite's footprint momentarily
 *    blank. Everything else here would flip that away on a back page;
 *    this app deliberately does not page-flip, so a two-pass sprite
 *    would punch a visible one-frame hole in the screen a kid is
 *    looking at.
 *
 * Returns false, having drawn nothing, on a bitstream whose blitter
 * predates this mode -- see kg_sprite_available().
 */
bool kg_sprite(int x, int y, const uint8_t *bits, int stride, int w, int h);

/* Probe once, not per draw. A binary built here may be run against an
 * older bitstream. */
bool kg_sprite_available(void);

/* A 1px frame, drawn as four FILLS rather than through the line
 * rasterizer.
 *
 * Deliberate: the blitter and the rasterizer do not order against each
 * other, and where rasterizer chrome and blitter glyphs share a 32-bit
 * VRAM word the chrome comes up partly missing -- position-dependent,
 * so it reads as a geometry bug (docs/widgets.md, "The blitter and the
 * rasterizer do not order against each other"). Nearly every frame in
 * this app has text inside it. A one-pixel fill is a perfectly good
 * line. */
void kg_frame(int x, int y, int w, int h, int color);

/* 5x8 text, the only font an app may use (wm owns glyph memory). Five
 * pixels of advance per character, so 64 columns across. */
void kg_text(int x, int y, const char *s, int color);

/* Text with an explicit background. Needed on any shaded or filled
 * surface: kg_text() paints a solid cell with a background of 0, so
 * ink 0 on cell 0 erases what it was drawn over rather than being
 * merely invisible (see z_win_draw_text2 in zwin.h). */
void kg_text2(int x, int y, const char *s, int fg, int bg);

int  kg_text_w(const char *s);
void kg_center_text(int y, const char *s, int color);
void kg_center_text2(int y, const char *s, int fg, int bg);

/* The header band: game name on the left, level and score on the
 * right, a rule underneath. Pass NULL for name to draw only the rule.
 * Numbers are formatted by hand -- see kg_utoa(). */
void kg_header(const char *name, int level, int score);

/* The bottom hint line, in the playfield's last text row. Only drawn
 * when the on-screen keyboard is hidden; with the keyboard up there is
 * no room, and the keyboard is itself the hint. */
void kg_status(const char *hint);

/*
 * "Tries left: * * *".
 *
 * A row of stars for remaining attempts reads at a glance for someone
 * who cannot yet read the number, which is the original's reasoning
 * and is why Number Guessing and Word Guess share this rather than
 * each drawing a counter. Word Guess uses it specifically so a
 * limited-guesses game needs no gallows.
 */
void kg_tries_left(int y, int tries_left, int tries_total);

/* -- number formatting ----------------------------------------------
 *
 * ONE printf with a conversion specifier links picolibc's formatter:
 * about 100KB, on an app that crashes on start if it outgrows the
 * loader's space (docs/app_runtime.md). This app has ten games all
 * wanting to show a score.
 *
 * So there is no printf here at all. These write into a caller's
 * buffer and return it, for use with kg_text().
 */
const char *kg_utoa(char *buf, int buflen, unsigned long v);
const char *kg_itoa(char *buf, int buflen, long v);

/* Appends `v` to `buf` (which must already hold a NUL-terminated
 * string), for building "SCORE 12" without a formatter. */
void kg_append(char *buf, int buflen, const char *s);
void kg_append_num(char *buf, int buflen, long v);

/* -- input ----------------------------------------------------------
 *
 * kg_getkey() blocks. kg_getkey_timeout() returns KG_KEY_TIMEOUT after
 * `ticks` (Z_TICK_HZ per second); 0 blocks forever.
 *
 * Only the keys the original allowed ever come out of these: arrows,
 * Enter, Escape, Backspace, Space, and letters and digits
 * (auto-uppercased). Everything else is swallowed, so a kid leaning on
 * the keyboard cannot get a game into a strange state. F2 is consumed
 * here too, as the mode toggle.
 */
kg_key_t kg_getkey(void);
kg_key_t kg_getkey_timeout(uint32_t ticks);

char kg_last_char(void);
int  kg_click_x(void);
int  kg_click_y(void);

/*
 * Down, Right or Space move to the next item; Up or Left to the
 * previous. Space is in there deliberately: reaching the arrow cluster
 * means finding two specific adjacent keys, which is not trivial for a
 * young kid, so one big easy-to-find key that always moves forward
 * matters. The original makes this point in ui.h and it is worth
 * keeping verbatim.
 *
 * Use these rather than writing the comparison out, so the convention
 * cannot drift between widgets.
 */
bool kg_key_is_next(kg_key_t k);
bool kg_key_is_prev(kg_key_t k);

/* Blocking pause, for "show this, then carry on" screens that must not
 * require reading an instruction. Keys pressed during it are
 * discarded, and wm messages are still serviced throughout -- a pause
 * that stopped answering redraws would leave a hole on the desktop for
 * its whole duration. */
void kg_pause_ms(int ms);

/* -- widgets --------------------------------------------------------
 *
 * Not a toolkit. Each one is something more than one game would
 * otherwise write twice.
 */

/* Runs a menu of `count` items; returns the index chosen, or -1 for
 * Escape. Click to choose directly. `title` may be NULL. */
int kg_menu(const char *title, const char *const *items, int count);

/* Modal message, up to KG_MSG_LINES lines. Returns true for Enter,
 * false for Escape. Restores the caller's repaint callback. */
#define KG_MSG_LINES 6
bool kg_message(const char *title, const char *const *lines, int nlines);

/*
 * A text field at (x,y), accepting at most maxlen characters from
 * `charset`. Returns true on Enter, false on Escape. `buf` needs
 * maxlen+1 bytes.
 *
 * Enter on an EMPTY field is ignored rather than submitted, so a true
 * return guarantees a non-empty answer and no caller has to special
 * case it. That is the original's behaviour and several games depend
 * on it.
 *
 * Shows the on-screen keyboard for `charset` while it runs, so a field
 * is fillable with the mouse alone.
 */
bool kg_input_line(int x, int y, char *buf, int maxlen, kg_charset_t charset);

#endif
