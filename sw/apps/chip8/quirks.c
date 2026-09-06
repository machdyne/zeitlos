/*
 * chip8 -- compatibility profiles.
 *
 * Three named starting points, not three implementations. Everything
 * here is data; core.c reads it and behaves accordingly.
 *
 * A profile is a STARTING POINT, not a verdict. Real ROMs disagree
 * with their own family (SUPER-CHIP titles written against 1.0 want
 * mem_inc = X1, which 1.1 does not do), so the app is expected to let
 * a per-ROM entry override individual fields. Getting the defaults
 * right just means most ROMs need no entry at all.
 *
 * Field-by-field justification lives in core.h next to each field.
 * What follows is only where each value came from.
 */

#include <string.h>

#include "core.h"

/* -- CHIP-8, as the COSMAC VIP ran it -------------------------------
 *
 * The reference for anything written 1977-1984. The three fields that
 * matter most here are vf_reset, display_wait and mem_inc: a VIP-era
 * game run without display_wait flickers badly (it was written
 * expecting one sprite per frame), and run without vf_reset it
 * mis-detects collisions. */
static const c8_quirks_t q_chip8 = {
	.vf_reset           = true,
	.mem_inc            = C8_MEM_INC_X1,
	.display_wait       = true,
	.clip_sprites       = true,
	.shift_vx           = false,
	.jump_vx            = false,
	.wide_sprite_lores  = false,
	.collision_rows     = false,
	.scroll_half_lores  = false,
	.clear_on_mode      = false,
	.addr_mask          = 0x0FFF,
	.flag_count         = 8
};

/* -- SUPER-CHIP 1.1, as it shipped on the HP48 ----------------------
 *
 * Deliberately 1.1 and not 1.0, because 1.1 is what the surviving
 * SUPER-CHIP catalogue was written against.
 *
 * mem_inc is NONE here, which is the single most surprising entry in
 * this file: SUPER-CHIP 1.1 leaves I untouched after FX55/FX65, where
 * every other interpreter in this table advances it. SUPER-CHIP 1.0
 * did advance by X+1, so a title that predates 1.1 will need that
 * overridden per-ROM. It presents as arrays being read back from the
 * wrong offset -- garbled graphics rather than a crash.
 *
 * collision_rows applies in hires only, which core.c does not
 * currently condition on: a lores SUPER-CHIP program that reads VF as
 * a count gets the count. No known ROM does, and making it
 * mode-dependent would add a branch to the sprite inner loop for a
 * case nothing exercises. Recorded here rather than silently. */
static const c8_quirks_t q_schip = {
	.vf_reset           = false,
	.mem_inc            = C8_MEM_INC_NONE,
	.display_wait       = false,
	.clip_sprites       = true,
	.shift_vx           = true,
	.jump_vx            = true,
	.wide_sprite_lores  = true,
	.collision_rows     = true,
	.scroll_half_lores  = true,
	.clear_on_mode      = false,
	.addr_mask          = 0x0FFF,
	.flag_count         = 8
};

/* -- XO-CHIP, as Octo defines it ------------------------------------
 *
 * Octo is the reference implementation in practice: essentially the
 * whole XO-CHIP catalogue is compiled by it, so where the written
 * specification and Octo's behaviour differ, Octo wins by weight of
 * ROMs.
 *
 * Note this reverts several SUPER-CHIP choices -- shifting and jumping
 * go back to the CHIP-8 forms, and sprites wrap rather than clip.
 * XO-CHIP is not "SUPER-CHIP plus planes"; it is a separate branch
 * from CHIP-8 that happens to keep SUPER-CHIP's screen modes. */
static const c8_quirks_t q_xochip = {
	.vf_reset           = false,
	.mem_inc            = C8_MEM_INC_X1,
	.display_wait       = false,
	.clip_sprites       = false,
	.shift_vx           = false,
	.jump_vx            = false,
	.wide_sprite_lores  = true,
	.collision_rows     = false,
	.scroll_half_lores  = false,
	.clear_on_mode      = true,
	.addr_mask          = 0xFFFF,
	.flag_count         = 16
};

static const c8_quirks_t *const profiles[C8_PROFILE_COUNT] = {
	&q_chip8, &q_schip, &q_xochip
};

static const char *const names[C8_PROFILE_COUNT] = {
	"chip8", "schip", "xochip"
};

const c8_quirks_t *c8_profile(c8_profile_t p) {
	if ((int)p < 0 || (int)p >= C8_PROFILE_COUNT) p = C8_PROFILE_CHIP8;
	return profiles[p];
}

const char *c8_profile_name(c8_profile_t p) {
	if ((int)p < 0 || (int)p >= C8_PROFILE_COUNT) return "?";
	return names[p];
}

int c8_profile_by_name_n(const char *s, int len) {

	int i, j;

	if (!s || len <= 0) return -1;

	for (i = 0; i < C8_PROFILE_COUNT; i++) {
		const char *a = names[i];
		for (j = 0; j < len; j++) {
			char ca = a[j], cb = s[j];
			if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
			if (ca != cb) break;
		}
		/* Both ends must line up: "chip" must not match "chip8", and
		 * "chip88" must not either. */
		if (j == len && a[len] == '\0') return i;
	}

	return -1;

}

int c8_profile_by_name(const char *s) {
	if (!s) return -1;
	return c8_profile_by_name_n(s, (int)strlen(s));
}
