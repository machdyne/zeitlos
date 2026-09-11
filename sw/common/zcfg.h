#ifndef ZCFG_H
#define ZCFG_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zcfg -- system configuration: /zeitlos.cfg, read into the kernel at
 * boot, readable by any app. See docs/config.md.
 *
 * -- the file --
 *
 *   # comment
 *   apps.term.auto_connect: port repl0
 *   system.rtc.timezone: Berlin
 *   system.video.mode = amber
 *
 * One setting per line. The key is letters, digits, '.', '_' and '-',
 * and must be DOTTED -- section.name, as every key is -- so a stray line
 * of prose ("remember to set the zone") is a malformed line rather than
 * a setting called "remember". It is followed by ':' or '=' or just
 * whitespace, then the value,
 * which runs to the end of the line with surrounding blanks trimmed.
 * No quoting and no inline comments -- a value may contain '#'. A key
 * that appears twice takes its LAST value.
 *
 * -- where it lives while the system runs --
 *
 * In the kernel (sw/os/cfg.c), loaded once by sh() after the sdcard is
 * mounted and BEFORE init() starts anything, so every app sees the same
 * values from its first instruction. No card, or no file, means an
 * empty store and every app uses its default. `cfg reload` (serial
 * console) or z_cfg_reload() re-reads the file after an edit.
 *
 * Why the kernel rather than each app parsing the file: one read at
 * boot instead of one per launch over bit-banged SPI, one set of values
 * everyone agrees on even while the file is being edited, and a
 * generation number that lets a running app notice a reload without
 * re-reading anything.
 *
 * -- two halves --
 *
 * The syscall wrappers (z_cfg_get() and friends) are for apps. The
 * parser, the text editor (z_cfg_text_set()) and the table of known
 * keys are plain functions with no I/O, shared by the kernel, by
 * sw/apps/settings and by the host tests. The kernel builds this file
 * with -DZCFG_KERNEL, which leaves the wrappers out.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define Z_CFG_PATH       "/zeitlos.cfg"

#define Z_CFG_KEY_MAX    64		// including the NUL
#define Z_CFG_VAL_MAX    128	// including the NUL
#define Z_CFG_LINE_MAX   256	// a longer line is ignored, not truncated

// The largest file sw/apps/settings will rewrite. The kernel reads any
// length (line by line); this only bounds an editor's buffers, which
// are static -- see save() in settings.c for why they cannot be
// malloc'd. 4KB is far more than a config file needs: the kernel's
// whole store holds 2KB of settings (sw/os/cfg.c).
#define Z_CFG_FILE_MAX   4096

// -- known keys --
//
// Everything built into the tree that reads a setting, with its default.
// A key not listed here is still loaded, kept and returned -- this table
// is for defaults and for telling a person what a key does, not a
// whitelist.
typedef struct {
	const char *key;
	const char *def;	// used when the file does not set the key
	const char *help;	// one line, for `cfg` and settings
} z_cfg_known_t;

extern const z_cfg_known_t z_cfg_known[];
extern const int z_cfg_known_count;

// The default for `key`, or "" for a key not in the table.
const char *z_cfg_default(const char *key);

// -- app API (syscalls) --

// Copies the effective value of `key` into `out`: the file's value if
// it sets one, otherwise the default from z_cfg_known (or ""). Returns
// true only if the value came from the FILE.
//
// Never fails in a way the caller has to handle: no kernel support
// (an old kernel, the simulator) reads as "not set".
bool z_cfg_get(const char *key, char *out, size_t outlen);

// Convenience forms, parsed from the effective value. A value that does
// not parse gives `def`.
//   bool: yes/no, true/false, on/off, 1/0 (any case)
bool z_cfg_get_bool(const char *key, bool def);
int32_t z_cfg_get_int(const char *key, int32_t def);

// Increments every time the kernel's store is (re)loaded. An app that
// caches a setting compares this cheaply and re-reads on a change.
// 0 means no kernel support.
uint32_t z_cfg_generation(void);

// The index'th setting loaded from the file, in file order. false past
// the end. For listing what is set, including keys this tree does not
// know.
bool z_cfg_entry(uint32_t index, char *key, size_t keylen,
	char *val, size_t vallen);

// Re-reads /zeitlos.cfg into the kernel. Returns the number of settings
// loaded, 0 for no file (defaults everywhere), -1 for no kernel
// support. *ignored (may be NULL) gets the count of malformed lines.
int z_cfg_reload(uint32_t *ignored);

// -- syscall argument blocks (sw/os/cfg.c) --
//
// Plain structs rather than z_obj_t maps, for the same reason
// sw/common/zfs.h's are: the kernel reads the caller's own pointers
// directly, in the caller's mapping, during the syscall.

typedef struct {
	const char	*key;		// in; NULL just reports the generation
	char		*val;		// out, NUL-terminated
	uint32_t	vallen;		// in
	uint32_t	found;		// out: 1 if the file sets it
	uint32_t	generation;	// out
} z_cfg_get_args_t;

typedef struct {
	uint32_t	index;		// in
	char		*key;		// out
	uint32_t	keylen;		// in
	char		*val;		// out
	uint32_t	vallen;		// in
	uint32_t	found;		// out: 1 if index was in range
} z_cfg_entry_args_t;

typedef struct {
	int32_t		count;		// out: settings loaded; 0 = no file
	uint32_t	ignored;	// out: malformed lines skipped
	uint32_t	generation;	// out
	uint32_t	done;		// out: 1 once the kernel handled it
} z_cfg_reload_args_t;

// -- pure helpers (no I/O) --

typedef enum {
	Z_CFG_LINE_BLANK = 0,	// empty, or a # comment
	Z_CFG_LINE_ENTRY,		// key and value written to the outputs
	Z_CFG_LINE_BAD			// not a setting; the outputs are ""
} z_cfg_line_t;

// Parses one line (no newline required; a trailing \r or \n is
// ignored). `len` bounds the read; the line need not be NUL-terminated.
// A key or value too long for its buffer makes the line BAD rather than
// silently truncating it.
z_cfg_line_t z_cfg_parse_line(const char *line, size_t len,
	char *key, size_t keylen, char *val, size_t vallen);

// True if `key` is a well-formed key: key characters only, with a '.'
// somewhere other than the first or last character.
bool z_cfg_key_valid(const char *key);

// Rewrites a config file's TEXT with `key` set to `val`, into `out`.
//
//   - every line that is not this key is copied byte for byte --
//     comments, blank lines, unknown keys, malformed lines, line endings
//   - the first line setting `key` becomes "key: val", and any later
//     duplicates are dropped (they would have overridden it)
//   - if no line sets it, "key: val" is appended
//   - `val` NULL removes every line setting `key`
//
// Returns the length written (NUL-terminated), or -1 if `out` is too
// small, `key` is invalid, or `val` does not fit a line. This is how an
// editor changes one setting without clobbering anything it does not
// understand.
int z_cfg_text_set(const char *in, size_t inlen, const char *key,
	const char *val, char *out, size_t outcap);

#endif
