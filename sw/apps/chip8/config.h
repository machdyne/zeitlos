#ifndef CHIP8_CONFIG_H
#define CHIP8_CONFIG_H

/*
 * chip8 -- per-ROM configuration.
 *
 * A profile (quirks.c) is a starting point, not a verdict. Real ROMs
 * disagree with their own family: SUPER-CHIP titles written against
 * 1.0 want I advanced after FX55, which 1.1 does not do, and the
 * symptom is garbled graphics rather than an error. Guessing from the
 * file extension, which is what the app does without this, is a weak
 * signal and often simply wrong.
 *
 * So a ROM directory may carry a text file naming what each ROM
 * actually needs.
 *
 * -- the file --
 *
 * `CHIP8.CFG`, in the same directory as the ROM. Uppercase and 8.3
 * because FAT short names are all this filesystem has (FF_USE_LFN is
 * 0; see sw/os/fs/fatfs/ffconf.h).
 *
 *     # comments run to end of line, ; also works
 *
 *     [*]                        applies to every ROM in this directory
 *     speed 20
 *
 *     [BLINKY.CH8]
 *     name Blinky
 *     profile schip
 *     speed 30
 *     set mem_inc=x1 shift=off
 *     pad up=2 down=8 left=4 right=6 a=5
 *
 * `[*]` is applied first and a named section on top of it, so a pack
 * can set one speed for everything and override it for the two ROMs
 * that need it.
 *
 * -- why one file per directory --
 *
 * Rather than a sidecar per ROM. A ROM pack is distributed as a
 * directory, and one file that travels with it is one thing to write,
 * one thing to review and one thing to ship. Two hundred `.CFG` files
 * next to two hundred `.CH8` files also doubles the length of every
 * listing in the file browser, which is a real cost on a 64-entry
 * screen.
 *
 * -- ordering does not matter --
 *
 * `profile` may appear after `set`. Overrides are recorded with a mask
 * of which fields were named and applied on top of the profile at the
 * end, so a file whose lines happen to be in the other order behaves
 * the same. A parser that applied each line as it read it would
 * silently discard every `set` above the `profile` line, which is
 * exactly the kind of thing nobody thinks to test.
 *
 * This file is pure: it parses text into a struct and includes nothing
 * from sw/common. Reading the file is chip8.c's job.
 */

#include <stdint.h>
#include <stdbool.h>

#include "core.h"

/* Buttons a pad mapping can name, in the order the `pad` line uses. */
typedef enum {
	C8_PAD_UP = 0, C8_PAD_DOWN, C8_PAD_LEFT, C8_PAD_RIGHT,
	C8_PAD_A, C8_PAD_B, C8_PAD_X, C8_PAD_Y,
	C8_PAD_START, C8_PAD_SELECT,
	C8_PAD_COUNT
} c8_pad_button_t;

/* Quirk fields a `set` line can name. Order is arbitrary; only the
 * mask below depends on it. */
typedef enum {
	C8_Q_VF_RESET = 0, C8_Q_MEM_INC, C8_Q_DISPLAY_WAIT, C8_Q_CLIP,
	C8_Q_SHIFT, C8_Q_JUMP, C8_Q_WIDE_LORES, C8_Q_COLLISION_ROWS,
	C8_Q_SCROLL_HALF, C8_Q_CLEAR_ON_MODE, C8_Q_MEMORY,
	C8_Q_COUNT
} c8_quirk_field_t;

#define C8_CFG_NAME_MAX 32

typedef struct {

	char name[C8_CFG_NAME_MAX];   /* display name, or empty */

	int profile;                  /* c8_profile_t, or -1 for unset */
	int speed;                    /* instructions per frame, or 0 */
	int palette;                  /* c8_palette_t, or -1 for unset */

	/* Quirk overrides. `quirks` holds only the fields whose bit is set
	 * in `set_mask`; the rest are meaningless. c8_config_apply()
	 * merges them onto a profile. */
	uint16_t set_mask;
	c8_quirks_t quirks;

	/* Pad button -> guest hex key, or 0xFF for "not mapped here".
	 * Absent buttons keep the app's default rather than becoming
	 * unmapped, so a `pad` line naming two buttons changes two
	 * buttons. */
	uint8_t pad[C8_PAD_COUNT];

	/* Whether any section matched at all. */
	bool found;

	/* Lines the parser could not understand. Reported rather than
	 * ignored: a typo in a quirk name would otherwise present as "this
	 * ROM still misbehaves", with the config looking correct. */
	int bad_lines;
	int first_bad_line;

} c8_config_t;

/* Everything unset. Safe to use as-is: it changes nothing. */
void c8_config_defaults(c8_config_t *cfg);

/* Parse `text` (NUL-terminated) for the `[*]` section and then the
 * section named `rom` (a bare filename, matched case-insensitively).
 * Returns cfg->found.
 *
 * `text` is not modified.
 */
bool c8_config_parse(c8_config_t *cfg, const char *text, const char *rom);

/* Merge the config's quirk overrides onto `base`. */
void c8_config_apply(const c8_config_t *cfg, c8_quirks_t *base);

/* Best guess at which machine a ROM is for, before CHIP8.CFG has been
 * read (and for the majority of ROMs, which have no entry).
 *
 * SIZE FIRST, extension second. A ROM larger than C8_ROM_MAX_4K cannot
 * be CHIP-8 or SUPER-CHIP -- there is no address space for it to live
 * in -- so however it is named, it is XO-CHIP. That is not a heuristic,
 * it is arithmetic, and it is a much stronger signal than a three
 * character extension that survived a trip through a FAT filesystem.
 *
 * Only then the extension: .xo8/.xo, .sc8/.sc, otherwise CHIP-8.
 * Case-insensitive, because FAT short names arrive uppercase and people
 * type lowercase, and the same ROM must not behave differently
 * depending on whether it was opened from `files` or typed at a shell.
 */
int c8_profile_hint(const char *name, uint32_t len);

#endif
