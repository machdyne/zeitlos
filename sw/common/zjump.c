/*
 * Jumploaders: their boot address, read and changed in place. See
 * zjump.h, and pack.c's -J in sw/apps/zfpga for the other side.
 */
#include "zjump.h"

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// a hex number at *p, advancing past it; -1 if there is none
static int64_t hex(const char **p) {
	int64_t v = 0;
	int n = 0, d;
	while ((d = hexval(**p)) >= 0) { v = v * 16 + d; (*p)++; n++; }
	return n ? v : -1;
}

// "b=OOOOOO.MM,..." / "c=FFFFFF.AAAAAA,..." -- pairs, into a and b
static int pairs(const char **p, char key, uint32_t *a, uint32_t *b, int max) {
	int n = 0;
	int64_t x, y;
	while (**p == ' ') (*p)++;
	if (**p != key || (*p)[1] != '=') return -1;
	*p += 2;
	for (;;) {
		if (n == max) return -1;
		if ((x = hex(p)) < 0 || **p != '.') return -1;
		(*p)++;
		if ((y = hex(p)) < 0) return -1;
		a[n] = (uint32_t)x;
		b[n] = (uint32_t)y;
		n++;
		if (**p != ',') return n;
		(*p)++;
	}
}

int zjump_parse(zjump_rd_t rd, void *ctx, uint32_t max, zjump_t *j) {
	char line[256];
	uint32_t o = 2, n;
	int found = 0, k;
	if (rd(ctx, 0) != 0xFF || rd(ctx, 1) != 0x00) return -1;
	// the header's NUL-terminated strings, until the FF that ends it
	while (o < max && rd(ctx, o) != 0xFF) {
		n = 0;
		while (o < max && rd(ctx, o) != 0x00) {
			if (n < sizeof(line) - 1) line[n++] = (char)rd(ctx, o);
			o++;
		}
		line[n] = 0;
		o++;
		if (n > 7 && line[0] == 'Z' && line[1] == 'J' && line[2] == 'U' && line[3] == 'M' &&
				line[4] == 'P' && line[5] == '1' && line[6] == ' ') {
			const char *p = line + 7;
			uint32_t m[ZJUMP_BITS];
			if (pairs(&p, 'b', j->b_off, m, ZJUMP_BITS) != ZJUMP_BITS) return -1;
			for (k = 0; k < ZJUMP_BITS; k++) {
				if (!m[k] || (m[k] & (m[k] - 1)) || m[k] > 0x80) return -1;
				j->b_mask[k] = (uint8_t)m[k];
			}
			if ((j->n_crc = pairs(&p, 'c', j->c_from, j->c_at, ZJUMP_MAX_CRC)) < 1) return -1;
			for (k = 0; k < j->n_crc; k++) if (j->c_from[k] >= j->c_at[k]) return -1;
			found = 1;
		}
	}
	if (!found || o + 5 > max) return -1;
	o++;
	if (rd(ctx, o) != 0xFF || rd(ctx, o + 1) != 0xFF || rd(ctx, o + 2) != 0xBD || rd(ctx, o + 3) != 0xB3)
		return -1;
	j->pre = o;
	return 0;
}

uint32_t zjump_target(const zjump_t *j, zjump_rd_t rd, void *ctx) {
	uint32_t v = 0;
	int k;
	for (k = 0; k < ZJUMP_BITS; k++)
		if (rd(ctx, j->pre + j->b_off[k]) & j->b_mask[k]) v |= 1u << k;
	return v << 16;
}

// CRC-16/BUYPASS: polynomial 0x8005, no reflection, initial 0 -- what
// the ECP5 checks and pack.c writes
static uint16_t crc16(uint16_t crc, uint8_t b) {
	int k;
	crc ^= (uint16_t)b << 8;
	for (k = 0; k < 8; k++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x8005 : (crc << 1));
	return crc;
}

void zjump_set(const zjump_t *j, uint32_t target, zjump_rd_t rd, zjump_wr_t wr, void *ctx) {
	uint32_t v = (target >> 16) & 0xFF, o;
	int k;
	for (k = 0; k < ZJUMP_BITS; k++) {
		uint32_t at = j->pre + j->b_off[k];
		uint8_t b = rd(ctx, at);
		b = (v >> k) & 1 ? (uint8_t)(b | j->b_mask[k]) : (uint8_t)(b & ~j->b_mask[k]);
		wr(ctx, at, b);
	}
	for (k = 0; k < j->n_crc; k++) {
		uint16_t c = 0;
		for (o = j->c_from[k]; o < j->c_at[k]; o++) c = crc16(c, rd(ctx, j->pre + o));
		wr(ctx, j->pre + j->c_at[k], (uint8_t)(c >> 8));
		wr(ctx, j->pre + j->c_at[k] + 1, (uint8_t)c);
	}
}

void zjump_span(const zjump_t *j, uint32_t *first, uint32_t *last) {
	uint32_t lo = 0xFFFFFFFFu, hi = 0;
	int k;
	for (k = 0; k < ZJUMP_BITS; k++) {
		if (j->b_off[k] < lo) lo = j->b_off[k];
		if (j->b_off[k] > hi) hi = j->b_off[k];
	}
	for (k = 0; k < j->n_crc; k++) {
		if (j->c_at[k] < lo) lo = j->c_at[k];
		if (j->c_at[k] + 1 > hi) hi = j->c_at[k] + 1;
	}
	*first = j->pre + lo;
	*last = j->pre + hi;
}
