/*
 * rtl/tb/cache_soc_lz4: the OS's own LZ4 decoder (sw/common/zexec_lz4.c,
 * what unpacks every compressed executable) on the cache SoC testbench.
 * The data is real RV32IM code (blob.h, gen.py). Unpacks it several
 * times -- data cache on and off, posted writes on and off, an I-cache
 * flush between -- and checks every byte. DONE gets the FNV-1a of the
 * last unpack; FAIL is set on any difference.
 */
#include <stdint.h>
#include "../../../sw/common/zexec_lz4.c"
#include "blob.h"

#define UART  (*(volatile uint32_t *)0xf0000000)
#define FAIL  (*(volatile uint32_t *)0xf000000c)
#define I_CTRL (*(volatile uint32_t *)0x70000100)
#define D_CTRL (*(volatile uint32_t *)0x70000110)
#define D_INFO (*(volatile uint32_t *)0x7000011c)

static void putc_(char c) { UART = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) {
	for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]);
}

typedef struct { const uint8_t *p; uint32_t left; uint32_t chunk; } src_t;

static uint32_t fill(void *ctx, uint8_t *buf, uint32_t cap) {
	src_t *s = (src_t *)ctx;
	uint32_t n = s->left < cap ? s->left : cap;
	if (n > s->chunk) n = s->chunk;
	for (uint32_t i = 0; i < n; i++) buf[i] = s->p[i];
	s->p += n;
	s->left -= n;
	return n;
}

static z_exec_lz4_t dec;
static uint8_t out[ORIG_LEN + 64] __attribute__((aligned(4)));

static uint32_t fnv(const uint8_t *p, uint32_t n) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

int main(void) {
	int dcache = ((D_INFO >> 16) == 0x1DCA);
	static const uint32_t dctrl[4] = { 0x5, 0x1, 0x0, 0x5 };
	static const uint32_t chunks[4] = { 256, 37, 256, 7 };
	uint32_t h = 0;
	for (int r = 0; r < 4; r++) {
		if (dcache) D_CTRL = dctrl[r];
		for (uint32_t i = 0; i < ORIG_LEN + 64; i++) out[i] = (uint8_t)(0xa5 ^ i);
		src_t s = { comp, COMP_LEN, chunks[r] };
		int rv = z_exec_lz4(&dec, out, ORIG_LEN, fill, &s);
		uint32_t bad = 0, first = 0;
		for (uint32_t i = 0; i < ORIG_LEN; i++)
			if (out[i] != orig[i]) { if (!bad) first = i; bad++; }
		h = fnv(out, ORIG_LEN);
		puts_("lz4 round "); putc_('0' + r); puts_(": rv "); puthex((uint32_t)rv);
		puts_(" bad "); puthex(bad);
		if (bad) { puts_(" first @"); puthex(first); puts_(" got "); puthex(out[first]); puts_(" want "); puthex(orig[first]); }
		puts_(" fnv "); puthex(h); putc_('\n');
		if (rv != 0 || bad || h != ORIG_FNV) FAIL = 10 + r;
		if (dcache) I_CTRL = 3;	/* flush, as the kernel does after a load */
	}
	return (int)h;
}
