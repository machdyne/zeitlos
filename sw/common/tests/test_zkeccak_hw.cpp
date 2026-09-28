/*
 * sw/common/zkeccak.c against the REAL rtl/keccak.v, under Verilator.
 * docs/keccak_hw.md, "Testing".
 *
 *   make -C sw/common/tests -f Makefile.zkeccakhw
 *
 * Random states through the block against the software permutation; all
 * of mlkem_vectors.txt (NIST's FIPS 203 vectors and kyber-py's) with
 * every permutation in the RTL; after every call the block released and
 * its state cleared; and the block held by someone else.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "Vkeccak.h"
#include "verilated.h"

extern "C" {
#include "../zkeccak.h"
#include "../zmlkem.h"
uint32_t zkeccak_mm_rd(uint32_t word);
void zkeccak_mm_wr(uint32_t word, uint32_t v);
uint32_t zkeccak_owner_id(void);
}

static Vkeccak *top;
static uint64_t cycles;
static long perms_hw;
static void tick() { top->clk = 0; top->eval(); top->clk = 1; top->eval(); cycles++; }

static uint32_t bus(uint32_t word, int we, uint32_t v) {
	top->wb_adr_i = word; top->wb_dat_i = v; top->wb_we_i = we;
	top->wb_stb_i = 1; top->wb_cyc_i = 1; top->wb_sel_i = 0xF;
	int guard = 0;
	do { tick(); } while (!top->wb_ack_o && ++guard < 50);
	if (guard >= 50) { printf("FAIL: bus access to %u never acked\n", word); exit(1); }
	uint32_t r = top->wb_dat_o;
	top->wb_stb_i = 0; top->wb_cyc_i = 0; top->wb_we_i = 0;
	tick();
	return r;
}
extern "C" uint32_t zkeccak_mm_rd(uint32_t w) { return bus(w, 0, 0); }
extern "C" void zkeccak_mm_wr(uint32_t w, uint32_t v) { if (w == 1 && v == 1) perms_hw++; bus(w, 1, v); }
extern "C" uint32_t zkeccak_owner_id(void) { return 42; }

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// released, and nothing left in it: fifty OUT reads, all zero
static bool clean() {
	if (zkeccak_mm_rd(3) != 0) return false;
	for (int i = 0; i < 50; i++) if (zkeccak_mm_rd(6) != 0) return false;
	return true;
}

static int unhex(const char *h, uint8_t *b, int cap) {
	int n = 0;
	for (; h[0] && h[1] && n < cap; h += 2) { unsigned v; sscanf(h, "%2x", &v); b[n++] = (uint8_t)v; }
	return n;
}

int main(int argc, char **argv) {
	Verilated::commandArgs(argc, argv);
	top = new Vkeccak;
	top->resetn = 0; tick(); tick(); top->resetn = 1; tick();
	srand(20260927);

	// -- random states: the block against software --
	{
		int bad = 0, hw = 0;
		uint64_t c0 = cycles;
		for (int t = 0; t < 200; t++) {
			uint64_t a[25], b[25];
			for (int i = 0; i < 25; i++) a[i] = ((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ (uint64_t)rand();
			memcpy(b, a, sizeof(a));
			zkeccak_no_hw = 0; zkeccak_f1600(a); hw += zkeccak_last_hw;
			zkeccak_no_hw = 1; zkeccak_f1600(b);
			if (memcmp(a, b, sizeof(a))) bad++;
		}
		zkeccak_no_hw = 0;
		CK(bad == 0 && hw == 200, "200 random states: %d differ from software, %d on the block", bad, hw);
		printf("  a permutation through the block: %llu bus cycles, transfers included\n", (unsigned long long)((cycles - c0) / 200));
	}
	CK(clean(), "after a permutation: released, and the state cleared");

	// -- ML-KEM-768, every permutation in the RTL --
	{
		static char line[20000], src[16], kind[8], a[8000], b[8000], c[8000], d[8000];
		static uint8_t x1[4000], x2[4000], x3[4000], x4[4000], o1[4000], o2[4000];
		FILE *f = fopen("mlkem_vectors.txt", "r");
		int n = 0, bad = 0;
		perms_hw = 0;
		if (!f || !fgets(line, sizeof(line), f)) { printf("no mlkem_vectors.txt\n"); return 1; }
		n = atoi(line);
		for (int i = 0; i < n; i++) {
			if (!fgets(line, sizeof(line), f)) break;
			a[0] = b[0] = c[0] = d[0] = 0;
			sscanf(line, "%15s %7s %7999s %7999s %7999s %7999s", src, kind, a, b, c, d);
			bool ok = true;
			if (!strcmp(kind, "kg")) {
				uint8_t coins[64];
				unhex(a, coins, 32); unhex(b, coins + 32, 32); unhex(c, x3, 4000); unhex(d, x4, 4000);
				zmlkem_keypair(o1, o2, coins);
				ok = !memcmp(o1, x3, ZMLKEM_EK_BYTES) && !memcmp(o2, x4, ZMLKEM_DK_BYTES);
			} else if (!strcmp(kind, "enc")) {
				unhex(a, x1, 4000); unhex(b, x2, 32); unhex(c, x3, 4000); unhex(d, x4, 32);
				ok = zmlkem_encaps(o1, o2, x1, x2) && !memcmp(o1, x3, ZMLKEM_CT_BYTES) && !memcmp(o2, x4, 32);
			} else if (!strcmp(kind, "dec")) {
				unhex(a, x1, 4000); unhex(b, x2, 4000); unhex(c, x3, 32);
				ok = zmlkem_decaps(o1, x2, x1) && !memcmp(o1, x3, 32);
			} else if (!strcmp(kind, "ekc")) {
				unhex(a, x1, 4000); ok = zmlkem_ek_ok(x1) == (atoi(b) == 1);
			} else if (!strcmp(kind, "dkc")) {
				unhex(a, x1, 4000); ok = zmlkem_dk_ok(x1) == (atoi(b) == 1);
			}
			if (!ok) bad++;
		}
		fclose(f);
		CK(bad == 0 && perms_hw > 10000, "ML-KEM-768: %d vectors (NIST's and kyber-py's), %d wrong, %ld permutations in the RTL", n, bad, perms_hw);
	}
	CK(clean(), "after ML-KEM: released, and the state cleared");

	// -- held by someone else --
	{
		uint64_t a[25], b[25];
		for (int i = 0; i < 25; i++) a[i] = b[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
		zkeccak_mm_wr(3, 77);
		zkeccak_f1600(a);
		int used = zkeccak_last_hw;
		zkeccak_no_hw = 1; zkeccak_f1600(b); zkeccak_no_hw = 0;
		CK(!used && !memcmp(a, b, sizeof(a)), "held by another: the block is not used, software answers, the same");
		CK(zkeccak_mm_rd(3) == 77, "and their claim untouched");
		zkeccak_mm_wr(3, 77u | 0x80000000u);
	}

	printf("zkeccak on the RTL: %d checks, %d failed\n", checks, fails);
	delete top;
	return fails != 0;
}
