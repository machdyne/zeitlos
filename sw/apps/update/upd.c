/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * update's core. See upd.h, and docs/update.md for the rules.
 *
 * -- the order of things --
 *
 *   1. Fetch the release's manifest (LATEST first for "latest") to the
 *      card, through its signature check when it must be signed, and
 *      check it: format, order, and that its snapshot id is the hash of
 *      its entries -- so a damaged transfer is refused, not obeyed.
 *   2. Check the card's /sys/manifest, if it has one.
 *   3. Plan, by merging the two (both are sorted by path, and read from
 *      the card a line at a time: no manifest is held in memory): which
 *      files to write, which to remove, which to leave. The card is read
 *      (hashed) only where the manifests cannot answer.
 *   4. Check the free space, then do it, file by file.
 *   5. Write /sys/manifest and /sys/version, describing what is really
 *      on the card now -- which is the release's entry for every file
 *      that is done, and the old entry for one that is not.
 *
 * -- one file --
 *
 *   fetch to <path>.upd, hashing as it arrives; wrong size or hash:
 *     try once more, then give up on the whole run (the server or the
 *     link is the problem, and every other file would fail the same way)
 *   <path> exists: rename it to <path>.old
 *     refused as busy: a program has it open for writing. Leave the
 *     .upd (the next run uses it without fetching again), say so, go on.
 *   rename <path>.upd to <path>
 *   remove <path>.old
 *
 * A run killed at any point leaves either the old file, the new file,
 * or no file and a .upd/.old beside it; the next run sees the file
 * missing or different and finishes the job, using a complete .upd
 * rather than fetching it again.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upd.h"
#include "zsha256.h"
#include "zpaths.h"         /* the card's fixed paths (docs/layout.md) */
#include "zexec_lz4.h"      /* format 3's LZ4 blocks */
#include "monocypher-ed25519.h" /* signatures (docs/distribution.md) */

#define FORMAT          4       // sets; 1 (no CRC), 2 (raw) and 3 (no sets) are still read
#define MAGIC           "zeitlos-manifest"
#define ID_DIGITS       12
#define OBJECT_DIGITS   20
#define HASH_HEX        64
#define NAME_MAX_LEN    32
#define IO_BUF          8192
#define PROGRESS_MS     500
#define PROGRESS_MIN    (64 * 1024)	// smaller files get one line

#define SYS_MANIFEST    Z_PATH_SYS_MANIFEST
#define SYS_VERSION     Z_PATH_SYS_VERSION

static const upd_io_t *io;

// -- output --------------------------------------------------------------

static void say(const char *fmt, ...) {
	char b[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	io->out(io->ctx, b);
}

// "612 KB", "4.2 MB", "980 bytes": whole numbers, no floating point.
static const char *size_str(uint32_t n, char *b, int cap) {
	if (n < 1024)
		snprintf(b, cap, "%lu bytes", (unsigned long)n);
	else if (n < 1024 * 1024)
		snprintf(b, cap, "%lu KB", (unsigned long)((n + 512) / 1024));
	else {
		// 32-bit throughout: n / 1024 is at most 4M, times 10 fits.
		uint32_t tenths = ((n / 1024) * 10 + 512) / 1024;
		snprintf(b, cap, "%lu.%lu MB", (unsigned long)(tenths / 10),
			(unsigned long)(tenths % 10));
	}
	return b;
}

// No 64-bit division on the board: bytes / 1024 is at most 4M, and
// times 1000 still fits in 32 bits.
static uint32_t kb_per_s(uint32_t bytes, uint32_t ms) {
	if (ms < 1) ms = 1;
	return (bytes / 1024) * 1000 / ms;
}

static uint32_t percent(uint32_t part, uint32_t whole) {
	if (!whole) return 100;
	return whole >= 100 ? part / (whole / 100) : part * 100 / whole;
}

// -- hashes --------------------------------------------------------------

static void to_hex(const uint8_t *d, int n, char *out) {
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < n; i++) {
		out[2 * i] = hx[d[i] >> 4];
		out[2 * i + 1] = hx[d[i] & 15];
	}
	out[2 * n] = 0;
}

static bool hash_eq(const char *a, const char *b) {
	return memcmp(a, b, HASH_HEX) == 0;
}

// -- CRC-32 ----------------------------------------------------------------
//
// zlib's (IEEE 802.3, reflected 0xEDB88320), four bytes a step from four
// 256-entry tables ("slicing by four"), built on first use: 4 KB of
// .bss, and roughly twenty times SHA-256's software speed on this CPU,
// which is what the card check runs at (manifest.py, "two hashes").

static uint32_t crc_t[4][256];
static bool crc_ready;

static void crc_init(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_t[0][i] = c;
	}
	for (uint32_t i = 0; i < 256; i++)
		for (int t = 1; t < 4; t++)
			crc_t[t][i] = (crc_t[t - 1][i] >> 8) ^ crc_t[0][crc_t[t - 1][i] & 0xFF];
	crc_ready = true;
}

// Running CRC: start from 0, feed pieces, the result is the CRC so far.
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
	if (!crc_ready) crc_init();
	uint32_t c = ~crc;
	while (n && ((uintptr_t)p & 3)) {
		c = crc_t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
		n--;
	}
	while (n >= 4) {
		// Aligned by the loop above, and the byte order is little-endian
		// (RISC-V, and every host the tests run on): one word load, not
		// four byte loads put together.
		c ^= *(const uint32_t *)(const void *)p;
		c = crc_t[3][c & 0xFF] ^ crc_t[2][(c >> 8) & 0xFF] ^
			crc_t[1][(c >> 16) & 0xFF] ^ crc_t[0][c >> 24];
		p += 4;
		n -= 4;
	}
	while (n--)
		c = crc_t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
	return ~c;
}

static int hexval(char c) {
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// -- paths ---------------------------------------------------------------

// "/" + a manifest path (+ suffix). false if it does not fit.
static bool card_path(char *out, const char *p, int plen, const char *suffix) {
	int sl = (int)strlen(suffix);
	if (1 + plen + sl + 1 > UPD_PATH_MAX) return false;
	out[0] = '/';
	memcpy(out + 1, p, plen);
	memcpy(out + 1 + plen, suffix, sl + 1);
	return true;
}

static bool path_ok(const char *p, int n) {
	if (n <= 0 || n > UPD_PATH_MAX - 8) return false;
	if (p[0] == '/' || p[0] == ' ' || p[n - 1] == '/' || p[n - 1] == ' ')
		return false;
	for (int i = 0; i < n; i++) {
		if (p[i] == '\r' || p[i] == '\n' || p[i] == '\\') return false;
		if (p[i] == '/' && i + 1 < n && p[i + 1] == '/') return false;
		bool start = i == 0 || p[i - 1] == '/';
		if (start && p[i] == '.') {
			int e = i;
			while (e < n && p[e] != '/') e++;
			if (e - i == 1 || (e - i == 2 && p[i + 1] == '.')) return false;
		}
	}
	return true;
}

static int path_cmp(const char *a, int al, const char *b, int bl) {
	int n = al < bl ? al : bl;
	int c = memcmp(a, b, n);
	if (c) return c;
	return al - bl;
}

static bool name_ok(const char *s) {
	int n = (int)strlen(s);
	if (n < 1 || n > NAME_MAX_LEN || !strcmp(s, "o")) return false;
	for (int i = 0; i < n; i++) {
		char c = s[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9');
		if (!alnum && !(i > 0 && (c == '.' || c == '_' || c == '-')))
			return false;
	}
	return true;
}

// A set name (release/lib/manifest.py, SET_RE), or "-" when `dash`.
static bool set_ok(const char *s, int n, bool dash) {
	if (dash && n == 1 && s[0] == '-') return true;
	if (n < 1 || n > 16 || s[0] < 'a' || s[0] > 'z') return false;
	for (int i = 1; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
				s[i] == '-'))
			return false;
	return true;
}

// -- the manifests, read from the card a line at a time --------------------
//
// No manifest is held in memory. With every set and the ask packs a
// release's is some 1,700 lines and 200 KB, and three at once -- the
// release's, the card's and the new one -- with their parsed entries
// ran a board's 512K heap out ("out of memory writing the manifest",
// October 2026). So the release's is spooled to the card as it arrives
// (SPOOL), the card's is /sys/manifest itself, and the new one is
// written straight to /sys/manifest.upd. Both are sorted by path, so
// they are read side by side, a line of each at a time (merge_t); the
// plan is one byte per file (act[]). Each step that needs them reads
// both files again: at a card's ~600 KB/s, a fraction of a second.

#define LINE_MAX_   (UPD_PATH_MAX + 192)
#define SET_NAME    16
#define SET_TITLE   40
#define SET_DESC    96
#define HEAD_MAX    4096
#define SPOOL       SYS_MANIFEST ".rel"     // the release's, fetched

typedef struct {
	const char *path;   // into a line; NOT terminated: path_len
	uint16_t path_len;
	const char *hash;   // SHA-256: 64 hex digits, into the line
	uint32_t size;
	uint32_t crc;       // CRC-32 (zlib's), when has_crc
	bool has_crc;       // false in a format-1 manifest
	char policy;        // 'r' or 'k'
	const char *set;    // format 4: its set, into the line ("-": not known);
	uint8_t set_len;    // NULL before format 4
} upd_entry_t;

typedef struct {
	char name[SET_NAME + 1], title[SET_TITLE + 1], desc[SET_DESC + 1];
} mset_t;

// A manifest's header, copied out of it, and what checking it found.
typedef struct {
	int format;
	char name[NAME_MAX_LEN + 1], version[32], snapshot[ID_DIGITS + 1];
	char source[80], made[32], packs[160], abi[24];
	char installed[UPD_SETS_MAX * (SET_NAME + 1) + 1];
	bool has_installed;
	mset_t sets[UPD_SETS_MAX];
	int nsets;
	int nfiles, ndirs;
	uint64_t total;             // bytes of all its files
	char head[HEAD_MAX];        // the header's text, as it is (for "unchanged?")
	uint32_t head_len;
} mhead_t;

// A manifest file, a line at a time.
typedef struct {
	int h;
	int format;
	uint8_t buf[IO_BUF];
	int bn, bp;
	bool eof;
	char line[LINE_MAX_];
	int ll;                     // its length, without the newline
	int n;                      // line number
	upd_entry_t e;              // the current file line, parsed
} mrd_t;

static void copy_field(char *out, int cap, const char *v, int vl) {
	if (vl > cap - 1) vl = cap - 1;
	if (vl < 0) vl = 0;
	memcpy(out, v, vl);
	out[vl] = 0;
}

static int mr_open(mrd_t *r, const char *path) {
	r->h = io->open_read(io->ctx, path);
	r->bn = r->bp = 0;
	r->eof = false;
	r->ll = 0;
	r->n = 0;
	r->format = 0;
	return r->h >= 0 ? UPD_OK : r->h;
}

static void mr_close(mrd_t *r) {
	if (r->h >= 0) io->close(io->ctx, r->h);
	r->h = -1;
}

// The next line into r->line. 1, 0 at the end, -1 a read error, -2 a
// line too long or the last one without its newline.
static int mr_line(mrd_t *r) {
	int n = 0;
	for (;;) {
		if (r->bp == r->bn) {
			if (r->eof) return n ? -2 : 0;
			int got = io->read(io->ctx, r->h, r->buf, sizeof r->buf);
			if (got < 0) return -1;
			if (got == 0) { r->eof = true; continue; }
			r->bn = got;
			r->bp = 0;
		}
		char c = (char)r->buf[r->bp++];
		if (c == '\n') break;
		if (n >= LINE_MAX_ - 1) return -2;
		r->line[n++] = c;
	}
	r->line[n] = 0;
	r->ll = n;
	r->n++;
	return 1;
}

// Line `l` (`ll` bytes) as a file entry of format `fmt`.
static bool parse_entry(char *l, int ll, int fmt, upd_entry_t *e) {
	char *nl = l + ll;
	memset(e, 0, sizeof *e);
	// f <64 hex> [<crc>] <size> <policy> [<set>] <path>
	if (ll < 2 + HASH_HEX + 6 + (fmt >= 2 ? 9 : 0) || l[0] != 'f' || l[1] != ' ')
		return false;
	char *q = l + 2;
	for (int i = 0; i < HASH_HEX; i++) {
		char c = q[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	e->hash = q;
	q += HASH_HEX;
	if (*q++ != ' ') return false;
	if (fmt >= 2) {
		uint32_t c = 0;
		for (int i = 0; i < 8; i++) {
			int v = hexval(q[i]);
			if (v < 0) return false;
			c = c << 4 | (uint32_t)v;
		}
		if (q[8] != ' ') return false;
		e->crc = c;
		e->has_crc = true;
		q += 9;
	}
	uint64_t sz = 0;
	char *d0 = q;
	while (q < nl && *q >= '0' && *q <= '9') {
		sz = sz * 10 + (uint32_t)(*q++ - '0');
		if (sz > 0xFFFFFFFFu) return false;
	}
	if (q == d0 || q >= nl || *q++ != ' ') return false;
	e->size = (uint32_t)sz;
	if (q + 2 >= nl || (q[0] != 'r' && q[0] != 'k') || q[1] != ' ') return false;
	e->policy = q[0];
	q += 2;
	if (fmt >= 4) {
		char *s0 = q;
		while (q < nl && *q != ' ') q++;
		if (q >= nl || !set_ok(s0, (int)(q - s0), true)) return false;
		e->set = s0;
		e->set_len = (uint8_t)(q - s0);
		q++;
	}
	e->path = q;
	e->path_len = (uint16_t)(nl - q);
	return path_ok(e->path, e->path_len);
}

// The next file line of an open manifest whose header has been read
// (mcheck() said its format). 1, 0 at the end, < 0 an error.
static int mr_file(mrd_t *r) {
	for (;;) {
		int g = mr_line(r);
		if (g <= 0) return g;
		if (r->n == 1) continue;                    // the magic
		if (r->ll >= 2 && r->line[0] == 'f' && r->line[1] == ' ')
			return parse_entry(r->line, r->ll, r->format, &r->e) ? 1 : -3;
	}
}

// The next folder (d line); 0 once the files start.
static int mr_dir(mrd_t *r, const char **d, int *dl) {
	for (;;) {
		int g = mr_line(r);
		if (g <= 0) return g;
		if (r->n == 1) continue;
		if (r->ll >= 2 && r->line[0] == 'f' && r->line[1] == ' ') return 0;
		if (r->ll >= 2 && r->line[0] == 'd' && r->line[1] == ' ') {
			*d = r->line + 2;
			*dl = r->ll - 2;
			return 1;
		}
	}
}

static bool hdr_key(const char *l, int ll, const char *k, const char **v, int *vl) {
	int kl = (int)strlen(k);
	if (ll < kl || memcmp(l, k, kl) || (ll > kl && l[kl] != ' ')) return false;
	*v = ll > kl ? l + kl + 1 : l + ll;
	*vl = ll > kl ? ll - kl - 1 : 0;
	return true;
}

static mrd_t rd_a, rd_b;        // the release's, the card's
static char prev_path[UPD_PATH_MAX];

// Reads and checks the whole manifest at `path`: its format, every line,
// the order, and that its snapshot id is the hash of its entries, so a
// damaged transfer or file is refused, not obeyed. Fills `h`. 0, -1 (err
// says why), -2 a format newer than this update's, or UPD_ENOENT.
static int mcheck(const char *path, mhead_t *h, char *err, int errlen) {
	mrd_t *r = &rd_a;
	memset(h, 0, sizeof *h);
	int o = mr_open(r, path);
	if (o == UPD_ENOENT) return UPD_ENOENT;
	if (o != UPD_OK) {
		snprintf(err, errlen, "cannot be read");
		return -1;
	}
	z_sha256_ctx c;
	z_sha256_init(&c);
	int rc = 0, prev_len = -1;
	bool in_body = false;
	for (;;) {
		int g = mr_line(r);
		if (g == 0) break;
		if (g < 0) {
			snprintf(err, errlen, g == -1 ? "cannot be read" :
				g == -2 && r->eof ? "the last line has no newline" :
				"line %d is too long", r->n + 1);
			rc = -1;
			break;
		}
		char *l = r->line;
		int ll = r->ll;
		if (r->n == 1) {
			int ml = (int)strlen(MAGIC);
			if (ll < ml + 2 || memcmp(l, MAGIC, ml) || l[ml] != ' ') {
				snprintf(err, errlen, "not a manifest");
				rc = -1;
				break;
			}
			h->format = atoi(l + ml + 1);
			if (h->format < 1) {
				snprintf(err, errlen, "not a manifest");
				rc = -1;
				break;
			}
			if (h->format > FORMAT) {
				snprintf(err, errlen, "manifest format %d is newer than "
					"this update's (%d)", h->format, FORMAT);
				rc = -2;
				break;
			}
			r->format = h->format;
			continue;
		}
		if (ll >= 2 && l[0] == 'f' && l[1] == ' ') {
			upd_entry_t e;
			in_body = true;
			if (!parse_entry(l, ll, h->format, &e)) {
				snprintf(err, errlen, "line %d is malformed", r->n);
				rc = -1;
				break;
			}
			if (prev_len >= 0 && path_cmp(prev_path, prev_len, e.path, e.path_len) >= 0) {
				snprintf(err, errlen, "line %d: files out of order", r->n);
				rc = -1;
				break;
			}
			memcpy(prev_path, e.path, e.path_len);
			prev_len = e.path_len;
			h->nfiles++;
			h->total += e.size;
			z_sha256_update(&c, l, ll);
			z_sha256_update(&c, "\n", 1);
		} else if (ll >= 2 && l[0] == 'd' && l[1] == ' ') {
			if (h->nfiles) {
				snprintf(err, errlen, "line %d: a folder after the files", r->n);
				rc = -1;
				break;
			}
			in_body = true;
			if (!path_ok(l + 2, ll - 2)) {
				snprintf(err, errlen, "line %d is malformed", r->n);
				rc = -1;
				break;
			}
			h->ndirs++;
			z_sha256_update(&c, l, ll);
			z_sha256_update(&c, "\n", 1);
		} else if (in_body) {
			snprintf(err, errlen, "line %d: not an entry", r->n);
			rc = -1;
			break;
		} else {
			// header: key value
			const char *v;
			int vl;
			if (h->head_len + ll + 1 < sizeof h->head) {
				memcpy(h->head + h->head_len, l, ll);
				h->head[h->head_len + ll] = '\n';
				h->head_len += ll + 1;
			}
			if (hdr_key(l, ll, "set", &v, &vl)) {
				// set <name> <title> | <description>
				const char *sp = memchr(v, ' ', vl);
				if (h->nsets >= UPD_SETS_MAX || !sp || !set_ok(v, (int)(sp - v), false)) {
					snprintf(err, errlen, "line %d is malformed", r->n);
					rc = -1;
					break;
				}
				mset_t *st = &h->sets[h->nsets++];
				copy_field(st->name, sizeof st->name, v, (int)(sp - v));
				const char *t = sp + 1, *e2 = v + vl, *bar = NULL;
				for (const char *x = t; x + 2 < e2; x++)
					if (x[0] == ' ' && x[1] == '|' && x[2] == ' ') { bar = x; break; }
				copy_field(st->title, sizeof st->title, t, (int)((bar ? bar : e2) - t));
				if (bar) copy_field(st->desc, sizeof st->desc, bar + 3, (int)(e2 - bar - 3));
			} else if (hdr_key(l, ll, "installed", &v, &vl)) {
				copy_field(h->installed, sizeof h->installed, v, vl);
				h->has_installed = true;
			}
#define HDR(k, f) else if (hdr_key(l, ll, k, &v, &vl)) copy_field(h->f, sizeof h->f, v, vl);
			HDR("name", name)
			HDR("version", version)
			HDR("snapshot", snapshot)
			HDR("source", source)
			HDR("made", made)
			HDR("packs", packs)
			HDR("abi", abi)
#undef HDR
		}
	}
	mr_close(r);
	if (rc) return rc;
	if (!h->format) {
		snprintf(err, errlen, "empty");
		return -1;
	}
	uint8_t d[Z_SHA256_DIGEST];
	char hex[2 * Z_SHA256_DIGEST + 1];
	z_sha256_final(&c, d);
	to_hex(d, Z_SHA256_DIGEST, hex);
	if ((int)strlen(h->snapshot) != ID_DIGITS || memcmp(h->snapshot, hex, ID_DIGITS)) {
		snprintf(err, errlen, "its snapshot id does not match its files "
			"(damaged in transfer?)");
		return -1;
	}
	return 0;
}
// -- reading and writing whole things ------------------------------------

// A card file into memory, NUL-terminated (one byte more). UPD_OK.
static int card_slurp(const char *path, char **out, uint32_t *len, uint32_t max) {
	uint32_t sz;
	int r = io->size(io->ctx, path, &sz);
	if (r != UPD_OK) return r;
	if (sz > max) return UPD_ERR;
	char *b = malloc(sz + 1);
	if (!b) return UPD_ERR;
	int h = io->open_read(io->ctx, path);
	if (h < 0) { free(b); return h; }
	uint32_t got = 0;
	while (got < sz) {
		int n = io->read(io->ctx, h, b + got, (int)(sz - got));
		if (n <= 0) break;
		got += (uint32_t)n;
	}
	io->close(io->ctx, h);
	if (got != sz) { free(b); return UPD_ERR; }
	b[sz] = 0;
	*out = b;
	*len = sz;
	return UPD_OK;
}

// What a card file is: its CRC-32 always, its SHA-256 only when asked
// (-v, or an entry with no CRC), since SHA-256 in software is what made
// the first check of a card slow. One read either way.
typedef struct {
	int rc;             // UPD_OK, UPD_ENOENT, UPD_ERR; 1: not read yet
	bool has_sha;
	uint32_t crc;
	char sha[HASH_HEX + 1];
} cardsum_t;

static int card_sum(const char *path, bool want_sha, cardsum_t *s) {
	static uint8_t buf[IO_BUF];
	z_sha256_ctx c;
	uint8_t d[Z_SHA256_DIGEST];
	uint32_t crc = 0;
	s->has_sha = false;
	int h = io->open_read(io->ctx, path);
	if (h < 0) return s->rc = h == UPD_ENOENT ? UPD_ENOENT : UPD_ERR;
	if (want_sha) z_sha256_init(&c);
	for (;;) {
		int n = io->read(io->ctx, h, buf, sizeof buf);
		if (n < 0) { io->close(io->ctx, h); return s->rc = UPD_ERR; }
		if (n == 0) break;
		crc = crc32_update(crc, buf, (uint32_t)n);
		if (want_sha) z_sha256_update(&c, buf, (uint32_t)n);
	}
	io->close(io->ctx, h);
	s->crc = crc;
	if (want_sha) {
		z_sha256_final(&c, d);
		to_hex(d, Z_SHA256_DIGEST, s->sha);
		s->has_sha = true;
	}
	return s->rc = UPD_OK;
}

// Is the summed file the one `e` describes? By CRC when the entry has
// one and -v is not asking for SHA-256; by SHA-256 otherwise.
static bool sum_is(const cardsum_t *s, const upd_entry_t *e, bool strong) {
	if (s->rc != UPD_OK) return false;
	if (e->has_crc && !strong) return s->crc == e->crc;
	return s->has_sha && hash_eq(s->sha, e->hash);
}

static char made_dir[UPD_PATH_MAX];

// Every folder above `path` ("/a/b/c" makes /a and /a/b).
static int mkdir_parents(const char *path) {
	char b[UPD_PATH_MAX];
	const char *slash = strrchr(path, '/');
	if (!slash || slash == path) return UPD_OK;
	int n = (int)(slash - path);
	if (n == (int)strlen(made_dir) && !memcmp(made_dir, path, n)) return UPD_OK;
	memcpy(b, path, n);
	b[n] = 0;
	for (int i = 1; i <= n; i++) {
		if (i < n && b[i] != '/') continue;
		char c = b[i];
		b[i] = 0;
		if (io->mkdir(io->ctx, b) != UPD_OK) {
			say("update: cannot make the folder %s\n", b);
			return UPD_ERR;
		}
		b[i] = c;
	}
	memcpy(made_dir, path, n);
	made_dir[n] = 0;
	return UPD_OK;
}

// Puts `upd` in the place of `target`: aside, in, remove the old.
// UPD_OK, UPD_EBUSY (left as it was, `upd` kept), UPD_ERR.
static int install(const char *upd, const char *target) {
	char old[UPD_PATH_MAX];
	uint32_t sz;
	int n = (int)strlen(target);
	if (n + 5 > UPD_PATH_MAX) return UPD_ERR;
	memcpy(old, target, n);
	memcpy(old + n, ".old", 5);
	bool had = io->size(io->ctx, target, &sz) == UPD_OK;
	if (had) {
		int r = io->rename(io->ctx, target, old);
		if (r == UPD_EEXIST) {
			io->unlink(io->ctx, old);
			r = io->rename(io->ctx, target, old);
		}
		if (r == UPD_EBUSY) return UPD_EBUSY;
		if (r != UPD_OK) return UPD_ERR;
	}
	if (io->rename(io->ctx, upd, target) != UPD_OK) {
		if (had) io->rename(io->ctx, old, target);
		return UPD_ERR;
	}
	// Also when there was no target: a run stopped between the two
	// renames left target missing and the .old beside it.
	io->unlink(io->ctx, old);
	return UPD_OK;
}

// A string onto the card at `path`, through path.upd.
static int write_text(const char *path, const char *text, uint32_t len) {
	char upd[UPD_PATH_MAX];
	snprintf(upd, sizeof upd, "%s.upd", path);
	if (mkdir_parents(path) != UPD_OK) return UPD_ERR;
	int h = io->open_write(io->ctx, upd);
	if (h < 0) return UPD_ERR;
	uint32_t done = 0;
	while (done < len) {
		uint32_t k = len - done > IO_BUF ? IO_BUF : len - done;
		int w = io->write(io->ctx, h, text + done, (int)k);
		if (w != (int)k) { io->close(io->ctx, h); io->unlink(io->ctx, upd); return UPD_ERR; }
		done += k;
	}
	if (io->close(io->ctx, h) != UPD_OK) { io->unlink(io->ctx, upd); return UPD_ERR; }
	return install(upd, path);
}

// -- the plan ------------------------------------------------------------

enum {
	A_SAME,         // the card already has it
	A_WRITE,        // fetch and put in place
	A_KEPT,         // k file edited by someone: left
	A_DONE,         // written
	A_BUSY,         // in use: left for the next run
	A_FAILED,       // could not: left as it was
	A_NOTREACHED,   // the run stopped first
	A_REMOVE,       // gone from the release, unchanged on the card
	A_REMOVED,
	A_CHANGED,      // gone from the release, but changed: left, unlisted
	A_GONE          // gone from the release and from the card
};

// One byte per file of the plan, in path order: the action, and two
// facts found while planning.
#define ACT(b)          ((b) & 0x0F)
#define F_EXISTS        0x10    // on the card
#define F_UPD_READY     0x20    // a complete .upd is already there
#define SET_ACT(b, a)   ((b) = (uint8_t)(((b) & 0xF0) | (a)))

// -- sets ------------------------------------------------------------------

typedef struct {
	const char *p[UPD_SETS_MAX];
	uint8_t n[UPD_SETS_MAX];
	int count;
} setlist_t;

static bool sl_has(const setlist_t *l, const char *s, int n) {
	for (int i = 0; i < l->count; i++)
		if (l->n[i] == n && !memcmp(l->p[i], s, n)) return true;
	return false;
}

static void sl_add(setlist_t *l, const char *s, int n) {
	if (n <= 0 || n > SET_NAME || l->count >= UPD_SETS_MAX || sl_has(l, s, n)) return;
	l->p[l->count] = s;
	l->n[l->count] = (uint8_t)n;
	l->count++;
}

// Names separated by spaces or commas.
static void sl_parse(setlist_t *l, const char *s, int n) {
	int i = 0;
	while (i < n) {
		while (i < n && (s[i] == ' ' || s[i] == ',')) i++;
		int b = i;
		while (i < n && s[i] != ' ' && s[i] != ',') i++;
		if (i > b) sl_add(l, s + b, i - b);
	}
}

static void sl_join(const setlist_t *l, char *out, int cap) {
	int o = 0;
	out[0] = 0;
	for (int i = 0; i < l->count && o + l->n[i] + 2 < cap; i++) {
		if (o) out[o++] = ' ';
		memcpy(out + o, l->p[i], l->n[i]);
		o += l->n[i];
		out[o] = 0;
	}
}

static bool offered(const mhead_t *R, const char *s, int n) {
	for (int j = 0; j < R->nsets; j++)
		if ((int)strlen(R->sets[j].name) == n && !memcmp(R->sets[j].name, s, n))
			return true;
	return false;
}

// -- the two manifests, side by side --
//
// The release's (SPOOL) and the card's (/sys/manifest), merged by path:
// each step is one file in either or both. A file of the release in a
// set the card does not get is not part of the run -- unless the card
// has it already: an update never takes away what the release still
// ships; sets only say what is added (docs/update.md, "Sets").

typedef struct {
	bool have_i;                // the card has a manifest
	int r_st, i_st;             // 1 an entry waiting, 0 at the end
	bool r_used, i_used;        // the last step took it
	const setlist_t *want;      // NULL: every file of the release
	int k;                      // the step's index, for act[]
} merge_t;

static int merge_start(merge_t *m, bool have_i, const setlist_t *want) {
	memset(m, 0, sizeof *m);
	m->have_i = have_i;
	m->want = want;
	m->k = -1;
	if (mr_open(&rd_a, SPOOL) != UPD_OK) return UPD_ERR;
	rd_a.format = 0;
	m->r_used = m->i_used = true;
	if (have_i && mr_open(&rd_b, SYS_MANIFEST) != UPD_OK) {
		mr_close(&rd_a);
		return UPD_ERR;
	}
	return UPD_OK;
}

static void merge_end(merge_t *m) {
	mr_close(&rd_a);
	if (m->have_i) mr_close(&rd_b);
}

static int rfmt, ifmt;          // the two files' formats (mcheck())

// The next step: *re and *ie the release's and the card's entries for
// it (either NULL), valid until the next call. 1, 0 at the end, < 0 a
// file could not be read (changed under us).
static int merge_next(merge_t *m, const upd_entry_t **re, const upd_entry_t **ie) {
	for (;;) {
		if (m->r_used) {
			rd_a.format = rfmt;
			m->r_st = mr_file(&rd_a);
			if (m->r_st < 0) return -1;
			m->r_used = false;
		}
		if (m->i_used) {
			m->i_st = 0;
			if (m->have_i) {
				rd_b.format = ifmt;
				m->i_st = mr_file(&rd_b);
				if (m->i_st < 0) return -1;
			}
			m->i_used = false;
		}
		if (!m->r_st && !m->i_st) return 0;
		int c;
		if (!m->r_st) c = 1;
		else if (!m->i_st) c = -1;
		else c = path_cmp(rd_a.e.path, rd_a.e.path_len, rd_b.e.path, rd_b.e.path_len);
		*re = c <= 0 ? &rd_a.e : NULL;
		*ie = c >= 0 ? &rd_b.e : NULL;
		m->r_used = c <= 0;
		m->i_used = c >= 0;
		if (*re && !*ie && m->want && (*re)->set &&
				!sl_has(m->want, (*re)->set, (*re)->set_len))
			continue;           // not this card's, and not on it
		m->k++;
		return 1;
	}
}

// -- the card's new manifest --
//
// The release's entry for each file now as the release has it, the old
// entry for one that is not, nothing for a file no longer shipped:
// written straight to /sys/manifest.upd, after a first pass that only
// hashes the entries for the snapshot id the header needs.

// The format the card's manifest is written in: the release's lines'
// shape, so that "is the card this release?" is one comparison of ids --
// 4 with sets, else 3.
static int card_format(int rf) {
	return rf >= 4 ? 4 : 3;
}

static int fmt_entry(char *out, int cap, const upd_entry_t *e, int fmt,
	const char *set, int set_len) {
	int n = snprintf(out, cap, "f %.64s %08lx %lu %c ", e->hash,
		(unsigned long)e->crc, (unsigned long)e->size, e->policy);
	if (fmt >= 4) {
		if (!set || set_len <= 0) { set = "-"; set_len = 1; }
		if (n + set_len + 1 < cap) {
			memcpy(out + n, set, set_len);
			n += set_len;
			out[n++] = ' ';
		}
	}
	if (n + e->path_len + 1 >= cap) return 0;
	memcpy(out + n, e->path, e->path_len);
	n += e->path_len;
	out[n++] = '\n';
	return n;
}

// Where emitted text goes: a hash, or a file (through a buffer).
typedef struct {
	z_sha256_ctx sha;
	bool hashing;
	int h;                  // -1: no file
	uint8_t *buf;
	uint32_t n;
	bool failed;
} wout_t;

static void w_put(wout_t *w, const void *p, uint32_t n) {
	if (w->hashing) z_sha256_update(&w->sha, p, n);
	if (w->h < 0 || w->failed) return;
	const uint8_t *s = p;
	while (n) {
		uint32_t room = IO_BUF - w->n, k = n < room ? n : room;
		memcpy(w->buf + w->n, s, k);
		w->n += k;
		s += k;
		n -= k;
		if (w->n == IO_BUF) {
			if (io->write(io->ctx, w->h, w->buf, (int)w->n) != (int)w->n) w->failed = true;
			w->n = 0;
		}
	}
}

static void w_flush(wout_t *w) {
	if (w->h >= 0 && w->n && !w->failed &&
			io->write(io->ctx, w->h, w->buf, (int)w->n) != (int)w->n)
		w->failed = true;
	w->n = 0;
}

// The release's folders, then every file of the plan as act[] says.
// UPD_OK, or UPD_ERR if a manifest could not be read.
static int emit_body(wout_t *w, bool have_i, const setlist_t *want,
	const uint8_t *act, int nact, int fmt) {
	char line[LINE_MAX_ + 32];
	if (mr_open(&rd_a, SPOOL) != UPD_OK) return UPD_ERR;
	const char *d;
	int dl, g;
	while ((g = mr_dir(&rd_a, &d, &dl)) > 0) {
		w_put(w, "d ", 2);
		w_put(w, d, (uint32_t)dl);
		w_put(w, "\n", 1);
	}
	mr_close(&rd_a);
	if (g < 0) return UPD_ERR;
	merge_t m;
	const upd_entry_t *re, *ie;
	if (merge_start(&m, have_i, want) != UPD_OK) return UPD_ERR;
	while ((g = merge_next(&m, &re, &ie)) > 0) {
		if (m.k >= nact) { g = -1; break; }
		int n = 0;
		switch (ACT(act[m.k])) {
		case A_SAME: case A_KEPT: case A_DONE:
			n = fmt_entry(line, sizeof line, re, fmt, re->set, re->set_len);
			break;
		case A_WRITE: case A_BUSY: case A_FAILED: case A_NOTREACHED:
		case A_REMOVE:
			// The old entry -- unless it came from a format-1
			// manifest and has no CRC: then nothing, and the next run
			// looks at the card copy itself. Its set: its own, else
			// the release's for the path, else not known.
			if (ie && ie->has_crc) {
				if (ie->set) n = fmt_entry(line, sizeof line, ie, fmt, ie->set, ie->set_len);
				else if (re) n = fmt_entry(line, sizeof line, ie, fmt, re->set, re->set_len);
				else n = fmt_entry(line, sizeof line, ie, fmt, NULL, 0);
			}
			break;
		default:
			break;
		}
		if (n) w_put(w, line, (uint32_t)n);
	}
	merge_end(&m);
	return g < 0 ? UPD_ERR : UPD_OK;
}

// The new manifest's header (snapshot id `id`) into `out`; its length.
static int emit_head(char *out, int cap, const mhead_t *R, const mhead_t *I,
	const char *installed, const char *id) {
	int n = snprintf(out, cap, "%s %d\nname %s\nversion %s\nsnapshot %s\n", MAGIC,
		card_format(R->format), R->name, R->version, id);
	// The ABI the files were built for: the release's, as its files are
	// what the card now has (docs/update.md, "ABI").
	if (R->abi[0]) n += snprintf(out + n, cap - n, "abi %s\n", R->abi);
	n += snprintf(out + n, cap - n, "source %s\nmade %s\n", R->source, R->made);
	// The packs the CARD has that are not listed: an image's (arkmed).
	const char *pk = I && I->packs[0] ? I->packs : R->packs;
	n += snprintf(out + n, cap - n, pk[0] ? "packs %s\n" : "packs\n", pk);
	// The sets the card has now (docs/update.md, "Sets").
	if (installed) n += snprintf(out + n, cap - n, "installed %s\n", installed);
	return n < cap ? n : cap - 1;
}
// -- one file ------------------------------------------------------------

typedef struct {
	int index, count;
	uint32_t bytes_total, bytes_done;
	uint32_t t0;
	uint32_t wire_done;     // bytes that came over the network (LZ4)
} progress_t;

// Fetches object `obj` into card file `upd`, checking it. UPD_OK, or
// UPD_ERR with a reason said; stopped: -10.
//
// Checked by CRC-32 and size; by SHA-256 too with -v (`strong`) or for
// an entry with no CRC. Software SHA-256 here is ~70 KB/s on a Lakritz,
// slower than TFTP delivers, so it would set the update's speed.
//
// `lz`: the object is LZ4 blocks (manifest format 3, release/lib/
// manifest.py): the file in 32 KB blocks, each after a 4-byte word --
// bit 31 set: stored as it is; clear: an LZ4 block -- whose low bits
// are the length that follows. Each block is collected, decoded into a
// 32 KB buffer, and passed on; the checks are on the decoded bytes,
// which are the file. Every length is checked against the manifest's
// size before anything is copied, so damage cannot write past a block
// or grow the file.

// What one fetch writes and checks, shared by the raw and gzip paths.
typedef struct {
	int h;                  // the .upd being written
	const upd_entry_t *e;
	uint8_t *wbuf;          // IO_BUF bytes, flushed to the card when full
	uint32_t fill, got;     // bytes in wbuf; bytes of the file so far
	uint32_t crc;
	bool use_sha;
	z_sha256_ctx sha;
	bool too_long, write_failed;
} sink_t;

static void sink_flush(sink_t *k) {
	if (!k->fill || k->write_failed) return;
	if (io->write(io->ctx, k->h, k->wbuf, (int)k->fill) != (int)k->fill)
		k->write_failed = true;
	k->fill = 0;
}

static void sink_put(sink_t *k, const uint8_t *p, uint32_t n) {
	if (k->got + n > k->e->size) { k->too_long = true; return; }
	k->crc = crc32_update(k->crc, p, n);
	if (k->use_sha) z_sha256_update(&k->sha, p, n);
	k->got += n;
	while (n) {
		uint32_t room = IO_BUF - k->fill, m = n < room ? n : room;
		memcpy(k->wbuf + k->fill, p, m);
		k->fill += m;
		p += m;
		n -= m;
		if (k->fill == IO_BUF) sink_flush(k);
	}
}

// -- LZ4 blocks ------------------------------------------------------------

#define LZ_BLOCK   32768
#define LZ_STORED  0x80000000u

typedef struct {
	uint32_t size;          // the file's size (the manifest's)
	uint32_t done;          // bytes of the file decoded so far
	uint8_t hdr[4];
	int nhdr;               // header bytes collected
	uint32_t need, have;    // this block's data: its length, collected
	uint32_t want;          // what it decodes to
	bool stored;
	const char *bad;
} lzdec_t;

static uint8_t lz_in[LZ_BLOCK], lz_out[LZ_BLOCK];

typedef struct { const uint8_t *p; uint32_t n; } lzsrc_t;

static uint32_t lz_fill(void *ctx, uint8_t *buf, uint32_t cap) {
	lzsrc_t *s = ctx;
	uint32_t n = s->n < cap ? s->n : cap;
	memcpy(buf, s->p, n);
	s->p += n;
	s->n -= n;
	return n;
}

// Feeds `n` bytes of the object; decoded file bytes go to `k`.
static void lz_feed(lzdec_t *z, sink_t *k, const uint8_t *p, uint32_t n) {
	static z_exec_lz4_t st;
	while (n && !z->bad && !k->too_long && !k->write_failed) {
		if (z->nhdr < 4) {
			if (z->done == z->size) { z->bad = "data after the end"; return; }
			z->hdr[z->nhdr++] = *p++;
			n--;
			if (z->nhdr < 4) continue;
			uint32_t w = (uint32_t)z->hdr[0] | (uint32_t)z->hdr[1] << 8 |
				(uint32_t)z->hdr[2] << 16 | (uint32_t)z->hdr[3] << 24;
			z->stored = (w & LZ_STORED) != 0;
			z->need = w & ~LZ_STORED;
			z->want = z->size - z->done < LZ_BLOCK ? z->size - z->done : LZ_BLOCK;
			z->have = 0;
			if (z->stored ? z->need != z->want : !(z->need && z->need < z->want)) {
				z->bad = "a block of the wrong length";
				return;
			}
			continue;
		}
		uint32_t m = z->need - z->have < n ? z->need - z->have : n;
		if (z->stored) {
			sink_put(k, p, m);                  // straight through
		} else {
			memcpy(lz_in + z->have, p, m);
		}
		z->have += m;
		p += m;
		n -= m;
		if (z->have < z->need) continue;
		if (!z->stored) {
			lzsrc_t src = { lz_in, z->need };
			if (z_exec_lz4(&st, lz_out, z->want, lz_fill, &src)) {
				z->bad = "an LZ4 block that does not decode";
				return;
			}
			sink_put(k, lz_out, z->want);
		}
		z->done += z->want;
		z->nhdr = 0;
	}
}

static int fetch_one(const char *obj, const char *upd, const upd_entry_t *e,
	progress_t *pg, const char *label, bool strong, bool lz) {

	static uint8_t in[UPD_CHUNK_MAX], wbuf[IO_BUF];
	char err[96];
	uint8_t d[Z_SHA256_DIGEST];
	char hex[HASH_HEX + 1];
	uint32_t wire = 0, last = io->ms(io->ctx), t_file = last;
	char sz[16];
	lzdec_t zd;
	memset(&zd, 0, sizeof zd);
	zd.size = e->size;

	err[0] = 0;
	if (io->fetch_open(io->ctx, obj, err, sizeof err) != UPD_OK) {
		say("\n  %s: %s\n", obj, err[0] ? err : "cannot fetch");
		return UPD_ERR;
	}
	sink_t k;
	memset(&k, 0, sizeof k);
	k.h = io->open_write(io->ctx, upd);
	if (k.h < 0) {
		io->fetch_close(io->ctx);
		say("\n  cannot write %s\n", upd);
		return UPD_ERR;
	}
	k.e = e;
	k.wbuf = wbuf;
	k.use_sha = strong || !e->has_crc;
	if (k.use_sha) z_sha256_init(&k.sha);

	int rc = UPD_OK;
	for (;;) {
		if (io->stop && io->stop(io->ctx)) { rc = -10; break; }
		int n = io->fetch_read(io->ctx, in, sizeof in, err, sizeof err);
		if (n < 0) {
			say("\n  %s: %s\n", obj, err[0] ? err : "transfer failed");
			rc = UPD_ERR;
			break;
		}
		if (n == 0) break;
		wire += (uint32_t)n;
		pg->wire_done += (uint32_t)n;
		if (!lz)
			sink_put(&k, in, (uint32_t)n);
		else
			lz_feed(&zd, &k, in, (uint32_t)n);
		if (k.too_long) {
			say("\n  %s: longer than the manifest says\n", obj);
			rc = UPD_ERR;
			break;
		}
		if (zd.bad) {
			say("\n  %s: damaged (%s)\n", obj, zd.bad);
			rc = UPD_ERR;
			break;
		}
		if (k.write_failed) break;
		uint32_t now = io->ms(io->ctx);
		if (e->size >= PROGRESS_MIN && now - last >= PROGRESS_MS) {
			last = now;
			uint32_t done_all = pg->bytes_done + k.got;
			uint32_t el = now - pg->t0;
			uint32_t rate = kb_per_s(done_all, el);
			uint32_t left = rate ? (pg->bytes_total - done_all) / 1024 / rate : 0;
			say("\r  [%d/%d] %s  %s  %lu%%  %lu KB/s  ~%lu s left   ",
				pg->index, pg->count, label, size_str(e->size, sz, sizeof sz),
				(unsigned long)percent(k.got, e->size),
				(unsigned long)rate, (unsigned long)left);
		}
	}
	io->fetch_close(io->ctx);
	sink_flush(&k);
	if (k.write_failed && rc == UPD_OK) {
		say("\n  writing %s failed (card full?)\n", upd);
		rc = UPD_ERR;
	}
	if (io->close(io->ctx, k.h) != UPD_OK && rc == UPD_OK) {
		say("\n  closing %s failed\n", upd);
		rc = UPD_ERR;
	}
	if (rc == UPD_OK && lz && (zd.done != zd.size || zd.nhdr)) {
		say("\n  %s: damaged (it ends in the middle of a block)\n", obj);
		rc = UPD_ERR;
	}
	if (rc == UPD_OK) {
		bool ok = k.got == e->size;
		if (ok && e->has_crc && k.crc != e->crc) ok = false;
		if (ok && k.use_sha) {
			z_sha256_final(&k.sha, d);
			to_hex(d, Z_SHA256_DIGEST, hex);
			ok = hash_eq(hex, e->hash);
		}
		if (!ok) {
			say("\n  %s: %s does not match the manifest\n", obj,
				k.got != e->size ? "its size" : "its hash");
			rc = UPD_ERR;
		}
	}
	if (rc != UPD_OK) {
		io->unlink(io->ctx, upd);
		return rc;
	}
	uint32_t el = io->ms(io->ctx) - t_file;
	(void)wire;
	if (e->size >= PROGRESS_MIN)
		say("\r  [%d/%d] %s  %s  ok  %lu KB/s                \n", pg->index,
			pg->count, label, size_str(e->size, sz, sizeof sz),
			(unsigned long)kb_per_s(e->size, el));
	else
		say("  [%d/%d] %s  %s  ok\n", pg->index, pg->count, label,
			size_str(e->size, sz, sizeof sz));
	return UPD_OK;
}

// A small server file (LATEST, manifest.sig) into `buf`, NUL-terminated.
static int fetch_small(const char *name, char *buf, int cap, int *len,
	char *err, int errlen) {
	if (io->fetch_open(io->ctx, name, err, errlen) != UPD_OK) return UPD_ERR;
	static uint8_t chunk[UPD_CHUNK_MAX];
	int n = 0;
	for (;;) {
		int r = io->fetch_read(io->ctx, chunk, sizeof chunk, err, errlen);
		if (r < 0) { io->fetch_close(io->ctx); return UPD_ERR; }
		if (r == 0) break;
		if (n + r > cap - 1) {
			io->fetch_close(io->ctx);
			snprintf(err, errlen, "larger than %d bytes", cap - 1);
			return UPD_ERR;
		}
		memcpy(buf + n, chunk, r);
		n += r;
	}
	io->fetch_close(io->ctx);
	buf[n] = 0;
	*len = n;
	return UPD_OK;
}

// -- signatures (docs/distribution.md) -------------------------------------
//
// <name>/manifest.sig: lines of "ed25519 <public key, 64 hex> <signature,
// 128 hex>", each an Ed25519 signature (RFC 8032) of the manifest's
// bytes. One by a key this machine trusts, that checks, is enough:
// several lines are how a distribution changes keys.
//
// The manifest is not held in memory, so the check is Monocypher's
// crypto_ed25519_check() in pieces: SHA-512 over R || A || the manifest
// as it arrives, reduced, then crypto_eddsa_check_equation().

#define SIG_MAX 8

typedef struct {
	uint8_t pub[32], sig[64];
	crypto_sha512_ctx c;
} cand_t;

static cand_t cand[SIG_MAX];
static int ncand;

static bool unhex(const char *s, int n, uint8_t *out) {
	for (int i = 0; i < n; i++) {
		int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
		if (a < 0 || b < 0) return false;
		out[i] = (uint8_t)(a << 4 | b);
	}
	return true;
}

int upd_keys_parse(const char *s, uint8_t *keys, int max) {
	int n = 0, i = 0, len = (int)strlen(s);
	while (i < len) {
		while (i < len && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) i++;
		int b = i;
		while (i < len && s[i] != ' ' && s[i] != ',' && s[i] != '\t') i++;
		if (i == b) continue;
		if (i - b != 64 || n >= max || !unhex(s + b, 32, keys + 32 * n)) return -1;
		n++;
	}
	return n;
}

// Reads manifest.sig and keeps the lines by keys this machine trusts.
// UPD_OK when there is at least one; said and UPD_ERR otherwise.
static int sig_start(const upd_opts_t *o, const char *dir, const char *name) {
	static char t[2048];
	char obj[96], err[96], fp[17];
	int tl;
	ncand = 0;
	if (o->nkeys <= 0) {
		say("update: no key to check %s's signature with: this update was "
			"built without one,\n"
			"        and /sys/zeitlos.cfg sets no system.update.key "
			"(docs/distribution.md)\n", name);
		return UPD_ERR;
	}
	snprintf(obj, sizeof obj, "%s/%s/manifest.sig", dir, name);
	err[0] = 0;
	if (fetch_small(obj, t, sizeof t, &tl, err, sizeof err) != UPD_OK) {
		say("update: %s is not signed (%s: %s), and releases from %s must be: "
			"refused\n", name, obj, err, o->source);
		return UPD_ERR;
	}
	fp[0] = 0;
	char *p = t, *end = t + tl;
	while (p < end) {
		char *nl = memchr(p, '\n', end - p);
		if (!nl) nl = end;
		cand_t *c = &cand[ncand];
		if (ncand < SIG_MAX && nl - p == 8 + 64 + 1 + 128 && !memcmp(p, "ed25519 ", 8) &&
				p[8 + 64] == ' ' && unhex(p + 8, 32, c->pub) &&
				unhex(p + 8 + 65, 64, c->sig)) {
			bool ours = false;
			for (int k = 0; k < o->nkeys; k++)
				if (!memcmp(o->keys + 32 * k, c->pub, 32)) ours = true;
			if (ours) {
				crypto_sha512_init(&c->c);
				crypto_sha512_update(&c->c, c->sig, 32);
				crypto_sha512_update(&c->c, c->pub, 32);
				ncand++;
			} else if (!fp[0]) {
				memcpy(fp, p + 8, 16);
				fp[16] = 0;
			}
		}
		p = nl + 1;
	}
	if (ncand) return UPD_OK;
	say("update: %s is signed%s%s, but not by a key this machine trusts.\n"
		"        If the distribution changed its key, set system.update.key "
		"in\n"
		"        /sys/zeitlos.cfg or flash a newer kernel and core apps "
		"(docs/distribution.md).\n",
		name, fp[0] ? " by key " : "", fp);
	return UPD_ERR;
}

static void sig_feed(const uint8_t *p, uint32_t n) {
	for (int i = 0; i < ncand; i++) crypto_sha512_update(&cand[i].c, p, n);
}

// UPD_OK if one of the trusted lines checks over what was fed.
static int sig_finish(const char *name) {
	char fp[17];
	for (int i = 0; i < ncand; i++) {
		uint8_t h[64], hr[32];
		crypto_sha512_final(&cand[i].c, h);
		crypto_eddsa_reduce(hr, h);
		if (crypto_eddsa_check_equation(cand[i].sig, cand[i].pub, hr) == 0) {
			to_hex(cand[i].pub, 8, fp);
			say("update: signed by key %s, which this machine trusts\n", fp);
			return UPD_OK;
		}
	}
	say("update: %s's signature does not check: the manifest was changed "
		"after it was\n"
		"        signed (in transit?). Refused; nothing was changed.\n", name);
	return UPD_ERR;
}

// The release's manifest from the server to SPOOL on the card, through
// the signature check when there is one. UPD_OK, or said and UPD_ERR.
static int fetch_manifest(const char *obj, const char *source, bool sig) {
	static uint8_t chunk[UPD_CHUNK_MAX], wbuf[IO_BUF];
	char err[96];
	err[0] = 0;
	if (io->fetch_open(io->ctx, obj, err, sizeof err) != UPD_OK) {
		say("update: %s from %s: %s\n", obj, source, err);
		return UPD_ERR;
	}
	int h = mkdir_parents(SPOOL) == UPD_OK ? io->open_write(io->ctx, SPOOL) : -1;
	if (h < 0) {
		io->fetch_close(io->ctx);
		say("update: cannot write %s\n", SPOOL);
		return UPD_ERR;
	}
	uint32_t total = 0, fill = 0;
	int rc = UPD_OK;
	for (;;) {
		int n = io->fetch_read(io->ctx, chunk, sizeof chunk, err, sizeof err);
		if (n < 0) {
			say("update: %s from %s: %s\n", obj, source, err);
			rc = UPD_ERR;
			break;
		}
		if (n == 0) break;
		total += (uint32_t)n;
		if (total > UPD_MANIFEST_MAX) {
			say("update: %s: larger than %u KB\n", obj,
				(unsigned)(UPD_MANIFEST_MAX / 1024));
			rc = UPD_ERR;
			break;
		}
		if (sig) sig_feed(chunk, (uint32_t)n);
		for (int i = 0; i < n; ) {
			int k = n - i < (int)(IO_BUF - fill) ? n - i : (int)(IO_BUF - fill);
			memcpy(wbuf + fill, chunk + i, k);
			fill += k;
			i += k;
			if (fill == IO_BUF) {
				if (io->write(io->ctx, h, wbuf, (int)fill) != (int)fill) rc = UPD_ERR;
				fill = 0;
			}
		}
		if (rc != UPD_OK) {
			say("update: writing %s failed (card full?)\n", SPOOL);
			break;
		}
	}
	io->fetch_close(io->ctx);
	if (rc == UPD_OK && fill && io->write(io->ctx, h, wbuf, (int)fill) != (int)fill) {
		say("update: writing %s failed (card full?)\n", SPOOL);
		rc = UPD_ERR;
	}
	if (io->close(io->ctx, h) != UPD_OK && rc == UPD_OK) {
		say("update: writing %s failed\n", SPOOL);
		rc = UPD_ERR;
	}
	return rc;
}
// -- the run -------------------------------------------------------------

static int cmp_text(const char *a, int an, const char *b, int bn) {
	int n = an < bn ? an : bn;
	int c = memcmp(a, b, n);
	return c ? c : an - bn;
}

static void say_manifest(const char *what, const mhead_t *m) {
	say("update: %s %s snapshot %s (%s, %s)\n", what,
		m->name[0] ? m->name : "?", m->snapshot[0] ? m->snapshot : "?",
		m->source[0] ? m->source : "?", m->made[0] ? m->made : "?");
}

// /sys/version for a manifest (release/lib/manifest.py, version_text()).
static int version_text(const mhead_t *m, const char *id, char *vt, int cap) {
	return snprintf(vt, cap, "zeitlos %s\nsnapshot %s %s %s %s\n",
		m->version[0] ? m->version : "?", m->name[0] ? m->name : "?", id,
		m->source[0] ? m->source : "?", m->made[0] ? m->made : "?");
}

// The card is up to date: finish what a run that stopped while writing
// /sys/manifest or /sys/version left -- /sys/version if it does not
// match, and the side files.
static void sys_files(const mhead_t *m) {
	char vt[256], *have;
	uint32_t hl;
	io->unlink(io->ctx, SYS_MANIFEST ".upd");
	io->unlink(io->ctx, SYS_MANIFEST ".old");
	int vl = version_text(m, m->snapshot, vt, sizeof vt);
	if (card_slurp(SYS_VERSION, &have, &hl, 4096) == UPD_OK) {
		bool same = hl == (uint32_t)vl && !memcmp(have, vt, vl);
		free(have);
		if (same) {
			io->unlink(io->ctx, SYS_VERSION ".upd");
			io->unlink(io->ctx, SYS_VERSION ".old");
			return;
		}
	}
	write_text(SYS_VERSION, vt, (uint32_t)vl);
}

static mhead_t RH, IH;          // the release's manifest, and the card's

int upd_run(const upd_io_t *io_, const upd_opts_t *o) {
	io = io_;
	made_dir[0] = 0;

	char name[NAME_MAX_LEN + 1], obj[96], err[96], sz[16];
	char p_card[UPD_PATH_MAX], p_upd[UPD_PATH_MAX];
	bool have_i = false;
	uint8_t *act = NULL;
	int status = UPD_FAILED;
	merge_t m;
	const upd_entry_t *re, *ie;
	int g;

	const char *dir = o->dir && o->dir[0] ? o->dir : "release";
	io->unlink(io->ctx, SPOOL);     // a run that stopped left it

	// -- 1. the release --
	bool following_latest = !strcmp(o->release, "latest");
	if (following_latest) {
		char t[64];
		int tl;
		snprintf(obj, sizeof obj, "%s/LATEST", dir);
		err[0] = 0;
		if (fetch_small(obj, t, sizeof t, &tl, err, sizeof err) != UPD_OK) {
			say("update: %s from %s: %s\n", obj, o->source, err);
			return UPD_FAILED;
		}
		int n = 0;
		while (n < tl && n < NAME_MAX_LEN && t[n] > ' ') {
			name[n] = t[n];
			n++;
		}
		name[n] = 0;
		if (!name_ok(name)) {
			say("update: %s does not name a release\n", obj);
			return UPD_FAILED;
		}
	} else {
		if (!name_ok(o->release)) {
			say("update: %s is not a release name\n", o->release);
			return UPD_USAGE;
		}
		snprintf(name, sizeof name, "%s", o->release);
	}
	// Signed, before anything in it is believed (docs/distribution.md):
	// the signature first, then the manifest through its check.
	if (o->need_sig && sig_start(o, dir, name) != UPD_OK) return UPD_FAILED;
	if (o->need_sig) say("update: checking the signature ...\n");
	snprintf(obj, sizeof obj, "%s/%s/manifest", dir, name);
	if (fetch_manifest(obj, o->source, o->need_sig) != UPD_OK) goto out;
	if (o->need_sig && sig_finish(name) != UPD_OK) goto out;
	int pr = mcheck(SPOOL, &RH, err, sizeof err);
	if (pr) {
		say("update: %s: %s\n", obj, err);
		if (pr == -2) {
			if (o->need_sig)
				say("        Flash the release's kernel and core apps, whose "
					"update reads it.\n");
			else
				say("        This release's own update can: fetch it with\n"
					"        tget SERVER %s/%s/update %s\n"
					"        and run update again.\n", dir, name, Z_DIR_APPS "/update");
		}
		goto out;
	}
	rfmt = RH.format;
	// The manifest names the release it is: one signed for another name
	// is not this one, whatever the server says (docs/distribution.md).
	if (strcmp(RH.name, name)) {
		say("update: %s is the manifest of %s, not of %s: refused\n", obj,
			RH.name[0] ? RH.name : "nothing", name);
		goto out;
	}
	say("update: %s from %s\n", name, o->source);
	say_manifest("release is", &RH);

	// -- 2. the card --
	//
	// /sys/manifest itself is replaced like any file, so a run stopped
	// between its two renames leaves it missing, with the new one as
	// .upd (complete: renamed only after it was closed) or the old one
	// as .old. Either says more than nothing, the new one most: it is put
	// back in place first.
	int ci = mcheck(SYS_MANIFEST, &IH, err, sizeof err);
	if (ci == UPD_ENOENT) {
		static const char *side[2] = { SYS_MANIFEST ".upd", SYS_MANIFEST ".old" };
		char e2[96];
		for (int s2 = 0; s2 < 2 && ci == UPD_ENOENT; s2++) {
			if (mcheck(side[s2], &IH, e2, sizeof e2) != 0) continue;
			if (io->rename(io->ctx, side[s2], SYS_MANIFEST) != UPD_OK) continue;
			say("update: %s was missing: using %s, from a run that stopped\n",
				SYS_MANIFEST, side[s2]);
			ci = mcheck(SYS_MANIFEST, &IH, err, sizeof err);
		}
	}
	if (ci == 0) {
		have_i = true;
		ifmt = IH.format;
		say_manifest("card has  ", &IH);
	} else if (ci == UPD_ENOENT) {
		say("update: the card has no %s (written before updates): checking "
			"every file\n", SYS_MANIFEST);
	} else {
		say("update: the card's %s is unreadable (%s): checking every "
			"file instead\n", SYS_MANIFEST, err);
	}

	// -- the sets (docs/update.md, "Sets") --
	//
	// The card's, the ones asked for, and base. A card that does not say
	// has them all: it was written before sets, when every card had
	// everything. So does one with no manifest at all, unless -s says
	// which (a blank card).
	setlist_t want, card, req, adding;
	memset(&want, 0, sizeof want);
	memset(&card, 0, sizeof card);
	memset(&req, 0, sizeof req);
	memset(&adding, 0, sizeof adding);
	bool R_sets = RH.format >= 4 && RH.nsets > 0;
	char installed[UPD_SETS_MAX * (SET_NAME + 1) + 1];
	installed[0] = 0;
	if (o->sets && o->sets[0]) sl_parse(&req, o->sets, (int)strlen(o->sets));
	if (req.count && !R_sets) {
		say("update: %s has no sets (an older release): -s cannot choose\n", name);
		status = UPD_USAGE;
		goto out;
	}
	if (R_sets) {
		bool all = sl_has(&req, "all", 3);
		for (int k = 0; k < req.count; k++) {
			if ((req.n[k] == 3 && !memcmp(req.p[k], "all", 3)) ||
					offered(&RH, req.p[k], req.n[k]))
				continue;
			say("update: %s has no set %.*s. It has:", name, req.n[k], req.p[k]);
			for (int j = 0; j < RH.nsets; j++) say(" %s", RH.sets[j].name);
			say("\n");
			status = UPD_USAGE;
			goto out;
		}
		bool card_says = have_i && IH.has_installed;
		bool card_all = have_i ? !IH.has_installed : !req.count;
		if (card_says) sl_parse(&card, IH.installed, (int)strlen(IH.installed));
		for (int j = 0; j < RH.nsets; j++) {
			const char *sn = RH.sets[j].name;
			int sl = (int)strlen(sn);
			bool base = sl == 4 && !memcmp(sn, "base", 4);
			bool on_card = card_all || sl_has(&card, sn, sl);
			if (all || on_card || base || sl_has(&req, sn, sl)) {
				sl_add(&want, sn, sl);
				if (!on_card && (have_i || !base)) sl_add(&adding, sn, sl);
			}
		}
		sl_join(&want, installed, sizeof installed);
		say("update: sets %s", installed);
		if (adding.count) {
			char a[UPD_SETS_MAX * (SET_NAME + 1) + 1];
			sl_join(&adding, a, sizeof a);
			say(" (adding %s)", a);
		}
		say("\n");
	}
	const setlist_t *sel = R_sets ? &want : NULL;

	// -- 3. the plan: how many steps, and is there anything to do --
	//
	// The snapshot id of what the card should hold -- the release's
	// folders and the files this run includes, as the card's manifest
	// will list them when everything is done -- against the card's.
	int nact = 0;
	{
		z_sha256_ctx c;
		uint8_t d[Z_SHA256_DIGEST];
		char hex[2 * Z_SHA256_DIGEST + 1], line[LINE_MAX_ + 32];
		int fmt = card_format(RH.format);
		z_sha256_init(&c);
		const char *dd;
		int dl;
		if (mr_open(&rd_a, SPOOL) != UPD_OK) goto reading;
		while ((g = mr_dir(&rd_a, &dd, &dl)) > 0) {
			z_sha256_update(&c, "d ", 2);
			z_sha256_update(&c, dd, dl);
			z_sha256_update(&c, "\n", 1);
		}
		mr_close(&rd_a);
		if (g < 0) goto reading;
		if (merge_start(&m, have_i, sel) != UPD_OK) goto reading;
		while ((g = merge_next(&m, &re, &ie)) > 0) {
			if (!re) continue;
			int n = fmt_entry(line, sizeof line, re, fmt, re->set, re->set_len);
			z_sha256_update(&c, line, n);
		}
		nact = m.k + 1;
		merge_end(&m);
		if (g < 0) goto reading;
		z_sha256_final(&c, d);
		to_hex(d, Z_SHA256_DIGEST, hex);
		bool same_sets = !R_sets || (IH.has_installed && !strcmp(IH.installed, installed));
		if (have_i && !o->verify && same_sets && !memcmp(IH.snapshot, hex, ID_DIGITS)) {
			say("update: up to date\n");
			sys_files(&IH);
			status = UPD_DONE;
			goto out;
		}
	}

	// A server's LATEST that went backwards is refused: it could be an
	// old release served on purpose, signed as it is. Naming the older
	// release goes back to it (docs/distribution.md, "Going back").
	if (o->need_sig && following_latest && have_i && IH.made[0] && RH.made[0] &&
			!o->force && cmp_text(IH.made, (int)strlen(IH.made),
				RH.made, (int)strlen(RH.made)) > 0) {
		say("update: the server's latest, %s, was made %s -- before what the "
			"card has (%s).\n", name, RH.made, IH.made);
		say("        Refused: a server going backwards may be serving an old "
			"release on purpose.\n"
			"        To go back to it, name it: update %s (or -f).\n", name);
		status = UPD_FAILED;
		goto out;
	}

	// -- the ABI: can the flash run these files? (docs/update.md, "ABI")
	//
	// The kernel and core apps in flash are not updated; the card's
	// programs talk to them. A release built against other interfaces
	// is refused before anything is written, unless -f.
	switch (upd_abi_check(RH.abi, (int)strlen(RH.abi), o->kernel_abi)) {
	case UPD_ABI_SAME:
		say("update: abi %s, as the flash has\n", RH.abi);
		break;
	case UPD_ABI_NO_RELEASE:
		say("update: the release does not say which ABI it was built for: "
			"not checked\n");
		break;
	case UPD_ABI_NO_KERNEL:
		say("update: warning: this kernel is older than the ABI check and "
			"cannot say\n"
			"        what it runs; updating anyway. If programs fail to "
			"start afterwards,\n"
			"        flash the release's kernel and core apps "
			"(docs/update.md, \"ABI\").\n");
		break;
	default:
		if (o->force || o->dry_run) {
			say("update: %s: the release is abi %s, the flash is abi %s:\n"
				"        its programs may not run until the flash has the "
				"release's\n"
				"        kernel and core apps (docs/update.md, \"ABI\")\n",
				o->force ? "-f" : "warning", RH.abi, o->kernel_abi);
			if (!o->force)
				say("        (without -n, this would be refused; -f to "
					"update anyway)\n");
			break;
		}
		say("update: refused: the release is built for abi %s, and the "
			"kernel and\n", RH.abi);
		say("        core apps in flash are abi %s. Its programs may not run "
			"on this flash.\n", o->kernel_abi);
		say("        Flash the release's kernel and core apps first, then run "
			"update again;\n"
			"        or update anyway with -f (docs/update.md, \"ABI\"). "
			"Nothing was changed.\n");
		status = UPD_FAILED;
		goto out;
	}

	// -- 4. the plan, file by file --
	act = calloc(nact ? nact : 1, 1);
	if (!act) {
		say("update: out of memory for the plan (%d files)\n", nact);
		goto out;
	}
	uint32_t t0 = io->ms(io->ctx), last = t0;
	int nwrite = 0, nremove = 0, nsame = 0, nkept = 0;
	uint32_t bytes = 0, bytes_freed = 0, largest = 0;
	if (merge_start(&m, have_i, sel) != UPD_OK) goto reading;
	while ((g = merge_next(&m, &re, &ie)) > 0) {
		int k = m.k;
		if (k >= nact) { g = -1; break; }
		const upd_entry_t *e = re ? re : ie;
		uint8_t *a = &act[k];
		uint32_t card_size = 0;
		// The card copy, read only when the manifests cannot answer,
		// and SHA-256 only when a CRC cannot (-v, a format-1 entry).
		cardsum_t cs = { 1, false, 0, { 0 } };
		bool want_sha = o->verify || (re && !re->has_crc) || (ie && !ie->has_crc);
		if (!card_path(p_card, e->path, e->path_len, "")) {
			say("update: path too long: %.*s\n", e->path_len, e->path);
			merge_end(&m);
			goto out;
		}
		bool exists = io->size(io->ctx, p_card, &card_size) == UPD_OK;
		if (exists) *a |= F_EXISTS;
		uint32_t now = io->ms(io->ctx);
		if (now - last >= 1000) {
			last = now;
			say("\r  checking %d of %d   ", k + 1, nact);
		}
#define SUM() (cs.rc == 1 ? card_sum(p_card, want_sha, &cs) : cs.rc)
		int action;
		if (re && re->policy == 'r') {
			if (!o->verify && ie && hash_eq(ie->hash, re->hash) &&
					exists && card_size == re->size)
				action = A_SAME;
			else if (exists && card_size == re->size &&
					SUM() == UPD_OK && sum_is(&cs, re, o->verify))
				action = A_SAME;
			else
				action = A_WRITE;
		} else if (re) {	// 'k'
			if (!exists)
				action = A_WRITE;
			else if (!o->verify && ie && hash_eq(ie->hash, re->hash))
				action = A_SAME;
			else if (SUM() != UPD_OK)
				action = A_KEPT;	// unreadable: leave it
			else if (sum_is(&cs, re, o->verify))
				action = A_SAME;
			else if (ie && sum_is(&cs, ie, o->verify))
				action = A_WRITE;	// never edited: the new one
			else
				action = A_KEPT;
		} else {	// in the card's manifest only
			if (!exists)
				action = A_GONE;
			else if (SUM() == UPD_OK && sum_is(&cs, ie, o->verify))
				action = A_REMOVE;
			else
				action = A_CHANGED;
		}
#undef SUM
		SET_ACT(*a, action);
		// The card has it but its manifest did not say so: a run that
		// stopped between putting a file in place and removing the
		// old one. Tidy up after it.
		if (action == A_SAME && (!ie || !hash_eq(ie->hash, re->hash))) {
			char p_old[UPD_PATH_MAX];
			if (card_path(p_old, e->path, e->path_len, ".old"))
				io->unlink(io->ctx, p_old);
		}
		switch (action) {
		case A_SAME: nsame++; break;
		case A_KEPT: nkept++; break;
		case A_REMOVE:
			nremove++;
			bytes_freed += card_size;
			break;
		case A_WRITE:
			nwrite++;
			bytes += re->size;
			if (exists) bytes_freed += card_size;
			if (re->size > largest) largest = re->size;
			// A complete .upd from a run that stopped: use it.
			if (card_path(p_upd, e->path, e->path_len, ".upd")) {
				uint32_t us;
				if (io->size(io->ctx, p_upd, &us) == UPD_OK) {
					cardsum_t ucs;
					if (us == re->size &&
							card_sum(p_upd, o->verify || !re->has_crc, &ucs) == UPD_OK &&
							sum_is(&ucs, re, o->verify))
						*a |= F_UPD_READY;
					else
						io->unlink(io->ctx, p_upd);
				}
			}
			break;
		}
	}
	merge_end(&m);
	if (g < 0) goto reading;
	if (nact && io->ms(io->ctx) - t0 >= 1000) say("\r                            \r");

	say("update: %d to write (%s), %d to remove, %d unchanged", nwrite,
		size_str(bytes, sz, sizeof sz), nremove, nsame);
	if (nkept) say(", %d kept as edited", nkept);
	say("\n");

	if (o->dry_run) {
		if (merge_start(&m, have_i, sel) != UPD_OK) goto reading;
		while ((g = merge_next(&m, &re, &ie)) > 0 && m.k < nact) {
			const upd_entry_t *e = re ? re : ie;
			const char *what = NULL;
			switch (ACT(act[m.k])) {
			case A_WRITE: what = act[m.k] & F_EXISTS ? "replace" : "add"; break;
			case A_REMOVE: what = "remove"; break;
			case A_KEPT: what = "keep (edited)"; break;
			case A_CHANGED: what = "leave (edited, no longer shipped)"; break;
			}
			if (what)
				say("  %-8s %.*s\n", what, e->path_len, e->path);
		}
		merge_end(&m);
		say("update: -n: nothing changed\n");
		status = UPD_DONE;
		goto out;
	}

	// -- 5. room, then the work --
	uint32_t free_kb = 0;
	if (nwrite && io->free_kb(io->ctx, &free_kb) == UPD_OK) {
		uint64_t need = (uint64_t)bytes + largest;
		uint64_t have = (uint64_t)free_kb * 1024 + bytes_freed;
		if (need > have) {
			// What it needs beyond what the old files give back.
			uint64_t more = need - bytes_freed;
			char a[16], b[16];
			say("update: not enough room on the card: about %s needed, %s free\n",
				size_str(more > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)more, a, sizeof a),
				size_str(free_kb > 4194303 ? 0xFFFFFFFFu : free_kb * 1024, b, sizeof b));
			goto out;
		}
	}

	{
		const char *dd;
		int dl;
		if (mr_open(&rd_a, SPOOL) != UPD_OK) goto reading;
		while ((g = mr_dir(&rd_a, &dd, &dl)) > 0) {
			char p_dir[UPD_PATH_MAX];
			if (card_path(p_dir, dd, dl, "")) {
				char probe[UPD_PATH_MAX + 4];
				snprintf(probe, sizeof probe, "%s/x", p_dir);
				mkdir_parents(probe);
			}
		}
		mr_close(&rd_a);
		if (g < 0) goto reading;
	}

	progress_t pg = { 0, nwrite, bytes, 0, io->ms(io->ctx), 0 };
	bool lz = RH.format >= 3;
	bool stopped = false, broken = false;
	int nbusy = 0, nfailed = 0, ndone = 0, nremoved = 0;
	if (merge_start(&m, have_i, sel) != UPD_OK) goto reading;
	while ((g = merge_next(&m, &re, &ie)) > 0) {
		if (m.k >= nact) { g = -1; break; }
		uint8_t *a = &act[m.k];
		if (ACT(*a) != A_WRITE) continue;
		pg.index++;
		if (stopped || broken) { SET_ACT(*a, A_NOTREACHED); continue; }
		if (io->stop && io->stop(io->ctx)) {
			stopped = true;
			SET_ACT(*a, A_NOTREACHED);
			continue;
		}
		card_path(p_card, re->path, re->path_len, "");
		card_path(p_upd, re->path, re->path_len, ".upd");
		char label[UPD_PATH_MAX];
		snprintf(label, sizeof label, "%.*s", re->path_len, re->path);
		if (mkdir_parents(p_card) != UPD_OK) {
			SET_ACT(*a, A_FAILED);
			nfailed++;
			broken = true;
			continue;
		}
		if (*a & F_UPD_READY) {
			say("  [%d/%d] %s  %s  already fetched\n", pg.index, pg.count,
				label, size_str(re->size, sz, sizeof sz));
		} else {
			snprintf(obj, sizeof obj, "%s/o/%.2s/%.*s%s", dir, re->hash,
				OBJECT_DIGITS, re->hash, lz ? ".lz" : "");
			// Signed: every file by SHA-256 against the signed manifest,
			// since a CRC keeps out accidents, not people.
			bool strong = o->verify || o->need_sig;
			int fr = fetch_one(obj, p_upd, re, &pg, label, strong, lz);
			if (fr == UPD_ERR) {
				say("  retrying %s\n", label);
				fr = fetch_one(obj, p_upd, re, &pg, label, strong, lz);
			}
			if (fr == -10) {
				stopped = true;
				SET_ACT(*a, A_NOTREACHED);
				continue;
			}
			if (fr != UPD_OK) {
				SET_ACT(*a, A_FAILED);
				nfailed++;
				broken = true;
				continue;
			}
		}
		int ir = install(p_upd, p_card);
		if (ir == UPD_EBUSY) {
			say("  [%d/%d] %s  in use by a running program: left for the "
				"next run\n", pg.index, pg.count, label);
			SET_ACT(*a, A_BUSY);
			nbusy++;
		} else if (ir != UPD_OK) {
			say("  [%d/%d] %s  could not be put in place\n", pg.index,
				pg.count, label);
			SET_ACT(*a, A_FAILED);
			nfailed++;
			broken = true;
		} else {
			SET_ACT(*a, A_DONE);
			ndone++;
			pg.bytes_done += re->size;
		}
	}
	merge_end(&m);
	if (g < 0) goto reading;

	if (merge_start(&m, have_i, sel) != UPD_OK) goto reading;
	while ((g = merge_next(&m, &re, &ie)) > 0) {
		if (m.k >= nact) { g = -1; break; }
		uint8_t *a = &act[m.k];
		if (ACT(*a) == A_REMOVE && !stopped && !broken) {
			card_path(p_card, ie->path, ie->path_len, "");
			if (io->unlink(io->ctx, p_card) == UPD_OK) {
				SET_ACT(*a, A_REMOVED);
				nremoved++;
				say("  removed %.*s\n", ie->path_len, ie->path);
			}
		} else if (ACT(*a) == A_CHANGED) {
			say("  left %.*s: no longer shipped, but changed here\n",
				ie->path_len, ie->path);
		}
	}
	merge_end(&m);
	if (g < 0) goto reading;

	// -- 6. what the card now holds --
	{
		static uint8_t wbuf[IO_BUF];
		static char head[HEAD_MAX];
		int fmt = card_format(RH.format);
		const char *inst = R_sets ? installed : NULL;
		wout_t w;
		memset(&w, 0, sizeof w);
		w.h = -1;
		w.hashing = true;
		z_sha256_init(&w.sha);
		if (emit_body(&w, have_i, sel, act, nact, fmt) != UPD_OK) goto reading;
		uint8_t d[Z_SHA256_DIGEST];
		char hex[2 * Z_SHA256_DIGEST + 1], id[ID_DIGITS + 1];
		z_sha256_final(&w.sha, d);
		to_hex(d, Z_SHA256_DIGEST, hex);
		memcpy(id, hex, ID_DIGITS);
		id[ID_DIGITS] = 0;
		int hl = emit_head(head, sizeof head, &RH, have_i ? &IH : NULL, inst, id);
		// The same as the card's already (header and entries): left.
		char magic[32];
		int ml = snprintf(magic, sizeof magic, "%s %d\n", MAGIC, fmt);
		bool same = have_i && !strcmp(IH.snapshot, id) && IH.format == fmt &&
			(uint32_t)(hl - ml) == IH.head_len && !memcmp(head + ml, IH.head, IH.head_len);
		if (!same) {
			bool ok = false;
			char upd[UPD_PATH_MAX];
			snprintf(upd, sizeof upd, "%s.upd", SYS_MANIFEST);
			memset(&w, 0, sizeof w);
			w.buf = wbuf;
			w.h = io->open_write(io->ctx, upd);
			if (w.h >= 0) {
				w_put(&w, head, (uint32_t)hl);
				if (emit_body(&w, have_i, sel, act, nact, fmt) == UPD_OK) {
					w_flush(&w);
					ok = !w.failed;
				}
				if (io->close(io->ctx, w.h) != UPD_OK) ok = false;
				if (ok) ok = install(upd, SYS_MANIFEST) == UPD_OK;
				else io->unlink(io->ctx, upd);
			}
			if (!ok)
				say("update: could not write %s -- run update again\n", SYS_MANIFEST);
			char vt[256];
			int vl = version_text(&RH, id, vt, sizeof vt);
			if (write_text(SYS_VERSION, vt, (uint32_t)vl) != UPD_OK)
				say("update: could not write %s\n", SYS_VERSION);
		}

		uint32_t el = io->ms(io->ctx) - pg.t0;
		say("update: %s in %lu s: %d written, %d removed", stopped ? "stopped" :
			(nbusy || nfailed) ? "partly done" : "done",
			(unsigned long)(el / 1000), ndone, nremoved);
		if (pg.bytes_done >= PROGRESS_MIN) {
			say(" (%s, %lu KB/s", size_str(pg.bytes_done, sz, sizeof sz),
				(unsigned long)kb_per_s(pg.bytes_done, el));
			if (lz && pg.wire_done) {
				char wb[16];
				say("; %s over the network", size_str(pg.wire_done, wb, sizeof wb));
			}
			say(")");
		}
		if (nbusy) say(", %d in use", nbusy);
		if (nfailed) say(", %d failed", nfailed);
		say(".\n");
		if (stopped || nbusy || nfailed || broken) {
			say("update: run it again to finish%s.\n",
				nbusy ? " (after closing the programs using those files)" : "");
			status = UPD_PARTIAL;
		} else {
			if (ndone || nremoved)
				say("update: programs that were running still use the old "
					"files; restart them (or reboot) to use the new ones.\n");
			status = UPD_DONE;
		}
	}
	goto out;

reading:
	say("update: reading %s or %s failed part way -- run update again\n",
		SPOOL, SYS_MANIFEST);
out:
	free(act);
	io->unlink(io->ctx, SPOOL);
	return status;
}

int upd_abi_check(const char *rel, int rel_len, const char *kernel) {
	if (!rel || rel_len <= 0) return UPD_ABI_NO_RELEASE;
	if (!kernel || !kernel[0]) return UPD_ABI_NO_KERNEL;
	if ((int)strlen(kernel) == rel_len && !memcmp(kernel, rel, rel_len))
		return UPD_ABI_SAME;
	return UPD_ABI_DIFFERENT;
}

int upd_show(const upd_io_t *io_, const char *kernel_abi) {
	io = io_;
	char *text;
	uint32_t len;
	if (card_slurp(SYS_VERSION, &text, &len, 4096) == UPD_OK) {
		say("%s", text);
		if (len && text[len - 1] != '\n') say("\n");
		free(text);
	}
	char err[96];
	int r = mcheck(SYS_MANIFEST, &IH, err, sizeof err);
	if (r == UPD_ENOENT) {
		say("no %s: this card was written before updates\n", SYS_MANIFEST);
		return UPD_DONE;
	}
	if (r) {
		say("%s: %s\n", SYS_MANIFEST, err);
		return UPD_FAILED;
	}
	char sz[16];
	say("%s: %d files, %s\n", SYS_MANIFEST, IH.nfiles,
		size_str(IH.total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)IH.total, sz, sizeof sz));
	// Its sets (docs/update.md, "Sets"); a card that does not say was
	// written before sets, with all of them.
	if (IH.has_installed)
		say("sets: %s\n", IH.installed);
	else
		say("sets: all (written before sets)\n");
	// The card's files' ABI and the flash's (docs/update.md, "ABI").
	const char *ka = kernel_abi && kernel_abi[0] ? kernel_abi : "unknown (an older kernel)";
	if (IH.abi[0])
		say("abi: card %s, flash %s%s\n", IH.abi, ka,
			upd_abi_check(IH.abi, (int)strlen(IH.abi), kernel_abi) == UPD_ABI_DIFFERENT
				? "  -- DIFFERENT: flash the matching kernel and core apps" : "");
	else
		say("abi: card not recorded, flash %s\n", ka);
	return UPD_DONE;
}
