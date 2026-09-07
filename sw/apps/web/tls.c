/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See tls.h.
 */

#include <string.h>
#include <stdio.h>

#include "tls.h"
#include "tls_crypto.h"
#include "verify.h"
#include "rsa.h"
#include "ecdsa.h"

#include "../../ext/monocypher/monocypher.h"

#ifdef TLS_HOST_TEST
// The host harness supplies its own randomness; see tests/test_tls.c.
void z_rng_bytes(void *buf, uint32_t len);
bool z_rng_secure(void);
#else
#include "../../common/zrng.h"
#endif

// -- wire constants -------------------------------------------------

#define REC_CHANGE_CIPHER_SPEC  20
#define REC_ALERT               21
#define REC_HANDSHAKE           22
#define REC_APPLICATION_DATA    23

#define HS_CLIENT_HELLO          1
#define HS_SERVER_HELLO          2
#define HS_NEW_SESSION_TICKET    4
#define HS_ENCRYPTED_EXTENSIONS  8
#define HS_CERTIFICATE          11
#define HS_CERTIFICATE_REQUEST  13
#define HS_CERTIFICATE_VERIFY   15
#define HS_FINISHED             20
#define HS_KEY_UPDATE           24

#define EXT_SERVER_NAME          0
#define EXT_SUPPORTED_GROUPS    10
#define EXT_SIGNATURE_ALGORITHMS 13
#define EXT_ALPN                16
#define EXT_SUPPORTED_VERSIONS  43
#define EXT_KEY_SHARE           51

#define GROUP_X25519        0x001d
#define SUITE_CHACHA20      0x1303

enum {
	ST_INIT = 0,
	ST_WAIT_SH,
	ST_WAIT_ENCRYPTED,		// EE, Certificate, CertificateVerify
	ST_WAIT_FINISHED,
	ST_ESTABLISHED,
	ST_CLOSED,
	ST_ERROR,
};

// The fixed random value a server puts in ServerHello.random to mean
// HelloRetryRequest (RFC 8446 section 4.1.3). Without this check an
// HRR is parsed as a ServerHello, its "key share" is a group
// identifier rather than a public key, and the handshake fails much
// later with something meaningless.
static const uint8_t hrr_random[32] = {
	0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
	0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
	0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
	0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static void fail(tls_ctx_t *c, const char *why) {
	if (c->state == ST_ERROR) return;
	c->state = ST_ERROR;
	c->err = why;
}

// -- byte writing ---------------------------------------------------

typedef struct { uint8_t *p; uint32_t n, cap; bool over; } wbuf_t;

static void w8(wbuf_t *w, uint8_t v) {
	if (w->n + 1 > w->cap) { w->over = true; return; }
	w->p[w->n++] = v;
}

static void w16(wbuf_t *w, uint16_t v) { w8(w, (uint8_t)(v >> 8)); w8(w, (uint8_t)v); }

static void wbytes(wbuf_t *w, const void *d, uint32_t n) {
	if (w->n + n > w->cap) { w->over = true; return; }
	memcpy(w->p + w->n, d, n);
	w->n += n;
}

// A 16-bit length prefix whose value is not known until the body has
// been written. Returns the offset to patch.
static uint32_t wlen16(wbuf_t *w) { uint32_t at = w->n; w16(w, 0); return at; }

static void wpatch16(wbuf_t *w, uint32_t at) {
	uint32_t body = w->n - at - 2;
	if (at + 2 > w->cap) return;
	w->p[at] = (uint8_t)(body >> 8);
	w->p[at + 1] = (uint8_t)body;
}

// -- byte reading ---------------------------------------------------

typedef struct { const uint8_t *p; uint32_t n, at; bool over; } rbuf_t;

static uint8_t r8(rbuf_t *r) {
	if (r->at + 1 > r->n) { r->over = true; return 0; }
	return r->p[r->at++];
}

static uint16_t r16(rbuf_t *r) {
	uint16_t hi = r8(r);
	return (uint16_t)((hi << 8) | r8(r));
}

static const uint8_t *rbytes(rbuf_t *r, uint32_t n) {
	const uint8_t *p;
	if (r->at + n > r->n) { r->over = true; return NULL; }
	p = r->p + r->at;
	r->at += n;
	return p;
}

// -- record output --------------------------------------------------

// Emits one record, encrypting it when transmit keys are active.
//
// TLS 1.3 wraps everything after the handshake keys are established
// in an outer record whose type says application_data regardless of
// what is inside; the real type is appended to the plaintext and
// recovered after decryption. Sending the true type in the outer
// header instead is the mistake that produces a handshake which works
// against a permissive server and fails against a strict one.
static void send_record(tls_ctx_t *c, uint8_t type,
	const uint8_t *body, uint32_t len) {

	static uint8_t out[TLS_MAX_RECORD + 5];
	uint8_t nonce[TLS_IV_LEN];
	uint8_t ad[5];
	uint32_t inner;

	if (c->state == ST_ERROR) return;

	if (!c->tx_encrypted) {
		if (len + 5 > sizeof(out)) { fail(c, "tls: outgoing record too large"); return; }
		out[0] = type;
		out[1] = 0x03; out[2] = 0x03;
		out[3] = (uint8_t)(len >> 8);
		out[4] = (uint8_t)len;
		memcpy(out + 5, body, len);
		c->send(c->user, out, len + 5);
		return;
	}

	inner = len + 1 + TLS_TAG_LEN;
	if (inner + 5 > sizeof(out)) { fail(c, "tls: outgoing record too large"); return; }

	// The additional data is the outer header exactly as it goes on
	// the wire, INCLUDING the length of the ciphertext and tag.
	ad[0] = REC_APPLICATION_DATA;
	ad[1] = 0x03; ad[2] = 0x03;
	ad[3] = (uint8_t)(inner >> 8);
	ad[4] = (uint8_t)inner;
	memcpy(out, ad, 5);

	memcpy(out + 5, body, len);
	out[5 + len] = type;			// the real content type, inside

	tls_nonce(nonce, c->tx_iv, c->tx_seq);
	tls_aead_seal(out + 5, out + 5 + len + 1, c->tx_key, nonce,
		ad, 5, out + 5, len + 1);

	c->tx_seq++;
	c->send(c->user, out, inner + 5);

}

static void send_alert(tls_ctx_t *c, uint8_t level, uint8_t desc) {
	uint8_t a[2] = { level, desc };
	send_record(c, REC_ALERT, a, 2);
}

// -- ClientHello ----------------------------------------------------

static void build_client_hello(tls_ctx_t *c, wbuf_t *w) {

	uint32_t ext_at, at;

	w16(w, 0x0303);						// legacy_version, always TLS 1.2 here
	wbytes(w, c->client_random, 32);

	// A 32-byte legacy_session_id, plus the dummy ChangeCipherSpec
	// sent after this, is "middlebox compatibility mode" (RFC 8446
	// appendix D.4). Both exist so the connection looks like a TLS
	// 1.2 resumption to middleboxes that would otherwise drop it.
	// Neither is optional in practice on the open internet.
	w8(w, 32);
	wbytes(w, c->session_id, 32);

	at = wlen16(w);						// cipher_suites
	w16(w, SUITE_CHACHA20);
	wpatch16(w, at);

	w8(w, 1); w8(w, 0);					// legacy_compression_methods: null

	ext_at = wlen16(w);

	// server_name. Sent even though nothing verifies it yet: without
	// SNI a shared-hosting server has no way to know which
	// certificate to present, and many answer with the wrong site or
	// refuse outright.
	{
		uint32_t hlen = (uint32_t)strlen(c->host);
		w16(w, EXT_SERVER_NAME);
		at = wlen16(w);
		{
			uint32_t list = wlen16(w);
			w8(w, 0);					// host_name
			{
				uint32_t nm = wlen16(w);
				wbytes(w, c->host, hlen);
				wpatch16(w, nm);
			}
			wpatch16(w, list);
		}
		wpatch16(w, at);
	}

	// supported_versions. This, not legacy_version, is what actually
	// selects TLS 1.3.
	w16(w, EXT_SUPPORTED_VERSIONS);
	at = wlen16(w);
	w8(w, 2);
	w16(w, 0x0304);
	wpatch16(w, at);

	w16(w, EXT_SUPPORTED_GROUPS);
	at = wlen16(w);
	{ uint32_t l = wlen16(w); w16(w, GROUP_X25519); wpatch16(w, l); }
	wpatch16(w, at);

	// signature_algorithms. Nothing here verifies a signature yet, so
	// this list is a claim about what we COULD check. It is still
	// mandatory -- a server that finds no acceptable algorithm aborts
	// the handshake -- and the entries are the ones Phase 3 will
	// actually implement, so the claim becomes true rather than
	// needing revisiting.
	w16(w, EXT_SIGNATURE_ALGORITHMS);
	at = wlen16(w);
	{
		uint32_t l = wlen16(w);
		w16(w, 0x0403);					// ecdsa_secp256r1_sha256
		w16(w, 0x0804);					// rsa_pss_rsae_sha256
		w16(w, 0x0401);					// rsa_pkcs1_sha256
		wpatch16(w, l);
	}
	wpatch16(w, at);

	// ALPN, naming HTTP/1.1 explicitly. Omitting it is legal and
	// leaves a server free to select HTTP/2, which this client cannot
	// speak -- and the failure would arrive as an unparseable
	// response body rather than as a handshake error.
	w16(w, EXT_ALPN);
	at = wlen16(w);
	{
		uint32_t l = wlen16(w);
		w8(w, 8);
		wbytes(w, "http/1.1", 8);
		wpatch16(w, l);
	}
	wpatch16(w, at);

	w16(w, EXT_KEY_SHARE);
	at = wlen16(w);
	{
		uint32_t l = wlen16(w);
		w16(w, GROUP_X25519);
		{ uint32_t k = wlen16(w); wbytes(w, c->pub, 32); wpatch16(w, k); }
		wpatch16(w, l);
	}
	wpatch16(w, at);

	wpatch16(w, ext_at);

}

// Wraps a handshake body in its 4-byte header, feeds the whole thing
// to the transcript, and sends it.
static void send_handshake(tls_ctx_t *c, uint8_t type,
	const uint8_t *body, uint32_t len) {

	static uint8_t msg[TLS_MAX_HS_MSG + 4];

	if (len + 4 > sizeof(msg)) { fail(c, "tls: handshake message too large"); return; }

	msg[0] = type;
	msg[1] = (uint8_t)(len >> 16);
	msg[2] = (uint8_t)(len >> 8);
	msg[3] = (uint8_t)len;
	memcpy(msg + 4, body, len);

	// The transcript covers handshake MESSAGES, not records: headers
	// in, record framing out. A transcript that includes record
	// headers produces a Finished the server rejects, and it is an
	// easy mistake because the two arrive together.
	z_sha256_update(&c->transcript, msg, len + 4);

	send_record(c, REC_HANDSHAKE, msg, len + 4);

}

bool tls_start(tls_ctx_t *c) {

	static uint8_t body[TLS_MAX_HS_MSG];
	wbuf_t w = { body, 0, sizeof(body), false };

	// See tls.h. This refusal is the same one sw/apps/net's SSH
	// client makes and for the same reason: a key derived from cycle
	// counter jitter is not a weakened session, it is an open one.
	if (!z_rng_secure()) {
		fail(c, "no secure randomness on this board -- refusing TLS");
		return false;
	}

	// There is deliberately no way to run this with verification off.
	// The flag that allowed it while the machinery was being written
	// is gone; see tls.h.
	if (!c->verify_ready || !c->find_root) {
		fail(c, "tls: no certificate root store -- refusing to connect");
		return false;
	}

	z_rng_bytes(c->client_random, 32);
	z_rng_bytes(c->session_id, 32);
	z_rng_bytes(c->priv, 32);
	crypto_x25519_public_key(c->pub, c->priv);

	z_sha256_init(&c->transcript);

	build_client_hello(c, &w);
	if (w.over) { fail(c, "tls: ClientHello did not fit"); return false; }

	send_handshake(c, HS_CLIENT_HELLO, body, w.n);

	// The dummy ChangeCipherSpec of middlebox compatibility mode. It
	// is not part of the transcript and means nothing; it exists so
	// the exchange looks like TLS 1.2 to things on the path.
	{
		uint8_t ccs = 1;
		send_record(c, REC_CHANGE_CIPHER_SPEC, &ccs, 1);
	}

	// The size of the flight we just put on the wire. If a server
	// answers nothing at all, the first question is whether we spoke
	// to it -- and a ClientHello of an implausible length is the
	// cheapest way to see that we did not.
	printf("tls: ClientHello %u bytes, session %s\n",
		(unsigned)w.n, "middlebox-compat");

	c->state = ST_WAIT_SH;
	return true;

}

// -- ServerHello ----------------------------------------------------

static void handle_server_hello(tls_ctx_t *c, const uint8_t *msg, uint32_t len) {

	rbuf_t r = { msg, len, 0, false };
	const uint8_t *random, *peer = NULL;
	uint16_t suite, ext_total;
	uint8_t sid_len;
	bool got_version = false;

	r16(&r);							// legacy_version
	random = rbytes(&r, 32);
	if (!random) { fail(c, "tls: truncated ServerHello"); return; }

	if (!memcmp(random, hrr_random, 32)) {
		// Only happens if the server rejects every group offered, and
		// x25519 is universal. Named explicitly so the failure is not
		// a mystery if it ever does.
		fail(c, "tls: server asked for a different key exchange group");
		return;
	}

	sid_len = r8(&r);
	rbytes(&r, sid_len);				// echoed session id, not checked

	suite = r16(&r);
	r8(&r);								// legacy_compression_method

	if (r.over) { fail(c, "tls: truncated ServerHello"); return; }

	if (suite != SUITE_CHACHA20) {
		fail(c, "tls: server chose a cipher suite we did not offer");
		return;
	}

	ext_total = r16(&r);
	{
		uint32_t end = r.at + ext_total;
		if (end > r.n) { fail(c, "tls: bad ServerHello extensions"); return; }

		while (r.at + 4 <= end) {

			uint16_t type = r16(&r);
			uint16_t elen = r16(&r);
			uint32_t next = r.at + elen;

			if (next > end) { fail(c, "tls: bad extension length"); return; }

			if (type == EXT_SUPPORTED_VERSIONS) {
				if (r16(&r) != 0x0304) {
					fail(c, "tls: server did not select TLS 1.3");
					return;
				}
				got_version = true;
			} else if (type == EXT_KEY_SHARE) {
				uint16_t group = r16(&r);
				uint16_t klen = r16(&r);
				if (group != GROUP_X25519 || klen != 32) {
					fail(c, "tls: server key share is not x25519");
					return;
				}
				peer = rbytes(&r, 32);
			}

			r.at = next;

		}
	}

	// A server that omits supported_versions is doing TLS 1.2. That
	// is not a downgrade to accept quietly: everything below depends
	// on 1.3 framing.
	if (!got_version) { fail(c, "tls: server did not select TLS 1.3"); return; }
	if (!peer) { fail(c, "tls: server sent no key share"); return; }

	// -- the key schedule --
	{
		uint8_t shared[32], early[TLS_HASH_LEN], th[TLS_HASH_LEN];
		z_sha256_ctx snapshot;

		crypto_x25519(shared, c->priv, peer);

		// An all-zero shared secret means a peer public key in the
		// small subgroup. Monocypher reports it this way rather than
		// with a return code.
		{
			uint8_t acc = 0;
			for (int i = 0; i < 32; i++) acc |= shared[i];
			if (!acc) { fail(c, "tls: bad server key share"); return; }
		}

		tls_early_secret(early);
		tls_handshake_secret(c->handshake_secret, early, shared);

		// The transcript hash is taken as a SNAPSHOT: the context has
		// to keep accumulating for the messages still to come.
		// Calling final() on the real context here is the bug that
		// makes every later Finished wrong.
		snapshot = c->transcript;
		z_sha256_final(&snapshot, th);

		tls_derive_secret(c->client_hs_secret, c->handshake_secret,
			"c hs traffic", th);
		tls_derive_secret(c->server_hs_secret, c->handshake_secret,
			"s hs traffic", th);

		tls_traffic_keys(c->rx_key, c->rx_iv, c->server_hs_secret);
		c->rx_seq = 0;
		c->rx_encrypted = true;

		crypto_wipe(shared, sizeof(shared));
		crypto_wipe(early, sizeof(early));
	}

	c->state = ST_WAIT_ENCRYPTED;

}

// -- server Finished, and the switch to application keys ------------

static void handle_server_finished(tls_ctx_t *c, const uint8_t *msg,
	uint32_t len) {

	uint8_t expect[TLS_HASH_LEN];
	uint8_t th_before[TLS_HASH_LEN], th_after[TLS_HASH_LEN];
	z_sha256_ctx snapshot;

	if (len != TLS_HASH_LEN) { fail(c, "tls: bad Finished length"); return; }

	// verify_data covers the transcript UP TO but NOT INCLUDING this
	// message -- and by now this message is already in the running
	// hash, header and all. That is why th_pre_msg exists: it was
	// copied before the header went in. See tls.h.
	snapshot = c->th_pre_msg;
	z_sha256_final(&snapshot, th_before);

	tls_finished(expect, c->server_hs_secret, th_before);

	if (crypto_verify32(expect, msg) != 0) {
		// Either the server is not who it shares a secret with, or
		// our schedule is wrong. In this phase, with no certificate
		// verification, this check is the ONLY thing tying the
		// connection to the key exchange at all.
		fail(c, "tls: server Finished did not verify");
		return;
	}

	// The server must have sent a certificate and proved it holds the
	// matching key. Reaching Finished without both means it skipped a
	// message, and continuing would authenticate nothing.
	//
	// The CHAIN is checked below, after our Finished goes out.
	if (!c->have_leaf) {
		fail(c, "tls: server never sent a certificate");
		return;
	}

	// Application secrets come from the transcript THROUGH the server
	// Finished -- which is now current, since it was added before
	// this function ran.
	snapshot = c->transcript;
	z_sha256_final(&snapshot, th_after);

	tls_master_secret(c->master_secret, c->handshake_secret);
	tls_derive_secret(c->client_ap_secret, c->master_secret,
		"c ap traffic", th_after);
	tls_derive_secret(c->server_ap_secret, c->master_secret,
		"s ap traffic", th_after);

	// The client's own Finished is sent under the HANDSHAKE keys and
	// covers the same transcript, so it has to go out before the
	// transmit keys are switched.
	{
		uint8_t verify[TLS_HASH_LEN];
		tls_traffic_keys(c->tx_key, c->tx_iv, c->client_hs_secret);
		c->tx_seq = 0;
		c->tx_encrypted = true;

		tls_finished(verify, c->client_hs_secret, th_after);
		send_handshake(c, HS_FINISHED, verify, TLS_HASH_LEN);
	}

	tls_traffic_keys(c->tx_key, c->tx_iv, c->client_ap_secret);
	c->tx_seq = 0;
	tls_traffic_keys(c->rx_key, c->rx_iv, c->server_ap_secret);
	c->rx_seq = 0;

	// NOW the chain, with our Finished already on the wire so the
	// server is not waiting on us while this runs. See
	// handle_certificate().
	{
		const char *verr = NULL;
		if (!verify_chain(c->chain, c->chain_n, c->host, c->now,
			c->find_root, c->root_user, &verr)) {
			fail(c, verr ? verr : "tls: certificate chain is not trusted");
			return;
		}
		c->chain_ok = true;
	}

	c->state = ST_ESTABLISHED;

}

// -- handshake message dispatch -------------------------------------

// The Certificate message (RFC 8446 section 4.4.2):
//
//   opaque certificate_request_context<0..255>;
//   CertificateEntry certificate_list<0..2^24-1>;
//   CertificateEntry = opaque cert_data<1..2^24-1>, Extension ext<0..2^16-1>
//
// The DER is not copied: `body` already points into ctx->cert, which
// lives as long as the handshake, so the chain is recorded as views.
static void handle_certificate(tls_ctx_t *c, const uint8_t *body,
	uint32_t len) {

	uint32_t at = 0, list_len, list_end;
	const char *perr = NULL;

	if (len < 1) { fail(c, "tls: truncated Certificate"); return; }

	at += 1u + body[0];					// certificate_request_context
	if (at + 3 > len) { fail(c, "tls: truncated Certificate"); return; }

	list_len = ((uint32_t)body[at] << 16) | ((uint32_t)body[at + 1] << 8) |
		body[at + 2];
	at += 3;
	list_end = at + list_len;

	if (list_end > len) { fail(c, "tls: bad certificate list length"); return; }

	c->chain_n = 0;

	while (at + 3 <= list_end) {

		uint32_t clen = ((uint32_t)body[at] << 16) |
			((uint32_t)body[at + 1] << 8) | body[at + 2];
		uint32_t ext_len;

		at += 3;
		if (at + clen > list_end) { fail(c, "tls: bad certificate length"); return; }

		if (c->chain_n < VERIFY_MAX_CHAIN) {
			c->chain[c->chain_n].der = body + at;
			c->chain[c->chain_n].len = clen;
			c->chain_n++;
		} else {
			// More certificates than a real chain ever has. Refusing
			// beats silently ignoring the tail, since the ignored
			// part might be the one that mattered.
			fail(c, "tls: certificate chain too long");
			return;
		}

		at += clen;

		if (at + 2 > list_end) { fail(c, "tls: truncated certificate entry"); return; }
		ext_len = ((uint32_t)body[at] << 8) | body[at + 1];
		at += 2 + ext_len;
		if (at > list_end) { fail(c, "tls: bad certificate extensions"); return; }

	}

	if (c->chain_n == 0) { fail(c, "tls: server sent an empty chain"); return; }

	// The leaf is parsed here because CertificateVerify -- the very
	// next message -- needs its public key.
	if (!x509_parse(c->chain[0].der, c->chain[0].len, &c->leaf, &perr)) {
		fail(c, perr ? perr : "tls: unparseable server certificate");
		return;
	}
	c->have_leaf = true;

	// The transcript through Certificate is what CertificateVerify
	// signs. Snapshot it now: by the time that message has been
	// reassembled it is itself in the running hash.
	{
		z_sha256_ctx snap = c->transcript;
		z_sha256_final(&snap, c->th_certs);
		c->have_th_certs = true;
	}

	// The chain is NOT verified here. It is verified after our
	// Finished has been sent -- see handle_server_finished().
	//
	// Verifying here is the textbook order and it does not survive
	// this machine. A chain check costs tens of seconds on this CPU
	// (three P-384 signatures at ~16s each, measured), and the server
	// is sitting waiting for our Finished the whole time. Real
	// servers give up: en.wikipedia.org closed the connection before
	// the check finished, so the handshake completed against a socket
	// that no longer existed.
	//
	// Deferring costs one thing and it is worth naming precisely: the
	// handshake completes with a peer whose certificate has not been
	// checked yet. NO APPLICATION DATA is sent or accepted in that
	// window -- tls_write() refuses until chain_ok, and an incoming
	// application record is rejected -- so what the peer gets is a
	// Finished message, which proves only that we derived the same
	// keys it did. It learns nothing, and the connection is torn down
	// before a request goes out if the chain turns out to be bad.

}

// CertificateVerify (RFC 8446 section 4.4.3): the proof that the peer
// holds the leaf's private key.
//
// Without this the chain means nothing. A chain is public -- anyone
// can obtain the real certificate for any site and replay it.
static void handle_certificate_verify(tls_ctx_t *c, const uint8_t *body,
	uint32_t len) {

	static uint8_t signed_content[64 + 33 + 1 + TLS_HASH_LEN];
	uint8_t hash[TLS_HASH_LEN];
	uint16_t alg, siglen;
	const uint8_t *sig;
	uint32_t n = 0;

	if (!c->have_leaf || !c->have_th_certs) {
		fail(c, "tls: CertificateVerify before Certificate");
		return;
	}

	if (len < 4) { fail(c, "tls: truncated CertificateVerify"); return; }

	alg = (uint16_t)((body[0] << 8) | body[1]);
	siglen = (uint16_t)((body[2] << 8) | body[3]);
	sig = body + 4;

	if (4u + siglen != len) { fail(c, "tls: bad CertificateVerify length"); return; }

	// The signed content is a fixed preamble, then the transcript.
	// The 64 spaces and the context string exist to make this
	// signature useless in any other protocol -- so getting them
	// exactly right is the whole point of them being there.
	memset(signed_content, 0x20, 64);
	n = 64;
	memcpy(signed_content + n, "TLS 1.3, server CertificateVerify", 33);
	n += 33;
	signed_content[n++] = 0x00;
	memcpy(signed_content + n, c->th_certs, TLS_HASH_LEN);
	n += TLS_HASH_LEN;

	z_sha256(hash, signed_content, n);

	switch (alg) {

	case 0x0804:						// rsa_pss_rsae_sha256
		if (c->leaf.key_alg != X509_KEY_RSA) {
			fail(c, "tls: signature algorithm does not match the key");
			return;
		}
		if (!rsa_verify_pss_sha256(c->leaf.rsa_n.p, c->leaf.rsa_n.len,
			c->leaf.rsa_e.p, c->leaf.rsa_e.len, sig, siglen, hash)) {
			fail(c, "tls: the server did not prove it holds its own key");
			return;
		}
		break;

	case 0x0403: {						// ecdsa_secp256r1_sha256
		der_t sv = { sig, siglen }, r, sc, seq;
		if (c->leaf.key_alg != X509_KEY_EC_P256 ||
			c->leaf.ec_point.len != 65) {
			fail(c, "tls: signature algorithm does not match the key");
			return;
		}
		if (!der_expect(&sv, DER_SEQUENCE, &seq) ||
			!der_expect(&seq, DER_INTEGER, &r) ||
			!der_expect(&seq, DER_INTEGER, &sc) || seq.len != 0) {
			fail(c, "tls: malformed ECDSA CertificateVerify");
			return;
		}
		if (!ec_verify(EC_CURVE_P256, c->leaf.ec_point.p,
			c->leaf.ec_point.len, r.p, r.len, sc.p, sc.len, hash, 32)) {
			fail(c, "tls: the server did not prove it holds its own key");
			return;
		}
		break;
	}

	// PKCS#1 v1.5 is NOT accepted here, even though certificates use
	// it constantly. TLS 1.3 forbids it for handshake signatures
	// (RFC 8446 section 4.4.3), and accepting it would be accepting
	// something no conforming server sends.
	default:
		fail(c, "tls: unsupported CertificateVerify algorithm");
		return;

	}

}

static void process_handshake_msg(tls_ctx_t *c, uint8_t type,
	const uint8_t *body, uint32_t len, bool complete) {

	switch (type) {

	case HS_SERVER_HELLO:
		if (!complete) { fail(c, "tls: ServerHello too large"); return; }
		if (c->state != ST_WAIT_SH) { fail(c, "tls: unexpected ServerHello"); return; }
		handle_server_hello(c, body, len);
		break;

	case HS_ENCRYPTED_EXTENSIONS:
		// Nothing in here is acted on yet. ALPN was offered, and a
		// server that selected something other than http/1.1 would
		// say so here -- Phase 4, along with the rest of content
		// negotiation.
		break;

	case HS_CERTIFICATE:
		if (!complete) { fail(c, "tls: certificate chain too large"); return; }
		handle_certificate(c, body, len);
		break;

	case HS_CERTIFICATE_VERIFY:
		if (!complete) { fail(c, "tls: CertificateVerify too large"); return; }
		handle_certificate_verify(c, body, len);
		break;

	case HS_CERTIFICATE_REQUEST:
		// Answered with an empty Certificate, which is what a client
		// with no certificate is supposed to send. Ignoring it makes
		// the server wait for a message that never comes.
		{
			uint8_t empty[4] = { 0, 0, 0, 0 };	// ctx len 0, list len 0 (3 bytes)
			send_handshake(c, HS_CERTIFICATE, empty, 4);
		}
		break;

	case HS_FINISHED:
		if (!complete) { fail(c, "tls: Finished too large"); return; }
		if (c->state != ST_WAIT_ENCRYPTED && c->state != ST_WAIT_FINISHED) {
			fail(c, "tls: unexpected Finished");
			return;
		}
		handle_server_finished(c, body, len);
		break;

	case HS_NEW_SESSION_TICKET:
		// Arrives after the handshake and is ignored: there is no
		// resumption here, so a ticket is something to store and
		// never use.
		break;

	case HS_KEY_UPDATE:
		// Handled rather than ignored. A server that sends one has
		// already switched its own keys, so ignoring it means every
		// subsequent record fails to decrypt.
		if (c->state != ST_ESTABLISHED) { fail(c, "tls: early KeyUpdate"); return; }
		tls_update_traffic_secret(c->server_ap_secret);
		tls_traffic_keys(c->rx_key, c->rx_iv, c->server_ap_secret);
		c->rx_seq = 0;
		// request_update == 1 asks us to rekey our side too.
		if (len >= 1 && body[0] == 1) {
			uint8_t zero = 0;
			send_handshake(c, HS_KEY_UPDATE, &zero, 1);
			tls_update_traffic_secret(c->client_ap_secret);
			tls_traffic_keys(c->tx_key, c->tx_iv, c->client_ap_secret);
			c->tx_seq = 0;
		}
		break;

	default:
		// Unknown handshake types are hashed and skipped rather than
		// treated as fatal: the transcript stays correct, which is
		// what matters.
		break;

	}

}

// Handshake bytes, which may span records and may be several messages
// in one record.
//
// Written as a streaming state machine rather than "reassemble a
// message then parse it" because a certificate chain is routinely
// larger than any buffer worth spending here, and it still has to
// reach the transcript byte for byte.
static void feed_handshake(tls_ctx_t *c, const uint8_t *p, uint32_t n) {

	while (n && c->state != ST_ERROR) {

		if (!c->hs_in_body) {

			uint32_t take = 4 - c->hs_hdr_len;
			if (take > n) take = n;
			memcpy(c->hs_hdr + c->hs_hdr_len, p, take);
			c->hs_hdr_len += take;
			p += take; n -= take;

			if (c->hs_hdr_len < 4) return;

			// Snapshot BEFORE the header goes into the running hash.
			// A Finished arriving later needs the transcript as it
			// stood at exactly this instant.
			c->th_pre_msg = c->transcript;

			z_sha256_update(&c->transcript, c->hs_hdr, 4);

			c->hs_type = c->hs_hdr[0];
			c->hs_need = ((uint32_t)c->hs_hdr[1] << 16) |
				((uint32_t)c->hs_hdr[2] << 8) | c->hs_hdr[3];
			c->hs_len = 0;
			c->hs_hdr_len = 0;
			c->hs_in_body = true;
			// The Certificate message goes into its own buffer; it
			// is the one message routinely larger than TLS_MAX_HS_MSG
			// and it is no longer something that can be skipped.
			if (c->hs_type == HS_CERTIFICATE) {
				c->hs_dst = c->cert;
				c->hs_cap = TLS_MAX_CERT;
			} else {
				c->hs_dst = c->hs;
				c->hs_cap = TLS_MAX_HS_MSG;
			}
			c->hs_skipping = (c->hs_need > c->hs_cap);

			c->n_hs_msgs++;
			c->last_hs_type = c->hs_type;

			if (c->hs_need == 0) {
				process_handshake_msg(c, c->hs_type, c->hs_dst, 0, true);
				c->hs_in_body = false;
			}

			continue;

		}

		{
			uint32_t take = c->hs_need - c->hs_len;
			if (take > n) take = n;

			z_sha256_update(&c->transcript, p, take);

			if (!c->hs_skipping) {
				memcpy(c->hs_dst + c->hs_len, p, take);
			}

			c->hs_len += take;
			p += take; n -= take;

			if (c->hs_len == c->hs_need) {
				process_handshake_msg(c, c->hs_type, c->hs_dst, c->hs_len,
					!c->hs_skipping);
				c->hs_in_body = false;
				c->hs_len = 0;
			}
		}

	}

}

// -- record input ---------------------------------------------------

static void process_record(tls_ctx_t *c, uint8_t type,
	uint8_t *body, uint32_t len) {

	if (c->rx_encrypted && type == REC_APPLICATION_DATA) c->n_records++;

	if (c->rx_encrypted && type == REC_APPLICATION_DATA) {

		uint8_t nonce[TLS_IV_LEN];
		uint8_t ad[5];
		uint32_t plain;

		if (len < TLS_TAG_LEN + 1) { fail(c, "tls: short encrypted record"); return; }

		plain = len - TLS_TAG_LEN;

		ad[0] = REC_APPLICATION_DATA;
		ad[1] = 0x03; ad[2] = 0x03;
		ad[3] = (uint8_t)(len >> 8);
		ad[4] = (uint8_t)len;

		tls_nonce(nonce, c->rx_iv, c->rx_seq);

		if (!tls_aead_open(body, c->rx_key, nonce, ad, 5,
			body, plain, body + plain)) {
			// Which record, how long, and under which keys. A tag
			// mismatch on the first record after a key change is a
			// different bug from one in the middle of a stream.
			static char msg[96];
			snprintf(msg, sizeof(msg),
				"tls: record %lu (%lu bytes, seq %lu) failed to decrypt",
				(unsigned long)c->n_records, (unsigned long)len,
				(unsigned long)c->rx_seq);
			fail(c, msg);
			return;
		}

		c->rx_seq++;

		// The real content type is the last non-zero byte; everything
		// after it is padding the server chose to add.
		while (plain > 0 && body[plain - 1] == 0) plain--;
		if (plain == 0) { fail(c, "tls: record with no content type"); return; }

		type = body[--plain];
		len = plain;

	} else if (type == REC_CHANGE_CIPHER_SPEC) {

		// Middlebox compatibility noise. Legal at any point before
		// the handshake completes, means nothing, and must NOT reach
		// the transcript.
		return;

	}

	switch (type) {

	case REC_HANDSHAKE:
		c->n_records++;
		feed_handshake(c, body, len);
		break;

	case REC_APPLICATION_DATA:
		if (c->state != ST_ESTABLISHED || !c->chain_ok) {
			fail(c, "tls: early application data");
			return;
		}
		if (len && c->data) c->data(c->user, body, len);
		break;

	case REC_ALERT:
		if (len >= 2) {
			// close_notify is an orderly shutdown, not a failure --
			// and it must be ANSWERED with one of our own before the
			// state changes, or the peer sits waiting for it.
			//
			// Found by tests/test_tls.c, which hung: OpenSSL's
			// unwrap() blocks until the client replies, so the
			// handshake and the whole request/response worked and the
			// test still timed out. On hardware this would have
			// looked like every page load leaving a socket open until
			// tcp.c's timeout, which with one TCB in the system means
			// the next fetch is refused as busy.
			if (body[1] == 0) {
				if (c->state == ST_ESTABLISHED) send_alert(c, 1, 0);
				c->state = ST_CLOSED;
				return;
			}
			{
				static char msg[64];
				snprintf(msg, sizeof(msg), "tls: server sent alert %u",
					(unsigned)body[1]);
				fail(c, msg);
			}
		} else {
			fail(c, "tls: malformed alert");
		}
		break;

	default:
		fail(c, "tls: unknown record type");
		break;

	}

}

void tls_feed(tls_ctx_t *c, const uint8_t *data, uint32_t len) {

	while (len && c->state != ST_ERROR && c->state != ST_CLOSED) {

		// -- header --
		if (c->rec_want == 0) {

			uint32_t take = 5 - c->rec_len;
			if (take > len) take = len;
			memcpy(c->rec + c->rec_len, data, take);
			c->rec_len += take;
			data += take; len -= take;

			if (c->rec_len < 5) return;

			{
				uint32_t body = ((uint32_t)c->rec[3] << 8) | c->rec[4];
				if (body > TLS_MAX_RECORD) {
					fail(c, "tls: oversized record");
					return;
				}
				c->rec_want = body;
			}

			// A zero-length record is legal for some types and is
			// simply nothing to process.
			if (c->rec_want == 0) {
				uint8_t type = c->rec[0];
				c->rec_len = 0;
				process_record(c, type, c->rec, 0);
				continue;
			}

			c->rec_len = 0;
			continue;

		}

		// -- body --
		{
			uint32_t take = c->rec_want - c->rec_len;
			if (take > len) take = len;
			memcpy(c->rec + 5 + c->rec_len, data, take);
			c->rec_len += take;
			data += take; len -= take;

			if (c->rec_len < c->rec_want) return;

			{
				uint8_t type = c->rec[0];
				uint32_t body_len = c->rec_want;
				c->rec_len = 0;
				c->rec_want = 0;
				process_record(c, type, c->rec + 5, body_len);
			}
		}

	}

}

// -- public ----------------------------------------------------------

void tls_set_verify(tls_ctx_t *c, int64_t now,
	verify_root_fn find_root, void *user) {
	c->now = now;
	c->find_root = find_root;
	c->root_user = user;
	c->verify_ready = true;
}

void tls_init(tls_ctx_t *c, const char *host,
	tls_send_fn send, tls_data_fn data, void *user) {
	memset(c, 0, sizeof(*c));
	c->send = send;
	c->data = data;
	c->user = user;
	snprintf(c->host, sizeof(c->host), "%s", host ? host : "");
	c->state = ST_INIT;
}

bool tls_write(tls_ctx_t *c, const uint8_t *data, uint32_t len) {

	// chain_ok as well as ESTABLISHED. They are set together today,
	// but the check is explicit so that deferring verification can
	// never turn into skipping it.
	if (c->state != ST_ESTABLISHED || !c->chain_ok) return false;

	// Split across records at the plaintext limit. A caller handing
	// over a large POST body should not have to know the limit
	// exists.
	while (len) {
		uint32_t n = len > 16384 ? 16384 : len;
		send_record(c, REC_APPLICATION_DATA, data, n);
		data += n;
		len -= n;
		if (c->state == ST_ERROR) return false;
	}

	return true;

}

void tls_close(tls_ctx_t *c) {
	if (c->state == ST_ESTABLISHED) send_alert(c, 1, 0);	// warning, close_notify
	c->state = ST_CLOSED;
}

bool tls_established(const tls_ctx_t *c) { return c->state == ST_ESTABLISHED; }
bool tls_closed(const tls_ctx_t *c) { return c->state == ST_CLOSED; }
bool tls_failed(const tls_ctx_t *c) { return c->state == ST_ERROR; }
// What the handshake actually got through, for when it does not
// finish. "4526 bytes and no complete flight" is not enough to tell a
// server that stopped sending from a client that stopped parsing.
void tls_progress(const tls_ctx_t *c, uint32_t *records, uint32_t *msgs,
	uint8_t *last_type, uint8_t *state) {
	if (records) *records = c->n_records;
	if (msgs) *msgs = c->n_hs_msgs;
	if (last_type) *last_type = c->last_hs_type;
	if (state) *state = c->state;
}

const char *tls_error(const tls_ctx_t *c) {
	return c->state == ST_ERROR ? (c->err ? c->err : "tls error") : NULL;
}
