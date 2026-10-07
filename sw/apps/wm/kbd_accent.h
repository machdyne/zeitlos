#ifndef KBD_ACCENT_H
#define KBD_ACCENT_H

/*
 * Whether a keysym is an accent wm should hold for the next letter.
 *
 * A dead key held with Ctrl, Super or Alt is not one. Those modifiers
 * already refuse to compose with an accent that is pending; the key
 * that would have started the accent has to follow the same rule, or a
 * shortcut matched by the key itself never runs on a layout where that
 * key is dead, and the accent stays armed for whatever is typed next.
 *
 * `alt` is wm's own idea of Alt: left Alt, or right Alt on a layout
 * that has no AltGr. AltGr is not Alt. It is how a layout types the
 * key's other character, and that character may itself be dead, so a
 * bare right Alt must still arm one. Shift is the same: it only
 * selects the key's other dead level. Neither one is in this test.
 *
 * fwd_mods is what z_kbd_event_to_keysym() reports, which has already
 * taken AltGr's right Alt back out.
 */

#include "../../common/zkbd.h"

static inline bool kbd_key_is_accent(uint32_t keysym, uint8_t fwd_mods, bool alt) {

	if (!Z_KEY_IS_DEAD(keysym)) return false;
	if (alt) return false;
	if (fwd_mods & (Z_KBD_MOD_CTRL | Z_KBD_MOD_GUI | Z_KBD_MOD_LALT))
		return false;
	return true;

}

#endif
