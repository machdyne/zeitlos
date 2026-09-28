/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A zfed session. docs/fed.md, "Sessions, exactly"; the interface in
 * fsess.h.
 *
 *   I -> R  clear  "ZFED1" | I's Ed25519 key | I's X25519 key | I's ML-KEM-768 key
 *                                                               1253 bytes
 *   R -> I  clear  "ZFED1" | R's X25519 key | ML-KEM ciphertext  1125 bytes
 *           th = SHA-256("zfed handshake 1" | the 1253 | the 1125)
 *           keys: HKDF-SHA-256, salt th, secret X25519(ours, theirs) | the
 *           ML-KEM shared secret -- HYBRID: a recording of the session stays
 *           safe while EITHER holds, against a quantum computer included
 *           (docs/fed.md, "Quantum computers")
 *   R -> I  AUTH   R's Ed25519 key | Ed25519("zfed session\0" | th | "r")
 *   I -> R  AUTH   Ed25519("zfed session\0" | th | "i")
 *   both    HELLO  our epoch | the topics we want
 *           GET    after N  ->  OBJ pos | object ... END M
 *           BYE    once our GET has its END
 *
 * Frames: a 2-byte big-endian length L, then L bytes: ciphertext and a
 * 16-byte Poly1305 tag. XChaCha20-Poly1305, the key for that direction,
 * the nonce a 64-bit counter (little-endian, then zeros), the 2 length
 * bytes authenticated as associated data. The first byte of a frame's
 * plaintext is its type.
 *
 * The responder decides whether to talk to the initiator from its key
 * in the first 37 bytes, before reading the rest or doing any
 * cryptography: a stranger gets nothing back and costs nothing
 * (docs/fed.md, "The handshake").
 */
#include <string.h>
#include <stdio.h>
#include "fsess.h"
#include "fstore.h"
#include "../../../common/zplat.h"
#include "../../../common/zsha256.h"
#include "../../../common/z25519.h"
#include "../../../ext/monocypher/monocypher.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

enum { P_M0 = 0, P_M1, P_AUTH, P_RUN };						// phases
enum { T_AUTH = 1, T_HELLO, T_GET, T_OBJ, T_END, T_BYE };	// frame types

static const char magic[5] = { 'Z', 'F', 'E', 'D', '1' };

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void fail(fsess_t *s, const char *why) {
	if (s->state != FS_RUNNING) return;
	s->state = FS_FAILED;
	snprintf(s->error, sizeof(s->error), "%s", why);
	crypto_wipe(s->eph_sk, sizeof(s->eph_sk));
	crypto_wipe(s->kem_dk, sizeof(s->kem_dk));
	crypto_wipe(s->kem_ss, sizeof(s->kem_ss));
	crypto_wipe(s->key_in, sizeof(s->key_in));
	crypto_wipe(s->key_out, sizeof(s->key_out));
}

// -- HMAC-SHA-256 and the keys --

static void hmac(uint8_t out[32], const uint8_t *key, uint32_t klen, const uint8_t *m1, uint32_t l1,
	const uint8_t *m2, uint32_t l2) {
	uint8_t k[64], pad[64], inner[32];
	z_sha256_ctx c;
	memset(k, 0, sizeof(k));
	if (klen > 64) z_sha256(k, key, klen); else memcpy(k, key, klen);
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
	z_sha256_init(&c); z_sha256_update(&c, pad, 64);
	z_sha256_update(&c, m1, l1);
	if (l2) z_sha256_update(&c, m2, l2);
	z_sha256_final(&c, inner);
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
	z_sha256_init(&c); z_sha256_update(&c, pad, 64); z_sha256_update(&c, inner, 32); z_sha256_final(&c, out);
	crypto_wipe(k, sizeof(k));
}

// th, then the two keys. False: the shared secret is all zeros (a
// low-order point) -- no session.
static bool keys(fsess_t *s) {
	static const char label[] = "zfed handshake 1";
	uint8_t dh[64], prk[32], zero[32] = { 0 };
	z_sha256_ctx c;
	z_sha256_init(&c);
	z_sha256_update(&c, label, sizeof(label) - 1);
	z_sha256_update(&c, s->m0, sizeof(s->m0));
	z_sha256_update(&c, s->m1, sizeof(s->m1));
	z_sha256_final(&c, s->th);
	z_x25519(dh, s->eph_sk, s->peer_eph);
	crypto_wipe(s->eph_sk, sizeof(s->eph_sk));
	if (!crypto_verify32(dh, zero)) return false;
	memcpy(dh + 32, s->kem_ss, 32);							// hybrid: X25519 | ML-KEM
	crypto_wipe(s->kem_ss, sizeof(s->kem_ss));
	hmac(prk, s->th, 32, dh, 64, NULL, 0);						// HKDF-Extract
	crypto_wipe(dh, sizeof(dh));
	{
		uint8_t i2r[32], r2i[32];
		hmac(i2r, prk, 32, (const uint8_t *)"zfed i2r\x01", 9, NULL, 0);	// HKDF-Expand, one block
		hmac(r2i, prk, 32, (const uint8_t *)"zfed r2i\x01", 9, NULL, 0);
		memcpy(s->key_out, s->initiator ? i2r : r2i, 32);
		memcpy(s->key_in, s->initiator ? r2i : i2r, 32);
		crypto_wipe(i2r, 32); crypto_wipe(r2i, 32);
	}
	crypto_wipe(prk, sizeof(prk));
	return true;
}

// What each side signs: "zfed session\0" | th | role.
static void auth_msg(const fsess_t *s, uint8_t role, uint8_t m[46]) {
	memcpy(m, "zfed session", 13);			// with its NUL
	memcpy(m + 13, s->th, 32);
	m[45] = role;
}

// -- out: raw bytes and frames --

static uint8_t *out_room(fsess_t *s, uint32_t n) {
	if (s->out_head && s->out_head + s->out_len + n > sizeof(s->out)) {
		memmove(s->out, s->out + s->out_head, s->out_len);
		s->out_head = 0;
	}
	if (s->out_head + s->out_len + n > sizeof(s->out)) return NULL;
	return s->out + s->out_head + s->out_len;
}

static void out_raw(fsess_t *s, const void *d, uint32_t n) {
	uint8_t *p = out_room(s, n);
	if (!p) { fail(s, "output full"); return; }
	memcpy(p, d, n);
	s->out_len += n;
}

// The plaintext area of the next frame (cap bytes); frame_end() seals it.
static uint8_t *frame_begin(fsess_t *s, uint32_t cap) {
	uint8_t *p = out_room(s, 2 + cap + 16);
	return p ? p + 2 : NULL;
}

static void nonce(uint64_t n, uint8_t out[24]) {
	memset(out, 0, 24);
	for (int i = 0; i < 8; i++) out[i] = (uint8_t)(n >> (8 * i));
}

static void frame_end(fsess_t *s, uint32_t len) {
	uint8_t *p = s->out + s->out_head + s->out_len, nc[24];
	uint32_t l = len + 16;
	p[0] = (uint8_t)(l >> 8); p[1] = (uint8_t)l;
	nonce(s->n_out++, nc);
	crypto_aead_lock(p + 2, p + 2 + len, s->key_out, nc, p, 2, p + 2, len);	// in place
	s->out_len += 2 + l;
}

static void send_small(fsess_t *s, uint8_t type, const uint8_t *body, uint32_t n) {
	uint8_t *f = frame_begin(s, 1 + n);
	if (!f) { fail(s, "output full"); return; }
	f[0] = type;
	if (n) memcpy(f + 1, body, n);
	frame_end(s, 1 + n);
}

static void send_hello(fsess_t *s) {
	uint32_t wl = (uint32_t)strlen(s->cfg.wants);
	uint8_t *f = frame_begin(s, 1 + 8 + wl);
	uint64_t e = fstore_epoch();
	if (!f) { fail(s, "output full"); return; }
	f[0] = T_HELLO;
	for (int i = 0; i < 8; i++) f[1 + i] = (uint8_t)(e >> (8 * i));
	memcpy(f + 9, s->cfg.wants, wl);
	frame_end(s, 9 + wl);
	s->hello_sent = true;
}

// -- the patterns --

bool fsess_wanted(const char *patterns, const char *topic) {
	size_t tl = strlen(topic);
	for (const char *p = patterns; *p; ) {
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l >= 2 && p[l - 1] == '*' && p[l - 2] == '/') {
			if (tl > l - 1 && !memcmp(topic, p, l - 1)) return true;		// "a/b/*": below a/b
		} else if (l == tl && !memcmp(topic, p, l)) return true;
		p += l + (e ? 1 : 0);
	}
	return false;
}

// -- serving the peer's GET: an OBJ at a time, while there is room --

// A node list: exempt from a link's limit -- it is what membership rests on.
static bool is_list(const fobj_t *o) {
	size_t n = strlen(o->topic);
	return !strcmp(o->type, "fed.nodes") && n > 6 && !strcmp(o->topic + n - 6, "/nodes");
}

static void serve(fsess_t *s) {
	for (int budget = 64; s->serving && budget > 0 && s->state == FS_RUNNING; budget--) {
		if (s->out_len > 1024) return;					// let it drain first
		uint32_t next = fstore_next_for(s->serve_pos, s->peer_slot);	// not what it sent us
		if (!next || next > s->serve_end) {
			uint8_t m[4];
			put32(m, s->serve_end);
			send_small(s, T_END, m, 4);
			s->serving = false;
			s->end_sent = true;
			return;
		}
		s->serve_pos = next;
		uint8_t *f = frame_begin(s, 5 + FOBJ_MAX);
		if (!f) { fail(s, "output full"); return; }
		int n = fstore_get(next, f + 5, FOBJ_MAX);
		if (n <= 0) continue;
		fobj_t o;
		if (fobj_parse(f + 5, (uint32_t)n, 0, &o, NULL) || !fsess_wanted(s->peer_wants, o.topic)) continue;
		if (s->cfg.max_object && (uint32_t)n > s->cfg.max_object && !is_list(&o)) { s->stats.withheld++; continue; }	// too big for this link
		f[0] = T_OBJ;
		put32(f + 1, next);
		frame_end(s, 5 + (uint32_t)n);
		s->stats.sent++;
	}
}

// -- what arrives --

static void got_obj(fsess_t *s, const uint8_t *b, uint32_t n) {
	fobj_t o;
	uint32_t pos;
	if (fobj_parse(b, n, s->cfg.now, &o, NULL) || o.size != n) { s->stats.got_rejected++; return; }
	if (!fsess_wanted(s->cfg.wants, o.topic)) { s->stats.got_rejected++; return; }
	if (s->cfg.max_object && n > s->cfg.max_object && !is_list(&o)) { s->stats.got_rejected++; return; }	// too big for this link
	if (s->cfg.origin_ok && !s->cfg.origin_ok(o.origin, o.topic, s->cfg.ctx)) { s->stats.got_rejected++; return; }
	if (fstore_have(o.id)) { s->stats.got_have++; return; }
	if (!s->cfg.trusted && fobj_verify(&o)) { s->stats.got_rejected++; return; }
	if (s->cfg.admit && !s->cfg.admit(&o, s->cfg.ctx)) { s->stats.got_rejected++; return; }
	int r = fstore_put_from(b, n, s->cfg.now, s->peer_slot, &pos);
	if (r == FSTORE_NEW) { s->stats.got_new++; if (s->cfg.stored) s->cfg.stored(&o, s->cfg.ctx); }
	else if (r > 0) s->stats.got_have++;
	else fail(s, "the store refused a write");
}

static void frame(fsess_t *s, uint8_t *p, uint32_t n) {
	if (n < 1) { fail(s, "an empty frame"); return; }
	uint8_t type = p[0];
	p++; n--;
	if (s->phase == P_AUTH) {
		uint8_t m[46];
		if (type != T_AUTH) { fail(s, "expected AUTH"); return; }
		if (s->initiator) {
			if (n != 96) { fail(s, "a bad AUTH"); return; }
			if (crypto_verify32(p, s->cfg.peer)) { fail(s, "not the peer we meant to reach"); return; }
			memcpy(s->peer_key, p, 32);
			auth_msg(s, 'r', m);
			if (z_ed25519_check(p + 32, s->peer_key, m, 46)) { fail(s, "the peer's signature does not check"); return; }
			uint8_t a[64];
			auth_msg(s, 'i', m);
			crypto_ed25519_sign(a, s->cfg.secret_key, m, 46);
			send_small(s, T_AUTH, a, 64);
		} else {
			if (n != 64) { fail(s, "a bad AUTH"); return; }
			auth_msg(s, 'i', m);
			if (z_ed25519_check(p, s->peer_key, m, 46)) { fail(s, "the peer's signature does not check"); return; }
		}
		s->phase = P_RUN;
		send_hello(s);
		return;
	}
	switch (type) {
	case T_HELLO: {
		if (n < 8 || n - 8 >= FSESS_WANTS_MAX) { fail(s, "a bad HELLO"); return; }
		uint64_t e = 0, ce; uint32_t cp;
		for (int i = 0; i < 8; i++) e |= (uint64_t)p[i] << (8 * i);
		s->peer_epoch = e;
		s->peer_slot = fstore_peer_slot(s->peer_key, e);
		memcpy(s->peer_wants, p + 8, n - 8);
		s->peer_wants[n - 8] = 0;
		// a cursor into a store that started again means nothing
		s->pull_from = (fstore_cursor(s->peer_key, &ce, &cp) && ce == e) ? cp : 0;
		uint8_t g[4];
		put32(g, s->pull_from);
		send_small(s, T_GET, g, 4);
		s->get_sent = true;
		return;
	}
	case T_GET:
		if (n != 4 || s->serving || s->end_sent) { fail(s, "a bad GET"); return; }
		s->serve_pos = get32(p);
		s->serve_end = fstore_last();
		s->serving = true;
		return;
	case T_OBJ:
		if (n < 4 || !s->get_sent || s->got_end) { fail(s, "a bad OBJ"); return; }
		got_obj(s, p + 4, n - 4);
		return;
	case T_END:
		if (n != 4 || !s->get_sent || s->got_end) { fail(s, "a bad END"); return; }
		s->got_end = true;
		// the cursor moves only once what we stored is safely written
		if (fstore_sync() < 0) { fail(s, "the store could not sync"); return; }
		if (fstore_set_cursor(s->peer_key, s->peer_epoch, get32(p)) < 0) { fail(s, "could not keep the cursor"); return; }
		send_small(s, T_BYE, NULL, 0);
		s->bye_sent = true;
		return;
	case T_BYE:
		if (n != 0 || !s->end_sent) { fail(s, "a bad BYE"); return; }
		s->got_bye = true;
		return;
	default:
		fail(s, "an unknown frame");
	}
}

static void check_done(fsess_t *s) {
	if (s->state == FS_RUNNING && s->bye_sent && s->got_bye && !s->serving && !s->out_len) {
		s->state = FS_DONE;
		crypto_wipe(s->key_in, sizeof(s->key_in));
		crypto_wipe(s->key_out, sizeof(s->key_out));
	}
}

// -- the interface --

void fsess_init(fsess_t *s, const fsess_cfg_t *cfg, bool initiator) {
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	if (!s->cfg.wants) s->cfg.wants = "";
	s->initiator = initiator;
	plat_random(s->eph_sk, 32);
	z_x25519_public_key(s->eph_pk, s->eph_sk);
	if (initiator) {
		uint8_t coins[64];
		if (!cfg->peer) { fail(s, "no peer key"); return; }
		memcpy(s->m0, magic, 5);
		memcpy(s->m0 + 5, cfg->secret_key + 32, 32);			// our public key: FIRST
		memcpy(s->m0 + 37, s->eph_pk, 32);
		plat_random(coins, sizeof(coins));
		zmlkem_keypair(s->m0 + 69, s->kem_dk, coins);
		crypto_wipe(coins, sizeof(coins));
		out_raw(s, s->m0, sizeof(s->m0));
		s->phase = P_M1;
	} else s->phase = P_M0;
}

void fsess_input(fsess_t *s, const uint8_t *d, uint32_t n) {
	s->last_in_ms = s->start_ms + 0;			// refreshed by fsess_poll's clock
	s->timed = false;
	while (n > 0 && s->state == FS_RUNNING) {
		if (s->phase == P_M0 || s->phase == P_M1) {
			uint32_t want = s->phase == P_M0 ? FSESS_M0 : FSESS_M1;
			// The responder decides on the key from the first 37 bytes --
			// before reading the rest, before any cryptography.
			if (s->phase == P_M0 && s->in_len < FSESS_M0_WHO) want = FSESS_M0_WHO;
			uint32_t k = want - s->in_len < n ? want - s->in_len : n;
			memcpy(s->in + s->in_len, d, k);
			s->in_len += k; d += k; n -= k;
			if (s->in_len < want) return;
			if (memcmp(s->in, magic, 5)) { fail(s, "not zfed"); return; }
			if (s->phase == P_M0 && s->in_len == FSESS_M0_WHO) {
				memcpy(s->peer_key, s->in + 5, 32);
				if (!s->cfg.allowed || !s->cfg.allowed(s->peer_key, s->cfg.ctx)) { fail(s, "not a node we talk to"); return; }
				continue;								// now the rest of it
			}
			s->in_len = 0;
			if (s->phase == P_M0) {
				uint8_t km[32];
				memcpy(s->m0, s->in, FSESS_M0);
				memcpy(s->peer_eph, s->in + 37, 32);
				memcpy(s->m1, magic, 5);
				memcpy(s->m1 + 5, s->eph_pk, 32);
				plat_random(km, sizeof(km));
				bool ok = zmlkem_encaps(s->m1 + 37, s->kem_ss, s->m0 + 69, km);
				crypto_wipe(km, sizeof(km));
				if (!ok) { fail(s, "a bad ML-KEM key"); return; }
				if (!keys(s)) { fail(s, "a low-order key"); return; }
				out_raw(s, s->m1, sizeof(s->m1));
				uint8_t a[96], m[46];
				memcpy(a, s->cfg.secret_key + 32, 32);
				auth_msg(s, 'r', m);
				crypto_ed25519_sign(a + 32, s->cfg.secret_key, m, 46);
				send_small(s, T_AUTH, a, 96);
			} else {
				memcpy(s->m1, s->in, FSESS_M1);
				memcpy(s->peer_eph, s->in + 5, 32);
				// a tampered ciphertext does not fail here: it gives another
				// secret (implicit rejection), and the AUTH frame will not decrypt
				bool ok = zmlkem_decaps(s->kem_ss, s->m1 + 37, s->kem_dk);
				crypto_wipe(s->kem_dk, sizeof(s->kem_dk));
				if (!ok) { fail(s, "our own ML-KEM key is broken"); return; }
				if (!keys(s)) { fail(s, "a low-order key"); return; }
			}
			s->phase = P_AUTH;
			continue;
		}
		// frames
		uint32_t k = sizeof(s->in) - s->in_len < n ? (uint32_t)sizeof(s->in) - s->in_len : n;
		memcpy(s->in + s->in_len, d, k);
		s->in_len += k; d += k; n -= k;
		while (s->in_len >= 2 && s->state == FS_RUNNING) {
			uint32_t l = ((uint32_t)s->in[0] << 8) | s->in[1];
			if (l < 16 || l > FSESS_FRAME_MAX + 16) { fail(s, "a bad frame length"); return; }
			if (s->in_len < 2 + l) break;
			uint8_t nc[24];
			nonce(s->n_in++, nc);
			if (crypto_aead_unlock(s->in + 2, s->in + 2 + l - 16, s->key_in, nc, s->in, 2, s->in + 2, l - 16)) {
				fail(s, "a frame that does not decrypt");
				return;
			}
			frame(s, s->in + 2, l - 16);
			memmove(s->in, s->in + 2 + l, s->in_len - 2 - l);
			s->in_len -= 2 + l;
			serve(s);
		}
	}
	serve(s);
	check_done(s);
}

uint32_t fsess_output(fsess_t *s, const uint8_t **p) {
	if (s->state == FS_RUNNING) serve(s);
	*p = s->out + s->out_head;
	return s->out_len;
}

void fsess_consumed(fsess_t *s, uint32_t n) {
	if (n > s->out_len) n = s->out_len;
	s->out_head += n;
	s->out_len -= n;
	if (!s->out_len) s->out_head = 0;
	if (s->state == FS_RUNNING) serve(s);
	check_done(s);
}

void fsess_poll(fsess_t *s, uint32_t now_ms) {
	if (s->state != FS_RUNNING) return;
	if (!s->start_ms) { s->start_ms = now_ms ? now_ms : 1; s->last_in_ms = s->start_ms; }
	if (!s->timed) { s->last_in_ms = now_ms; s->timed = true; }
	// SIGNED differences: a platform may pass a time read a moment BEFORE
	// the session began (fed on Zeitlos read its clock at the top of a
	// loop pass, then made an inbound session while handling a message).
	// Unsigned, "a millisecond before the start" was 4 billion ms later,
	// and the new session failed at once -- test_zfed.c found it, once
	// its net held data as the real one does.
	if (s->phase != P_RUN && (int32_t)(now_ms - s->start_ms) > FSESS_SHAKE_MS) fail(s, "the handshake took too long");
	else if ((int32_t)(now_ms - s->last_in_ms) > FSESS_IDLE_MS) fail(s, "nothing from the peer for too long");
}

void fsess_closed(fsess_t *s) {
	if (s->state == FS_RUNNING) fail(s, "the connection closed");
}
