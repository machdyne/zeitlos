/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See authkeys.h.
 */
#include <string.h>
#include "authkeys.h"
#include "../ssh/ssh_wire.h"
#include "../ssh/ssh_crypto.h"
#include "../../../common/zsha256.h"
#include "../../../ext/monocypher/monocypher.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"
#include "../../web/rsa.h"

#define RSA_MIN_BITS 2048
#define RSA_MAX_BITS 4096

// -- base64, standard alphabet, padded or not --

static int b64v(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

// Decodes `n` characters into out (at most cap bytes). -1 on anything
// that is not base64, or too long.
static int b64_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap) {
	uint32_t acc = 0, bits = 0, k = 0;
	while (n && in[n - 1] == '=') n--;
	for (uint32_t i = 0; i < n; i++) {
		int v = b64v(in[i]);
		if (v < 0) return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (k >= cap) return -1;
			out[k++] = (uint8_t)(acc >> bits);
		}
	}
	return (int)k;
}

static bool hex_decode(const char *in, uint32_t n, uint8_t *out) {
	for (uint32_t i = 0; i < n; i++) {
		char c = in[i];
		int v = (c >= '0' && c <= '9') ? c - '0' : ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') ? (c | 0x20) - 'a' + 10 : -1;
		if (v < 0) return false;
		if (i & 1) out[i / 2] |= (uint8_t)v; else out[i / 2] = (uint8_t)(v << 4);
	}
	return true;
}

// -- key blobs --

// An mpint's magnitude: its bytes without leading zeros.
static const uint8_t *mpint_mag(const uint8_t *p, uint32_t *n) {
	while (*n && *p == 0) { p++; (*n)--; }
	return p;
}

static uint32_t bits_of(const uint8_t *p, uint32_t n) {
	if (!n) return 0;
	uint32_t b = (n - 1) * 8;
	for (uint8_t top = p[0]; top; top >>= 1) b++;
	return b;
}

int authkeys_blob_type(const uint8_t *blob, uint32_t len, uint32_t *bits) {
	ssh_rd r;
	const uint8_t *t, *a, *b;
	uint32_t tn, an, bn;
	ssh_rd_init(&r, blob, len);
	t = ssh_rd_string(&r, &tn);
	if (r.bad) return -1;
	if (ssh_str_eq(t, tn, "ssh-ed25519")) {
		a = ssh_rd_string(&r, &an);
		return (!r.bad && an == 32 && !ssh_rd_left(&r)) ? AK_ED25519 : -1;
	}
	if (ssh_str_eq(t, tn, "ssh-rsa")) {
		a = ssh_rd_string(&r, &an);         // e
		b = ssh_rd_string(&r, &bn);         // n
		if (r.bad || ssh_rd_left(&r)) return -1;
		(void)mpint_mag(a, &an);
		b = mpint_mag(b, &bn);
		if (!an) return -1;
		if (bits) *bits = bits_of(b, bn);
		return AK_RSA;
	}
	return -1;
}

void authkeys_fingerprint(char *out, uint32_t cap, const uint8_t *blob, uint32_t len) {
	ssh_fingerprint(out, cap, blob, len);
}

static void fp_of(uint8_t fp[32], const uint8_t *blob, uint32_t len) {
	z_sha256_ctx c;
	z_sha256_init(&c);
	z_sha256_update(&c, blob, len);
	z_sha256_final(&c, fp);
}

// -- the file --

static bool is_space(char c) { return c == ' ' || c == '\t'; }

void authkeys_parse(authkeys_t *ak, const char *text, uint32_t len,
		void (*warn)(int line, const char *why)) {
	uint32_t i = 0;
	int line = 0;
	memset(ak, 0, sizeof(*ak));

	while (i < len) {
		uint32_t start = i, end;
		while (i < len && text[i] != '\n') i++;
		end = i;
		if (i < len) i++;
		line++;
		while (end > start && (text[end - 1] == '\r' || is_space(text[end - 1]))) end--;
		while (start < end && is_space(text[start])) start++;
		if (start == end || text[start] == '#') continue;

		// the first word
		uint32_t w1 = start;
		while (w1 < end && !is_space(text[w1])) w1++;
		const char *word = text + start;
		uint32_t wn = w1 - start;
		ak_entry_t ent;
		memset(&ent, 0, sizeof(ent));
		const char *why = NULL;

		if (wn > 7 && !memcmp(word, "SHA256:", 7)) {
			uint8_t fp[33];
			if (b64_decode(word + 7, wn - 7, fp, sizeof(fp)) != 32) why = "a SHA256: fingerprint is 43 base64 characters";
			else { memcpy(ent.fp, fp, 32); ent.type = AK_ANY; }
		} else if (wn == 64 && hex_decode(word, 64, ent.fp)) {
			ent.type = AK_ANY;
		} else if ((wn == 11 && !memcmp(word, "ssh-ed25519", 11)) || (wn == 7 && !memcmp(word, "ssh-rsa", 7))) {
			uint32_t b = w1, e2;
			static uint8_t blob[640];
			uint32_t bits = 0;
			while (b < end && is_space(text[b])) b++;
			e2 = b;
			while (e2 < end && !is_space(text[e2])) e2++;
			int bl = b64_decode(text + b, e2 - b, blob, sizeof(blob));
			int type = bl > 0 ? authkeys_blob_type(blob, (uint32_t)bl, &bits) : -1;
			if (bl <= 0 || type < 0) why = "the key is not valid base64 of an ssh-ed25519 or ssh-rsa key";
			else if ((type == AK_ED25519) != (wn == 11)) why = "the key's type does not match the line's";
			else if (type == AK_RSA && (bits < RSA_MIN_BITS || bits > RSA_MAX_BITS))
				why = "RSA keys must be 2048 to 4096 bits";
			else { fp_of(ent.fp, blob, (uint32_t)bl); ent.type = (uint8_t)type; }
		} else {
			// anything else before the key -- OpenSSH's options
			why = "options (from=, command=, ...) are not supported: the line is ignored";
		}

		if (why) {
			ak->refused++;
			if (warn) warn(line, why);
			continue;
		}
		if (ak->n >= AK_MAX) {
			ak->refused++;
			if (warn) warn(line, "more than 32 keys: the rest are ignored");
			continue;
		}
		ak->e[ak->n++] = ent;
	}
}

bool authkeys_allows(const authkeys_t *ak, const char *alg, const uint8_t *blob, uint32_t len) {
	uint32_t bits = 0;
	int type = authkeys_blob_type(blob, len, &bits);
	uint8_t fp[32];

	if (!strcmp(alg, "ssh-ed25519")) { if (type != AK_ED25519) return false; }
	else if (!strcmp(alg, "rsa-sha2-256")) {
		if (type != AK_RSA || bits < RSA_MIN_BITS || bits > RSA_MAX_BITS) return false;
	} else return false;

	fp_of(fp, blob, len);
	for (int i = 0; i < ak->n; i++)
		if ((ak->e[i].type == AK_ANY || ak->e[i].type == type) && crypto_verify32(ak->e[i].fp, fp) == 0)
			return true;
	return false;
}

bool authkeys_verify(const char *alg, const uint8_t *blob, uint32_t blob_len,
		const uint8_t *sig, uint32_t sig_len, const uint8_t *data, uint32_t data_len) {
	ssh_rd kr, sr;
	const uint8_t *st, *sv, *kt, *k1, *k2;
	uint32_t stn, svn, ktn, k1n, k2n;

	// the signature: string alg, string value -- and the alg must be
	// the one the request named
	ssh_rd_init(&sr, sig, sig_len);
	st = ssh_rd_string(&sr, &stn);
	sv = ssh_rd_string(&sr, &svn);
	if (sr.bad || !ssh_str_eq(st, stn, alg)) return false;

	ssh_rd_init(&kr, blob, blob_len);
	kt = ssh_rd_string(&kr, &ktn);
	k1 = ssh_rd_string(&kr, &k1n);

	if (!strcmp(alg, "ssh-ed25519")) {
		if (kr.bad || !ssh_str_eq(kt, ktn, "ssh-ed25519") || k1n != 32 || svn != 64) return false;
		return crypto_ed25519_check(sv, k1, data, data_len) == 0;
	}

	if (!strcmp(alg, "rsa-sha2-256")) {
		// blob: string "ssh-rsa", mpint e, mpint n
		static uint8_t s_pad[RSA_MAX_BITS / 8];
		uint8_t h[32];
		k2 = ssh_rd_string(&kr, &k2n);
		if (kr.bad || !ssh_str_eq(kt, ktn, "ssh-rsa")) return false;
		k1 = mpint_mag(k1, &k1n);
		k2 = mpint_mag(k2, &k2n);
		uint32_t bits = bits_of(k2, k2n);
		if (bits < RSA_MIN_BITS || bits > RSA_MAX_BITS || !k1n) return false;
		// The signature is the modulus's length (RFC 8332 3); a signer
		// that dropped leading zeros is padded back, as OpenSSH does.
		sv = mpint_mag(sv, &svn);
		if (svn > k2n) return false;
		memset(s_pad, 0, k2n - svn);
		memcpy(s_pad + (k2n - svn), sv, svn);
		z_sha256_ctx c;
		z_sha256_init(&c);
		z_sha256_update(&c, data, data_len);
		z_sha256_final(&c, h);
		return rsa_verify_pkcs1_sha256(k2, k2n, k1, k1n, s_pad, k2n, h);
	}
	return false;
}
