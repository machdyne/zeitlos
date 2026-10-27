/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- what learn needs from the machine it runs on. learn.c gives
 * the board's (the fs service, the RTC, speech, dialogs, running apps);
 * the tests give the build machine's. Everything else in learn is the
 * same code on both.
 */

#ifndef LPLAT_H
#define LPLAT_H

#include <stdint.h>
#include <stdbool.h>

#include "lformat.h"

typedef struct {
	lf_fs_t		fs;			// list and read (the format's)
	// part of a file: at most cap bytes from offset off; the length, -1 if it cannot be read
	long		(*read_at)(const char *path, long off, char *buf, long cap);
	// writes: a file whole or not at all (NAME.part, then renamed)
	bool		(*write_whole)(const char *path, const char *data, long len);
	// a line added to the end of a file (created if missing)
	bool		(*append)(const char *path, const char *data, long len);
	bool		(*mkdir)(const char *path);		// one level; true if it exists after
	bool		(*exists)(const char *path);
	// time
	uint32_t	(*now)(void);		// seconds since 1970 (UTC), 0 if the clock is not set
	uint32_t	(*ms)(void);		// milliseconds, any origin
	// speech (both may do nothing)
	void		(*speak)(const char *text);
	void		(*speak_stop)(void);
	// boxes, modal
	bool		(*prompt)(const char *title, const char *msg, const char *initial, char *out, int cap);
	bool		(*confirm)(const char *title, const char *msg);
	// another app (a `try` step)
	bool		(*run)(const char *app, const char *arg);
} lplat_t;

extern const lplat_t *LP;

#endif
