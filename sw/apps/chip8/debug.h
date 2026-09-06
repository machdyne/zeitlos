#ifndef CHIP8_DEBUG_H
#define CHIP8_DEBUG_H

/*
 * chip8 -- the debugger pane.
 *
 * Registers, a live disassembly around PC, and the run/pause state,
 * drawn as text below the guest image.
 *
 * -- why a pane and not a second window --
 *
 * A second window is the obvious shape and it does not work here.
 * Z_WM_SET_CLIP (zwm.h) carries no window id: an app that owns two
 * windows cannot tell which one a visible-region update is for, so
 * whichever arrived last would win and one window would happily draw
 * over the other. zdialog.c gets away with it because its windows are
 * modal and strictly one at a time; a debugger that is useful is
 * neither.
 *
 * Growing the one window costs nothing extra -- the app already
 * rebuilds its window when the scale changes -- and sidesteps the
 * question entirely.
 *
 * -- why this keeps a copy of what it drew --
 *
 * The first version cleared the pane with a fill and redrew every line
 * over the top, sixty times a second. On a framebuffer that is scanned
 * out directly, with no back buffer anywhere, that is a clear and a
 * redraw the display can catch mid-way: the pane visibly flashed.
 *
 * There is no double buffer to reach for in a window, so the fix is to
 * never blank anything. z_font_5x8 is resident in glyph memory (wm
 * loads it at startup), so z_win_draw_text() goes through the hardware
 * glyph blitter, which paints a SOLID CELL -- background included.
 * Every character drawn therefore erases exactly what was under it, in
 * the same operation, with no blank state in between.
 *
 * That makes the fill unnecessary, provided every line is padded to a
 * fixed width so a shortened line overwrites its own tail. And once
 * nothing is being blanked, the remaining work is to not redraw what
 * has not changed: the cache below is diffed per CHARACTER and only
 * runs of changed cells are drawn. In practice that is the few digits
 * of a register that moved, not 500 glyph blits a frame.
 */

#include <stdbool.h>

#include "../../common/zwin.h"

#include "core.h"

/* Panel geometry. COLS is set by the widest line -- the register line
 * -- and W follows from it at 5 pixels a character. */
#define C8_DBG_COLS  47
#define C8_DBG_W     (C8_DBG_COLS * 5)
#define C8_DBG_LINES 13
#define C8_DBG_H     (C8_DBG_LINES * 8)

/* Instructions of disassembly shown below the register lines. */
#define C8_DBG_DIS_LINES 8

/* Draw the panel at (x, y) in window-content coordinates.
 *
 * `force` redraws every cell regardless of the cache -- pass it after
 * anything that changed the pixels without going through here: a
 * Z_WM_REDRAW, a window move, a rebuilt window.
 */
void c8_debug_draw(const z_win_t *win, int x, int y, const c8_t *vm,
	bool paused, int ipf, const char *profile_name, bool force);

/* Forget what is on screen. Call when the pane stops being visible, so
 * the next time it is shown it does not diff against a cache that
 * describes pixels nobody can see any more. */
void c8_debug_invalidate(void);

#endif
