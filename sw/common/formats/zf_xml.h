#ifndef ZF_XML_H
#define ZF_XML_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A pull tokenizer for the XML inside Office documents and EPUBs: the
 * caller asks for the next start tag, end tag or run of text. No tree,
 * no validation, no namespaces resolved -- names come as written,
 * "w:p", and zx_local() strips the prefix. Entities and character
 * references are decoded, CDATA is text, comments, processing
 * instructions and the DOCTYPE are skipped. An empty element <x/>
 * comes as a START with `empty` set and then its END.
 *
 * Text arrives in pieces of up to ZX_TEXT_MAX bytes, never cutting a
 * UTF-8 character; a caller that wants all of an element's text joins
 * them. Attributes that do not fit ZX_ATTR_MAX are dropped, not
 * truncated.
 */

#include "zf.h"

#define ZX_NAME_MAX		64
#define ZX_ATTR_MAX		2048
#define ZX_TEXT_MAX		1024

typedef enum { ZX_EOF = 0, ZX_START, ZX_END, ZX_TEXT, ZX_ERR } zx_tok_t;

typedef struct {
	zf_rd_t		*rd;
	char		name[ZX_NAME_MAX];
	bool		empty;
	char		attrs[ZX_ATTR_MAX];		// "name\0value\0name\0value\0" ...
	uint32_t	alen;
	char		text[ZX_TEXT_MAX + 1];	// NUL-terminated
	uint32_t	tlen;
	bool		end_next;				// the END of an empty element is owed
	bool		in_cdata;
	int			err;
} zx_t;

void zx_init(zx_t *x, zf_rd_t *rd);
zx_tok_t zx_next(zx_t *x);

// The value of attribute `name` (as written, prefix and all) of the
// last START, or NULL.
const char *zx_attr(const zx_t *x, const char *name);
// The same, matching only the part after any prefix: "val" finds
// "w:val".
const char *zx_attr_local(const zx_t *x, const char *local);
// The name without its prefix.
const char *zx_local(const char *name);

// Skips to the END of the element whose START was just returned.
zx_tok_t zx_skip(zx_t *x);

#endif
