#ifndef ZF_EM_H
#define ZF_EM_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What every document reader needs between its parser and the
 * writer it feeds:
 *
 *  - paragraphs opened when their first text arrives (a reader that
 *    has seen <p> does not yet know whether anything will be in it),
 *    or eagerly, for formats in which an empty paragraph is a blank
 *    line the author put there (docx, odt, txt)
 *  - begin() called before the first paragraph, with a title if one
 *    turned up first (html's <title>, epub's dc:title)
 *  - runs of the same style joined, and split only between UTF-8
 *    characters, so a writer never sees half of one
 *  - optionally, whitespace collapsed the way HTML and ODF collapse
 *    it: any run of it is one space, none at a paragraph's start or end
 *  - bytes that are not UTF-8 read as Windows-1252 (zf_em_bytes)
 */

#include "zf.h"

#define ZF_EM_BUF	512

typedef struct {
	zf_doc_t	*d;
	bool		begun, open, lazy_open;
	int			kind, level;		// of the paragraph pending or open
	bool		pending;			// a paragraph is asked for, not yet open
	int			style;
	char		buf[ZF_EM_BUF];
	uint32_t	n;
	bool		collapse;			// HTML-style whitespace
	bool		space;				// a collapsed space is owed
	bool		any;				// text in this paragraph yet
	char		title[128];
	int			err;
	// zf_em_bytes' partial UTF-8 character
	uint8_t		u8[4];
	int			u8n, u8need;
} zf_em_t;

void zf_em_init(zf_em_t *e, zf_doc_t *d, bool collapse);
// A paragraph of this kind. With `eager`, it is opened now, and will
// be written even if empty.
int zf_em_para(zf_em_t *e, int kind, int level, bool eager);
// Changes the kind of the paragraph asked for or open, if nothing has
// been written in it yet.
void zf_em_kind(zf_em_t *e, int kind, int level);
int zf_em_text(zf_em_t *e, const char *s, uint32_t n, int style);
// Text that is never collapsed: <text:s/>, &nbsp;, tabs in ODF.
int zf_em_raw(zf_em_t *e, const char *s, uint32_t n, int style);
// Bytes that should be UTF-8 and may be Windows-1252 instead.
int zf_em_bytes(zf_em_t *e, const uint8_t *s, uint32_t n, int style);
int zf_em_brk(zf_em_t *e);
int zf_em_end_para(zf_em_t *e);
int zf_em_end(zf_em_t *e);
bool zf_em_in_para(const zf_em_t *e);

#endif
