/*
 * Host tests for workspace.c -- docs/window_manager.md, "Workspaces".
 *
 *   make -C sw/apps/wm test
 */
#include <stdio.h>
#include "../workspace.h"

static int checks, fails;
#define CK(c, w) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, w); } } while (0)

int main(void) {

	// -- stepping wraps, both ways --
	CK(ws_step(0, 1) == 1, "next");
	CK(ws_step(WS_COUNT - 1, 1) == 0, "next after the last is the first");
	CK(ws_step(0, -1) == WS_COUNT - 1, "previous before the first is the last");
	CK(ws_step(5, -1) == 4, "previous");
	for (int w = 0; w < WS_COUNT; w++)
		CK(ws_step(ws_step(w, 1), -1) == w, "next then previous is where you were");

	// -- digit keys, by physical key --
	CK(ws_for_digit_usage(0x1E) == 0, "1 is the first");
	CK(ws_for_digit_usage(0x26) == 8, "9 is the ninth");
	CK(ws_for_digit_usage(0x27) == 9, "0 is the tenth");
	CK(ws_for_digit_usage(0x1D) == -1 && ws_for_digit_usage(0x28) == -1 &&
		ws_for_digit_usage(0x2F) == -1, "other keys select nothing");
	for (int u = 0x1E; u <= 0x27; u++) {
		int w = ws_for_digit_usage((uint16_t)u);
		CK(w >= 0 && w < WS_COUNT, "every digit is a workspace");
	}

	// -- what moves with a window --
	{
		//                  0     1     2     3     4     5
		bool used[6]    = { 1,    1,    1,    1,    0,    1 };
		uint32_t own[6] = { 10,   10,   20,   20,   20,   30 };
		bool modal[6]   = { 0,    0,    0,    1,    0,    0 };
		CK(ws_move_set(0, 6, used, own, modal) == (1u << 0),
			"a window without a dialog moves alone, not with its sibling");
		CK(ws_move_set(2, 6, used, own, modal) == ((1u << 2) | (1u << 3)),
			"a window whose app has a dialog open takes the dialog along");
		CK(ws_move_set(3, 6, used, own, modal) == ((1u << 2) | (1u << 3)),
			"a dialog takes the window it blocks along");
		CK(!(ws_move_set(2, 6, used, own, modal) & (1u << 4)),
			"an unused slot never moves");
		CK(ws_move_set(5, 6, used, own, modal) == (1u << 5), "another owner is untouched");
		CK(ws_move_set(4, 6, used, own, modal) == 0, "an unused slot has nothing to move");
		CK(ws_move_set(-1, 6, used, own, modal) == 0, "no window, nothing");
	}

	printf("test_workspace: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
