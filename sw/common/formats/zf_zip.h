#ifndef ZF_ZIP_H
#define ZF_ZIP_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ZIP, which is what docx, xlsx, odt, ods and epub are underneath.
 *
 * Reading: the central directory at the end of the file is the index;
 * an entry is read through a zf_src_t, stored or deflated (sw/common/
 * zinflate.c, raw), its CRC-32 checked at the end and its output cut
 * off at ZF_ENTRY_MAX. Encrypted entries and ZIP64 are refused. One
 * entry is inflated at a time: the 32KB window is shared.
 *
 * Writing: stored entries only -- no deflate encoder, and none is
 * needed: Word, Excel and LibreOffice all open a zip whose entries are
 * stored. Each local header is written with its CRC and sizes zero and
 * patched by seeking back once the entry is complete, so nothing has
 * to be held in memory; the output must be seekable.
 */

#include "zf.h"
#include "../zinflate.h"
#include "zf_xml.h"

typedef struct {
	char		name[ZF_PATH_MAX];
	uint32_t	crc, csize, usize, lho;
	uint16_t	method, flags;
} zz_entry_t;

typedef struct {
	zf_io_t		*io;
	uint32_t	cd_off, cd_size, count;
	uint32_t	at;			// next central directory record
	uint32_t	i;
} zz_t;

int zz_open(zz_t *z, zf_io_t *io);
// The next entry in the central directory: 1, or 0 after the last, or
// a ZF_E_*. zz_rewind() starts again.
int zz_next(zz_t *z, zz_entry_t *e);
void zz_rewind(zz_t *z);
// The entry called `name` (exactly), 1 if found.
int zz_find(zz_t *z, const char *name, zz_entry_t *e);

typedef struct {
	zz_t		*z;
	zz_entry_t	e;
	uint32_t	foff;		// where in the file the next compressed byte is
	uint32_t	in_left;	// compressed bytes still to read
	uint32_t	out;		// bytes produced
	uint32_t	crc;
	z_inflate_t	inf;
	uint8_t		inbuf[2048];
	uint32_t	ipos, ilen;
	bool		done;
	int			err;
} zz_rd_t;

// Opens entry `e` for reading through `src` (whose ctx is `r`).
int zz_entry_src(zz_t *z, const zz_entry_t *e, zz_rd_t *r, zf_src_t *src);
// Closes it: frees the shared window for the next entry.
void zz_entry_close(zz_rd_t *r);

// One entry, opened by name, with a buffered reader and an XML
// tokenizer over it -- what the docx, odt, epub, xlsx and ods readers
// all want. One at a time (the inflate window is shared).
typedef struct {
	zz_rd_t		zr;
	zf_src_t	src;
	zf_rd_t		rd;
	zx_t		x;
	bool		open;
} zz_ent_t;

extern zz_ent_t zz_ent;
// ZF_OK, or ZF_E_FORMAT if there is no such entry.
int zz_ent_open(zz_t *z, const char *name);
// The entry's read error, if it had one.
int zz_ent_close(void);

// The 32KB inflate window, for an inflater outside a zip (gzip):
// NULL while an entry has it.
uint8_t *zz_window_take(void);
void zz_window_give(void);

// -- writing --

#define ZW_MAX_ENTRIES	16

typedef struct {
	zf_io_t		*io;
	struct { char name[64]; uint32_t crc, size, off; } ent[ZW_MAX_ENTRIES];
	int			n;
	bool		open;
} zw_t;

void zw_init(zw_t *w, zf_io_t *io);
int zw_begin(zw_t *w, const char *name);
int zw_write(zw_t *w, const void *buf, uint32_t n);
int zw_puts(zw_t *w, const char *s);
int zw_end(zw_t *w);
int zw_finish(zw_t *w);
// A whole small entry at once.
int zw_file(zw_t *w, const char *name, const char *content);

#endif
