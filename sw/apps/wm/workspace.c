/*
 * Zeitlos -- sw/apps/wm
 *
 * See workspace.h.
 */

#include "workspace.h"

int ws_step(int ws, int dir) {
	return ((ws + dir) % WS_COUNT + WS_COUNT) % WS_COUNT;
}

int ws_for_digit_usage(uint16_t usage) {
	if (usage >= 0x1E && usage <= 0x26) return usage - 0x1E;	// 1..9
	if (usage == 0x27) return 9;							// 0
	return -1;
}

uint32_t ws_move_set(int idx, int n, const bool *used,
	const uint32_t *owner, const bool *modal) {

	uint32_t set;
	bool owner_has_modal = false;

	if (idx < 0 || idx >= n || !used[idx]) return 0;
	set = 1u << idx;

	for (int i = 0; i < n; i++)
		if (used[i] && owner[i] == owner[idx] && modal[i]) owner_has_modal = true;

	if (owner_has_modal)
		for (int i = 0; i < n; i++)
			if (used[i] && owner[i] == owner[idx]) set |= 1u << i;

	return set;

}
