#ifndef ZWALK_H
#define ZWALK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A walk over a folder tree, one small step at a time: find (by name,
 * by text inside files, or both), copy, delete. docs/zwalk.md.
 *
 * -- one step, one filesystem call --
 *
 * zw_step() does at most ONE call to the filesystem driver -- list a
 * folder, read or write one chunk, make a folder, delete one entry --
 * and returns. The caller runs it from its event loop between
 * messages, so the window keeps answering, and stopping is
 * zw_cancel() before the next step: the most it ever waits for is
 * the one call already in flight (a few milliseconds on an SD card).
 * Nothing here blocks, sleeps or loops over a whole file or folder.
 *
 * -- memory --
 *
 * The caller hands zw_start() one block (an arena, or a static
 * buffer). It holds the listing of the folder being walked, the chunk
 * buffer, and the types of the entries; the tree's depth is a fixed
 * stack in zwalk_t (ZW_DEPTH). Nothing is allocated. A folder whose
 * listing does not fit the block is walked as far as it fits, and
 * zwalk_t.truncated says so; delete lists it again until it is empty.
 *
 * Folders are listed again when the walk comes back up to them,
 * rather than holding every level's listing: one more call per
 * folder, in exchange for memory that does not grow with depth.
 * find and copy pick up where they were (a folder being walked is not
 * changing under them -- copy refuses a destination inside its
 * source); delete starts its folder over, since everything before is
 * gone.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zfs.h"		// Z_FS_PATH_MAX

#define ZW_DEPTH		16	// folders deep below the start
#define ZW_TEXT_MAX		64	// longest text a find looks for
#define ZW_NAME_MAX		64	// longest name pattern
#define ZW_CHUNK		2048	// bytes per read/write step

#define ZW_FIND		0
#define ZW_COPY		1
#define ZW_DELETE	2

// zw_step() results
#define ZW_MORE		0	// call again
#define ZW_HIT		1	// find: zwalk_t.hit is a match; call again
#define ZW_DONE		2	// finished (counters say what was done)
#define ZW_FAIL		3	// stopped: zwalk_t.err, at zwalk_t.hit

// zwalk_t.err
#define ZW_E_NONE		0
#define ZW_E_NOTFOUND	1	// the start is not there
#define ZW_E_READ		2	// a file could not be read
#define ZW_E_WRITE		3	// copy: a write failed (full?)
#define ZW_E_MKDIR		4	// copy: a folder could not be made
#define ZW_E_DELETE		5	// delete: an entry could not be removed (in use?)
#define ZW_E_INSIDE		6	// copy: the destination is inside the source
#define ZW_E_MEMORY		7	// the block is too small to start
#define ZW_E_EXISTS		8	// copy: the destination is already there

typedef struct {
	uint16_t	idx;		// next entry of this folder's listing
	uint16_t	slen;		// src's length at this level
	uint16_t	dlen;		// dst's length at this level
} zw_frame_t;

typedef struct {

	int			op;

	// -- find --
	char		name[ZW_NAME_MAX];	// glob (* ?) or a part of the name; "" any
	char		text[ZW_TEXT_MAX];	// text inside the file; "" none
	int			tlen;
	bool		name_glob;

	// -- where --
	char		src[Z_FS_PATH_MAX];		// the entry being worked on
	char		dst[Z_FS_PATH_MAX];		// copy: where it goes
	zw_frame_t	st[ZW_DEPTH + 1];
	int			depth;
	bool		root_file;			// the start is a file, not a folder

	// -- the listing of st[depth]'s folder --
	char		*pool;
	uint32_t	poolcap;
	uint8_t		*types;
	uint32_t	maxent;
	uint32_t	count;
	uint32_t	cur;				// entry index `off` points at
	uint32_t	off;				// byte offset of entry `cur` in pool
	bool		listed;
	bool		list_trunc;			// this listing was cut short

	// -- a file in progress (copy, or a text find) --
	uint8_t		*buf;				// ZW_CHUNK + ZW_TEXT_MAX
	int			in, out;
	int			carry;				// find: bytes kept from the last chunk
	bool		infile;

	// -- what happened --
	char		hit[Z_FS_PATH_MAX];	// ZW_HIT: the match; ZW_FAIL: where
	int			err;
	uint32_t	files, dirs;		// copied / deleted / looked at
	uint32_t	bytes;				// copied, or read by a text find
	uint32_t	skipped;			// too deep, or a name too long
	bool		truncated;			// some listing did not fit the block
	bool		partial;			// copy: something was written and is now left

} zwalk_t;

// Sets up a walk. `src` is a folder (find, copy, delete) or a file
// (copy, delete); `dst` is copy's destination, which must not exist.
// `mem`/`memcap` is the working block: 4KB is enough for small
// folders, 32KB for any a file list shows. False (and w->err) if it
// cannot start.
bool zw_start(zwalk_t *w, int op, const char *src, const char *dst,
	void *mem, uint32_t memcap);

// find only: what to look for. `name` is a glob if it has * or ?,
// else a part of the name, case-insensitive either way; `text` is
// looked for inside files, case-insensitive (ASCII). Either may be
// "" -- both "" lists everything. Call after zw_start().
void zw_find_set(zwalk_t *w, const char *name, const char *text);

// One step. ZW_MORE, ZW_HIT, ZW_DONE or ZW_FAIL.
int zw_step(zwalk_t *w);

// Stops now: closes what is open and deletes the file copy was
// writing (zwalk_t.partial then says whether anything else is left).
// Safe to call at any time, and after ZW_DONE/ZW_FAIL.
void zw_cancel(zwalk_t *w);

// Case-insensitive match of `name` against a find pattern, as
// zw_find_set() describes. Exposed for the host test.
bool zw_name_match(const char *pat, bool glob, const char *name);

// True if `inner` is `outer` or below it (case-insensitive, FAT).
bool zw_path_inside(const char *inner, const char *outer);

// Short English for a ZW_E_* code.
const char *zw_strerror(int err);

#endif
