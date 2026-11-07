/*
 * Zeitlos -- sw/common/formats: making an archive -- zip, tar, tar.gz --
 * from files the caller hands over one at a time. zf.h, "Packing".
 *
 * -- deflate --
 *
 * zip and gzip both carry deflate (RFC 1951). This is a small
 * compressor: LZ77 over a 16KB window with hash chains, greedy
 * matching, and the fixed Huffman codes -- no dynamic tables. Text
 * comes out at about half its size, a quarter to a third bigger than
 * `gzip -9` makes it (this tree's docs: 5.1MB to 2.6MB, gzip 2.0MB);
 * incompressible bytes grow by at most an eighth (a literal above 143
 * takes nine bits). zip stores, rather
 * than deflates, the kinds of file that are compressed already.
 *
 * The output is one fixed-code block that runs to the end of the
 * input, then an empty final block: a stream does not know which data
 * is its last, and an empty block costs ten bits.
 *
 * Its memory (72KB) comes from zf_heap. With less there, it writes
 * stored blocks instead: still deflate, still read by everything, just
 * not smaller.
 *
 * -- zip --
 *
 * Each entry's local header is written first and its CRC and sizes
 * patched in once the data is through (io->seek), as zf_zip.c's
 * writer does for docx. The central directory is kept in zf_heap after
 * the compressor until the end, about 60 bytes an entry. No ZIP64:
 * 65535 entries, and ZF_OUT_MAX bounds the size long before 4GB.
 *
 * -- tar --
 *
 * ustar, 512-byte headers. A path longer than 100 bytes is split into
 * ustar's prefix and name where it can be, else carried in a GNU
 * "././@LongLink" entry before its header -- both of which zf_arch.c
 * and every tar read. Files are rw-r--r--, folders rwxr-xr-x, owned by
 * nobody in particular (uid 0, no user names).
 */

#include <string.h>

#include "zf.h"

// -- bits --

static int put(zf_pack_t *p, const void *b, uint32_t n) {
	int32_t r = zf_write(p->out, b, n);
	return r < 0 ? (int)r : ZF_OK;
}

// The compressed stream goes to the archive (zip, tgz's inner tar) via
// a small buffer: bits come out a few at a time.
static int z_out_flush(zf_pack_t *p) {
	if (!p->zlen) return ZF_OK;
	int r = put(p, p->zbuf, p->zlen);
	p->zcsize += p->zlen;
	p->zlen = 0;
	return r;
}

static int z_byte(zf_pack_t *p, uint8_t b) {
	p->zbuf[p->zlen++] = b;
	return p->zlen == sizeof p->zbuf ? z_out_flush(p) : ZF_OK;
}

static int z_bits(zf_pack_t *p, uint32_t v, int n) {
	p->bitbuf |= v << p->bitcnt;
	p->bitcnt += n;
	while (p->bitcnt >= 8) {
		int r = z_byte(p, (uint8_t)p->bitbuf);
		if (r) return r;
		p->bitbuf >>= 8;
		p->bitcnt -= 8;
	}
	return ZF_OK;
}

static int z_align(zf_pack_t *p) {
	int r = ZF_OK;
	if (p->bitcnt) r = z_byte(p, (uint8_t)p->bitbuf);
	p->bitbuf = 0;
	p->bitcnt = 0;
	return r;
}

// a Huffman code, sent most significant bit first
static int z_code(zf_pack_t *p, uint32_t code, int len) {
	uint32_t r = 0;
	for (int i = 0; i < len; i++) { r = (r << 1) | (code & 1); code >>= 1; }
	return z_bits(p, r, len);
}

// the fixed literal/length code (RFC 1951, 3.2.6)
static int z_lit(zf_pack_t *p, int c) {
	if (c < 144) return z_code(p, 0x30u + (uint32_t)c, 8);
	if (c < 256) return z_code(p, 0x190u + (uint32_t)(c - 144), 9);
	if (c < 280) return z_code(p, (uint32_t)(c - 256), 7);
	return z_code(p, 0xC0u + (uint32_t)(c - 280), 8);
}

static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int z_match(zf_pack_t *p, int len, int dist) {
	int i = 28, r;
	while (len_base[i] > len) i--;
	if ((r = z_lit(p, 257 + i))) return r;
	if (len_extra[i] && (r = z_bits(p, (uint32_t)(len - len_base[i]), len_extra[i]))) return r;
	i = 29;
	while (dist_base[i] > dist) i--;
	if ((r = z_code(p, (uint32_t)i, 5))) return r;
	if (dist_extra[i]) return z_bits(p, (uint32_t)(dist - dist_base[i]), dist_extra[i]);
	return ZF_OK;
}

// -- LZ77 --

#define ZW			16384u				// the window
#define ZHASH		4096u
#define ZNIL		0xFFFFu
#define ZMIN_LOOK	262u				// a whole match, and the next hash
#define ZCHAIN		48
#define ZMEM		(2 * ZW + ZW * 2 + ZHASH * 2)

static uint32_t z_hash(const uint8_t *s) {
	return (((uint32_t)s[0] << 10) ^ ((uint32_t)s[1] << 5) ^ s[2]) & (ZHASH - 1);
}

static void z_insert(zf_pack_t *p, uint32_t pos) {
	uint32_t h = z_hash(p->win + pos);
	p->prev[pos & (ZW - 1)] = p->head[h];
	p->head[h] = (uint16_t)pos;
}

// Encodes the window from pos while enough of it is ahead (all of it
// when `all`).
static int z_run(zf_pack_t *p, bool all) {
	while (p->pos < p->fill && (all || p->fill - p->pos >= ZMIN_LOOK)) {
		uint32_t avail = p->fill - p->pos, best = 0, bdist = 0;
		if (avail >= 3) {
			uint32_t maxl = avail < 258 ? avail : 258;
			uint32_t h = z_hash(p->win + p->pos);
			uint32_t c = p->head[h];
			const uint8_t *s = p->win + p->pos;
			for (int n = ZCHAIN; c != ZNIL && n > 0; n--) {
				if (c >= p->pos || p->pos - c > ZW - ZMIN_LOOK) break;
				const uint8_t *t = p->win + c;
				if (t[best] == s[best] && t[0] == s[0]) {
					uint32_t l = 0;
					while (l < maxl && t[l] == s[l]) l++;
					if (l > best) { best = l; bdist = p->pos - c; if (l == maxl) break; }
				}
				uint32_t nx = p->prev[c & (ZW - 1)];
				if (nx >= c) break;			// stale: from before a slide
				c = nx;
			}
		}
		int r;
		if (best >= 3) {
			if ((r = z_match(p, (int)best, (int)bdist))) return r;
			for (uint32_t i = 0; i < best; i++, p->pos++)
				if (p->pos + 2 < p->fill) z_insert(p, p->pos);
		} else {
			if ((r = z_lit(p, p->win[p->pos]))) return r;
			if (avail >= 3) z_insert(p, p->pos);
			p->pos++;
		}
	}
	return ZF_OK;
}

static void z_slide(zf_pack_t *p) {
	memmove(p->win, p->win + ZW, p->fill - ZW);
	p->fill -= ZW;
	p->pos -= ZW;
	for (uint32_t i = 0; i < ZHASH; i++)
		p->head[i] = p->head[i] != ZNIL && p->head[i] >= ZW ? (uint16_t)(p->head[i] - ZW) : ZNIL;
	for (uint32_t i = 0; i < ZW; i++)
		p->prev[i] = p->prev[i] != ZNIL && p->prev[i] >= ZW ? (uint16_t)(p->prev[i] - ZW) : ZNIL;
}

static int z_begin(zf_pack_t *p) {
	p->bitbuf = 0;
	p->bitcnt = 0;
	p->zcsize = 0;
	p->pos = p->fill = 0;
	if (p->win) {
		memset(p->head, 0xFF, ZHASH * 2);
		memset(p->prev, 0xFF, ZW * 2);
		return z_bits(p, 1u << 1, 3);		// not final, fixed codes
	}
	return ZF_OK;
}

static int z_write(zf_pack_t *p, const uint8_t *d, uint32_t n) {
	int r;
	if (!p->win) {
		// stored blocks: no memory for a window
		while (n) {
			uint32_t k = n > 65535 ? 65535 : n;
			uint8_t h[4] = { (uint8_t)k, (uint8_t)(k >> 8), (uint8_t)~k, (uint8_t)(~k >> 8) };
			if ((r = z_bits(p, 0, 3)) || (r = z_align(p))) return r;
			for (int i = 0; i < 4; i++) if ((r = z_byte(p, h[i]))) return r;
			if ((r = z_out_flush(p))) return r;
			if ((r = put(p, d, k))) return r;
			p->zcsize += k;
			d += k;
			n -= k;
		}
		return ZF_OK;
	}
	while (n) {
		if (p->fill == 2 * ZW) {
			if ((r = z_run(p, false))) return r;
			z_slide(p);
		}
		uint32_t k = 2 * ZW - p->fill;
		if (k > n) k = n;
		memcpy(p->win + p->fill, d, k);
		p->fill += k;
		d += k;
		n -= k;
	}
	return ZF_OK;
}

static int z_finish(zf_pack_t *p) {
	int r;
	if (p->win) {
		if ((r = z_run(p, true))) return r;
		if ((r = z_lit(p, 256))) return r;		// the end of the long block
	}
	if ((r = z_bits(p, 1u | (1u << 1), 3))) return r;	// final, fixed
	if ((r = z_lit(p, 256))) return r;
	if ((r = z_align(p))) return r;
	return z_out_flush(p);
}

// -- small helpers --

static void le16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *b, uint32_t v) { le16(b, v); le16(b + 2, v >> 16); }

static void octal(char *f, int width, uint32_t v) {
	f[width - 1] = 0;
	for (int i = width - 2; i >= 0; i--) { f[i] = (char)('0' + (v & 7)); v >>= 3; }
}

// FAT's date and time to Unix time, as UTC (FAT does not say)
static uint32_t unix_time(uint16_t d, uint16_t t) {
	if (!d) return 0;
	static const uint16_t mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	uint32_t y = 1980u + (d >> 9), m = (d >> 5) & 15, dd = d & 31;
	if (m < 1 || m > 12) m = 1;
	if (dd < 1) dd = 1;
	uint32_t days = (y - 1970) * 365 + (y - 1969) / 4 + mdays[m - 1] + dd - 1;
	if (m > 2 && y % 4 == 0) days++;		// 1980..2107: every fourth year
	return days * 86400u + (uint32_t)(t >> 11) * 3600u + ((t >> 5) & 63) * 60u + (t & 31) * 2u;
}

// -- the archive's own stream: straight out (zip, tar) or through gzip (tgz) --

static int a_write(zf_pack_t *p, const void *d, uint32_t n) {
	if (p->to != ZF_T_TGZ) return put(p, d, n);
	p->gcrc = zf_crc32(p->gcrc, d, n);
	p->gsize += n;
	return z_write(p, (const uint8_t *)d, n);
}

// -- tar --

static int tar_header(zf_pack_t *p, const char *name, char type, uint32_t size, uint32_t mtime) {
	uint8_t h[512];
	memset(h, 0, sizeof h);
	size_t nl = strlen(name);
	if (nl <= 100) {
		memcpy(h, name, nl);
	} else {
		// ustar's prefix: the path split at a '/', name <= 100, prefix <= 155
		const char *cut = NULL;
		for (const char *s = name; *s; s++)
			if (*s == '/' && s - name <= 155 && strlen(s + 1) <= 100 && s[1]) { cut = s; break; }
		if (cut) {
			memcpy(h + 345, name, (size_t)(cut - name));
			memcpy(h, cut + 1, strlen(cut + 1));
		} else {
			// GNU: the name as the data of an entry of its own
			int r = tar_header(p, "././@LongLink", 'L', (uint32_t)nl + 1, 0);
			if (r) return r;
			uint8_t blk[512];
			for (size_t o = 0; o < nl + 1; o += 512) {
				memset(blk, 0, sizeof blk);
				size_t k = nl + 1 - o > 512 ? 512 : nl + 1 - o;
				memcpy(blk, name + o, k > nl - o ? nl - o : k);
				if ((r = a_write(p, blk, 512))) return r;
			}
			memcpy(h, name, 100);
		}
	}
	octal((char *)h + 100, 8, type == '5' ? 0755 : 0644);
	octal((char *)h + 108, 8, 0);
	octal((char *)h + 116, 8, 0);
	octal((char *)h + 124, 12, size);
	octal((char *)h + 136, 12, mtime);
	memset(h + 148, ' ', 8);
	h[156] = (uint8_t)type;
	memcpy(h + 257, "ustar", 6);
	h[263] = '0'; h[264] = '0';
	uint32_t sum = 0;
	for (int i = 0; i < 512; i++) sum += h[i];
	octal((char *)h + 148, 7, sum);
	h[155] = ' ';
	return a_write(p, h, 512);
}

// -- zip: the central directory, kept until the end --

// one record: crc, csize, usize, offset (4 each), date, time, method,
// name length (2 each), the name
#define CD_FIXED 24

static int cd_add(zf_pack_t *p, const char *name, uint32_t crc, uint32_t cs, uint32_t us,
	uint32_t off, uint16_t d, uint16_t t, uint16_t method) {
	size_t nl = strlen(name);
	if (p->n == 65535) return ZF_E_LIMIT;
	if (p->cdlen + CD_FIXED + nl > p->cdcap) return ZF_E_NOMEM;
	uint8_t *c = p->cd + p->cdlen;
	le32(c, crc); le32(c + 4, cs); le32(c + 8, us); le32(c + 12, off);
	le16(c + 16, d); le16(c + 18, t); le16(c + 20, method); le16(c + 22, (uint32_t)nl);
	memcpy(c + CD_FIXED, name, nl);
	p->cdlen += CD_FIXED + (uint32_t)nl;
	p->n++;
	return ZF_OK;
}

// Kinds that are compressed already: stored, not deflated again.
static bool packed_kind(const char *name) {
	static const char *const ext[] = {
		"zip", "gz", "tgz", "bz2", "xz", "7z", "jpg", "jpeg", "png", "gif", "webp",
		"mp3", "mp4", "m4a", "ogg", "opus", "flac", "docx", "xlsx", "pptx", "odt",
		"ods", "odp", "epub", "jar", "apk", "z", "zst", "lz4",
	};
	const char *e = zf_ext(name);
	if (!*e) return false;
	for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
		if (zf_ieq(e, ext[i])) return true;
	return false;
}

// -- the API --

int zf_pack_begin(zf_pack_t *p, zf_io_t *out, zf_type_t to, uint32_t total) {
	memset(p, 0, sizeof *p);
	p->out = out;
	p->to = to;
	p->total = total;
	if (to != ZF_T_ZIP && to != ZF_T_TAR && to != ZF_T_TGZ) return ZF_E_UNSUPP;
	uint8_t *heap = zf_heap;
	uint32_t cap = zf_heap_size;
	if (heap && cap >= ZMEM + (to == ZF_T_ZIP ? 4096u : 0u)) {
		p->win = heap;
		p->prev = (uint16_t *)(void *)(heap + 2 * ZW);
		p->head = (uint16_t *)(void *)(heap + 2 * ZW + ZW * 2);
		heap += ZMEM;
		cap -= ZMEM;
	}
	if (to == ZF_T_ZIP) {
		if (!heap || cap < 1024) return ZF_E_NOMEM;
		p->cd = heap;
		p->cdcap = cap;
	}
	if (to == ZF_T_TGZ) {
		static const uint8_t gz[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
		int r = put(p, gz, sizeof gz);
		if (r) return r;
		p->gcrc = 0;
		return z_begin(p);
	}
	return ZF_OK;
}

int zf_pack_add(zf_pack_t *p, const char *name, bool dir, uint32_t size,
	uint16_t fdate, uint16_t ftime, zf_io_t *in) {
	static uint8_t buf[4096];
	char nm[ZF_PATH_MAX + 2];
	size_t nl = strlen(name);
	int r;
	if (!nl || nl + 2 > sizeof nm || name[0] == '/') return ZF_E_PATH;
	memcpy(nm, name, nl + 1);
	if (dir) { nm[nl++] = '/'; nm[nl] = 0; size = 0; }

	uint16_t method = 0;
	uint32_t off = p->out->written, crc = 0, done = 0;
	if (p->to == ZF_T_ZIP) {
		method = !dir && size >= 64 && p->win && !packed_kind(name) ? 8 : 0;
		uint8_t h[30];
		memset(h, 0, sizeof h);
		le32(h, 0x04034b50);
		le16(h + 4, method ? 20 : 10);
		le16(h + 6, 0x0800);					// UTF-8 names
		le16(h + 8, method);
		le16(h + 10, ftime);
		le16(h + 12, fdate);
		le16(h + 26, (uint32_t)nl);
		if ((r = put(p, h, 30)) || (r = put(p, nm, (uint32_t)nl))) return r;
		if (method && (r = z_begin(p))) return r;
		p->zcsize = 0;
	} else {
		if ((r = tar_header(p, nm, dir ? '5' : '0', size, unix_time(fdate, ftime)))) return r;
	}

	while (done < size) {
		if (zf_cancel_poll && zf_cancel_poll()) return ZF_E_CANCEL;
		uint32_t k = size - done > sizeof buf ? (uint32_t)sizeof buf : size - done;
		int32_t got = in->read(in, buf, k);
		if (got <= 0) return ZF_E_IO;			// shorter than it said: changed while read
		done += (uint32_t)got;
		p->done += (uint32_t)got;
		if (zf_progress && p->total) zf_progress((uint32_t)((uint64_t)p->done * 1000u / p->total));
		if (p->to == ZF_T_ZIP) {
			crc = zf_crc32(crc, buf, (uint32_t)got);
			r = method ? z_write(p, buf, (uint32_t)got) : put(p, buf, (uint32_t)got);
		} else {
			r = a_write(p, buf, (uint32_t)got);
		}
		if (r) return r;
	}

	if (p->to == ZF_T_ZIP) {
		uint32_t cs = size;
		if (method) {
			if ((r = z_finish(p))) return r;
			cs = p->zcsize;
		}
		uint8_t q[12];
		le32(q, crc); le32(q + 4, cs); le32(q + 8, size);
		uint32_t here = p->out->written;
		if (!p->out->seek(p->out, off + 14) || p->out->write(p->out, q, 12) != 12 ||
			!p->out->seek(p->out, here))
			return ZF_E_IO;
		return cd_add(p, nm, crc, cs, size, off, fdate, ftime, method);
	}
	if (size % 512) {
		memset(buf, 0, 512);
		if ((r = a_write(p, buf, 512 - size % 512))) return r;
	}
	return ZF_OK;
}

int zf_pack_end(zf_pack_t *p) {
	int r;
	if (p->to == ZF_T_ZIP) {
		uint32_t cd = p->out->written;
		for (uint32_t o = 0; o < p->cdlen; ) {
			const uint8_t *c = p->cd + o;
			uint32_t nl = (uint32_t)(c[22] | (c[23] << 8));
			uint8_t h[46];
			memset(h, 0, sizeof h);
			le32(h, 0x02014b50);
			le16(h + 4, 0x031E);					// made by: Unix, 3.0
			le16(h + 6, c[20] ? 20 : 10);
			le16(h + 8, 0x0800);
			memcpy(h + 10, c + 20, 2);				// method
			memcpy(h + 12, c + 18, 2);				// time
			memcpy(h + 14, c + 16, 2);				// date
			memcpy(h + 16, c, 12);					// crc, sizes
			le16(h + 28, nl);
			bool d = nl && c[CD_FIXED + nl - 1] == '/';
			le32(h + 38, (d ? 040755u : 0100644u) << 16 | (d ? 0x10u : 0u));
			memcpy(h + 42, c + 12, 4);				// offset
			if ((r = put(p, h, 46)) || (r = put(p, c + CD_FIXED, nl))) return r;
			o += CD_FIXED + nl;
		}
		uint8_t e[22];
		memset(e, 0, sizeof e);
		le32(e, 0x06054b50);
		le16(e + 8, p->n);
		le16(e + 10, p->n);
		le32(e + 12, p->out->written - cd);
		le32(e + 16, cd);
		return put(p, e, 22);
	}
	uint8_t z[512];
	memset(z, 0, sizeof z);
	if ((r = a_write(p, z, 512)) || (r = a_write(p, z, 512))) return r;
	if (p->to == ZF_T_TGZ) {
		if ((r = z_finish(p))) return r;
		uint8_t t[8];
		le32(t, p->gcrc);
		le32(t + 4, p->gsize);
		return put(p, t, 8);
	}
	return ZF_OK;
}
