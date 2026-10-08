#ifndef ZDATA_H
#define ZDATA_H

/*
 * zdata.h -- an app's own data folder, /data/<app>/ (docs/layout.md).
 *
 *     char path[64];
 *     if (z_data_file(path, sizeof path, "scores.txt"))
 *         fs_write_file(path, buf, n);        // /data/<app>/scores.txt
 *
 * Every app is installed in two places: its program in /apps and
 * everything else in /data/<app>/. This is how an app names a file in
 * the second without spelling out either "/data" or its own name, so
 * neither can be wrong and neither has to be in sw/common/zpaths.h.
 *
 * The folder is created on the first call (and the call after that
 * finds it there, which is fine): an app never has to remember to make
 * it, and a card on which somebody deleted it -- "reset this app" --
 * comes back to life on the next write.
 *
 * <app> is Z_APP_NAME, which sw/common/app.mk defines for every app as
 * its APP: the same name it has in /apps and in the dock. A file
 * compiled without it (the kernel, a host test) cannot use this, and
 * the #error below says so rather than writing somewhere else.
 *
 * Files the release ships into an app's folder, or that another app
 * reads, are named in zpaths.h instead: the release tool has to know
 * them too, and it reads that file, not this one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "zpaths.h"
#include "zfsapp.h"

#ifndef Z_APP_NAME
#error "z_data_file() needs Z_APP_NAME, which sw/common/app.mk defines for every app"
#endif

/* /data/<app>/<leaf> into `out`, after making sure /data/<app> exists.
 * False if it does not fit; `out` is then not a usable path. */
static inline bool z_data_file(char *out, size_t cap, const char *leaf) {

	int n = snprintf(out, cap, "%s", Z_DIR_DATA "/" Z_APP_NAME);
	if (n < 0 || (size_t)n >= cap) return false;
	fs_mkdir(out);

	n = snprintf(out, cap, "%s/%s", Z_DIR_DATA "/" Z_APP_NAME, leaf);
	return n >= 0 && (size_t)n < cap;

}

#endif
