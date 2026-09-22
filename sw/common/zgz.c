/*
 * Zeitlos -- reading a .gz file. See zgz.h.
 */
#include <stdint.h>
#include <stdbool.h>
#include "zfsapp.h"
#include "zgz.h"

// CRC-32 (IEEE, reflected, as gzip): a table built on first use, 1 KB.
static uint32_t crc_tab[256];
static bool crc_ready;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
	uint32_t i, k, c;
	if (!crc_ready) {
		for (i = 0; i < 256; i++) {
			for (c = i, k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_tab[i] = c;
		}
		crc_ready = true;
	}
	crc = ~crc;
	for (i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int z_gz_open(z_gz_t *g, const char *path, uint8_t *window) {
	g->handle = fs_open_read(path);
	if (g->handle < 0) return -1;
	z_inflate_init(&g->z, window, Z_INFLATE_GZIP, 0);
	g->z.gzip_fields = true;        // `gzip file` always writes a name
	g->in_len = g->in_pos = 0;
	g->eof = false;
	g->rv = Z_INFLATE_OK;
	g->crc = 0;
	g->size = 0;
	return 0;
}

int32_t z_gz_read(z_gz_t *g, void *buf, uint32_t n) {
	uint32_t out = 0;
	if (g->rv == Z_INFLATE_DONE) return 0;
	if (g->rv < 0) return g->rv;
	while (out < n) {
		uint32_t il, ol;
		int rv;
		if (g->in_pos == g->in_len && !g->eof) {
			int r = fs_read_chunk(g->handle, g->in, (int)sizeof(g->in));
			if (r < 0) { g->rv = Z_GZ_E_READ; break; }
			if (r == 0) g->eof = true;
			g->in_len = (uint32_t)(r > 0 ? r : 0);
			g->in_pos = 0;
		}
		il = g->in_len - g->in_pos;
		ol = n - out;
		rv = z_inflate(&g->z, g->in + g->in_pos, &il, (uint8_t *)buf + out, &ol);
		g->in_pos += il;
		g->crc = crc32_update(g->crc, (uint8_t *)buf + out, ol);
		g->size += ol;
		out += ol;
		if (rv == Z_INFLATE_DONE) {
			// the trailer zinflate kept: the CRC-32 and length (mod 2^32)
			g->rv = (le32(g->z.hdr) == g->crc && le32(g->z.hdr + 4) == g->size)
				? Z_INFLATE_DONE : Z_GZ_E_CRC;
			break;
		}
		if (rv < 0) { g->rv = rv; break; }
		// no progress, and the file has nothing more: it was cut short
		if (il == 0 && ol == 0 && g->eof) { g->rv = Z_GZ_E_TRUNCATED; break; }
	}
	if (out) return (int32_t)out;
	return g->rv == Z_INFLATE_DONE ? 0 : g->rv;
}

void z_gz_close(z_gz_t *g) {
	if (g->handle >= 0) fs_close_handle(g->handle);
	g->handle = -1;
}

const char *z_gz_strerror(int32_t rv) {
	if (rv == Z_GZ_E_TRUNCATED) return "truncated";
	if (rv == Z_GZ_E_READ) return "read error";
	if (rv == Z_GZ_E_CRC) return "CRC or length mismatch: the file is corrupt";
	return z_inflate_strerror((int)rv);
}
