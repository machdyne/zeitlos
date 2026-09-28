/*
 * Host test for sw/apps/fed/core/fobj.c. docs/fed.md, "Testing".
 *
 *   make -C sw/apps/fed test
 *
 * fobj_vectors.txt: objects made by an INDEPENDENT implementation
 * (fobj_ref.py: Python's `cryptography` and `hashlib`, the rules written
 * from the spec), each rule broken on purpose, and random mutations --
 * every verdict, id, size and signature check must agree. Then objects
 * made here are written out for check_fobj.py to verify the other way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/fobj.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int unhex(const char *h, uint8_t *b) {
	int n = 0;
	for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); b[n++] = (uint8_t)v; }
	return n;
}

int main(void) {
	static char line[2 * FOBJ_MAX + 4096], hx[2 * FOBJ_MAX + 64], idh[80];
	static uint8_t buf[FOBJ_MAX + 64];
	FILE *f = fopen("fobj_vectors.txt", "r");
	if (!f) { printf("no fobj_vectors.txt\n"); return 1; }
	int n = 0, verdict_bad = 0, id_bad = 0, sig_bad = 0, kept = 0, signed_ok = 0;
	if (!fgets(line, sizeof(line), f)) return 1;
	n = atoi(line);
	for (int c = 0; c < n; c++) {
		int want, wsig; unsigned wsize; unsigned long long now;
		if (fscanf(f, "%d %d %u %79s %llu %s", &want, &wsig, &wsize, idh, &now, hx) != 6) { printf("short file\n"); return 1; }
		int len = unhex(hx, buf);
		fobj_t o;
		uint32_t where;
		int r = fobj_parse(buf, (uint32_t)len, now, &o, &where);
		if ((r == 0) != (want == 1)) {
			verdict_bad++;
			if (verdict_bad <= 5) printf("  case %d: fobj %s (%s at %u), reference %s\n", c, r ? "refuses" : "accepts",
				r ? fobj_strerror(r) : "-", where, want ? "accepts" : "refuses");
			continue;
		}
		if (r) continue;
		kept++;
		char got[65];
		fobj_hex(o.id, 32, got);
		if (strcmp(got, idh) || o.size != wsize) { id_bad++; if (id_bad <= 3) printf("  case %d: id or size differ\n", c); }
		int v = fobj_verify(&o) == 0;
		signed_ok += v;
		if (v != wsig) { sig_bad++; if (sig_bad <= 3) printf("  case %d: signature %d, reference %d\n", c, v, wsig); }
	}
	CK(verdict_bad == 0, "%d objects from the reference: %d verdicts differ", n, verdict_bad);
	CK(id_bad == 0, "%d kept the rules: %d ids or sizes differ", kept, id_bad);
	CK(sig_bad == 0, "%d signature checks (%d good): %d differ", kept, signed_ok, sig_bad);

	// -- made here: read back, and written out for the reference to check --
	{
		FILE *out = fopen("/tmp/fobj_c_made.txt", "w");
		uint8_t seed[32], sk[64], pk[32];
		int made = 0, back = 0;
		for (int i = 0; i < 12; i++) {
			fobj_t o;
			static uint8_t payload[FOBJ_PAYLOAD_MAX];
			memset(seed, i + 1, 32);
			crypto_ed25519_key_pair(sk, pk, seed);
			memset(&o, 0, sizeof(o));
			snprintf(o.topic, sizeof(o.topic), i % 2 ? "timeless/forum/general" : "fed/node/%02d", i);
			snprintf(o.type, sizeof(o.type), "bbs.post");
			o.format = (uint8_t)(1 + i % 3);
			o.kind = (uint8_t)(1 + i % 2);
			if (o.kind == FOBJ_STATE) snprintf(o.key, sizeof(o.key), "k%d", i);
			o.time = 1790505386ULL + (uint64_t)i;
			o.seq = (uint64_t)i + 1;
			o.len = (uint32_t)(i == 11 ? FOBJ_PAYLOAD_MAX : i * 37);
			for (uint32_t k = 0; k < o.len; k++) payload[k] = o.format == FOBJ_BYTES ? (uint8_t)(k * 7) : (uint8_t)('a' + k % 26);
			int sz = fobj_make(&o, payload, sk, buf, sizeof(buf));
			if (sz > 0) made++;
			fobj_t p;
			if (sz > 0 && fobj_parse(buf, (uint32_t)sz, 0, &p, NULL) == 0 && fobj_verify(&p) == 0 &&
					!memcmp(p.id, o.id, 32) && p.size == (uint32_t)sz && !memcmp(p.origin, pk, 32)) back++;
			char h[65];
			fobj_hex(o.id, 32, h);
			fprintf(out, "%s ", h);
			for (int k = 0; k < sz; k++) fprintf(out, "%02x", buf[k]);
			fprintf(out, "\n");
		}
		fclose(out);
		CK(made == 12 && back == 12, "made here: %d of 12 made, %d read back, checked, same id", made, back);
	}

	// -- fobj_make refuses what fobj_parse would --
	{
		uint8_t seed[32] = { 9 }, sk[64], pk[32];
		fobj_t o;
		crypto_ed25519_key_pair(sk, pk, seed);
		struct { const char *topic, *type, *key; int kind, format; unsigned long long seq; uint32_t len; int err; } bad[] = {
			{ "a//b", "t", "", FOBJ_LOG, FOBJ_TEXT, 1, 0, FOBJ_E_TOPIC },
			{ "a", "T", "", FOBJ_LOG, FOBJ_TEXT, 1, 0, FOBJ_E_TYPE },
			{ "a", "t", "", FOBJ_STATE, FOBJ_TEXT, 1, 0, FOBJ_E_KEY },
			{ "a", "t", "k", FOBJ_LOG, FOBJ_TEXT, 1, 0, FOBJ_E_KEY },
			{ "a", "t", "", FOBJ_LOG, FOBJ_TEXT, 0, 0, FOBJ_E_SEQ },
			{ "a", "t", "", FOBJ_LOG, 7, 1, 0, FOBJ_E_FORMAT },
			{ "a", "t", "", FOBJ_LOG, FOBJ_TEXT, 1, FOBJ_PAYLOAD_MAX + 1, FOBJ_E_LEN },
		};
		int right = 0;
		for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			static uint8_t pl[FOBJ_PAYLOAD_MAX + 1];
			memset(&o, 0, sizeof(o));
			snprintf(o.topic, sizeof(o.topic), "%s", bad[i].topic);
			snprintf(o.type, sizeof(o.type), "%s", bad[i].type);
			snprintf(o.key, sizeof(o.key), "%s", bad[i].key);
			o.kind = (uint8_t)bad[i].kind; o.format = (uint8_t)bad[i].format; o.seq = bad[i].seq; o.len = bad[i].len;
			if (fobj_make(&o, pl, sk, buf, sizeof(buf)) == -bad[i].err) right++;
		}
		CK(right == (int)(sizeof(bad) / sizeof(bad[0])), "fobj_make refuses bad fields, with the right error (%d)", right);
		char sid[17];
		fobj_short_id(pk, sid);
		CK(strlen(sid) == 16, "a short id: 16 hex digits (%s)", sid);
	}

	printf("fobj: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
