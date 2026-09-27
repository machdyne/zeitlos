#ifndef WORKSPACE_H
#define WORKSPACE_H

/*
 * Zeitlos -- sw/apps/wm
 *
 * Workspaces: the decisions, apart from wm's window table so that they
 * can be tested on the build machine (tests/test_workspace.c). What wm
 * does with them -- hiding, showing, focus -- is in wm.c, "-- workspaces
 * --". docs/window_manager.md, "Workspaces".
 */

#include <stdint.h>
#include <stdbool.h>

// Ten: Super+1 .. Super+9 and Super+0. A workspace costs a byte for its
// remembered focus; each window, one byte for the workspace it is on.
#define WS_COUNT  10

// The workspace `dir` steps from `ws` (+1 next, -1 previous), wrapping:
// after the last comes the first, before the first the last.
int ws_step(int ws, int dir);

// The workspace a digit key selects, by its HID usage -- the PHYSICAL
// key, as wm's other shortcuts are (on AZERTY the digit row needs Shift
// for its digits, and a character match would miss it): 1..9 are 0..8,
// 0 is 9 -- the tenth, at the end of the row as it is on the keyboard.
// -1 for any other key.
int ws_for_digit_usage(uint16_t usage);

// Which windows go when window `idx` moves to another workspace, as a
// bit per window slot. The window itself -- and, if its owner has a
// modal window open (or it IS that modal), every window of that owner:
// wm blocks all of an owner's windows while its modal is up
// (blocked_by_modal()), so a dialog left on one workspace would hold
// its app's window hostage on another. `n` slots of `used`, `owner`
// and `modal` (Z_WIN_FLAG_MODAL) describe the table.
uint32_t ws_move_set(int idx, int n, const bool *used,
	const uint32_t *owner, const bool *modal);

#endif
