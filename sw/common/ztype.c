/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * File types. See ztype.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zeitlos.h"
#include "zexec.h"
#include "zfsapp.h"
#include "ztype.h"

const z_ftype_t z_ftypes[] = {

	// Plain text, in the three spellings that turn up in practice.
	// MD is here because a Markdown file is text and the editor is
	// the right thing to open it with -- not because anything
	// renders Markdown.
	{ "TXT", "text", "Text"          },
	{ "ASC", "text", "Text"          },
	// MD opens in the VIEWER, not the editor: a Markdown file is
	// something to read far more often than something to edit, and
	// `read` renders it rather than showing the markup. `text` still
	// opens one perfectly well when asked directly.
	{ "MD",  "read", "Markdown text" },

	// Zeitlos bitmap -- sw/common/zbm.h, written by sw/apps/draw.
	//
	// Stays mapped to `draw` rather than `view`, deliberately: a ZBM
	// is this system's own editable drawing, so the editor is what
	// you want on a double-click. `view` opens one perfectly well
	// when asked directly (it decodes ZBM too), the same way `text`
	// still opens a .MD.
	{ "ZBM", "draw", "Bitmap image"  },

	// Images -- sw/apps/view, decoders in sw/common/zimg.c.
	//
	// PNG is listed even though its decoder is compiled out by
	// default (Z_IMG_HAVE_PNG, zimg.h). view recognises the file and
	// says the format isn't built in, which is a far more useful
	// answer than the file browser's "no application handles this".
	{ "BMP", "view", "Bitmap image"  },
	{ "GIF", "view", "GIF image"     },
	{ "JPG", "view", "JPEG image"    },
	{ "JPEG","view", "JPEG image"    },
	{ "PNG", "view", "PNG image"     },
	{ "PNM", "view", "Netpbm image"  },
	{ "PBM", "view", "Netpbm image"  },
	{ "PGM", "view", "Netpbm image"  },
	{ "PPM", "view", "Netpbm image"  },

	{ NULL,  NULL,   NULL            },

};

// case-insensitive compare of two extensions
static bool ext_eq(const char *a, const char *b) {

	for (;; a++, b++) {

		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);

		if (ca != cb) return false;
		if (!ca) return true;

	}

}

const char *z_ftype_ext(const char *path) {

	if (!path) return NULL;

	// Scan from the end, and stop at a path separator: a dot in a
	// DIRECTORY name ("/MY.STUFF/README") must not be mistaken for
	// the file's own extension.
	const char *dot = NULL;

	for (const char *p = path; *p; p++) {
		if (*p == '/') dot = NULL;
		else if (*p == '.') dot = p;
	}

	if (!dot || !dot[1]) return NULL;	// no dot, or a trailing one

	return dot + 1;

}

static const z_ftype_t *find(const char *path) {

	const char *ext = z_ftype_ext(path);
	if (!ext) return NULL;

	for (int i = 0; z_ftypes[i].ext; i++)
		if (ext_eq(z_ftypes[i].ext, ext)) return &z_ftypes[i];

	return NULL;

}

const char *z_ftype_app_for(const char *path) {

	const z_ftype_t *t = find(path);
	return t ? t->app : NULL;

}

const char *z_ftype_desc_for(const char *path) {

	const z_ftype_t *t = find(path);
	return t ? t->desc : NULL;

}

bool z_ftype_is_executable(const char *path) {

	if (!path) return false;

	// Only a file with NO extension is a candidate. Something called
	// FOO.DAT is not a program that happens to be mislabelled, it is
	// a data file, and guessing otherwise risks executing it.
	if (z_ftype_ext(path)) return false;

	int h = fs_open_read(path);
	if (h < 0) return false;

	uint8_t magic[4] = { 0, 0, 0, 0 };
	int n = fs_read_chunk(h, magic, sizeof(magic));

	fs_close_handle(h);

	if (n != (int)sizeof(magic)) return false;

	return magic[0] == Z_EXEC_MAGIC0 && magic[1] == Z_EXEC_MAGIC1 &&
		magic[2] == Z_EXEC_MAGIC2 && magic[3] == Z_EXEC_MAGIC3;

}
