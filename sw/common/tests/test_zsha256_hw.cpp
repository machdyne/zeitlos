/*
 * sw/common/zsha256.c's hardware path, run against the REAL rtl/sha256.v
 * under Verilator. docs/sha256_hw.md, "Testing".
 *
 *   make -C sw/common/tests sha256hw      (needs verilator)
 *
 * The driver is compiled twice: once with Z_SHA256_HW_TEST, its register
 * accesses going to this file's zsha_hw_rd()/zsha_hw_wr(), which drive the
 * block's bus a clock at a time; and once plain, as the reference. Every
 * digest from the first must equal the second's (and the published ones).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "Vsha256.h"
#include "verilated.h"

extern "C" {
#include "../zsha256.h"
// the hardware build, renamed by the Makefile
void hw_sha256_init(z_sha256_ctx *ctx);
void hw_sha256_update(z_sha256_ctx *ctx, const void *data, uint32_t len);
void hw_sha256_final(z_sha256_ctx *ctx, uint8_t out[32]);
uint32_t zsha_hw_rd(uint32_t word);
void zsha_hw_wr(uint32_t word, uint32_t v);
uint32_t zsha_hw_owner_id(void);
}

static Vsha256 *top;
static uint64_t cycles;
static int starts;

static void tick() {
	top->clk = 0; top->eval();
	top->clk = 1; top->eval();
	cycles++;
}

static uint32_t bus(uint32_t word, int we, uint32_t v) {
	top->wb_adr_i = word; top->wb_dat_i = v; top->wb_we_i = we;
	top->wb_stb_i = 1; top->wb_cyc_i = 1; top->wb_sel_i = 0xF;
	int guard = 0;
	do { tick(); } while (!top->wb_ack_o && ++guard < 50);
	if (guard >= 50) { printf("FAIL: bus access to %u never acked\n", word); exit(1); }
	uint32_t r = top->wb_dat_o;
	top->wb_stb_i = 0; top->wb_cyc_i = 0; top->wb_we_i = 0;	// on the ack, as the CPU's bus does
	tick();
	return r;
}

extern "C" uint32_t zsha_hw_rd(uint32_t w) { return bus(w, 0, 0); }
extern "C" void zsha_hw_wr(uint32_t w, uint32_t v) { if (w == 1 && (v & 1)) starts++; bus(w, 1, v); }
static uint32_t owner_id = 42;
extern "C" uint32_t zsha_hw_owner_id(void) { return owner_id; }

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const uint8_t *d, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); }

// the whole message through each build, the hardware one in `chunks`
static void both(const uint8_t *m, uint32_t len, uint32_t chunk, uint8_t hw[32], uint8_t ref[32]) {
	z_sha256_ctx a, b;
	hw_sha256_init(&a);
	for (uint32_t o = 0; o < len; ) {
		uint32_t n = len - o < chunk ? len - o : chunk;
		hw_sha256_update(&a, m + o, n);
		o += n;
	}
	hw_sha256_final(&a, hw);
	z_sha256_init(&b);
	z_sha256_update(&b, m, len);
	z_sha256_final(&b, ref);
}

static bool block_clean() {
	if (zsha_hw_rd(3) != 0) return false;			// released
	for (int i = 0; i < 8; i++) if (zsha_hw_rd(8 + i) != 0) return false;   // H zeroed
	return true;
}

int main(int argc, char **argv) {
	Verilated::commandArgs(argc, argv);
	top = new Vsha256;
	top->resetn = 0; tick(); tick(); top->resetn = 1; tick();

	uint8_t hw[32], ref[32];
	char hs[65];
	static uint8_t buf[70000];

	// -- published vectors --
	struct { const char *m; const char *d; } kv[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
	};
	for (auto &v : kv) {
		starts = 0;
		both((const uint8_t *)v.m, (uint32_t)strlen(v.m), 1000, hw, ref);
		hex(hw, hs);
		CK(!strcmp(hs, v.d) && starts > 0, "\"%.10s\": %s (%d blocks in hardware)", v.m, hs, starts);
	}
	CK(block_clean(), "after a hash: the block released, H zeroed");

	// a million 'a', 1,000 bytes at a time
	{
		z_sha256_ctx a;
		memset(buf, 'a', 1000);
		starts = 0;
		hw_sha256_init(&a);
		for (int i = 0; i < 1000; i++) hw_sha256_update(&a, buf, 1000);
		hw_sha256_final(&a, hw);
		hex(hw, hs);
		CK(!strcmp(hs, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") && starts == 15626,
			"a million 'a': %s, %d blocks in hardware", hs, starts);
	}

	// -- random messages, random chunks, misaligned --
	srand(20260927);
	int mism = 0;
	for (int t = 0; t < 300; t++) {
		uint32_t len = (uint32_t)(rand() % 700), off = (uint32_t)(rand() % 4), chunk = 1 + (uint32_t)(rand() % 150);
		for (uint32_t i = 0; i < len; i++) buf[off + i] = (uint8_t)rand();
		both(buf + off, len, chunk, hw, ref);
		if (memcmp(hw, ref, 32)) mism++;
	}
	CK(mism == 0, "300 random messages, lengths 0-699, chunks 1-150, offsets 0-3: %d differ", mism);
	CK(block_clean(), "and the block left clean");

	// -- the block held by someone else: software, and the claim untouched --
	zsha_hw_wr(3, 77);
	starts = 0;
	for (uint32_t i = 0; i < 300; i++) buf[i] = (uint8_t)(i * 7);
	both(buf, 300, 300, hw, ref);
	CK(!memcmp(hw, ref, 32) && starts == 0, "held by another: right answer, in software (%d blocks in hardware)", starts);
	CK(zsha_hw_rd(3) == 77, "and their claim untouched");
	zsha_hw_wr(3, 77u | 0x80000000u);

	// -- a claim lost between two calls (another process in between) --
	{
		z_sha256_ctx a;
		for (uint32_t i = 0; i < 256; i++) buf[i] = (uint8_t)(i ^ 0x5A);
		hw_sha256_init(&a);
		hw_sha256_update(&a, buf, 100);				// hardware: one block
		zsha_hw_wr(3, 77);						// someone takes it
		zsha_hw_wr(8, 0xDEADBEEF);				// and leaves junk in H
		hw_sha256_update(&a, buf + 100, 100);		// software now
		zsha_hw_wr(3, 77u | 0x80000000u);			// they give it back
		hw_sha256_update(&a, buf + 200, 56);		// hardware again
		hw_sha256_final(&a, hw);
		z_sha256(ref, buf, 256);
		CK(!memcmp(hw, ref, 32), "the block taken and junk left between two calls: still right");
	}

	// -- cost --
	{
		z_sha256_ctx a;
		memset(buf, 1, 64 * 64);
		hw_sha256_init(&a);
		uint64_t c0 = cycles;
		hw_sha256_update(&a, buf, 64 * 64);
		uint64_t c1 = cycles;
		printf("  64 blocks in one call: %llu bus cycles, %llu a block, claim and state traffic included\n",
			(unsigned long long)(c1 - c0), (unsigned long long)((c1 - c0) / 64));
	}

	printf("zsha256 on the RTL: %d checks, %d failed\n", checks, fails);
	delete top;
	return fails != 0;
}
