/*
 * Host test for kbd_key_is_accent() -- docs/keyboard_layouts.md,
 * "Dead keys". The decision of whether a dead key is held as an
 * accent, or is a shortcut (or is dropped), is that function and
 * nothing else.
 *
 *   make -C sw/apps/wm test
 */
#include <stdio.h>
#include "../kbd_accent.h"

static int checks, fails;
#define CK(c, w) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, w); } } while (0)

int main(void) {

	uint32_t dead = Z_KEY_DEAD(0);		/* dead_grave, what es puts on [ */
	uint32_t acute = Z_KEY_DEAD(1);		/* dead_acute, what de puts on = */

	CK(kbd_key_is_accent(dead, 0, false), "a bare dead key is an accent");
	CK(kbd_key_is_accent(acute, Z_KBD_MOD_SHIFT, false),
		"Shift still selects the other dead level");
	CK(kbd_key_is_accent(dead, Z_KBD_MOD_RALT, false),
		"right Alt alone is AltGr, and still an accent");

	CK(!kbd_key_is_accent(dead, Z_KBD_MOD_GUI, false),
		"Super+[ is not an accent");
	CK(!kbd_key_is_accent(dead, Z_KBD_MOD_GUI | Z_KBD_MOD_CTRL, false),
		"Ctrl+Super+[ is not an accent");
	CK(!kbd_key_is_accent(dead, Z_KBD_MOD_GUI, true),
		"Alt+Super+[ is not an accent");
	CK(!kbd_key_is_accent(acute, Z_KBD_MOD_GUI, false),
		"Super+Equal is not an accent");
	CK(!kbd_key_is_accent(acute, 0, true),
		"Alt+Equal is not an accent");
	CK(!kbd_key_is_accent(dead, Z_KBD_MOD_CTRL, false),
		"Ctrl on a dead key does not arm an accent either");
	CK(!kbd_key_is_accent(dead, Z_KBD_MOD_LALT, false),
		"left Alt in the forwarded modifiers is Alt");

	CK(!kbd_key_is_accent('a', 0, false), "a letter is not an accent");
	CK(!kbd_key_is_accent('a', Z_KBD_MOD_GUI, false),
		"Super+letter is not decided here");
	CK(!kbd_key_is_accent('[', Z_KBD_MOD_RALT, false),
		"AltGr+[ is the character, not a dead key");
	CK(!kbd_key_is_accent(Z_KEY_NONE, 0, false), "no keysym is not an accent");

	/* The three keys the shortcuts are matched on, as dead keysyms
	 * (the table value is not what wm sees: z_kbd_translate maps
	 * Z_KBD_DEAD_BASE + n to Z_KEY_DEAD(n)). */
	CK(kbd_key_is_accent(Z_KEY_DEAD(1), 0, false) &&
		!kbd_key_is_accent(Z_KEY_DEAD(1), 0, true) &&
		!kbd_key_is_accent(Z_KEY_DEAD(1), Z_KBD_MOD_GUI, false),
		"Equal: accent alone, shortcut with Alt or Super");
	CK(!kbd_key_is_accent(Z_KEY_DEAD(0), Z_KBD_MOD_GUI, false),
		"[ with Super is the workspace shortcut");
	CK(!kbd_key_is_accent(Z_KEY_DEAD(7), Z_KBD_MOD_GUI, false),
		"a dead key with Super is not an accent");

	if (fails) {
		printf("%d of %d failed\n", fails, checks);
		return 1;
	}
	printf("ok %d\n", checks);
	return 0;

}
