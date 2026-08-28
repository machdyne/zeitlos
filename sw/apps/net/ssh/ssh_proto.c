/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The SSH-2 client session engine. See ssh_proto.h for the design and
 * for why nothing in this file knows what TCP is.
 */

#include <string.h>
#include <stdio.h>

#include "ssh_proto.h"
#include "ssh_wire.h"
#include "../../../ext/monocypher/monocypher.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

// -- message numbers (RFC 4253 / 4252 / 4254) --

#define MSG_DISCONNECT           1
#define MSG_IGNORE               2
#define MSG_UNIMPLEMENTED        3
#define MSG_DEBUG                4
#define MSG_SERVICE_REQUEST      5
#define MSG_SERVICE_ACCEPT       6
#define MSG_EXT_INFO             7
#define MSG_KEXINIT             20
#define MSG_NEWKEYS             21
#define MSG_KEX_ECDH_INIT       30
#define MSG_KEX_ECDH_REPLY      31
#define MSG_USERAUTH_REQUEST    50
#define MSG_USERAUTH_FAILURE    51
#define MSG_USERAUTH_SUCCESS    52
#define MSG_USERAUTH_BANNER     53
#define MSG_GLOBAL_REQUEST      80
#define MSG_REQUEST_SUCCESS     81
#define MSG_REQUEST_FAILURE     82
#define MSG_CHANNEL_OPEN        90
#define MSG_CHANNEL_OPEN_CONFIRM 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA        94
#define MSG_CHANNEL_EXTENDED_DATA 95
#define MSG_CHANNEL_EOF         96
#define MSG_CHANNEL_CLOSE       97
#define MSG_CHANNEL_REQUEST     98
#define MSG_CHANNEL_SUCCESS     99
#define MSG_CHANNEL_FAILURE    100

// -- states --

enum {
	ST_VERSION,			// waiting for the server's identification line
	ST_KEXINIT,			// waiting for SSH_MSG_KEXINIT
	ST_KEXREPLY,		// waiting for KEX_ECDH_REPLY
	ST_HOSTKEY,			// stalled: owner is deciding about the host key
	ST_NEWKEYS,			// waiting for SSH_MSG_NEWKEYS
	ST_SERVICE,			// waiting for SERVICE_ACCEPT
	ST_AUTH,			// waiting for USERAUTH_SUCCESS/FAILURE
	ST_AUTH_WAIT_PW,	// stalled: owner is collecting a password
	ST_CHANNEL,			// waiting for CHANNEL_OPEN_CONFIRMATION
	ST_PTY,				// waiting for the pty-req reply
	ST_SHELL,			// waiting for the shell reply
	ST_OPEN,			// interactive
	ST_CLOSED
};

#define CLIENT_ID "SSH-2.0-Zeitlos_1.0"

// Our one algorithm per slot. See ssh_proto.h on why there is no
// preference ordering: with a single candidate, "negotiation" is just
// asking whether the server's list contains it.
#define ALG_KEX      "curve25519-sha256"
#define ALG_HOSTKEY  "ssh-ed25519"
#define ALG_CIPHER   "chacha20-poly1305@openssh.com"
#define ALG_MAC      "none"
#define ALG_COMP     "none"

// Channel flow control. Small on purpose: whatever the server sends
// has to fit in rx[] and then be relayed onward through a zport that
// has no backpressure (docs/ports.md), so a large window would only
// buy the chance to drop more at once.
#define LOCAL_WINDOW      16384
#define LOCAL_MAX_PACKET  1024
#define WINDOW_REFILL     8192

static void fail(ssh_proto_t *s, const char *why) {
	if (s->state == ST_CLOSED) return;
	s->state = ST_CLOSED;
	s->failed = true;
	s->event(s->user, SSH_EV_CLOSED, NULL, 0, why);
}

static void status(ssh_proto_t *s, const char *text) {
	s->event(s->user, SSH_EV_STATUS, NULL, 0, text);
}

// -- packet transmission --
//
// Builds one binary packet around `payload` and hands it to the write
// callback. Padding follows RFC 4253 as amended by
// PROTOCOL.chacha20poly1305: the 4-byte length field is NOT counted,
// so (padding_length + payload + padding) is what must be a multiple
// of 8, with at least 4 bytes of padding.
static bool send_packet(ssh_proto_t *s, const uint8_t *payload, uint32_t len) {

	uint32_t pad, total, aadlen, base;
	uint8_t *p = s->tx;

	// THE PADDING RULE IS DIFFERENT BEFORE AND AFTER NEWKEYS, and
	// getting that wrong produces a client that completes the version
	// exchange, sends a KEXINIT the server reads happily, and then
	// gets hung up on with no error message.
	//
	// RFC 4253 section 6: padding makes
	//     packet_length || padding_length || payload || padding
	// a multiple of the cipher block size (8 for "none"). ALL FOUR
	// fields, including the 4-byte length.
	//
	// chacha20-poly1305@openssh.com changes that. The length field is
	// encrypted separately with K_1 and is additional authenticated
	// data, not part of the encrypted block stream, so OpenSSH
	// excludes it: only padding_length || payload || padding has to
	// come out a multiple of 8 (see its packet.c, where padlen is
	// computed as block_size - ((len - aadlen) % block_size)).
	//
	// During the handshake the cipher is still "none" and aadlen is 0,
	// so the plain rule applies. Using the AEAD rule there put every
	// handshake packet 4 bytes out of alignment -- a 164-byte KEXINIT
	// payload went out as 180 bytes on the wire instead of 176 -- and
	// OpenSSH dropped the connection immediately after KEX_ECDH_INIT.
	//
	// This got past the host tests because sw/test/test_ssh_session.c
	// used the same wrong rule for its own packets and never checked
	// alignment. It now does both.
	aadlen = s->tx_cipher.active ? SSH_AEAD_LEN_LEN : 0;
	base = (SSH_AEAD_LEN_LEN - aadlen) + 1 + len;

	pad = 8 - (base % 8);
	if (pad < 4) pad += 8;

	total = 1 + len + pad;

	if (total + SSH_AEAD_LEN_LEN + SSH_AEAD_TAG_LEN > sizeof(s->tx)) {
		fail(s, "ssh: outbound packet too large");
		return false;
	}

	p[0] = (uint8_t)(total >> 24); p[1] = (uint8_t)(total >> 16);
	p[2] = (uint8_t)(total >> 8);  p[3] = (uint8_t)total;
	p[4] = (uint8_t)pad;
	memcpy(p + 5, payload, len);

	// Padding must be random per RFC 4253. It is also the only place
	// in an outbound packet an observer sees bytes we chose freely, so
	// a lazy constant here would be a small but real fingerprint.
	s->random(s->user, p + 5 + len, pad);

	ssh_aead_seal(&s->tx_cipher, p, total);

	// The tag only exists once the cipher is live. Before NEWKEYS the
	// packet is plaintext and ends at the padding -- adding the tag
	// length unconditionally puts 16 bytes of uninitialised buffer on
	// the wire after every handshake packet, which a server reads as
	// the start of the next packet and rejects as a garbage length.
	// The receive path already gets this right (see the rx_need
	// computation in ssh_proto_feed); this is the send-side twin of
	// the same conditional, and it was missing. Found immediately by
	// sw/test/test_ssh_session.c.
	return s->write(s->user, p, SSH_AEAD_LEN_LEN + total +
		(s->tx_cipher.active ? SSH_AEAD_TAG_LEN : 0));

}

// -- KEXINIT --

static bool send_kexinit(ssh_proto_t *s) {

	uint8_t buf[512];
	ssh_wr w;
	uint8_t cookie[16];

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_KEXINIT);

	s->random(s->user, cookie, sizeof(cookie));
	ssh_wr_bytes(&w, cookie, sizeof(cookie));

	ssh_wr_cstr(&w, ALG_KEX);
	ssh_wr_cstr(&w, ALG_HOSTKEY);
	ssh_wr_cstr(&w, ALG_CIPHER);		// client to server
	ssh_wr_cstr(&w, ALG_CIPHER);		// server to client
	ssh_wr_cstr(&w, ALG_MAC);
	ssh_wr_cstr(&w, ALG_MAC);
	ssh_wr_cstr(&w, ALG_COMP);
	ssh_wr_cstr(&w, ALG_COMP);
	ssh_wr_cstr(&w, "");				// languages
	ssh_wr_cstr(&w, "");
	ssh_wr_bool(&w, false);				// no guessed kex packet follows
	ssh_wr_u32(&w, 0);					// reserved

	if (!ssh_wr_ok(&w)) { fail(s, "ssh: kexinit too large"); return false; }

	// I_C goes into the exchange hash as a string. Doing it here, at
	// send time, is what makes it unnecessary to keep a copy of the
	// server's KEXINIT later: the hash order is V_C, V_S, I_C, I_S,
	// and by now we have the first three. I_S is fed straight from the
	// receive buffer when it arrives, and never stored.
	{
		uint8_t l[4];
		uint32_t n = ssh_wr_len(&w);
		l[0] = (uint8_t)(n >> 24); l[1] = (uint8_t)(n >> 16);
		l[2] = (uint8_t)(n >> 8);  l[3] = (uint8_t)n;
		ssh_sha256_update(&s->hash, l, 4);
		ssh_sha256_update(&s->hash, buf, n);
	}

	return send_packet(s, buf, ssh_wr_len(&w));

}

static bool send_kex_ecdh_init(ssh_proto_t *s) {

	uint8_t buf[64];
	ssh_wr w;

	s->random(s->user, s->eph_secret, 32);
	crypto_x25519_public_key(s->eph_public, s->eph_secret);

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_KEX_ECDH_INIT);
	ssh_wr_string(&w, s->eph_public, 32);

	return send_packet(s, buf, ssh_wr_len(&w));

}

static void handle_kexinit(ssh_proto_t *s, const uint8_t *pl, uint32_t len) {

	ssh_rd r;
	const uint8_t *kex, *hk, *enc_cs, *enc_sc;
	uint32_t kex_n, hk_n, cs_n, sc_n;

	// I_S into the exchange hash, as a string, in full and unparsed.
	{
		uint8_t l[4];
		l[0] = (uint8_t)(len >> 24); l[1] = (uint8_t)(len >> 16);
		l[2] = (uint8_t)(len >> 8);  l[3] = (uint8_t)len;
		ssh_sha256_update(&s->hash, l, 4);
		ssh_sha256_update(&s->hash, pl, len);
	}

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);					// message id
	ssh_rd_skip(&r, 16);			// cookie

	kex    = ssh_rd_string(&r, &kex_n);
	hk     = ssh_rd_string(&r, &hk_n);
	enc_cs = ssh_rd_string(&r, &cs_n);
	enc_sc = ssh_rd_string(&r, &sc_n);

	if (r.bad) { fail(s, "ssh: malformed KEXINIT"); return; }

	// Each check gets its own message. "Handshake failed" tells a user
	// nothing they can act on; "this server does not offer ChaCha20"
	// tells them exactly what is wrong and that it is not their
	// network.
	if (!ssh_namelist_has(kex, kex_n, ALG_KEX)) {
		fail(s, "ssh: server does not offer curve25519-sha256 key exchange");
		return;
	}
	if (!ssh_namelist_has(hk, hk_n, ALG_HOSTKEY)) {
		fail(s, "ssh: server has no ssh-ed25519 host key");
		return;
	}
	if (!ssh_namelist_has(enc_cs, cs_n, ALG_CIPHER) ||
		!ssh_namelist_has(enc_sc, sc_n, ALG_CIPHER)) {
		fail(s, "ssh: server does not offer chacha20-poly1305@openssh.com");
		return;
	}

	status(s, "ssh: key exchange");

	if (!send_kex_ecdh_init(s)) return;
	s->state = ST_KEXREPLY;

}

// -- KEX_ECDH_REPLY: the security-critical one --

static void handle_kex_reply(ssh_proto_t *s, const uint8_t *pl, uint32_t len) {

	ssh_rd r, kr, sr;
	const uint8_t *k_s, *q_s, *sig, *hk_type, *hk_key, *sig_type, *sig_blob;
	uint32_t k_s_n, q_s_n, sig_n, hk_type_n, hk_key_n, sig_type_n, sig_blob_n;
	uint8_t shared[32];
	uint8_t zero[32];
	char fp[64];
	uint8_t lenbuf[4];

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);

	k_s = ssh_rd_string(&r, &k_s_n);	// host key blob
	q_s = ssh_rd_string(&r, &q_s_n);	// server ephemeral public
	sig = ssh_rd_string(&r, &sig_n);	// signature blob

	if (r.bad || q_s_n != 32) {
		fail(s, "ssh: malformed KEX_ECDH_REPLY");
		return;
	}

	// The host key blob is string("ssh-ed25519") || string(key32).
	ssh_rd_init(&kr, k_s, k_s_n);
	hk_type = ssh_rd_string(&kr, &hk_type_n);
	hk_key  = ssh_rd_string(&kr, &hk_key_n);
	if (kr.bad || !ssh_str_eq(hk_type, hk_type_n, ALG_HOSTKEY) ||
		hk_key_n != 32) {
		fail(s, "ssh: host key is not a valid ssh-ed25519 key");
		return;
	}

	// The signature blob is string("ssh-ed25519") || string(sig64).
	ssh_rd_init(&sr, sig, sig_n);
	sig_type = ssh_rd_string(&sr, &sig_type_n);
	sig_blob = ssh_rd_string(&sr, &sig_blob_n);
	if (sr.bad || !ssh_str_eq(sig_type, sig_type_n, ALG_HOSTKEY) ||
		sig_blob_n != 64) {
		fail(s, "ssh: host key signature is malformed");
		return;
	}

	// Shared secret.
	crypto_x25519(shared, s->eph_secret, q_s);

	// A peer public key that drives X25519 to an all-zero output is a
	// small-order point, and accepting it means the "shared" secret is
	// a constant the attacker also knows. Monocypher does not reject
	// these for us -- it documents that the caller must check -- so
	// this is the check. Constant-time compare, because the answer
	// depends on secret-adjacent data.
	memset(zero, 0, sizeof(zero));
	if (crypto_verify32(shared, zero) == 0) {
		crypto_wipe(shared, sizeof(shared));
		fail(s, "ssh: server sent a degenerate key exchange value");
		return;
	}

	// K as mpint -- see ssh_wire.h on why the encoding, not the raw
	// 32 bytes, is what gets hashed.
	{
		ssh_wr w;
		ssh_wr_init(&w, s->k_mpint, sizeof(s->k_mpint));
		ssh_wr_mpint(&w, shared, 32);
		s->k_mpint_len = ssh_wr_len(&w);
		if (!ssh_wr_ok(&w)) {
			crypto_wipe(shared, sizeof(shared));
			fail(s, "ssh: shared secret encoding overflow");
			return;
		}
	}
	crypto_wipe(shared, sizeof(shared));

	// Finish the exchange hash: ... || K_S || Q_C || Q_S || K.
	// V_C, V_S, I_C, I_S went in earlier.
	lenbuf[0] = (uint8_t)(k_s_n >> 24); lenbuf[1] = (uint8_t)(k_s_n >> 16);
	lenbuf[2] = (uint8_t)(k_s_n >> 8);  lenbuf[3] = (uint8_t)k_s_n;
	ssh_sha256_update(&s->hash, lenbuf, 4);
	ssh_sha256_update(&s->hash, k_s, k_s_n);

	lenbuf[0] = 0; lenbuf[1] = 0; lenbuf[2] = 0; lenbuf[3] = 32;
	ssh_sha256_update(&s->hash, lenbuf, 4);
	ssh_sha256_update(&s->hash, s->eph_public, 32);
	ssh_sha256_update(&s->hash, lenbuf, 4);
	ssh_sha256_update(&s->hash, q_s, 32);

	ssh_sha256_update(&s->hash, s->k_mpint, s->k_mpint_len);
	ssh_sha256_final(&s->hash, s->exchange_hash);

	// THE point of the whole handshake: this signature is what ties
	// the key exchange to an identity. Without it the exchange still
	// produces a perfectly good shared secret -- with whoever is in
	// the middle.
	if (crypto_ed25519_check(sig_blob, hk_key,
			s->exchange_hash, SSH_SHA256_DIGEST) != 0) {
		fail(s, "ssh: HOST KEY SIGNATURE IS INVALID -- possible interception");
		return;
	}

	// First KEX fixes the session id for the life of the connection; a
	// rekey computes a new exchange hash but must NOT replace it.
	if (!s->have_session_id) {
		memcpy(s->session_id, s->exchange_hash, SSH_SHA256_DIGEST);
		s->have_session_id = true;
	}

	ssh_fingerprint(fp, sizeof(fp), k_s, k_s_n);

	if (s->rekeying) {
		// The key is already trusted -- it is the same one from the
		// first exchange, and re-asking would be noise. (Verifying it
		// has not CHANGED mid-session is a known_hosts job for
		// ssh_hostkey.c.)
		ssh_proto_accept_host(s);
	} else {
		s->state = ST_HOSTKEY;
		s->event(s->user, SSH_EV_HOSTKEY, k_s, k_s_n, fp);
	}

}

// The two directions switch at DIFFERENT MOMENTS and that is the whole
// point of splitting these.
//
// RFC 4253 section 7.3: a NEWKEYS message takes effect for everything
// the SENDER transmits after it. So our own NEWKEYS -- itself sent
// under the old key -- switches only our transmit side. The receive
// side keeps the old key until the SERVER's NEWKEYS arrives, because
// that message is still encrypted (or, during the first exchange,
// still plaintext) under the previous state.
//
// Installing both at once looks harmless and produces a session that
// fails on the very next packet with an implausible length, because
// the client is decrypting the server's plaintext NEWKEYS with a
// brand new key. That is exactly what sw/test/test_ssh_session.c hit.
static void install_tx_key(ssh_proto_t *s) {

	uint8_t key_c[SSH_AEAD_KEY_LEN];

	ssh_derive_key(key_c, s->k_mpint, s->k_mpint_len,
		s->exchange_hash, 'C', s->session_id);
	ssh_cipher_set_key(&s->tx_cipher, key_c);
	crypto_wipe(key_c, sizeof(key_c));

}

static void install_rx_key(ssh_proto_t *s) {

	uint8_t key_d[SSH_AEAD_KEY_LEN];

	ssh_derive_key(key_d, s->k_mpint, s->k_mpint_len,
		s->exchange_hash, 'D', s->session_id);
	ssh_cipher_set_key(&s->rx_cipher, key_d);
	crypto_wipe(key_d, sizeof(key_d));

	// Both directions are keyed now, so the material they came from
	// can go. Doing this in install_tx_key() would leave the receive
	// derivation with nothing to work from.
	crypto_wipe(s->k_mpint, sizeof(s->k_mpint));
	s->k_mpint_len = 0;
	crypto_wipe(s->eph_secret, sizeof(s->eph_secret));

}

// Replays whatever arrived while the engine was stalled. Copied out
// first because ssh_proto_feed() may stall again (a rekey's host key,
// a second password prompt) and would otherwise be appending to the
// buffer it is reading from.
static void drain_pending(ssh_proto_t *s) {

	uint8_t tmp[sizeof(s->pend)];
	uint32_t n = s->pend_len;

	if (!n) return;

	memcpy(tmp, s->pend, n);
	s->pend_len = 0;

	ssh_proto_feed(s, tmp, n);

}

void ssh_proto_accept_host(ssh_proto_t *s) {

	uint8_t msg = MSG_NEWKEYS;

	if (s->state != ST_HOSTKEY && !s->rekeying) return;

	if (!send_packet(s, &msg, 1)) return;

	// Our own NEWKEYS is sent under the OLD key and takes effect for
	// everything after it; the receive direction switches only when
	// the server's NEWKEYS arrives. Installing both here is correct
	// because ssh_cipher_set_key() does not reset sequence numbers --
	// see ssh_crypto.h on why that matters across a rekey.
	install_tx_key(s);

	s->state = ST_NEWKEYS;

	drain_pending(s);

}

// -- authentication --

static bool send_service_request(ssh_proto_t *s) {
	uint8_t buf[64];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_SERVICE_REQUEST);
	ssh_wr_cstr(&w, "ssh-userauth");
	return send_packet(s, buf, ssh_wr_len(&w));
}

// The "none" method. Every server rejects it; the point is the
// FAILURE reply, which lists the methods that would work. Sending
// password blind to a server that only takes publickey wastes a real
// authentication attempt against whatever retry limit it enforces.
static bool send_auth_none(ssh_proto_t *s) {
	uint8_t buf[128];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_USERAUTH_REQUEST);
	ssh_wr_cstr(&w, s->username);
	ssh_wr_cstr(&w, "ssh-connection");
	ssh_wr_cstr(&w, "none");
	return send_packet(s, buf, ssh_wr_len(&w));
}

static bool send_auth_password(ssh_proto_t *s) {

	uint8_t buf[SSH_MAX_USER + SSH_MAX_PASS + 64];
	ssh_wr w;
	bool ok;

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_USERAUTH_REQUEST);
	ssh_wr_cstr(&w, s->username);
	ssh_wr_cstr(&w, "ssh-connection");
	ssh_wr_cstr(&w, "password");
	ssh_wr_bool(&w, false);				// not a password change
	ssh_wr_cstr(&w, s->password);

	ok = send_packet(s, buf, ssh_wr_len(&w));

	// The packet is encrypted and gone; this copy is not. Wiping both
	// the scratch buffer and the stored password matters more here
	// than almost anywhere else in the codebase, because `net` is a
	// long-lived process whose memory outlives the session.
	crypto_wipe(buf, sizeof(buf));
	crypto_wipe((uint8_t *)s->password, sizeof(s->password));
	s->have_password = false;

	return ok;

}

// -- channel --

static bool send_channel_open(ssh_proto_t *s) {
	uint8_t buf[64];
	ssh_wr w;
	s->local_chan = 0;
	s->local_window = LOCAL_WINDOW;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_OPEN);
	ssh_wr_cstr(&w, "session");
	ssh_wr_u32(&w, s->local_chan);
	ssh_wr_u32(&w, LOCAL_WINDOW);
	ssh_wr_u32(&w, LOCAL_MAX_PACKET);
	return send_packet(s, buf, ssh_wr_len(&w));
}

static bool send_pty_req(ssh_proto_t *s) {

	uint8_t buf[128];
	ssh_wr w;

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_REQUEST);
	ssh_wr_u32(&w, s->remote_chan);
	ssh_wr_cstr(&w, "pty-req");
	ssh_wr_bool(&w, true);				// want reply
	ssh_wr_cstr(&w, "vt100");
	// 80x25 matches VT_COLS/VT_ROWS in sw/common/zvt100.h exactly, and
	// `term` windows are not resizable, so there is no window-change
	// story to implement -- a rare case where a fixed size is right
	// rather than lazy.
	ssh_wr_u32(&w, 80);
	ssh_wr_u32(&w, 25);
	ssh_wr_u32(&w, 0);					// pixel width, unused
	ssh_wr_u32(&w, 0);					// pixel height, unused
	ssh_wr_cstr(&w, "");				// empty terminal modes

	return send_packet(s, buf, ssh_wr_len(&w));

}

static bool send_shell_req(ssh_proto_t *s) {
	uint8_t buf[64];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_REQUEST);
	ssh_wr_u32(&w, s->remote_chan);
	ssh_wr_cstr(&w, "shell");
	ssh_wr_bool(&w, true);
	return send_packet(s, buf, ssh_wr_len(&w));
}

// Top the window back up once we have consumed enough. We relay data
// onward the instant it arrives, so "consumed" is immediate and this
// is really just a periodic credit refresh.
static void maybe_adjust_window(ssh_proto_t *s, uint32_t consumed) {

	uint8_t buf[16];
	ssh_wr w;

	if (s->local_window > consumed) s->local_window -= consumed;
	else s->local_window = 0;

	if (s->local_window > LOCAL_WINDOW - WINDOW_REFILL) return;

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_WINDOW_ADJUST);
	ssh_wr_u32(&w, s->remote_chan);
	ssh_wr_u32(&w, LOCAL_WINDOW - s->local_window);

	if (send_packet(s, buf, ssh_wr_len(&w)))
		s->local_window = LOCAL_WINDOW;

}

// -- dispatch --

// Channel traffic, valid from CHANNEL_OPEN_CONFIRMATION onward.
//
// Split out of the ST_OPEN case because these messages are NOT
// confined to ST_OPEN. A server is free to send a window adjust, or
// the shell's first output, before or between the replies to our
// pty-req and shell requests -- and treating one of those as "the
// reply" is how this client used to decide the server had refused to
// start a shell when it had done nothing of the kind.
//
// Returns true if `msg` was a channel message and has been dealt with.
static bool handle_channel_msg(ssh_proto_t *s, uint8_t msg,
	const uint8_t *pl, uint32_t len) {

	ssh_rd r;

	switch (msg) {

	case MSG_CHANNEL_DATA: {
		const uint8_t *d;
		uint32_t d_n;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		ssh_rd_u32(&r);
		d = ssh_rd_string(&r, &d_n);
		if (r.bad) { fail(s, "ssh: malformed channel data"); return true; }
		if (d && d_n) s->event(s->user, SSH_EV_DATA, d, d_n, NULL);
		maybe_adjust_window(s, d_n);
		return true;
	}

	case MSG_CHANNEL_EXTENDED_DATA: {
		const uint8_t *d;
		uint32_t d_n;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		ssh_rd_u32(&r);
		ssh_rd_u32(&r);					// data type code (stderr)
		d = ssh_rd_string(&r, &d_n);
		if (r.bad) return true;
		if (d && d_n) s->event(s->user, SSH_EV_BANNER, d, d_n, NULL);
		maybe_adjust_window(s, d_n);
		return true;
	}

	case MSG_CHANNEL_WINDOW_ADJUST:
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		ssh_rd_u32(&r);
		s->remote_window += ssh_rd_u32(&r);
		return true;

	case MSG_CHANNEL_EOF:
		return true;

	case MSG_CHANNEL_CLOSE:
		fail(s, "ssh: session ended");
		return true;

	case MSG_CHANNEL_REQUEST:
		// exit-status and friends. Nothing to do, and want_reply is
		// almost never set on these.
		return true;

	default:
		return false;

	}

}

static void handle_packet(ssh_proto_t *s, const uint8_t *pl, uint32_t len) {

	ssh_rd r;
	uint8_t msg;

	if (!len) return;
	msg = pl[0];

	// Messages a server can send at ANY time, handled before the state
	// machine gets a look in. Real servers send all of these
	// unprompted and a client that treats them as protocol errors
	// fails against OpenSSH in normal operation.
	switch (msg) {

	case MSG_IGNORE:
	case MSG_DEBUG:
	case MSG_UNIMPLEMENTED:
	case MSG_EXT_INFO:
		return;

	case MSG_DISCONNECT: {
		const uint8_t *desc;
		uint32_t desc_n;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		ssh_rd_u32(&r);					// reason code
		desc = ssh_rd_string(&r, &desc_n);
		if (!r.bad && desc && desc_n) {
			s->event(s->user, SSH_EV_BANNER, desc, desc_n, NULL);
		}
		fail(s, "ssh: server closed the connection");
		return;
	}

	case MSG_GLOBAL_REQUEST: {
		// OpenSSH sends hostkeys-00@openssh.com right after auth. We
		// implement no global request, but a request with want_reply
		// set MUST be answered or the server waits for us.
		uint8_t reply = MSG_REQUEST_FAILURE;
		const uint8_t *name;
		uint32_t name_n;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		name = ssh_rd_string(&r, &name_n);
		(void)name;
		if (!r.bad && ssh_rd_bool(&r)) send_packet(s, &reply, 1);
		return;
	}

	case MSG_USERAUTH_BANNER: {
		const uint8_t *txt;
		uint32_t txt_n;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		txt = ssh_rd_string(&r, &txt_n);
		if (!r.bad && txt && txt_n)
			s->event(s->user, SSH_EV_BANNER, txt, txt_n, NULL);
		return;
	}

	case MSG_KEXINIT:
		// A KEXINIT arriving while the session is open is a
		// server-initiated rekey -- OpenSSH forces one after an hour
		// or a gigabyte. Answer with our own and run the exchange
		// again; the channel stays up throughout.
		if (s->state == ST_OPEN) {
			status(s, "ssh: rekeying");
			s->rekeying = true;
			ssh_sha256_init(&s->hash);
			// V_C and V_S are unchanged and go in again, in order.
			{
				uint8_t l[4];
				uint32_t n = (uint32_t)strlen(CLIENT_ID);
				l[0]=0;l[1]=0;l[2]=(uint8_t)(n>>8);l[3]=(uint8_t)n;
				ssh_sha256_update(&s->hash, l, 4);
				ssh_sha256_update(&s->hash, CLIENT_ID, n);
				n = s->v_s_len;
				l[2]=(uint8_t)(n>>8);l[3]=(uint8_t)n;
				ssh_sha256_update(&s->hash, l, 4);
				ssh_sha256_update(&s->hash, s->v_s, n);
			}
			if (!send_kexinit(s)) return;
			handle_kexinit(s, pl, len);
			return;
		}
		break;

	default:
		break;
	}

	switch (s->state) {

	case ST_KEXINIT:
		if (msg != MSG_KEXINIT) { fail(s, "ssh: expected KEXINIT"); return; }
		handle_kexinit(s, pl, len);
		return;

	case ST_KEXREPLY:
		if (msg != MSG_KEX_ECDH_REPLY) {
			fail(s, "ssh: expected KEX_ECDH_REPLY");
			return;
		}
		handle_kex_reply(s, pl, len);
		return;

	case ST_NEWKEYS:
		if (msg != MSG_NEWKEYS) { fail(s, "ssh: expected NEWKEYS"); return; }
		// This packet was read under the OLD receive key; the new one
		// applies from the next one onward. See install_rx_key().
		install_rx_key(s);
		if (s->rekeying) {
			// Nothing else changes: the channel, the auth state and
			// the sequence numbers all carry on.
			s->rekeying = false;
			s->state = ST_OPEN;
			return;
		}
		status(s, "ssh: authenticating");
		if (!send_service_request(s)) return;
		s->state = ST_SERVICE;
		return;

	case ST_SERVICE:
		if (msg != MSG_SERVICE_ACCEPT) {
			fail(s, "ssh: server refused the userauth service");
			return;
		}
		if (!send_auth_none(s)) return;
		s->state = ST_AUTH;
		return;

	case ST_AUTH:
		if (msg == MSG_USERAUTH_SUCCESS) {
			status(s, "ssh: opening shell");
			if (!send_channel_open(s)) return;
			s->state = ST_CHANNEL;
			return;
		}
		if (msg == MSG_USERAUTH_FAILURE) {
			const uint8_t *methods;
			uint32_t methods_n;
			ssh_rd_init(&r, pl, len);
			ssh_rd_u8(&r);
			methods = ssh_rd_string(&r, &methods_n);
			if (r.bad) { fail(s, "ssh: malformed USERAUTH_FAILURE"); return; }

			if (!ssh_namelist_has(methods, methods_n, "password")) {
				// Publickey-only is the common case here and deserves
				// to be named: the user needs to know it is the
				// server's policy, not a wrong password.
				fail(s, "ssh: server does not accept password authentication");
				return;
			}

			if (++s->auth_attempts > 3) {
				fail(s, "ssh: too many authentication failures");
				return;
			}

			s->state = ST_AUTH_WAIT_PW;
			s->event(s->user, SSH_EV_NEED_PASSWORD, NULL, 0,
				s->auth_attempts > 1 ? "Permission denied, please try again."
				                     : NULL);
			return;
		}
		fail(s, "ssh: unexpected message during authentication");
		return;

	case ST_CHANNEL:
		if (msg == MSG_CHANNEL_OPEN_CONFIRM) {
			ssh_rd_init(&r, pl, len);
			ssh_rd_u8(&r);
			ssh_rd_u32(&r);						// our channel id
			s->remote_chan = ssh_rd_u32(&r);
			s->remote_window = ssh_rd_u32(&r);
			s->remote_max_packet = ssh_rd_u32(&r);
			if (r.bad) { fail(s, "ssh: malformed channel confirmation"); return; }
			if (!send_pty_req(s)) return;
			s->state = ST_PTY;
			return;
		}
		fail(s, "ssh: server refused to open a session channel");
		return;

	case ST_PTY:
		// Only a request reply moves this on. A server without a pty,
		// or one that refuses, is still usable -- the shell just will
		// not be interactive -- so CHANNEL_FAILURE is not fatal here
		// and both answers lead to the same place.
		if (msg == MSG_CHANNEL_SUCCESS || msg == MSG_CHANNEL_FAILURE) {
			if (!send_shell_req(s)) return;
			s->state = ST_SHELL;
			return;
		}
		if (handle_channel_msg(s, msg, pl, len)) return;
		return;

	case ST_SHELL:
		if (msg == MSG_CHANNEL_SUCCESS) {
			s->state = ST_OPEN;
			s->event(s->user, SSH_EV_READY, NULL, 0, NULL);
			return;
		}
		if (msg == MSG_CHANNEL_FAILURE) {
			fail(s, "ssh: server refused to start a shell");
			return;
		}
		// Anything else is ordinary channel traffic that happens to
		// have arrived before the reply -- window adjusts and the
		// shell's own first output both do this. Handle it and keep
		// waiting. Treating it as a refusal is precisely the bug that
		// made a perfectly good OpenSSH session report "server refused
		// to start a shell".
		if (handle_channel_msg(s, msg, pl, len)) return;
		// Genuinely unexpected: name the number, because the whole
		// point of getting this far is to be able to say what arrived.
		printf("ssh: unexpected message %d while awaiting shell reply\n",
			(int)msg);
		return;

	case ST_OPEN:
		if (handle_channel_msg(s, msg, pl, len)) return;
		return;

	default:
		return;
	}

}

// -- receive framing --

static void feed_version(ssh_proto_t *s, const uint8_t **pp, uint32_t *pn) {

	const uint8_t *p = *pp;
	uint32_t n = *pn;

	// The server may send any number of banner lines before its real
	// identification; only the line starting "SSH-" counts (RFC 4253
	// section 4.2). Lines are CRLF terminated, but tolerating a bare
	// LF costs nothing and some servers do it.
	while (n) {

		if (s->v_s_len >= sizeof(s->v_s) - 1) {
			fail(s, "ssh: server identification line too long");
			return;
		}

		if (*p == '\n') {
			s->v_s[s->v_s_len] = 0;
			if (s->v_s_len && s->v_s[s->v_s_len - 1] == '\r')
				s->v_s[--s->v_s_len] = 0;

			if (s->v_s_len >= 4 && !memcmp(s->v_s, "SSH-", 4)) {

				if (memcmp(s->v_s, "SSH-2.0", 7) &&
					memcmp(s->v_s, "SSH-1.99", 8)) {
					fail(s, "ssh: server speaks an unsupported SSH version");
					return;
				}

				p++; n--;

				// V_C then V_S into the exchange hash, in that order,
				// each as a string without its CR/LF.
				{
					uint8_t l[4];
					uint32_t m = (uint32_t)strlen(CLIENT_ID);
					ssh_sha256_init(&s->hash);
					l[0]=0;l[1]=0;l[2]=(uint8_t)(m>>8);l[3]=(uint8_t)m;
					ssh_sha256_update(&s->hash, l, 4);
					ssh_sha256_update(&s->hash, CLIENT_ID, m);
					m = s->v_s_len;
					l[2]=(uint8_t)(m>>8);l[3]=(uint8_t)m;
					ssh_sha256_update(&s->hash, l, 4);
					ssh_sha256_update(&s->hash, s->v_s, m);
				}

				status(s, "ssh: negotiating");
				if (!send_kexinit(s)) return;
				s->state = ST_KEXINIT;

				*pp = p; *pn = n;
				return;

			}

			// A banner line, not the identification. Discard and keep
			// looking.
			s->v_s_len = 0;
			p++; n--;
			continue;
		}

		s->v_s[s->v_s_len++] = (char)*p;
		p++; n--;

	}

	*pp = p; *pn = n;

}

void ssh_proto_feed(ssh_proto_t *s, const uint8_t *data, uint32_t len) {

	uint32_t take;

	while (len && s->state != ST_CLOSED) {

		if (s->state == ST_VERSION) {
			feed_version(s, &data, &len);
			continue;
		}

		// Stalled on an owner decision. The bytes still have to be
		// taken -- see ssh_proto.h's `pend` comment on why returning
		// without consuming loses them permanently.
		if (s->state == ST_HOSTKEY || s->state == ST_AUTH_WAIT_PW) {
			if (s->pend_len + len > sizeof(s->pend)) {
				fail(s, "ssh: server sent too much while awaiting a decision");
				return;
			}
			memcpy(s->pend + s->pend_len, data, len);
			s->pend_len += len;
			return;
		}

		if (!s->rx_header_done) {

			take = SSH_AEAD_LEN_LEN - s->rx_len;
			if (take > len) take = len;
			memcpy(s->rx + s->rx_len, data, take);
			s->rx_len += take;
			data += take; len -= take;

			if (s->rx_len < SSH_AEAD_LEN_LEN) return;

			s->rx_payload = ssh_aead_peek_len(&s->rx_cipher, s->rx);

			// The length came off the wire before anything was
			// authenticated, so it is entirely attacker controlled.
			// Both bounds matter: zero would loop forever, and the
			// upper bound is what stops a hostile length from being
			// used to size a copy.
			if (s->rx_payload < 8 || s->rx_payload > 262144) {
				fail(s, "ssh: implausible packet length");
				return;
			}

			s->rx_discard = (s->rx_payload > SSH_MAX_PACKET);
			s->rx_need = SSH_AEAD_LEN_LEN + s->rx_payload +
				(s->rx_cipher.active ? SSH_AEAD_TAG_LEN : 0);
			s->rx_header_done = true;

		}

		if (s->rx_discard) {
			// An oversize packet is not fatal. We cannot authenticate
			// what we do not hold, so its contents are lost -- but a
			// long USERAUTH_BANNER or DEBUG is exactly the kind of
			// thing that is safe to lose, and killing the session over
			// it would be worse. Consume and carry on; the sequence
			// number still advances so the stream stays in step.
			take = s->rx_need - s->rx_len;
			if (take > len) take = len;
			s->rx_len += take;
			data += take; len -= take;
			if (s->rx_len < s->rx_need) return;
			s->rx_cipher.seq++;
			s->rx_len = 0;
			s->rx_header_done = false;
			s->rx_discard = false;
			continue;
		}

		take = s->rx_need - s->rx_len;
		if (take > len) take = len;
		memcpy(s->rx + s->rx_len, data, take);
		s->rx_len += take;
		data += take; len -= take;

		if (s->rx_len < s->rx_need) return;

		if (!ssh_aead_open(&s->rx_cipher, s->rx, s->rx_payload)) {
			// A bad tag is unrecoverable and is never a transient
			// glitch: TCP has already guaranteed the bytes arrived
			// intact, so this means forgery, a key mismatch, or a bug.
			fail(s, "ssh: packet authentication failed");
			return;
		}

		{
			uint8_t pad = s->rx[SSH_AEAD_LEN_LEN];
			uint32_t payload_len;

			if ((uint32_t)pad + 1 > s->rx_payload) {
				fail(s, "ssh: bad packet padding");
				return;
			}
			payload_len = s->rx_payload - pad - 1;

			handle_packet(s, s->rx + SSH_AEAD_LEN_LEN + 1, payload_len);
		}

		s->rx_len = 0;
		s->rx_header_done = false;

	}

}

// -- public API --

void ssh_proto_init(ssh_proto_t *s, void *user, ssh_write_fn write,
	ssh_event_fn event, ssh_random_fn random, const char *username) {

	memset(s, 0, sizeof(*s));
	s->user = user;
	s->write = write;
	s->event = event;
	s->random = random;
	s->state = ST_VERSION;

	if (username) {
		strncpy(s->username, username, sizeof(s->username) - 1);
		s->username[sizeof(s->username) - 1] = 0;
	}

	ssh_cipher_init(&s->tx_cipher);
	ssh_cipher_init(&s->rx_cipher);

	// RFC 4253 requires the identification to end CRLF. Sent before
	// anything is known about the server, which is why the version
	// exchange is the one part of SSH that is not a binary packet.
	s->write(s->user, (const uint8_t *)CLIENT_ID "\r\n",
		(uint32_t)strlen(CLIENT_ID) + 2);

}

void ssh_proto_set_user(ssh_proto_t *s, const char *username) {
	strncpy(s->username, username, sizeof(s->username) - 1);
	s->username[sizeof(s->username) - 1] = 0;
}

void ssh_proto_password(ssh_proto_t *s, const char *password) {

	if (s->state != ST_AUTH_WAIT_PW) return;

	strncpy(s->password, password, sizeof(s->password) - 1);
	s->password[sizeof(s->password) - 1] = 0;
	s->have_password = true;

	if (send_auth_password(s)) {
		s->state = ST_AUTH;
		drain_pending(s);
	}

}

bool ssh_proto_send(ssh_proto_t *s, const uint8_t *data, uint32_t len) {

	uint8_t buf[LOCAL_MAX_PACKET + 32];
	ssh_wr w;

	if (s->state != ST_OPEN) return false;
	if (len > s->remote_window) return false;
	if (len > LOCAL_MAX_PACKET) return false;

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_DATA);
	ssh_wr_u32(&w, s->remote_chan);
	ssh_wr_string(&w, data, len);

	if (!ssh_wr_ok(&w)) return false;
	if (!send_packet(s, buf, ssh_wr_len(&w))) return false;

	s->remote_window -= len;
	return true;

}

void ssh_proto_disconnect(ssh_proto_t *s, const char *why) {

	uint8_t buf[128];
	ssh_wr w;

	if (s->state == ST_CLOSED) return;

	if (s->state == ST_OPEN) {
		ssh_wr_init(&w, buf, sizeof(buf));
		ssh_wr_u8(&w, MSG_DISCONNECT);
		ssh_wr_u32(&w, 11);				// SSH_DISCONNECT_BY_APPLICATION
		ssh_wr_cstr(&w, why ? why : "client exiting");
		ssh_wr_cstr(&w, "");
		send_packet(s, buf, ssh_wr_len(&w));
	}

	ssh_cipher_wipe(&s->tx_cipher);
	ssh_cipher_wipe(&s->rx_cipher);
	crypto_wipe(s->eph_secret, sizeof(s->eph_secret));
	crypto_wipe((uint8_t *)s->password, sizeof(s->password));

	s->state = ST_CLOSED;
	s->event(s->user, SSH_EV_CLOSED, NULL, 0, why ? why : "ssh: disconnected");

}

bool ssh_proto_is_open(const ssh_proto_t *s) {
	return s->state == ST_OPEN;
}
