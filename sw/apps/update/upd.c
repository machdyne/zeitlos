/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * update's core. See upd.h, and docs/update.md for the rules.
 *
 * -- the order of things --
 *
 *   1. Fetch the release's manifest (LATEST first for "latest"), and
 *      check it: format, order, and that its snapshot id is the hash of
 *      its entries -- so a damaged transfer is refused, not obeyed.
 *   2. Read the card's /sys/manifest, if it has one.
 *   3. Plan, by merging the two (both are sorted by path): which files
 *      to write, which to remove, which to leave. The card is read
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

#define FORMAT          2       // 1 (no CRC) is still read
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

// -- the manifest --------------------------------------------------------

static void id_of(const upd_manifest_t *m, char out[ID_DIGITS + 1]);

int upd_manifest_parse(upd_manifest_t *m, char *text, uint32_t len,
	char *err, int errlen) {

	memset(m, 0, sizeof *m);
	m->text = text;
	m->len = len;
	text[len] = 0;

	// One entry per line is room enough; folders are few, so counted.
	int lines = 1, dlines = 1;
	for (uint32_t i = 0; i < len; i++)
		if (text[i] == '\n') {
			lines++;
			if (i + 2 < len && text[i + 1] == 'd' && text[i + 2] == ' ') dlines++;
		}
	m->files = calloc(lines, sizeof *m->files);
	m->dirs = calloc(dlines, sizeof *m->dirs);
	m->dir_len = calloc(dlines, sizeof *m->dir_len);
	if (!m->files || !m->dirs || !m->dir_len) {
		snprintf(err, errlen, "out of memory for %d lines", lines);
		return -1;
	}

	char *p = text, *end = text + len;
	int n = 0;
	while (p < end) {
		char *nl = memchr(p, '\n', end - p);
		if (!nl) {
			snprintf(err, errlen, "the last line has no newline");
			return -1;
		}
		int ll = (int)(nl - p);
		n++;
		if (n == 1) {
			int ml = (int)strlen(MAGIC);
			if (ll < ml + 2 || memcmp(p, MAGIC, ml) || p[ml] != ' ') {
				snprintf(err, errlen, "not a manifest");
				return -1;
			}
			m->format = atoi(p + ml + 1);
			if (m->format < 1) {
				snprintf(err, errlen, "not a manifest");
				return -1;
			}
			if (m->format > FORMAT) {
				snprintf(err, errlen, "manifest format %d is newer than "
					"this update's (%d)", m->format, FORMAT);
				return -2;
			}
		} else if (ll >= 2 && p[0] == 'f' && p[1] == ' ') {
			upd_entry_t *e = &m->files[m->nfiles];
			// f <64 hex> <size> <policy> <path>
			char *q = p + 2;
			if (ll < 2 + HASH_HEX + 6 + (m->format >= 2 ? 9 : 0)) goto bad;
			for (int i = 0; i < HASH_HEX; i++) {
				char c = q[i];
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) goto bad;
			}
			e->hash = q;
			q += HASH_HEX;
			if (*q++ != ' ') goto bad;
			if (m->format >= 2) {
				uint32_t c = 0;
				for (int i = 0; i < 8; i++) {
					int v = hexval(q[i]);
					if (v < 0) goto bad;
					c = c << 4 | (uint32_t)v;
				}
				if (q[8] != ' ') goto bad;
				e->crc = c;
				e->has_crc = true;
				q += 9;
			}
			uint64_t sz = 0;
			char *d0 = q;
			while (q < nl && *q >= '0' && *q <= '9') {
				sz = sz * 10 + (uint32_t)(*q++ - '0');
				if (sz > 0xFFFFFFFFu) goto bad;
			}
			if (q == d0 || *q++ != ' ') goto bad;
			e->size = (uint32_t)sz;
			if (q + 2 >= nl || (q[0] != 'r' && q[0] != 'k') || q[1] != ' ') goto bad;
			e->policy = q[0];
			q += 2;
			e->path = q;
			e->path_len = (uint16_t)(nl - q);
			if (!path_ok(e->path, e->path_len)) goto bad;
			if (m->nfiles && path_cmp(m->files[m->nfiles - 1].path,
					m->files[m->nfiles - 1].path_len, e->path, e->path_len) >= 0) {
				snprintf(err, errlen, "line %d: files out of order", n);
				return -1;
			}
			m->nfiles++;
		} else if (ll >= 2 && p[0] == 'd' && p[1] == ' ') {
			if (m->nfiles) {
				snprintf(err, errlen, "line %d: a folder after the files", n);
				return -1;
			}
			if (!path_ok(p + 2, ll - 2)) goto bad;
			m->dirs[m->ndirs] = p + 2;
			m->dir_len[m->ndirs] = (uint16_t)(ll - 2);
			m->ndirs++;
		} else if (m->nfiles || m->ndirs) {
			snprintf(err, errlen, "line %d: not an entry", n);
			return -1;
		} else {
			// header: key value
			char *sp = memchr(p, ' ', ll);
			int kl = sp ? (int)(sp - p) : ll;
			const char *v = sp ? sp + 1 : nl;
			int vl = (int)(nl - v);
#define HDR(k, f) if (kl == (int)strlen(k) && !memcmp(p, k, kl)) { m->f = v; m->f##_len = vl; }
			HDR("name", name)
			else HDR("version", version)
			else HDR("snapshot", snapshot)
			else HDR("source", source)
			else HDR("made", made)
			else HDR("packs", packs)
#undef HDR
		}
		p = nl + 1;
		continue;
bad:
		snprintf(err, errlen, "line %d is malformed", n);
		return -1;
	}
	if (!n) {
		snprintf(err, errlen, "empty");
		return -1;
	}
	char id[ID_DIGITS + 1];
	id_of(m, id);
	if (!m->snapshot || m->snapshot_len != ID_DIGITS ||
			memcmp(m->snapshot, id, ID_DIGITS)) {
		snprintf(err, errlen, "its snapshot id does not match its files "
			"(damaged in transfer?)");
		return -1;
	}
	return 0;
}

void upd_manifest_free(upd_manifest_t *m) {
	free(m->text);
	free(m->files);
	free(m->dirs);
	free(m->dir_len);
	memset(m, 0, sizeof *m);
}

// SHA-256 over the d and f lines, each with its newline: the snapshot id
// (release/lib/manifest.py, snapshot_id()).
static void id_of(const upd_manifest_t *m, char out[ID_DIGITS + 1]) {
	z_sha256_ctx c;
	uint8_t d[Z_SHA256_DIGEST];
	char hex[2 * Z_SHA256_DIGEST + 1];
	z_sha256_init(&c);
	for (int i = 0; i < m->ndirs; i++) {
		z_sha256_update(&c, "d ", 2);
		z_sha256_update(&c, m->dirs[i], m->dir_len[i]);
		z_sha256_update(&c, "\n", 1);
	}
	for (int i = 0; i < m->nfiles; i++) {
		const upd_entry_t *e = &m->files[i];
		char line[32];
		z_sha256_update(&c, "f ", 2);
		z_sha256_update(&c, e->hash, HASH_HEX);
		int n = m->format >= 2 ?
			snprintf(line, sizeof line, " %08lx %lu %c ", (unsigned long)e->crc,
				(unsigned long)e->size, e->policy) :
			snprintf(line, sizeof line, " %lu %c ",
				(unsigned long)e->size, e->policy);
		z_sha256_update(&c, line, n);
		z_sha256_update(&c, e->path, e->path_len);
		z_sha256_update(&c, "\n", 1);
	}
	z_sha256_final(&c, d);
	to_hex(d, Z_SHA256_DIGEST, hex);
	memcpy(out, hex, ID_DIGITS);
	out[ID_DIGITS] = 0;
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

// A server file into memory, NUL-terminated (one byte more).
static int fetch_slurp(const char *name, char **out, uint32_t *len,
	char *err, int errlen) {
	if (io->fetch_open(io->ctx, name, err, errlen) != UPD_OK) return UPD_ERR;
	// Room for the base card's manifest (43 KB in October 2026) at once:
	// growing by doubling holds two blocks at a time, and this one stays
	// for the whole run.
	uint32_t cap = 65536 - 1, n = 0;
	char *b = malloc(cap + 1);
	if (!b) {
		io->fetch_close(io->ctx);
		snprintf(err, errlen, "out of memory");
		return UPD_ERR;
	}
	for (;;) {
		if (cap - n < UPD_CHUNK_MAX) {
			if (cap >= UPD_MANIFEST_MAX) {
				snprintf(err, errlen, "larger than %u KB",
					(unsigned)(UPD_MANIFEST_MAX / 1024));
				goto fail;
			}
			char *nb = realloc(b, cap * 2 + 1);
			if (!nb) {
				snprintf(err, errlen, "out of memory");
				goto fail;
			}
			b = nb;
			cap *= 2;
		}
		int r = io->fetch_read(io->ctx, (uint8_t *)b + n, (int)(cap - n), err, errlen);
		if (r < 0) goto fail;
		if (r == 0) break;
		n += (uint32_t)r;
	}
	io->fetch_close(io->ctx);
	// To its size: it is kept for the whole run. newlib shrinks a block
	// where it is.
	char *fit = realloc(b, n + 1);
	if (fit) b = fit;
	b[n] = 0;
	*out = b;
	*len = n;
	return UPD_OK;
fail:
	io->fetch_close(io->ctx);
	free(b);
	return UPD_ERR;
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

typedef struct {
	int r, i;           // index in the release / installed manifest, or -1
	uint8_t action;
	bool exists;        // on the card
	bool upd_ready;     // a complete .upd is already there
	uint32_t card_size;
} item_t;

// Text written into ONE buffer of a size counted beforehand: the same
// code runs twice, first with `count` set (it only adds up lengths),
// then into exactly that much memory. Growing a buffer by doubling, as
// this did first, holds the old block and the new one at once; with
// both manifests in memory that ran a board's 256 KB heap out
// ("out of memory writing the manifest").
typedef struct { char *b; uint32_t n, cap; bool count, oom; } sbuf_t;

static void sb_add(sbuf_t *s, const char *p, uint32_t n) {
	if (s->count) { s->n += n; return; }
	if (s->oom || s->n + n + 1 > s->cap) { s->oom = true; return; }
	memcpy(s->b + s->n, p, n);
	s->n += n;
	s->b[s->n] = 0;
}

static void sb_str(sbuf_t *s, const char *p) { sb_add(s, p, (uint32_t)strlen(p)); }

// One file line, in this program's format (2). The caller leaves out an
// entry with no CRC (from a format-1 manifest).
static void sb_entry(sbuf_t *s, const upd_entry_t *e) {
	char head[96];
	int n = snprintf(head, sizeof head, "f %.64s %08lx %lu %c ", e->hash,
		(unsigned long)e->crc, (unsigned long)e->size, e->policy);
	sb_add(s, head, (uint32_t)n);
	sb_add(s, e->path, e->path_len);
	sb_add(s, "\n", 1);
}

static void sb_field(sbuf_t *s, const char *key, const char *v, int vl) {
	sb_str(s, key);
	if (v && vl > 0) {
		sb_add(s, " ", 1);
		sb_add(s, v, (uint32_t)vl);
	}
	sb_add(s, "\n", 1);
}

// The card's new manifest: the release's entry for each file now as the
// release has it, the old entry for one that is not, nothing for a file
// no longer shipped. *id_off and *body_off say where the snapshot id
// goes and where the entries start.
static void emit(sbuf_t *s, const upd_manifest_t *R, const upd_manifest_t *I,
	const item_t *items, int nitems, uint32_t *id_off, uint32_t *body_off) {

	char magic[32];
	snprintf(magic, sizeof magic, "%s %d\n", MAGIC, FORMAT);
	sb_str(s, magic);
	sb_field(s, "name", R->name, R->name_len);
	sb_field(s, "version", R->version, R->version_len);
	sb_str(s, "snapshot ");
	*id_off = s->n;
	sb_add(s, "000000000000\n", ID_DIGITS + 1);	// filled in afterwards
	sb_field(s, "source", R->source, R->source_len);
	sb_field(s, "made", R->made, R->made_len);
	// The packs the CARD has: an update release lists none, and leaves
	// them where they are (docs/update.md, "Not yet").
	if (I && I->packs) sb_field(s, "packs", I->packs, I->packs_len);
	else sb_field(s, "packs", R->packs, R->packs_len);
	*body_off = s->n;

	for (int d = 0; d < R->ndirs; d++) {
		sb_add(s, "d ", 2);
		sb_add(s, R->dirs[d], R->dir_len[d]);
		sb_add(s, "\n", 1);
	}
	for (int k = 0; k < nitems; k++) {
		const item_t *it = &items[k];
		switch (it->action) {
		case A_SAME: case A_KEPT: case A_DONE:
			sb_entry(s, &R->files[it->r]);
			break;
		case A_WRITE: case A_BUSY: case A_FAILED: case A_NOTREACHED:
		case A_REMOVE:
			// The old entry -- unless it came from a format-1
			// manifest and has no CRC: then nothing, and the next run
			// looks at the card copy itself.
			if (it->i >= 0 && I->files[it->i].has_crc)
				sb_entry(s, &I->files[it->i]);
			break;
		default:
			break;
		}
	}
}

// Returns the text (one allocation, exactly its size); *id gets its
// snapshot id: SHA-256 over the entry lines, as the release tool makes it.
static char *new_manifest(const upd_manifest_t *R, const upd_manifest_t *I,
	const item_t *items, int nitems, uint32_t *len, char id[ID_DIGITS + 1]) {

	sbuf_t s = { NULL, 0, 0, true, false };
	uint32_t id_off, body_off;
	emit(&s, R, I, items, nitems, &id_off, &body_off);
	uint32_t need = s.n;

	s.b = malloc(need + 1);
	if (!s.b) return NULL;
	s.n = 0;
	s.cap = need + 1;
	s.count = false;
	emit(&s, R, I, items, nitems, &id_off, &body_off);
	if (s.oom || s.n != need) { free(s.b); return NULL; }

	z_sha256_ctx c;
	uint8_t d[Z_SHA256_DIGEST];
	char hex[2 * Z_SHA256_DIGEST + 1];
	z_sha256_init(&c);
	z_sha256_update(&c, s.b + body_off, s.n - body_off);
	z_sha256_final(&c, d);
	to_hex(d, Z_SHA256_DIGEST, hex);
	memcpy(id, hex, ID_DIGITS);
	id[ID_DIGITS] = 0;
	memcpy(s.b + id_off, id, ID_DIGITS);
	*len = s.n;
	return s.b;
}

// -- one file ------------------------------------------------------------

typedef struct {
	int index, count;
	uint32_t bytes_total, bytes_done;
	uint32_t t0;
} progress_t;

// Fetches object `obj` into card file `upd`, checking it. UPD_OK, or
// UPD_ERR with a reason said; stopped: -10.
// Checked by CRC-32 and size; by SHA-256 too with -v (`strong`) or for
// an entry with no CRC. Software SHA-256 here is ~70 KB/s on a Lakritz,
// slower than TFTP delivers, so it would set the update's speed.
static int fetch_one(const char *obj, const char *upd, const upd_entry_t *e,
	progress_t *pg, const char *label, bool strong) {

	static uint8_t buf[IO_BUF + UPD_CHUNK_MAX];
	char err[96];
	z_sha256_ctx c;
	uint8_t d[Z_SHA256_DIGEST];
	char hex[HASH_HEX + 1];
	uint32_t got = 0, fill = 0, last = io->ms(io->ctx), t_file = last;
	char sz[16];

	err[0] = 0;
	if (io->fetch_open(io->ctx, obj, err, sizeof err) != UPD_OK) {
		say("\n  %s: %s\n", obj, err[0] ? err : "cannot fetch");
		return UPD_ERR;
	}
	int h = io->open_write(io->ctx, upd);
	if (h < 0) {
		io->fetch_close(io->ctx);
		say("\n  cannot write %s\n", upd);
		return UPD_ERR;
	}
	bool use_sha = strong || !e->has_crc;
	uint32_t crc = 0;
	if (use_sha) z_sha256_init(&c);
	int rc = UPD_OK;
	for (;;) {
		if (io->stop && io->stop(io->ctx)) { rc = -10; break; }
		int n = io->fetch_read(io->ctx, buf + fill, UPD_CHUNK_MAX, err, sizeof err);
		if (n < 0) {
			say("\n  %s: %s\n", obj, err[0] ? err : "transfer failed");
			rc = UPD_ERR;
			break;
		}
		if (n > 0) {
			if (got + (uint32_t)n > e->size) {
				say("\n  %s: longer than the manifest says\n", obj);
				rc = UPD_ERR;
				break;
			}
			crc = crc32_update(crc, buf + fill, (uint32_t)n);
			if (use_sha) z_sha256_update(&c, buf + fill, (uint32_t)n);
			got += (uint32_t)n;
			fill += (uint32_t)n;
		}
		if (fill >= IO_BUF || (n == 0 && fill)) {
			if (io->write(io->ctx, h, buf, (int)fill) != (int)fill) {
				say("\n  writing %s failed (card full?)\n", upd);
				rc = UPD_ERR;
				break;
			}
			fill = 0;
		}
		if (n == 0) break;
		uint32_t now = io->ms(io->ctx);
		if (e->size >= PROGRESS_MIN && now - last >= PROGRESS_MS) {
			last = now;
			uint32_t done_all = pg->bytes_done + got;
			uint32_t el = now - pg->t0;
			uint32_t rate = kb_per_s(done_all, el);
			uint32_t left = rate ? (pg->bytes_total - done_all) / 1024 / rate : 0;
			say("\r  [%d/%d] %s  %s  %lu%%  %lu KB/s  ~%lu s left   ",
				pg->index, pg->count, label, size_str(e->size, sz, sizeof sz),
				(unsigned long)percent(got, e->size),
				(unsigned long)rate, (unsigned long)left);
		}
	}
	io->fetch_close(io->ctx);
	if (io->close(io->ctx, h) != UPD_OK && rc == UPD_OK) {
		say("\n  closing %s failed\n", upd);
		rc = UPD_ERR;
	}
	if (rc == UPD_OK) {
		bool ok = got == e->size;
		if (ok && e->has_crc && crc != e->crc) ok = false;
		if (ok && use_sha) {
			z_sha256_final(&c, d);
			to_hex(d, Z_SHA256_DIGEST, hex);
			ok = hash_eq(hex, e->hash);
		}
		if (!ok) {
			say("\n  %s: %s does not match the manifest\n", obj,
				got != e->size ? "its size" : "its hash");
			rc = UPD_ERR;
		}
	}
	if (rc != UPD_OK) {
		io->unlink(io->ctx, upd);
		return rc;
	}
	uint32_t el = io->ms(io->ctx) - t_file;
	if (e->size >= PROGRESS_MIN)
		say("\r  [%d/%d] %s  %s  ok  %lu KB/s                \n", pg->index,
			pg->count, label, size_str(e->size, sz, sizeof sz),
			(unsigned long)kb_per_s(e->size, el));
	else
		say("  [%d/%d] %s  %s  ok\n", pg->index, pg->count, label,
			size_str(e->size, sz, sizeof sz));
	return UPD_OK;
}

// -- the run -------------------------------------------------------------

static void say_manifest(const char *what, const upd_manifest_t *m) {
	say("update: %s %.*s snapshot %.*s (%.*s, %.*s)\n", what,
		m->name_len, m->name ? m->name : "?",
		m->snapshot_len, m->snapshot ? m->snapshot : "?",
		m->source_len, m->source ? m->source : "?",
		m->made_len, m->made ? m->made : "?");
}

// /sys/version for a manifest (release/lib/manifest.py, version_text()).
static int version_text(const upd_manifest_t *m, const char *id, char *vt, int cap) {
	return snprintf(vt, cap, "zeitlos %.*s\nsnapshot %.*s %s %.*s %.*s\n",
		m->version_len, m->version ? m->version : "?",
		m->name_len, m->name ? m->name : "?", id,
		m->source_len, m->source ? m->source : "?",
		m->made_len, m->made ? m->made : "?");
}

// The card is up to date: finish what a run that stopped while writing
// /sys/manifest or /sys/version left -- the file itself if it was
// recovered from its .upd or .old, /sys/version if it does not match,
// and the side files.
static void sys_files(const upd_manifest_t *m, bool recovered) {
	char vt[256], id[ID_DIGITS + 1], *have;
	uint32_t hl;
	if (recovered) write_text(SYS_MANIFEST, m->text, m->len);
	io->unlink(io->ctx, SYS_MANIFEST ".upd");
	io->unlink(io->ctx, SYS_MANIFEST ".old");
	memcpy(id, m->snapshot, ID_DIGITS);
	id[ID_DIGITS] = 0;
	int vl = version_text(m, id, vt, sizeof vt);
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

int upd_run(const upd_io_t *io_, const upd_opts_t *o) {
	io = io_;
	made_dir[0] = 0;

	char name[NAME_MAX_LEN + 1], obj[96], err[96], sz[16];
	char p_card[UPD_PATH_MAX], p_upd[UPD_PATH_MAX];
	upd_manifest_t R, I;
	bool have_I = false;
	item_t *items = NULL;
	int status = UPD_FAILED;
	char *text;
	uint32_t len;

	memset(&R, 0, sizeof R);
	memset(&I, 0, sizeof I);
	const char *dir = o->dir && o->dir[0] ? o->dir : "release";

	// -- 1. the release --
	if (!strcmp(o->release, "latest")) {
		snprintf(obj, sizeof obj, "%s/LATEST", dir);
		err[0] = 0;
		if (fetch_slurp(obj, &text, &len, err, sizeof err) != UPD_OK) {
			say("update: %s from %s: %s\n", obj, o->source, err);
			return UPD_FAILED;
		}
		int n = 0;
		while (n < (int)len && n < NAME_MAX_LEN && text[n] > ' ') {
			name[n] = text[n];
			n++;
		}
		name[n] = 0;
		free(text);
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
	snprintf(obj, sizeof obj, "%s/%s/manifest", dir, name);
	err[0] = 0;
	if (fetch_slurp(obj, &text, &len, err, sizeof err) != UPD_OK) {
		say("update: %s from %s: %s\n", obj, o->source, err);
		return UPD_FAILED;
	}
	int pr = upd_manifest_parse(&R, text, len, err, sizeof err);
	if (pr) {
		say("update: %s: %s\n", obj, err);
		if (pr == -2)
			say("        This release's own update can: fetch it with\n"
				"        tget SERVER %s/%s/update %s\n"
				"        and run update again.\n", dir, name, Z_DIR_APPS "/update");
		upd_manifest_free(&R);
		return UPD_FAILED;
	}
	say("update: %s from %s\n", name, o->source);
	say_manifest("release is", &R);

	// -- 2. the card --
	//
	// /sys/manifest itself is replaced like any file, so a run stopped
	// between its two renames leaves it missing, with the new one as
	// .upd (complete: renamed only after it was closed) or the old one
	// as .old. Either says more than nothing, the new one most.
	int got = card_slurp(SYS_MANIFEST, &text, &len, UPD_MANIFEST_MAX);
	bool recovered = false;
	if (got != UPD_OK) {
		static const char *side[2] = { SYS_MANIFEST ".upd", SYS_MANIFEST ".old" };
		for (int s2 = 0; s2 < 2 && got != UPD_OK; s2++) {
			if (card_slurp(side[s2], &text, &len, UPD_MANIFEST_MAX) != UPD_OK)
				continue;
			if (upd_manifest_parse(&I, text, len, err, sizeof err) == 0) {
				upd_manifest_free(&I);
				card_slurp(side[s2], &text, &len, UPD_MANIFEST_MAX);
				say("update: %s was missing: using %s, from a run that "
					"stopped\n", SYS_MANIFEST, side[s2]);
				got = UPD_OK;
				recovered = true;
			} else {
				upd_manifest_free(&I);
			}
		}
	}
	if (got == UPD_OK) {
		if (upd_manifest_parse(&I, text, len, err, sizeof err) == 0) {
			have_I = true;
			say_manifest("card has  ", &I);
		} else {
			say("update: the card's %s is unreadable (%s): checking every "
				"file instead\n", SYS_MANIFEST, err);
			upd_manifest_free(&I);
		}
	} else {
		say("update: the card has no %s (written before updates): checking "
			"every file\n", SYS_MANIFEST);
	}
	if (have_I && !o->verify && I.snapshot_len == ID_DIGITS &&
			!memcmp(I.snapshot, R.snapshot, ID_DIGITS)) {
		say("update: up to date\n");
		sys_files(&I, recovered);
		status = UPD_DONE;
		goto out;
	}

	// -- 3. the plan --
	int cap = R.nfiles + (have_I ? I.nfiles : 0);
	items = calloc(cap ? cap : 1, sizeof *items);
	if (!items) {
		say("update: out of memory\n");
		goto out;
	}
	int nitems = 0, ri = 0, ii = 0;
	while (ri < R.nfiles || (have_I && ii < I.nfiles)) {
		item_t *it = &items[nitems++];
		it->r = it->i = -1;
		int c;
		if (ri >= R.nfiles) c = 1;
		else if (!have_I || ii >= I.nfiles) c = -1;
		else c = path_cmp(R.files[ri].path, R.files[ri].path_len,
			I.files[ii].path, I.files[ii].path_len);
		if (c <= 0) it->r = ri++;
		if (c >= 0) it->i = ii++;
	}

	uint32_t t0 = io->ms(io->ctx), last = t0;
	int nwrite = 0, nremove = 0, nsame = 0, nkept = 0, nchanged = 0;
	uint32_t bytes = 0, bytes_freed = 0, largest = 0;
	for (int k = 0; k < nitems; k++) {
		item_t *it = &items[k];
		const upd_entry_t *re = it->r >= 0 ? &R.files[it->r] : NULL;
		const upd_entry_t *ie = it->i >= 0 ? &I.files[it->i] : NULL;
		const upd_entry_t *e = re ? re : ie;
		// The card copy, read only when the manifests cannot answer,
		// and SHA-256 only when a CRC cannot (-v, a format-1 entry).
		cardsum_t cs = { 1, false, 0, { 0 } };
		bool want_sha = o->verify || (re && !re->has_crc) || (ie && !ie->has_crc);
		if (!card_path(p_card, e->path, e->path_len, "")) {
			say("update: path too long: %.*s\n", e->path_len, e->path);
			goto out;
		}
		it->exists = io->size(io->ctx, p_card, &it->card_size) == UPD_OK;
		uint32_t now = io->ms(io->ctx);
		if (now - last >= 1000) {
			last = now;
			say("\r  checking %d of %d   ", k + 1, nitems);
		}
#define SUM() (cs.rc == 1 ? card_sum(p_card, want_sha, &cs) : cs.rc)
		if (re && re->policy == 'r') {
			if (!o->verify && ie && hash_eq(ie->hash, re->hash) &&
					it->exists && it->card_size == re->size)
				it->action = A_SAME;
			else if (it->exists && it->card_size == re->size &&
					SUM() == UPD_OK && sum_is(&cs, re, o->verify))
				it->action = A_SAME;
			else
				it->action = A_WRITE;
		} else if (re) {	// 'k'
			if (!it->exists)
				it->action = A_WRITE;
			else if (!o->verify && ie && hash_eq(ie->hash, re->hash))
				it->action = A_SAME;
			else if (SUM() != UPD_OK)
				it->action = A_KEPT;	// unreadable: leave it
			else if (sum_is(&cs, re, o->verify))
				it->action = A_SAME;
			else if (ie && sum_is(&cs, ie, o->verify))
				it->action = A_WRITE;	// never edited: the new one
			else
				it->action = A_KEPT;
		} else {	// in the card's manifest only
			if (!it->exists)
				it->action = A_GONE;
			else if (SUM() == UPD_OK && sum_is(&cs, ie, o->verify))
				it->action = A_REMOVE;
			else
				it->action = A_CHANGED;
		}
#undef SUM
		// The card has it but its manifest did not say so: a run that
		// stopped between putting a file in place and removing the
		// old one. Tidy up after it.
		if (it->action == A_SAME && (!ie || !hash_eq(ie->hash, re->hash))) {
			char p_old[UPD_PATH_MAX];
			if (card_path(p_old, e->path, e->path_len, ".old"))
				io->unlink(io->ctx, p_old);
		}
		switch (it->action) {
		case A_SAME: nsame++; break;
		case A_KEPT: nkept++; break;
		case A_CHANGED: nchanged++; break;
		case A_REMOVE:
			nremove++;
			bytes_freed += it->card_size;
			break;
		case A_WRITE:
			nwrite++;
			bytes += re->size;
			if (it->exists) bytes_freed += it->card_size;
			if (re->size > largest) largest = re->size;
			// A complete .upd from a run that stopped: use it.
			if (card_path(p_upd, e->path, e->path_len, ".upd")) {
				uint32_t us;
				if (io->size(io->ctx, p_upd, &us) == UPD_OK) {
					cardsum_t ucs;
					if (us == re->size &&
							card_sum(p_upd, o->verify || !re->has_crc, &ucs) == UPD_OK &&
							sum_is(&ucs, re, o->verify))
						it->upd_ready = true;
					else
						io->unlink(io->ctx, p_upd);
				}
			}
			break;
		}
	}
	if (nitems && io->ms(io->ctx) - t0 >= 1000) say("\r                            \r");

	say("update: %d to write (%s), %d to remove, %d unchanged", nwrite,
		size_str(bytes, sz, sizeof sz), nremove, nsame);
	if (nkept) say(", %d kept as edited", nkept);
	say("\n");

	if (o->dry_run) {
		for (int k = 0; k < nitems; k++) {
			const item_t *it = &items[k];
			const upd_entry_t *e = it->r >= 0 ? &R.files[it->r] : &I.files[it->i];
			const char *what = NULL;
			switch (it->action) {
			case A_WRITE: what = it->exists ? "replace" : "add"; break;
			case A_REMOVE: what = "remove"; break;
			case A_KEPT: what = "keep (edited)"; break;
			case A_CHANGED: what = "leave (edited, no longer shipped)"; break;
			}
			if (what)
				say("  %-8s %.*s\n", what, e->path_len, e->path);
		}
		say("update: -n: nothing changed\n");
		status = UPD_DONE;
		goto out;
	}

	// -- 4. room, then the work --
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

	for (int d = 0; d < R.ndirs; d++) {
		char p_dir[UPD_PATH_MAX];
		if (card_path(p_dir, R.dirs[d], R.dir_len[d], "")) {
			char probe[UPD_PATH_MAX + 4];
			snprintf(probe, sizeof probe, "%s/x", p_dir);
			mkdir_parents(probe);
		}
	}

	progress_t pg = { 0, nwrite, bytes, 0, io->ms(io->ctx) };
	bool stopped = false, broken = false;
	int nbusy = 0, nfailed = 0, ndone = 0, nremoved = 0;
	for (int k = 0; k < nitems; k++) {
		item_t *it = &items[k];
		if (it->action != A_WRITE) continue;
		const upd_entry_t *re = &R.files[it->r];
		pg.index++;
		if (stopped || broken) { it->action = A_NOTREACHED; continue; }
		if (io->stop && io->stop(io->ctx)) {
			stopped = true;
			it->action = A_NOTREACHED;
			continue;
		}
		card_path(p_card, re->path, re->path_len, "");
		card_path(p_upd, re->path, re->path_len, ".upd");
		char label[UPD_PATH_MAX];
		snprintf(label, sizeof label, "%.*s", re->path_len, re->path);
		if (mkdir_parents(p_card) != UPD_OK) {
			it->action = A_FAILED;
			nfailed++;
			broken = true;
			continue;
		}
		if (it->upd_ready) {
			say("  [%d/%d] %s  %s  already fetched\n", pg.index, pg.count,
				label, size_str(re->size, sz, sizeof sz));
		} else {
			snprintf(obj, sizeof obj, "%s/o/%.2s/%.*s", dir, re->hash,
				OBJECT_DIGITS, re->hash);
			int fr = fetch_one(obj, p_upd, re, &pg, label, o->verify);
			if (fr == UPD_ERR) {
				say("  retrying %s\n", label);
				fr = fetch_one(obj, p_upd, re, &pg, label, o->verify);
			}
			if (fr == -10) {
				stopped = true;
				it->action = A_NOTREACHED;
				continue;
			}
			if (fr != UPD_OK) {
				it->action = A_FAILED;
				nfailed++;
				broken = true;
				continue;
			}
		}
		int ir = install(p_upd, p_card);
		if (ir == UPD_EBUSY) {
			say("  [%d/%d] %s  in use by a running program: left for the "
				"next run\n", pg.index, pg.count, label);
			it->action = A_BUSY;
			nbusy++;
		} else if (ir != UPD_OK) {
			say("  [%d/%d] %s  could not be put in place\n", pg.index,
				pg.count, label);
			it->action = A_FAILED;
			nfailed++;
			broken = true;
		} else {
			it->action = A_DONE;
			ndone++;
			pg.bytes_done += re->size;
		}
	}

	if (!stopped && !broken) {
		for (int k = 0; k < nitems; k++) {
			item_t *it = &items[k];
			if (it->action != A_REMOVE) continue;
			const upd_entry_t *ie = &I.files[it->i];
			card_path(p_card, ie->path, ie->path_len, "");
			if (io->unlink(io->ctx, p_card) == UPD_OK) {
				it->action = A_REMOVED;
				nremoved++;
				say("  removed %.*s\n", ie->path_len, ie->path);
			}
		}
	}
	for (int k = 0; k < nitems; k++)
		if (items[k].action == A_CHANGED) {
			const upd_entry_t *ie = &I.files[items[k].i];
			say("  left %.*s: no longer shipped, but changed here\n",
				ie->path_len, ie->path);
		}

	// -- 5. what the card now holds --
	{
		char id[ID_DIGITS + 1];
		uint32_t mlen;
		char *mt = new_manifest(&R, have_I ? &I : NULL, items, nitems, &mlen, id);
		if (!mt) {
			say("update: out of memory writing the manifest. The files are in "
			"place; run update again and it will write it.\n");
			goto out;
		}
		if (!have_I || mlen != I.len || memcmp(mt, I.text, mlen)) {
			if (write_text(SYS_MANIFEST, mt, mlen) != UPD_OK)
				say("update: could not write %s -- run update again\n",
					SYS_MANIFEST);
			char vt[256];
			int vl = version_text(&R, id, vt, sizeof vt);
			if (write_text(SYS_VERSION, vt, (uint32_t)vl) != UPD_OK)
				say("update: could not write %s\n", SYS_VERSION);
		}
		free(mt);

		uint32_t el = io->ms(io->ctx) - pg.t0;
		say("update: %s in %lu s: %d written, %d removed", stopped ? "stopped" :
			(nbusy || nfailed) ? "partly done" : "done",
			(unsigned long)(el / 1000), ndone, nremoved);
		if (pg.bytes_done >= PROGRESS_MIN)
			say(" (%s, %lu KB/s)", size_str(pg.bytes_done, sz, sizeof sz),
				(unsigned long)kb_per_s(pg.bytes_done, el));
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

out:
	free(items);
	upd_manifest_free(&R);
	if (have_I) upd_manifest_free(&I);
	return status;
}

int upd_show(const upd_io_t *io_) {
	io = io_;
	char *text;
	uint32_t len;
	if (card_slurp(SYS_VERSION, &text, &len, 4096) == UPD_OK) {
		say("%s", text);
		if (len && text[len - 1] != '\n') say("\n");
		free(text);
	}
	upd_manifest_t m;
	char err[96];
	if (card_slurp(SYS_MANIFEST, &text, &len, UPD_MANIFEST_MAX) != UPD_OK) {
		say("no %s: this card was written before updates\n", SYS_MANIFEST);
		return UPD_DONE;
	}
	if (upd_manifest_parse(&m, text, len, err, sizeof err)) {
		say("%s: %s\n", SYS_MANIFEST, err);
		upd_manifest_free(&m);
		return UPD_FAILED;
	}
	uint64_t total = 0;
	for (int i = 0; i < m.nfiles; i++) total += m.files[i].size;
	char sz[16];
	say("%s: %d files, %s\n", SYS_MANIFEST, m.nfiles,
		size_str(total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total, sz, sizeof sz));
	upd_manifest_free(&m);
	return UPD_DONE;
}
