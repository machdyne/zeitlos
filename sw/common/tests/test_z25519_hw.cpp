/*
 * sw/common/z25519.c against the REAL rtl/montmul.v (register file on),
 * under Verilator, with Monocypher as the oracle for every result.
 * docs/z25519.md, "Testing".
 *
 *   make -C sw/common/tests -f Makefile.z25519hw
 *
 * X25519: RFC 7748's vectors, random scalars and points, and the u
 * values that matter (0, 1, p - 1, p, p + 1, 2^255 - 1, the top bit set)
 * -- every output byte-equal to crypto_x25519()'s.
 * Ed25519: random signatures from Monocypher, tampered copies, and every
 * edge case in ed25519_edge.h -- every verdict equal to
 * crypto_ed25519_check()'s. Nodes with the block and without it must
 * agree on what is valid.
 * Also: the block held by another (Monocypher answers; their claim
 * untouched), and after every call the block released and every
 * register, and the classic result, zeroed.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "Vmontmul.h"
#include "verilated.h"

extern "C" {
#include "../z25519.h"
#include "../../ext/monocypher/monocypher.h"
#include "../../ext/monocypher/monocypher-ed25519.h"
uint32_t z25519_mm_rd(uint32_t word);
void z25519_mm_wr(uint32_t word, uint32_t v);
uint32_t z25519_owner_id(void);
}
#include "ed25519_edge.h"

static Vmontmul *top;
static uint64_t cycles;

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
extern "C" uint32_t z25519_mm_rd(uint32_t w) { return bus(w, 0, 0); }
extern "C" void z25519_mm_wr(uint32_t w, uint32_t v) { bus(w, 1, v); }
extern "C" uint32_t z25519_owner_id(void) { return 42; }

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void rnd(uint8_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)rand(); }
static void hexb(const char *h, uint8_t *b) { for (int i = 0; h[2*i]; i++) { unsigned v; sscanf(h + 2*i, "%2x", &v); b[i] = (uint8_t)v; } }

// released, and nothing left in the block
static bool block_clean() {
	if (z25519_mm_rd(4) != 0) return false;
	for (int r = 0; r < 16; r++) {
		z25519_mm_wr(5, (uint32_t)r << 4);
		for (int i = 0; i < 12; i++) if (z25519_mm_rd(6) != 0) return false;
	}
	for (int i = 0; i < 12; i++) if (z25519_mm_rd(52 + i) != 0) return false;
	return true;
}

int main(int argc, char **argv) {
	Verilated::commandArgs(argc, argv);
	top = new Vmontmul;
	top->resetn = 0; tick(); tick(); top->resetn = 1; tick();
	srand(20260927);

	uint8_t k[32], u[32], out[32], ref[32];
	int hw_calls = 0;

	// -- X25519: RFC 7748, section 5.2 --
	struct { const char *k, *u, *o; } rv[] = {
		{ "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
		  "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
		  "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552" },
		{ "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
		  "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
		  "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957" },
	};
	for (auto &v : rv) {
		hexb(v.k, k); hexb(v.u, u); hexb(v.o, ref);
		uint64_t c0 = cycles;
		z_x25519(out, k, u);
		CK(z25519_last_hw && !memcmp(out, ref, 32), "RFC 7748 vector (%s)", z25519_last_hw ? "on the block" : "NOT on the block");
		static bool shown;
		if (!shown) { printf("  X25519 on the block: %llu bus cycles\n", (unsigned long long)(cycles - c0)); shown = true; }
	}
	CK(block_clean(), "after X25519: released, every register and R zeroed");

	// -- X25519: random, and the u values that matter --
	{
		int bad = 0, n = 0;
		uint8_t special[8][32];
		memset(special, 0, sizeof(special));
		special[1][0] = 1;						// 1
		memset(special[2], 0xff, 32); special[2][0] = 0xec; special[2][31] = 0x7f;	// p - 1
		memset(special[3], 0xff, 32); special[3][0] = 0xed; special[3][31] = 0x7f;	// p
		memset(special[4], 0xff, 32); special[4][0] = 0xee; special[4][31] = 0x7f;	// p + 1
		memset(special[5], 0xff, 32);			// 2^256 - 1: top bit dropped, then >= p
		rnd(special[6], 32); special[6][31] |= 0x80;	// top bit set
		rnd(special[7], 32);
		for (int i = 0; i < 8; i++) {
			for (int j = 0; j < 3; j++) {
				rnd(k, 32);
				z_x25519(out, k, special[i]); hw_calls += z25519_last_hw;
				crypto_x25519(ref, k, special[i]);
				n++; if (memcmp(out, ref, 32)) bad++;
			}
		}
		for (int i = 0; i < 12; i++) {
			rnd(k, 32); rnd(u, 32);
			z_x25519(out, k, u); hw_calls += z25519_last_hw;
			crypto_x25519(ref, k, u);
			n++; if (memcmp(out, ref, 32)) bad++;
		}
		CK(bad == 0 && hw_calls == n, "X25519: %d cases (random, 0, 1, p-1, p, p+1, 2^256-1, top bit), %d differ from Monocypher, %d on the block", n, bad, hw_calls);
	}

	// -- public keys: X25519(secret, 9), as Monocypher's --
	{
		int bad = 0;
		for (int i = 0; i < 6; i++) {
			rnd(k, 32);
			z_x25519_public_key(out, k);
			crypto_x25519_public_key(ref, k);
			if (memcmp(out, ref, 32) || !z25519_last_hw) bad++;
		}
		CK(bad == 0, "z_x25519_public_key: 6 secrets, %d differ from crypto_x25519_public_key or missed the block", bad);
	}

	// -- Ed25519: RFC 8032 test 1 --
	{
		uint8_t seed[32], sk[64], pk[32], sig[64], want[64];
		hexb("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
		crypto_ed25519_key_pair(sk, pk, seed);
		crypto_ed25519_sign(sig, sk, NULL, 0);
		hexb("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", want);
		uint64_t c0 = cycles;
		int r = z_ed25519_check(sig, pk, NULL, 0);
		printf("  an Ed25519 check on the block: %llu bus cycles\n", (unsigned long long)(cycles - c0));
		CK(!memcmp(sig, want, 64) && r == 0 && z25519_last_hw, "RFC 8032 test 1 verifies, on the block");
		sig[3] ^= 4;
		CK(z_ed25519_check(sig, pk, NULL, 0) == -1, "and a bit flipped in R does not");
	}
	CK(block_clean(), "after a check: released, every register and R zeroed");

	// -- Ed25519: random signatures, and tampered ones --
	{
		int bad = 0, n = 0, acc = 0;
		for (int t = 0; t < 12; t++) {
			uint8_t seed[32], sk[64], pk[32], sig[64], msg[64];
			rnd(seed, 32); rnd(msg, 64);
			size_t len = (size_t)(rand() % 65);
			crypto_ed25519_key_pair(sk, pk, seed);
			crypto_ed25519_sign(sig, sk, msg, len);
			for (int v = 0; v < 3; v++) {
				uint8_t s2[64]; memcpy(s2, sig, 64);
				uint8_t p2[32]; memcpy(p2, pk, 32);
				if (v == 1) s2[32 + rand() % 32] ^= (uint8_t)(1 << (rand() % 8));
				if (v == 2) p2[rand() % 32] ^= (uint8_t)(1 << (rand() % 8));
				int a = z_ed25519_check(s2, p2, msg, len), b = crypto_ed25519_check(s2, p2, msg, len);
				n++; if (a != b) bad++; if (a == 0) acc++;
			}
		}
		CK(bad == 0 && acc >= 12, "Ed25519: %d random and tampered, %d disagree with Monocypher (%d accepted)", n, bad, acc);
	}

	// -- Ed25519: the edge cases --
	{
		int bad = 0, n = (int)(sizeof(edge) / sizeof(edge[0])), acc = 0, onblock = 0;
		for (int i = 0; i < n; i++) {
			int a = z_ed25519_check(edge[i].sig, edge[i].pk, edge[i].msg, edge[i].len);
			onblock += z25519_last_hw;
			int b = crypto_ed25519_check(edge[i].sig, edge[i].pk, edge[i].msg, edge[i].len);
			if (a != b) { bad++; if (bad < 6) printf("  edge %d: block %d, Monocypher %d\n", i, a, b); }
			if (b == 0) acc++;
		}
		CK(bad == 0 && onblock == n, "Ed25519 edge cases: %d (small-order points, non-canonical encodings, s >= L), %d disagree; Monocypher accepts %d", n, bad, acc);
	}

	// -- the block held by someone else --
	{
		z25519_mm_wr(4, 77);
		rnd(k, 32); rnd(u, 32);
		z_x25519(out, k, u);
		crypto_x25519(ref, k, u);
		CK(!z25519_last_hw && !memcmp(out, ref, 32), "held by another: Monocypher answers");
		CK(z25519_mm_rd(4) == 77, "and their claim untouched");
		z25519_mm_wr(4, 77u | 0x80000000u);
	}

	printf("z25519 on the RTL: %d checks, %d failed\n", checks, fails);
	delete top;
	return fails != 0;
}
