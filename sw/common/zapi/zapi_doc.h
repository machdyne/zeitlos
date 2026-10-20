/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * (help "name") and (apropos "text"): looking things up in zapi.txt,
 * the API reference tools/gen_zapi.py makes from zapi.c's `//:` entries
 * and zapi_scheme.txt, in flash at /docs/zapi.txt (docs/scheme_api.md).
 *
 * No Scheme and no filesystem in here: the file comes through a read
 * function shaped like fs_read_chunk(), and the answer goes to stdout.
 * That keeps it testable on the host (tests/test_zapi_doc.c).
 *
 * The format it relies on, which gen_zapi.py guarantees:
 *
 *   - an entry starts after a blank line, with its call shapes, each a
 *     line starting "(name" at column 0;
 *   - its description lines are indented two spaces, examples "  > ";
 *   - a blank line ends it;
 *   - no line is longer than ZDOC_LINE - 1 (longer ones are cut).
 */

#ifndef ZAPI_DOC_H
#define ZAPI_DOC_H

#define ZDOC_LINE			128
#define ZDOC_APROPOS_MAX	100		// lines of matches before "... N more"

// fs_read_chunk()'s contract: bytes read, 0 at the end, < 0 on error
typedef int (*zdoc_read_fn)(void *ctx, void *buf, int max);

// Prints the entry for `name`. Returns how many lines it printed: 0 if
// there is no such entry.
int zdoc_help(const char *name, zdoc_read_fn rd, void *ctx);

// Prints one line per entry whose name, call shapes or description
// contain `text`, ignoring case: the name and the first line of its
// description. "" matches everything. Returns how many matched, all of
// them, though only ZDOC_APROPOS_MAX are printed.
int zdoc_apropos(const char *text, zdoc_read_fn rd, void *ctx);

#endif
