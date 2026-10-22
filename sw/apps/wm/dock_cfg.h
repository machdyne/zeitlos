#ifndef Z_DOCK_CFG_H
#define Z_DOCK_CFG_H

/*
 * Zeitlos -- what the dock shows, read from two files (docs/dock.md).
 *
 *   /data/wm/wm.cfg    the arrangement: which apps, in what order
 *   /data/wm/dock.zic  the icons, and what each app needs to run
 *
 * Both are in the flash archive (CORE_FILES), so every machine has
 * them with or without a card; a file at the same path on the card wins
 * over the one in flash, as for every file in flash. That is how a
 * person changes the dock: their own /data/wm/wm.cfg on the card
 * replaces the built-in one whole.
 *
 * This file is the pure part: parsing wm.cfg one line at a time,
 * checking and reading a dock.zic that is already in memory, and
 * turning a row of names into the list of icons. No syscalls, no
 * drawing, no wm globals -- tests/test_dock_cfg.c runs it on a host.
 * wm.c does the file reading, the "is it installed" question and the
 * drawing.
 */

#include <stdint.h>
#include <stdbool.h>

#define DK_NAME_MAX     16	// an app's name, NUL included (Z_PROC_NAME_MAX is 24)
#define DK_SPOKEN_MAX   24	// what speech says for it, NUL included
#define DK_ICON_BYTES   128	// 32x32, 1bpp, 4 bytes a row, MSB first
#define DK_ITEMS_MAX    48	// icons in the main row and its groups together
#define DK_POOL         768	// bytes of names for the rows and the groups
#define DK_GROUPS_MAX   10	// `group` lines
#define DK_GROUP_ID_MAX 12	// a group's id ("games"), NUL included
#define DK_LINE_MAX     160	// longest line of wm.cfg, NUL included

// -- dock.zic --------------------------------------------------------
//
//   offset 0   "ZIC2"
//          4   u16 count       entries
//          6   u16 entry_size  32
//          8   u32 spoken      bytes of spoken names, after the entries
//         12   u32 icons       bytes of icons, after the spoken names
//         16   entries, count x 32, in name order:
//                name[16] u32 feature u32 feature2 u16 flags
//                u16 spoken_off u16 icon_off u16 icon_len
//              spoken names, NUL-terminated
//              icons, compressed (dk_icon_expand())
//
// Little-endian. wm reads it front to back: the entries into a table
// (only while it builds the dock), then the spoken names and the icons
// of what its dock shows, and nothing else, so an icon that is not on
// the dock costs no memory. Entries are in name order (the generator
// sorts them), which is the order @rest adds them in.
//
// An icon is 32 rows of 4 bytes, MSB first, bit 1 = ink, with every
// byte XORed with the one a row above, then run-length coded: a byte c
// below 0x80 is c+1 literal bytes after it, c from 0x80 is (c & 0x7f)+1
// zero bytes. About 60% of 128 bytes for these icons.
//
// Made by sw/data/icons/gen_dock_icon_data.py from the PNGs and
// apps.txt beside it.

#define DK_ZIC_MAGIC    "ZIC2"
#define DK_ZIC_HEADER   16
#define DK_ZIC_ENTRY    32
#define DK_ZIC_MAX      65536	// a bigger file is taken as damaged
#define DK_ZIC_ENTRIES  64	// the most entries wm reads
#define DK_ZIC_NAMES    384	// bytes of their names, NULs included (about 8 each)

// Entry flags. HIDDEN is the file's: not added by @rest (casino's
// tables, which casino starts; the dock's own icons), still shown when a
// row names it. WANT and NOHW are the reader's.
#define DK_ZIC_HIDDEN   0x01u
#define DK_ZIC_WANT     0x02u	// read its spoken name and icon (dk_zic_read_parts())
#define DK_ZIC_NOHW     0x80u	// needs hardware this board does not have

#define DK_NONE         0xffffu

typedef struct {
	uint16_t	name;		// in dk_zic_t.names
	uint8_t		flags;		// DK_ZIC_*
	uint8_t		icon_len;	// the icon, compressed; 0: none (or not read)
	uint16_t	spoken;		// offset: in the file's spoken names, then, once
	uint16_t	icon;		//   read, in the caller's store; DK_NONE: none
} dk_zic_entry_t;

// What wm holds of dock.zic while it builds the dock: the entries, with
// their names. Not the icons -- dk_zic_read_parts() reads those it wants
// straight into the caller's store.
typedef struct {
	int		count;
	uint32_t	spoken_bytes, icon_bytes;	// the two sections' sizes
	dk_zic_entry_t	e[DK_ZIC_ENTRIES];
	char		names[DK_ZIC_NAMES];
	uint16_t	names_used;
} dk_zic_t;

// The header, checked: false if it is not a dock.zic this reads. `size`,
// when not 0, is the file's, which must match.
bool dk_zic_header(const uint8_t raw[DK_ZIC_HEADER], uint32_t size, dk_zic_t *z);

// Does this board have the hardware an entry needs (Z_FEATURE_*,
// Z_FEATURE2_* bits)? NULL: everything is there.
typedef bool (*dk_has_fn)(uint32_t feature, uint32_t feature2);

// Entry `i`, checked: false if its name has no end or does not fit,
// or its spoken name or icon would lie outside their section.
bool dk_zic_entry(const uint8_t raw[DK_ZIC_ENTRY], dk_zic_t *z, int i, dk_has_fn has);

const char *dk_zic_name(const dk_zic_t *z, int i);

// The entry called `name`, or -1. `z` may be NULL.
int dk_zic_find(const dk_zic_t *z, const char *name);

// A compressed icon `blob` of `len` bytes into 128 bytes. False if it
// is malformed (too long, too short, or running past its end).
bool dk_icon_expand(const uint8_t *blob, int len, uint8_t out[DK_ICON_BYTES]);

// -- reading dock.zic front to back --
//
// `rd` reads the next bytes of the file into `buf`, at most `n`, and
// returns how many (0 at the end, < 0 on an error) -- as the kernel's
// FS_READ_CHUNK does. Short reads are fine.
typedef int (*dk_read_fn)(void *ctx, void *buf, uint32_t n);

// The header and the entries. `buf` (at least DK_ZIC_HEADER bytes, the
// bigger the fewer reads) is room to read through. The number of
// entries, or -1 if the file is not a whole, sane dock.zic.
int dk_zic_read_table(dk_read_fn rd, void *ctx, dk_zic_t *z, uint8_t *buf, uint32_t buflen,
	dk_has_fn has);

// The rest of the file: the spoken names and the icons of the entries
// marked DK_ZIC_WANT, appended to `store` from *used on, and the
// entries changed to say where (spoken DK_NONE for none, icon_len 0 for
// no icon); the others are left with none. `buf` is room to read
// through. False if the file ends early, is out of order, or the store
// is full; what was kept until then stays.
bool dk_zic_read_parts(dk_read_fn rd, void *ctx, dk_zic_t *z, uint8_t *buf, uint32_t buflen,
	uint8_t *store, uint32_t cap, uint32_t *used);

// -- wm.cfg ----------------------------------------------------------
//
//   # a comment, to the end of the line
//   side bottom                  bottom (top, left, right: later)
//   dots on                      a dot per running copy (on, off)
//   row wide term files text @games @misc
//   row tall term files text
//     read web                   a line that starts with a space or
//                                a tab continues the line above it
//   group games Games chess snake blocks
//   group misc "Other apps" @rest
//
// `row wide` is the main view for a dock along the bottom or the top,
// `row tall` for one along the left or the right. `@id` in a row is a
// group's icon; the group opens in place. @rest is every app with an
// icon that the row and its groups do not name, in name order.

enum { DK_SIDE_BOTTOM, DK_SIDE_TOP, DK_SIDE_LEFT, DK_SIDE_RIGHT };
enum { DK_ROW_WIDE, DK_ROW_TALL, DK_ROWS };

// The lists of names the file gives: the two rows, then one per group.
#define DK_LISTS        (DK_ROWS + DK_GROUPS_MAX)

typedef void (*dk_report_fn)(void *ctx, int line, const char *msg, const char *word);

typedef struct {
	char		id[DK_GROUP_ID_MAX];	// "games": a row names it @games
	char		name[DK_SPOKEN_MAX];	// "Games": what is said for its icon
} dk_group_t;

typedef struct {

	uint8_t		side;			// DK_SIDE_*
	bool		dots;			// a dot per running copy under each icon (default on)

	// Each list: a row (DK_ROW_*) or a group (DK_ROWS + its index).
	bool		has[DK_LISTS];		// the file gave it
	int16_t		rest[DK_LISTS];		// index of @rest in it, or -1
	uint16_t	start[DK_LISTS];	// first name, in pool
	uint16_t	count[DK_LISTS];	// names, @rest not counted

	dk_group_t	groups[DK_GROUPS_MAX];
	uint8_t		ngroups;

	char		pool[DK_POOL];		// names, each NUL-terminated; a group's icon as "@id"
	uint16_t	used;

	int8_t		cont;			// the list a continuation line adds to, or -1
	int		errors;

	dk_report_fn	report;
	void		*ctx;

} dk_cfg_t;

void dk_cfg_init(dk_cfg_t *c, dk_report_fn report, void *ctx);

// One line, without its newline. `lineno` is for messages.
void dk_cfg_line(dk_cfg_t *c, const char *line, int lineno);

// A whole file already in memory, line by line. A line longer than
// DK_LINE_MAX - 1 is reported and left out.
void dk_cfg_text(dk_cfg_t *c, const char *text, uint32_t len);

// The row a dock on `side` uses.
int dk_side_row(int side);

// The group called `id` (without the '@'), or -1.
int dk_group_find(const dk_cfg_t *c, const char *id);

// -- the list --------------------------------------------------------
//
// The dock as a flat list: the main row's items first, then each group
// the row opens, its members in a run of their own (a view).

typedef struct {
	char		name[DK_NAME_MAX];	// the app; for a group's icon, its id
	int16_t		zic;			// its entry in dock.zic, or -1: no icon
	int8_t		view;			// -1: an app; else the view this icon opens
	// Filled by wm once the icons are read; not by dk_list():
	uint8_t		icon_len;		// its icon, compressed, or 0: none
	uint16_t	icon_off;		//   at this offset in wm's icon store
	uint16_t	spoken_off;		// its spoken name in wm's store, or DK_NONE
} dk_item_t;

typedef struct {
	uint8_t	start, count;		// its members, in the list
	uint8_t	group;			// which of the file's groups
} dk_view_t;

typedef struct {
	int		n;			// items in all
	int		nmain;			// the main row: items 0 .. nmain-1
	int		nviews;
	dk_view_t	views[DK_GROUPS_MAX];
} dk_dock_t;

// Row `row` of `c` as the dock: each name with its icon, each @group
// as an icon that opens a view of its members, @rest replaced by the
// icons the row and its groups do not name (once: where it first
// appears). A group's icon is "@id" in dock.zic, or "@group", or none.
// More items than `max` are reported once and left out; an unknown
// group is reported and left out. `zic` may be NULL (no icons): names
// stay, @rest adds nothing. Returns d->n.
int dk_list(const dk_cfg_t *c, int row, const dk_zic_t *zic,
	dk_item_t *out, int max, dk_dock_t *d);

// Keeps the apps whose keep[] is non-zero (a group's icon: ignored),
// in order, and then drops a group left with no apps -- its view and
// every icon that opened it. Views keep their order. Returns d->n.
int dk_keep(dk_item_t *items, const uint8_t *keep, dk_dock_t *d);

// -- running apps ----------------------------------------------------

// Is a process whose z_proc_list() name is `pname` a copy of `app`? The
// kernel reports the program a process was started from ("text"), or
// the name it registered, which is a base and a number ("web0").
bool dk_proc_is(const char *pname, const char *app);

// The dots under an icon: one per running copy, at most this many.
#define DK_DOTS_MAX     6

#endif
