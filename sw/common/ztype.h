#ifndef ZTYPE_H
#define ZTYPE_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * File types -- which app opens which extension.
 *
 * One table, in sw/common rather than inside the file browser, because
 * the browser is not the only thing that will want to answer "what
 * opens this?". A shell `open` command, a future desktop with icons on
 * it, and an app deciding whether it can accept a dropped file all ask
 * the same question, and none of them should carry their own list.
 *
 * -- matching --
 *
 * By extension, case-insensitively, and nothing else. There is no
 * content sniffing here: FAT short names (FF_USE_LFN is 0, see
 * sw/os/fs/fatfs/ffconf.h) give every file a 3-character extension to
 * work with, and reading the head of every file in a directory just to
 * draw a list would be an unreasonable amount of card traffic for the
 * benefit.
 *
 * The one exception is a file with NO extension, which is assumed to
 * be an executable -- and there the assumption IS checked, because
 * running the wrong thing is not a recoverable mistake. See
 * z_ftype_is_executable().
 */

#include <stdint.h>
#include <stdbool.h>

typedef struct {

	// Extension WITHOUT the dot, uppercase, at most 3 characters --
	// what FAT short names give us.
	const char	*ext;

	// The app to launch, as z_proc_run() (zeitlos.h) expects it: a
	// bare program name, no path and no extension. The file being
	// opened is handed over separately, through wm's pending launch
	// argument (Z_WM_SET_ARG, zwm.h).
	const char	*app;

	// Shown to the user when there is nothing to launch. Kept short
	// enough to fit a dialog line.
	const char	*desc;

} z_ftype_t;

// The table itself, NULL-terminated. Adding a type is one line here
// and nothing else -- see the file header comment for why nothing
// keeps its own copy of this.
extern const z_ftype_t z_ftypes[];

// The extension of `path`, without the dot, or NULL if it has none.
// Points into `path`; no copy is made.
const char *z_ftype_ext(const char *path);

// The app that opens `path`, or NULL if nothing does. NULL covers both
// "no extension" (ask z_ftype_is_executable() instead) and "an
// extension nothing claims".
const char *z_ftype_app_for(const char *path);

// A human-readable name for the type, or NULL if unknown.
const char *z_ftype_desc_for(const char *path);

// True if `path` has no extension AND actually starts with the ZEXE
// magic (sw/common/zexec.h).
//
// Both halves matter. A file with no extension is only ASSUMED to be a
// program, and that assumption has to be checked before acting on it,
// because the loader will not check it for us: a file without the
// magic is treated as the legacy raw executable format and loaded and
// jumped into regardless (see zexec.h, "Backward compatibility"). So
// double-clicking a README with no extension would run it. This reads
// the first four bytes and refuses if they aren't "ZEXE".
//
// Costs one open/read/close of four bytes, so call it when the user
// actually asks to open something, not while drawing a listing.
bool z_ftype_is_executable(const char *path);

#endif
