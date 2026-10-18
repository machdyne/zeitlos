#ifndef ZF_H
#define ZF_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Document, table and archive formats -- read and written by
 * sw/apps/convert, the one program that links this directory
 * (docs/convert.md).
 *
 * -- the shape of it --
 *
 * Every conversion is a reader feeding a writer through one of two
 * streams of events:
 *
 *   documents   paragraphs (body, heading 1-6, list item, preformatted)
 *               holding runs of UTF-8 text, bold and/or italic, and
 *               line breaks -- docx, odt, rtf, html, epub, md, txt in;
 *               docx, odt, rtf, html, md, txt out
 *   tables      cells in row order, each text, a number or a formula --
 *               xlsx, ods, csv, tsv, zss in; the same out
 *
 * and archives (zip, gz, tar, tar.gz), which are extracted into a
 * directory. Nothing holds a whole file: readers pull from a stream,
 * writers push to one, and the memory a conversion needs is the 32KB
 * inflate window, a few buffers, and -- for xlsx, whose cells point
 * into a table of strings -- that table.
 *
 * -- I/O --
 *
 * Through zf_io_t, so the same code runs on the board (sw/apps/convert
 * wraps fs_*) and on the build machine (tests/ wraps stdio).
 *
 * -- untrusted input --
 *
 * Every file here may be hostile. Lengths are checked against what
 * was read, never trusted; an archive entry is cut off at
 * ZF_ENTRY_MAX bytes of output and a conversion at ZF_OUT_MAX, so a
 * small file that expands to gigabytes is an error, not a full disk;
 * CRCs are checked; nesting is bounded; a path in an archive that
 * climbs out of the target directory is refused.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -- limits --

#define ZF_ENTRY_MAX	(64u * 1024 * 1024)	// one archive entry, inflated
#define ZF_OUT_MAX		(256u * 1024 * 1024)	// one conversion's output
#define ZF_PATH_MAX		256

// -- errors (negative) --

#define ZF_OK			0
#define ZF_E_IO			(-1)	// a read or write failed
#define ZF_E_FORMAT		(-2)	// not a file of the kind it claimed to be
#define ZF_E_CORRUPT	(-3)	// malformed, truncated, or a CRC that does not match
#define ZF_E_UNSUPP		(-4)	// a feature this reads past: encryption, ZIP64, ...
#define ZF_E_LIMIT		(-5)	// bigger than ZF_ENTRY_MAX / ZF_OUT_MAX allow
#define ZF_E_NOMEM		(-6)
#define ZF_E_PATH		(-7)	// an archive path outside the target
#define ZF_E_CANCEL		(-8)	// zf_io_t's cancel said stop

const char *zf_strerror(int e);

// -- I/O --

typedef struct zf_io zf_io_t;
struct zf_io {
	void		*h;
	int32_t		(*read)(zf_io_t *io, void *buf, uint32_t n);		// bytes, 0 at end, <0 error
	int32_t		(*write)(zf_io_t *io, const void *buf, uint32_t n);	// n, or <0
	bool		(*seek)(zf_io_t *io, uint32_t pos);					// absolute
	uint32_t	size;		// of a file opened for reading
	uint32_t	pos;		// kept by the adapter
	uint32_t	written;	// total, checked against ZF_OUT_MAX
};

// A directory to extract into: files are created by path relative to
// it, and directories made on the way.
typedef struct zf_dir zf_dir_t;
struct zf_dir {
	void		*h;
	// Opens `rel` for writing (directories before it already made).
	bool		(*open)(zf_dir_t *d, const char *rel, zf_io_t *out);
	void		(*close)(zf_dir_t *d, zf_io_t *out);
	bool		(*mkdir)(zf_dir_t *d, const char *rel);
	// Removes a file left half-written by a failure or a cancel. May be
	// NULL.
	void		(*remove)(zf_dir_t *d, const char *rel);
};

// A sink's own errors and the caller's chance to stop: polled between
// pieces of work. Returning true ends the conversion with ZF_E_CANCEL.
extern bool (*zf_cancel_poll)(void);
// Progress, 0..1000 of the input read, when it can be told.
extern void (*zf_progress)(uint32_t permille);

// Scratch memory for the few readers that need a table -- xlsx's
// shared strings, an epub's manifest. The program sets it (convert
// uses its arena); a reader that finds it too small fails with
// ZF_E_NOMEM rather than truncating. One conversion at a time owns it.
extern uint8_t *zf_heap;
extern uint32_t zf_heap_size;

int32_t zf_write(zf_io_t *o, const void *buf, uint32_t n);
int32_t zf_puts(zf_io_t *o, const char *s);

// -- a buffered reader over a source --

typedef struct zf_src zf_src_t;
struct zf_src {
	void		*ctx;
	int32_t		(*read)(zf_src_t *s, uint8_t *buf, uint32_t n);
};

#define ZF_RD_BUF	4096

typedef struct {
	zf_src_t	*src;
	uint8_t		buf[ZF_RD_BUF];
	uint32_t	pos, len;
	bool		eof;
	int			err;
	uint32_t	consumed;
} zf_rd_t;

void zf_rd_init(zf_rd_t *r, zf_src_t *src);
int zf_rd_getc(zf_rd_t *r);			// a byte, or -1 at end (r->err says why)
int zf_rd_peek(zf_rd_t *r);

// A zf_src_t straight from a zf_io_t (a whole file).
void zf_src_io(zf_src_t *s, zf_io_t *io);

// -- small helpers --

uint32_t zf_crc32(uint32_t crc, const void *p, uint32_t n);
int zf_utf8_put(uint32_t cp, char *out);
// Windows-1252 byte to codepoint (the five undefined bytes map to
// themselves).
uint32_t zf_cp1252(uint8_t b);
// &name; and &#N; / &#xH; -- `ent` is what is between & and ;. The
// codepoint, or 0 if unknown.
uint32_t zf_entity(const char *ent);
bool zf_ieq(const char *a, const char *b);
bool zf_ieq_n(const char *a, const char *b, int n);
const char *zf_ext(const char *path);

// -- documents --

enum {
	ZF_P_BODY = 0,
	ZF_P_HEAD,		// level 1-6
	ZF_P_ITEM,		// level 1-, a list item
	ZF_P_PRE,		// preformatted: lines kept as they are
};

#define ZF_S_BOLD	1
#define ZF_S_ITAL	2

typedef struct zf_doc zf_doc_t;
struct zf_doc {
	void	*ctx;
	int		(*begin)(zf_doc_t *d, const char *title);
	int		(*para)(zf_doc_t *d, int kind, int level);		// starts one
	int		(*text)(zf_doc_t *d, const char *s, uint32_t n, int style);
	int		(*brk)(zf_doc_t *d);							// a line break in it
	int		(*end_para)(zf_doc_t *d);
	int		(*end)(zf_doc_t *d);
};

// The document writers. `w` is a writer's state, at least
// ZF_DOCW_STATE bytes; out must be seekable for docx and odt.
#define ZF_DOCW_STATE	2048
typedef enum { ZF_DW_TXT, ZF_DW_MD, ZF_DW_HTML, ZF_DW_RTF, ZF_DW_DOCX, ZF_DW_ODT } zf_dw_kind_t;
int zf_docw_open(zf_doc_t *d, void *state, zf_dw_kind_t kind, zf_io_t *out);

// The document readers: each walks `in` and calls `d`.
int zf_read_docx(zf_io_t *in, zf_doc_t *d);
int zf_read_odt(zf_io_t *in, zf_doc_t *d);
int zf_read_rtf(zf_io_t *in, zf_doc_t *d);
int zf_read_html(zf_io_t *in, zf_doc_t *d);
int zf_read_epub(zf_io_t *in, zf_doc_t *d);
int zf_read_md(zf_io_t *in, zf_doc_t *d);			// Markdown: headings, lists, emphasis
int zf_read_txt(zf_io_t *in, zf_doc_t *d);			// a paragraph per line; UTF-8, UTF-16 or Windows-1252

// HTML from any byte source -- epub's chapters, and zf_read_html().
// With body_only, only what is inside <body> is read, and successive
// calls for the same `d` continue one document -- an epub's chapters:
// zf_html_reset(d), zf_html_title(), zf_html_src(..., true) per
// chapter, then zf_html_finish(d).
int zf_html_src(zf_src_t *src, zf_doc_t *d, bool body_only);
void zf_html_reset(zf_doc_t *d);
void zf_html_title(const char *title);
int zf_html_finish(zf_doc_t *d);

// -- tables --

enum { ZF_C_TEXT = 0, ZF_C_NUM, ZF_C_FORMULA };

typedef struct zf_tab zf_tab_t;
struct zf_tab {
	void	*ctx;
	int		(*begin)(zf_tab_t *t);
	// row and col from 0, in row order and, within a row, column order.
	// `value` is the text, the number as written ("12.5"), or for a
	// formula its cached value (may be ""); `formula` is the formula,
	// in this system's syntax ("=SUM(A1:A3)"), or NULL.
	int		(*cell)(zf_tab_t *t, uint32_t row, uint32_t col, int kind,
				const char *value, const char *formula);
	int		(*end)(zf_tab_t *t);
};

#define ZF_TABW_STATE	4096
typedef enum { ZF_TW_CSV, ZF_TW_TSV, ZF_TW_ZSS, ZF_TW_XLSX, ZF_TW_ODS } zf_tw_kind_t;
int zf_tabw_open(zf_tab_t *t, void *state, zf_tw_kind_t kind, zf_io_t *out);

int zf_read_xlsx(zf_io_t *in, zf_tab_t *t);
int zf_read_ods(zf_io_t *in, zf_tab_t *t);
int zf_read_csv(zf_io_t *in, zf_tab_t *t, char sep);	// ',' or '\t'
int zf_read_zss(zf_io_t *in, zf_tab_t *t);

// Cells the last conversion could not write (outside A1:Z999 for zss).
extern uint32_t zf_tab_dropped;
bool zf_is_num(const char *s);

// -- archives --

int zf_extract_zip(zf_io_t *in, zf_dir_t *out);
int zf_extract_tar(zf_src_t *in, zf_dir_t *out);
int zf_extract_gz(zf_io_t *in, zf_io_t *out);		// one file
int zf_extract_tgz(zf_io_t *in, zf_dir_t *out);

// -- what a file is --

typedef enum {
	ZF_T_UNKNOWN = 0,
	ZF_T_TXT, ZF_T_MD, ZF_T_HTML, ZF_T_RTF, ZF_T_DOCX, ZF_T_ODT, ZF_T_EPUB,
	ZF_T_CSV, ZF_T_TSV, ZF_T_ZSS, ZF_T_XLSX, ZF_T_ODS,
	ZF_T_ZIP, ZF_T_GZ, ZF_T_TAR, ZF_T_TGZ,
} zf_type_t;

// From the name, then -- for a zip, and to catch a misnamed file --
// from the first bytes. `in` may be NULL (the name alone).
zf_type_t zf_detect(const char *name, zf_io_t *in);
const char *zf_type_name(zf_type_t t);
// "docx", "tar.gz", "markdown", ... -> the type (for convert -t).
zf_type_t zf_type_from_name(const char *name);
// Whether zf_convert() does from -> to. An archive's `to` is
// ZF_T_UNKNOWN: it is extracted into a folder (a .gz into one file).
bool zf_can_convert(zf_type_t from, zf_type_t to);
bool zf_type_is_doc(zf_type_t t);
bool zf_type_is_tab(zf_type_t t);
bool zf_type_is_arch(zf_type_t t);

// The whole conversion: `in` of type `from` into `out` of type `to`
// (a file), or into `dir` for an archive. Returns ZF_OK or a ZF_E_*.
int zf_convert(zf_io_t *in, zf_type_t from, zf_io_t *out, zf_type_t to,
	zf_dir_t *dir);

#endif
