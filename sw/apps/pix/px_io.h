#ifndef PX_IO_H
#define PX_IO_H

/*
 * pix -- the file formats. See docs/pix_app.md, "Files".
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Every reader takes a whole file already in memory and fills a
 * px_doc_t; every writer returns a whole file in a malloc()ed buffer.
 * No file system here, so the host tests run all of it.
 *
 *   .zic  ZIC2, the dock's icons (sw/apps/wm/dock_cfg.h has the layout)
 *   .zfn  ZFN1, a bitmap font (tools/gen_fonts.py has the layout)
 *   .zbm  ZBM1, a 1bpp picture (sw/common/zbm.h)
 *   .zsp  sprite sheet, text: "zsp 1", "colours N", then
 *         "sprite NAME W H" and H rows of W characters ('.' clear,
 *         '0' colour 0, '1'..'9' 'a'..'f' an index; in a 2-colour
 *         sheet anything but '.' and '0' is ink, as wm's cursor reader
 *         takes it). Other lines are kept as they are and written back.
 */

#include <stdint.h>
#include <stdbool.h>

#include "px_doc.h"

/* The kind of a file, by its first bytes, else by its name's ending;
 * 0 if neither says. */
int px_detect(const uint8_t *data, uint32_t len, const char *path);

/* Read a file of `kind` into `d` (which is reset first). On failure,
 * false and a reason in `err`. */
bool px_read(px_doc_t *d, int kind, const uint8_t *data, uint32_t len,
	char *err, int errlen);

/* .zic only: lay the icons of an overrides file (icons.zic) over `d`,
 * replacing a cell's pixels and spoken name by name, adding a cell for
 * a name `d` lacks. Every cell touched is flagged PX_CELL_CHANGED, so
 * a later save of the overrides carries it again. */
bool px_zic_overlay(px_doc_t *d, const uint8_t *data, uint32_t len,
	char *err, int errlen);

/* The file for `d`. `changed_only` writes only the cells flagged
 * PX_CELL_CHANGED (the .zic overrides file). Returns NULL if out of
 * memory, else a buffer the caller frees, its size in *len. */
uint8_t *px_write(const px_doc_t *d, bool changed_only, uint32_t *len);

#endif
