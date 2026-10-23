#ifndef PX_IMPORT_H
#define PX_IMPORT_H

/*
 * pix -- pictures from elsewhere: PNG, GIF, BMP, PNM, JPEG (and a
 * colour ZBM), through the system's decoders (sw/common/zimg.h, in
 * colour), reduced to a palette. See docs/pix_app.md, "Importing".
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Two ways in:
 *
 *   px_import_picture()  a new colour picture whose palette is made
 *                        from the image: every colour kept when there
 *                        are 256 or fewer (as RGB444), else 256 chosen
 *                        by median cut; then each pixel the nearest
 *   px_import_piece()    a piece in the document's own palette, each
 *                        colour the nearest entry (an exact match is
 *                        itself), see-through where the image is
 *                        (alpha under half); the editor puts it down
 *                        as a pasted selection
 *
 * Images come in at most 640 x 480 (zimg.h); a bigger one is halved
 * until it fits (a ZBM is cut to it instead). JPEG, grey PNM and a
 * one-bit ZBM come in grey: their decoders give no colour. See-through
 * comes from a PNG: RGBA, grey with alpha, a palette's transparent
 * entries (not yet a GIF's). The source is read twice -- once to count the
 * colours, once for the pixels -- so it must rewind; nothing larger
 * than a row and a 4096-entry table is held besides the result.
 * Pure C: the host tests run it on files in memory.
 */

#include "px_doc.h"
#include "px_edit.h"

/* Where the bytes come from, and how to start again. */
typedef struct {
	int  (*read)(void *ctx, uint8_t *buf, int len);
	bool (*rewind)(void *ctx);
	void *ctx;
} px_src_t;

/* A source over bytes in memory. */
typedef struct {
	const uint8_t *data;
	uint32_t len, pos;
} px_mem_t;
void px_src_mem(px_src_t *s, px_mem_t *m, const uint8_t *data, uint32_t len);

/* Whether the first bytes are an image pix can bring in. */
bool px_import_sniff(const uint8_t *hdr, int n);

/* A new colour picture in d (reset first). *colours_seen is how many
 * different colours the image had, as RGB444. */
bool px_import_picture(px_doc_t *d, px_src_t *src, int *colours_seen, char *err, int errlen);

/* A piece in d's palette (or, in two colours, ink where the image is
 * lighter than half: white is ink, as in the dock's icon PNGs), and
 * see-through where the image is. */
bool px_import_piece(const px_doc_t *d, px_src_t *src, px_piece_t *p, char *err, int errlen);

#endif
