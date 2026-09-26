/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The SSH-2 server engine. See ssh_server.h; ssh_proto.c is the client,
 * and most of the hard-won detail here -- the padding rule before and
 * after NEWKEYS, the mpint-encoded shared secret, installing the two
 * directions' keys at different moments -- is documented there first.
 */
#include <string.h>
#include <stdio.h>

#include "ssh_server.h"
#include "ssh_wire.h"
#include "../../../ext/monocypher/monocypher.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

#define MSG_DISCONNECT            1
#define MSG_IGNORE                2
#define MSG_UNIMPLEMENTED         3
#define MSG_DEBUG                 4
#define MSG_SERVICE_REQUEST       5
#define MSG_SERVICE_ACCEPT        6
#define MSG_KEXINIT              20
#define MSG_NEWKEYS              21
#define MSG_KEX_ECDH_INIT        30
#define MSG_KEX_ECDH_REPLY       31
#define MSG_USERAUTH_REQUEST     50
#define MSG_USERAUTH_FAILURE     51
#define MSG_USERAUTH_SUCCESS     52
#define MSG_USERAUTH_PK_OK       60
#define MSG_EXT_INFO              7
#define MSG_GLOBAL_REQUEST       80
#define MSG_REQUEST_FAILURE      82
#define MSG_CHANNEL_OPEN         90
#define MSG_CHANNEL_OPEN_CONFIRM 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA         94
#define MSG_CHANNEL_EXTENDED_DATA 95
#define MSG_CHANNEL_EOF          96
#define MSG_CHANNEL_CLOSE        97
#define MSG_CHANNEL_REQUEST      98
#define MSG_CHANNEL_SUCCESS      99
#define MSG_CHANNEL_FAILURE     100

// RFC 4253 section 11.1 reason codes we use
#define DISC_PROTOCOL_ERROR       2
#define DISC_KEY_EXCHANGE_FAILED  3
#define DISC_BY_APPLICATION      11
#define DISC_NO_MORE_AUTH        14

enum {
	ST_VERSION,         // waiting for the client's identification line
	ST_KEXINIT,         // waiting for its KEXINIT
	ST_ECDH,            // waiting for KEX_ECDH_INIT
	ST_NEWKEYS,         // our NEWKEYS sent, waiting for its
	ST_SERVICE,         // waiting for SERVICE_REQUEST ssh-userauth
	ST_AUTH,            // authenticating
	ST_OPEN,            // authenticated: channels
	ST_CLOSED,
};

#define SERVER_ID "SSH-2.0-Zeitlos_netserve"

// What we offer, in our KEXINIT. The kex list ends with the strict-kex
// marker, which is a flag, not an algorithm (see ssh_server.h). The MAC
// list is never used -- the AEAD authenticates -- but the negotiation
// still has to find a name in common, and OpenSSH's client lists no
// "none": offering the usual names is what lets it proceed.
#define KEX_OFFER  "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com"
#define ALG_HOSTKEY "ssh-ed25519"
#define ALG_CIPHER "chacha20-poly1305@openssh.com"
#define MAC_OFFER  "hmac-sha2-256-etm@openssh.com,hmac-sha2-256,umac-128-etm@openssh.com,none"
#define ALG_COMP   "none"

#define MAX_AUTH_FAILURES 3

// -- small helpers --

static void log_line(ssh_server_t *s, const char *t) {
	s->event(s->user, SSHS_EV_LOG, 0, 0, t);
}

static void closed(ssh_server_t *s, const char *why) {
	if (s->state == ST_CLOSED) return;
	s->state = ST_CLOSED;
	crypto_wipe(s->eph_secret, sizeof(s->eph_secret));
	crypto_wipe(s->k_mpint, sizeof(s->k_mpint));
	s->event(s->user, SSHS_EV_CLOSED, 0, 0, why);
}

static void put_u32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void hash_string(ssh_server_t *s, const void *d, uint32_t n) {
	uint8_t l[4];
	put_u32(l, n);
	ssh_sha256_update(&s->hash, l, 4);
	ssh_sha256_update(&s->hash, d, n);
}

// -- packets --

// ssh_proto.c's send_packet(), from the server's side: the same
// padding rule (ALL FOUR fields to a multiple of 8 before NEWKEYS, the
// length excluded after), the same tag only once the cipher is live.
static bool send_packet(ssh_server_t *s, const uint8_t *payload, uint32_t len) {
	uint32_t aadlen = s->tx_cipher.active ? SSH_AEAD_LEN_LEN : 0;
	uint32_t base = (SSH_AEAD_LEN_LEN - aadlen) + 1 + len;
	uint32_t pad = 8 - (base % 8), total;
	uint8_t *p = s->tx;

	if (s->state == ST_CLOSED) return false;
	if (pad < 4) pad += 8;
	total = 1 + len + pad;
	if (total + SSH_AEAD_LEN_LEN + SSH_AEAD_TAG_LEN > sizeof(s->tx)) {
		closed(s, "ssh: outbound packet too large");
		return false;
	}
	put_u32(p, total);
	p[4] = (uint8_t)pad;
	memmove(p + 5, payload, len);           // payload may be built in tx itself
	s->random(s->user, p + 5 + len, pad);
	ssh_aead_seal(&s->tx_cipher, p, total);
	if (!s->write(s->user, p, SSH_AEAD_LEN_LEN + total +
			(s->tx_cipher.active ? SSH_AEAD_TAG_LEN : 0))) {
		closed(s, "ssh: could not send");
		return false;
	}
	return true;
}

void sshs_disconnect(ssh_server_t *s, const char *why) {
	uint8_t buf[160];
	ssh_wr w;
	if (s->state == ST_CLOSED) return;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_DISCONNECT);
	ssh_wr_u32(&w, DISC_BY_APPLICATION);
	ssh_wr_cstr(&w, why);
	ssh_wr_cstr(&w, "");
	if (ssh_wr_ok(&w)) send_packet(s, buf, ssh_wr_len(&w));
	closed(s, why);
}

static void disconnect_code(ssh_server_t *s, uint32_t code, const char *why) {
	uint8_t buf[160];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_DISCONNECT);
	ssh_wr_u32(&w, code);
	ssh_wr_cstr(&w, why);
	ssh_wr_cstr(&w, "");
	if (ssh_wr_ok(&w)) send_packet(s, buf, ssh_wr_len(&w));
	closed(s, why);
}

// -- key exchange --

static bool send_kexinit(ssh_server_t *s) {
	ssh_wr w;
	uint8_t cookie[16];

	ssh_wr_init(&w, s->i_s, sizeof(s->i_s));
	ssh_wr_u8(&w, MSG_KEXINIT);
	s->random(s->user, cookie, sizeof(cookie));
	ssh_wr_bytes(&w, cookie, sizeof(cookie));
	ssh_wr_cstr(&w, KEX_OFFER);
	ssh_wr_cstr(&w, ALG_HOSTKEY);
	ssh_wr_cstr(&w, ALG_CIPHER);
	ssh_wr_cstr(&w, ALG_CIPHER);
	ssh_wr_cstr(&w, MAC_OFFER);
	ssh_wr_cstr(&w, MAC_OFFER);
	ssh_wr_cstr(&w, ALG_COMP);
	ssh_wr_cstr(&w, ALG_COMP);
	ssh_wr_cstr(&w, "");
	ssh_wr_cstr(&w, "");
	ssh_wr_bool(&w, false);
	ssh_wr_u32(&w, 0);
	if (!ssh_wr_ok(&w)) { closed(s, "ssh: kexinit too large"); return false; }
	// Kept: the exchange hash wants V_C, V_S, I_C, I_S in that order,
	// and the client's KEXINIT has not arrived yet.
	s->i_s_len = ssh_wr_len(&w);
	s->our_kexinit_sent = true;
	s->kexing = true;
	return send_packet(s, s->i_s, s->i_s_len);
}

// The first name in a comma list, for the guessed-packet rule.
static bool first_is(const uint8_t *list, uint32_t n, const char *name) {
	uint32_t k = (uint32_t)strlen(name);
	return n >= k && !memcmp(list, name, k) && (n == k || list[k] == ',');
}

static void handle_kexinit(ssh_server_t *s, const uint8_t *pl, uint32_t len, uint32_t seq) {
	ssh_rd r;
	const uint8_t *kex, *hk, *c_cs, *c_sc, *m_cs, *m_sc, *z_cs, *z_sc;
	uint32_t kex_n, hk_n, c_cs_n, c_sc_n, m_cs_n, m_sc_n, z_cs_n, z_sc_n;
	bool guess;

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);
	ssh_rd_skip(&r, 16);
	kex = ssh_rd_string(&r, &kex_n);
	hk = ssh_rd_string(&r, &hk_n);
	c_cs = ssh_rd_string(&r, &c_cs_n);
	c_sc = ssh_rd_string(&r, &c_sc_n);
	m_cs = ssh_rd_string(&r, &m_cs_n);
	m_sc = ssh_rd_string(&r, &m_sc_n);
	z_cs = ssh_rd_string(&r, &z_cs_n);
	z_sc = ssh_rd_string(&r, &z_sc_n);
	ssh_rd_skip_string(&r);
	ssh_rd_skip_string(&r);
	guess = ssh_rd_bool(&r);
	if (r.bad) { disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: malformed KEXINIT"); return; }

	bool kex_ok = ssh_namelist_has(kex, kex_n, "curve25519-sha256") ||
		ssh_namelist_has(kex, kex_n, "curve25519-sha256@libssh.org");
	if (!kex_ok || !ssh_namelist_has(hk, hk_n, ALG_HOSTKEY) ||
			!ssh_namelist_has(c_cs, c_cs_n, ALG_CIPHER) || !ssh_namelist_has(c_sc, c_sc_n, ALG_CIPHER) ||
			!ssh_namelist_has(z_cs, z_cs_n, ALG_COMP) || !ssh_namelist_has(z_sc, z_sc_n, ALG_COMP)) {
		disconnect_code(s, DISC_KEY_EXCHANGE_FAILED,
			"ssh: no algorithms in common (this server has curve25519, ed25519, chacha20-poly1305)");
		return;
	}
	(void)m_cs; (void)m_sc; (void)m_cs_n; (void)m_sc_n;    // the AEAD authenticates

	// A guessed kex packet follows, and the guess was not ours: ignore it.
	if (guess && !first_is(kex, kex_n, "curve25519-sha256") &&
			!first_is(kex, kex_n, "curve25519-sha256@libssh.org"))
		s->ignore_next = true;

	// RFC 8308: the client takes extensions. It is how server-sig-algs
	// reaches it -- without which OpenSSH will not offer an RSA key.
	if (!s->first_kex_done && ssh_namelist_has(kex, kex_n, "ext-info-c"))
		s->ext_info_c = true;

	// Strict KEX: decided at the first exchange, kept for the rest.
	if (!s->first_kex_done && ssh_namelist_has(kex, kex_n, "kex-strict-c-v00@openssh.com")) {
		s->strict = true;
		// ...and in strict mode the client's KEXINIT must have been its
		// very first packet. Anything before it -- an IGNORE slipped in
		// by someone on the path, Terrapin's opening move -- went by
		// before we knew the mode, so this is where it is caught.
		if (seq != 0) {
			disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: strict kex: KEXINIT was not the first packet");
			return;
		}
	}

	// A rekey the client started: our KEXINIT answers it.
	if (!s->our_kexinit_sent && !send_kexinit(s)) return;

	ssh_sha256_init(&s->hash);
	hash_string(s, s->v_c, s->v_c_len);
	hash_string(s, SERVER_ID, (uint32_t)strlen(SERVER_ID));
	hash_string(s, pl, len);
	hash_string(s, s->i_s, s->i_s_len);
	s->state = ST_ECDH;
}

static void handle_ecdh_init(ssh_server_t *s, const uint8_t *pl, uint32_t len) {
	ssh_rd r;
	const uint8_t *q_c;
	uint32_t q_c_n;
	uint8_t q_s[32], shared[32], zero[32], sig[64], buf[256];
	ssh_wr w;

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);
	q_c = ssh_rd_string(&r, &q_c_n);
	if (r.bad || q_c_n != 32) { disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: malformed KEX_ECDH_INIT"); return; }

	s->random(s->user, s->eph_secret, 32);
	crypto_x25519_public_key(q_s, s->eph_secret);
	crypto_x25519(shared, s->eph_secret, q_c);
	crypto_wipe(s->eph_secret, sizeof(s->eph_secret));

	// A small-order public value drives the result to zero, a secret
	// the attacker knows too. Monocypher leaves the check to us.
	memset(zero, 0, sizeof(zero));
	if (crypto_verify32(shared, zero) == 0) {
		crypto_wipe(shared, sizeof(shared));
		disconnect_code(s, DISC_KEY_EXCHANGE_FAILED, "ssh: degenerate key exchange value");
		return;
	}
	ssh_wr_init(&w, s->k_mpint, sizeof(s->k_mpint));
	ssh_wr_mpint(&w, shared, 32);
	s->k_mpint_len = ssh_wr_len(&w);
	crypto_wipe(shared, sizeof(shared));

	// H = hash(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)
	hash_string(s, s->host_blob, sizeof(s->host_blob));
	hash_string(s, q_c, 32);
	hash_string(s, q_s, 32);
	ssh_sha256_update(&s->hash, s->k_mpint, s->k_mpint_len);
	ssh_sha256_final(&s->hash, s->exchange_hash);
	if (!s->have_session_id) {
		memcpy(s->session_id, s->exchange_hash, SSH_SHA256_DIGEST);
		s->have_session_id = true;
	}

	// The signature over H is what ties this exchange to this host.
	crypto_ed25519_sign(sig, s->host_secret, s->exchange_hash, SSH_SHA256_DIGEST);

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_KEX_ECDH_REPLY);
	ssh_wr_string(&w, s->host_blob, sizeof(s->host_blob));
	ssh_wr_string(&w, q_s, 32);
	ssh_wr_u32(&w, 4 + 11 + 4 + 64);
	ssh_wr_cstr(&w, ALG_HOSTKEY);
	ssh_wr_string(&w, sig, 64);
	if (!ssh_wr_ok(&w) || !send_packet(s, buf, ssh_wr_len(&w))) return;

	// Our NEWKEYS, then our transmit side switches -- the receive side
	// waits for the client's (ssh_proto.c, install_tx_key()).
	uint8_t nk = MSG_NEWKEYS, key[SSH_AEAD_KEY_LEN];
	if (!send_packet(s, &nk, 1)) return;
	ssh_derive_key(key, s->k_mpint, s->k_mpint_len, s->exchange_hash, 'D', s->session_id);
	ssh_cipher_set_key(&s->tx_cipher, key);
	crypto_wipe(key, sizeof(key));
	if (s->strict) s->tx_cipher.seq = 0;
	s->state = ST_NEWKEYS;

	// The first packet after our first NEWKEYS may be EXT_INFO (RFC 8308
	// 2.4): the signature algorithms public keys may use here.
	if (!s->first_kex_done && s->ext_info_c && (s->methods & SSHS_AUTH_PUBLICKEY)) {
		static const char algs[] = "ssh-ed25519,rsa-sha2-256";
		uint8_t buf[80];
		ssh_wr w;
		ssh_wr_init(&w, buf, sizeof(buf));
		ssh_wr_u8(&w, MSG_EXT_INFO);
		ssh_wr_u32(&w, 1);
		ssh_wr_cstr(&w, "server-sig-algs");
		ssh_wr_cstr(&w, algs);
		if (send_packet(s, buf, ssh_wr_len(&w))) log_line(s, "ssh: ext-info: server-sig-algs=ssh-ed25519,rsa-sha2-256");
	}
}

static void handle_newkeys(ssh_server_t *s) {
	uint8_t key[SSH_AEAD_KEY_LEN];
	ssh_derive_key(key, s->k_mpint, s->k_mpint_len, s->exchange_hash, 'C', s->session_id);
	ssh_cipher_set_key(&s->rx_cipher, key);
	crypto_wipe(key, sizeof(key));
	crypto_wipe(s->k_mpint, sizeof(s->k_mpint));
	s->k_mpint_len = 0;
	// This packet was the last under the old keys; the next one is 0.
	if (s->strict) s->rx_cipher.seq = 0;
	s->kexing = false;
	s->our_kexinit_sent = false;
	if (!s->first_kex_done) {
		s->first_kex_done = true;
		s->state = ST_SERVICE;
	} else {
		s->state = s->authed ? ST_OPEN : ST_AUTH;
	}
}

// -- authentication --

// FAILURE names what is offered, so a client knows what to try next.
static void auth_failure(ssh_server_t *s) {
	uint8_t buf[48];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_USERAUTH_FAILURE);
	ssh_wr_cstr(&w, (s->methods & SSHS_AUTH_PUBLICKEY) ?
		((s->methods & SSHS_AUTH_PASSWORD) ? "publickey,password" : "publickey") : "password");
	ssh_wr_bool(&w, false);
	send_packet(s, buf, ssh_wr_len(&w));
}

static bool too_many(ssh_server_t *s) {
	if (++s->auth_failures < MAX_AUTH_FAILURES) return false;
	disconnect_code(s, DISC_NO_MORE_AUTH, "Too many authentication failures");
	return true;
}

static void logged_in(ssh_server_t *s, const char *how) {
	char line[96];
	uint8_t ok = MSG_USERAUTH_SUCCESS;
	if (!send_packet(s, &ok, 1)) return;
	s->authed = true;
	s->state = ST_OPEN;
	snprintf(line, sizeof(line), "ssh: user '%.32s' logged in (%s)", s->username, how);
	log_line(s, line);
}

// "publickey" (RFC 4252 section 7). Without a signature it is a
// question -- would this key do? -- answered from the list alone, and
// not a failed attempt if the answer is no: a client with several keys
// asks about each. With one, the client has signed the session id and
// the request itself; that signature is what logs it in.
static void userauth_publickey(ssh_server_t *s, ssh_rd *r) {
	const uint8_t *alg, *blob, *sig;
	uint32_t alg_n, blob_n, sig_n;
	char algs[24], line[96];
	bool has_sig = ssh_rd_bool(r);

	alg = ssh_rd_string(r, &alg_n);
	blob = ssh_rd_string(r, &blob_n);
	if (r->bad || alg_n >= sizeof(algs)) { auth_failure(s); return; }
	memcpy(algs, alg, alg_n);
	algs[alg_n] = 0;

	if (strcmp(algs, "ssh-ed25519") && strcmp(algs, "rsa-sha2-256")) {
		// rsa-sha2-512, ecdsa, and SHA-1 "ssh-rsa": not offered
		// (server-sig-algs said so), so no answer but "not this one".
		auth_failure(s);
		return;
	}
	if (!s->pk_check || !s->pk_check(s->user, s->username, algs, blob, blob_n)) {
		snprintf(line, sizeof(line), "ssh: %s key for '%.32s' is not in the list", algs, s->username);
		log_line(s, line);
		auth_failure(s);
		return;
	}
	if (!has_sig) {
		uint8_t buf[600];
		snprintf(line, sizeof(line), "ssh: %s key for '%.32s' is listed", algs, s->username);
		log_line(s, line);
		ssh_wr w;
		ssh_wr_init(&w, buf, sizeof(buf));
		ssh_wr_u8(&w, MSG_USERAUTH_PK_OK);
		ssh_wr_string(&w, alg, alg_n);
		ssh_wr_string(&w, blob, blob_n);
		if (ssh_wr_ok(&w)) send_packet(s, buf, ssh_wr_len(&w));
		else auth_failure(s);
		return;
	}
	sig = ssh_rd_string(r, &sig_n);
	if (r->bad) { auth_failure(s); return; }

	// What the client signed: string session_id, byte 50, string user,
	// string "ssh-connection", string "publickey", TRUE, string alg,
	// string blob -- the request itself, bound to this session.
	{
		uint8_t data[SSH_SHA256_DIGEST + 700];
		ssh_wr w;
		ssh_wr_init(&w, data, sizeof(data));
		ssh_wr_string(&w, s->session_id, SSH_SHA256_DIGEST);
		ssh_wr_u8(&w, MSG_USERAUTH_REQUEST);
		ssh_wr_cstr(&w, s->username);
		ssh_wr_cstr(&w, "ssh-connection");
		ssh_wr_cstr(&w, "publickey");
		ssh_wr_bool(&w, true);
		ssh_wr_string(&w, alg, alg_n);
		ssh_wr_string(&w, blob, blob_n);
		if (ssh_wr_ok(&w) && s->pk_verify &&
				s->pk_verify(s->user, algs, blob, blob_n, sig, sig_n, data, ssh_wr_len(&w))) {
			logged_in(s, algs);
			return;
		}
	}
	snprintf(line, sizeof(line), "ssh: %s signature for '%.32s' did not verify", algs, s->username);
	log_line(s, line);
	if (!too_many(s)) auth_failure(s);
}

static void handle_userauth(ssh_server_t *s, const uint8_t *pl, uint32_t len) {
	ssh_rd r;
	const uint8_t *user, *service, *method, *pw;
	uint32_t user_n, service_n, method_n, pw_n;

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);
	user = ssh_rd_string(&r, &user_n);
	service = ssh_rd_string(&r, &service_n);
	method = ssh_rd_string(&r, &method_n);
	if (r.bad || !ssh_str_eq(service, service_n, "ssh-connection")) {
		disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: bad userauth request");
		return;
	}
	if (user_n > sizeof(s->username) - 1) user_n = sizeof(s->username) - 1;
	memcpy(s->username, user, user_n);
	s->username[user_n] = 0;

	if (ssh_str_eq(method, method_n, "publickey") && (s->methods & SSHS_AUTH_PUBLICKEY)) {
		userauth_publickey(s, &r);
		return;
	}
	if (!ssh_str_eq(method, method_n, "password") || !(s->methods & SSHS_AUTH_PASSWORD)) {
		// "none" is how a client asks what we take; anything else is
		// a method we do not offer. Either way: the list.
		auth_failure(s);
		return;
	}
	bool change = ssh_rd_bool(&r);
	pw = ssh_rd_string(&r, &pw_n);
	if (r.bad || change) { auth_failure(s); return; }

	int rc = s->auth(s->user, s->username, pw, pw_n);
	if (rc == SSHS_AUTH_OK) {
		logged_in(s, "password");
		return;
	}
	if (rc == SSHS_AUTH_REFUSE) {
		disconnect_code(s, DISC_NO_MORE_AUTH, "Too many attempts -- try again later");
		return;
	}
	if (!too_many(s)) auth_failure(s);
}

// -- the channel --

static void chan_reply(ssh_server_t *s, uint8_t msg) {
	uint8_t buf[8];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, msg);
	ssh_wr_u32(&w, s->remote_chan);
	send_packet(s, buf, ssh_wr_len(&w));
}

static void handle_channel_open(ssh_server_t *s, const uint8_t *pl, uint32_t len) {
	ssh_rd r;
	const uint8_t *type;
	uint32_t type_n, sender, window, maxp;
	uint8_t buf[80];
	ssh_wr w;

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);
	type = ssh_rd_string(&r, &type_n);
	sender = ssh_rd_u32(&r);
	window = ssh_rd_u32(&r);
	maxp = ssh_rd_u32(&r);
	if (r.bad) { disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: bad channel open"); return; }

	if (s->chan_open || !ssh_str_eq(type, type_n, "session")) {
		ssh_wr_init(&w, buf, sizeof(buf));
		ssh_wr_u8(&w, MSG_CHANNEL_OPEN_FAILURE);
		ssh_wr_u32(&w, sender);
		ssh_wr_u32(&w, s->chan_open ? 4 : 3);        // resource shortage / unknown type
		ssh_wr_cstr(&w, s->chan_open ? "one session per connection" : "only session channels");
		ssh_wr_cstr(&w, "");
		send_packet(s, buf, ssh_wr_len(&w));
		return;
	}
	s->chan_open = true;
	s->remote_chan = sender;
	s->remote_window = window;
	s->remote_max = maxp;
	s->local_window = SSHS_LOCAL_WINDOW;
	s->local_credit = 0;

	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_OPEN_CONFIRM);
	ssh_wr_u32(&w, sender);
	ssh_wr_u32(&w, 0);                          // our channel number
	ssh_wr_u32(&w, SSHS_LOCAL_WINDOW);
	ssh_wr_u32(&w, SSHS_LOCAL_WINDOW);          // max packet: no more than the window
	send_packet(s, buf, ssh_wr_len(&w));
}

static void handle_channel_request(ssh_server_t *s, const uint8_t *pl, uint32_t len) {
	ssh_rd r;
	const uint8_t *type;
	uint32_t type_n;
	bool want;

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);
	ssh_rd_u32(&r);
	type = ssh_rd_string(&r, &type_n);
	want = ssh_rd_bool(&r);
	if (r.bad) { disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: bad channel request"); return; }

	if (ssh_str_eq(type, type_n, "pty-req")) {
		// The ports are a fixed-size terminal; the client's size and
		// modes are noted and not needed.
		if (want) chan_reply(s, MSG_CHANNEL_SUCCESS);
	} else if (ssh_str_eq(type, type_n, "shell") && !s->shell) {
		s->shell = true;
		if (want) chan_reply(s, MSG_CHANNEL_SUCCESS);
		s->event(s->user, SSHS_EV_SHELL, 0, 0, 0);
	} else {
		// exec, subsystem (sftp, scp), env, x11-req, agent forwarding,
		// window-change, keepalive@openssh.com: not taken.
		if (want) chan_reply(s, MSG_CHANNEL_FAILURE);
	}
}

static void handle_channel_data(ssh_server_t *s, const uint8_t *pl, uint32_t len) {
	ssh_rd r;
	const uint8_t *d;
	uint32_t n;

	ssh_rd_init(&r, pl, len);
	ssh_rd_u8(&r);
	ssh_rd_u32(&r);
	d = ssh_rd_string(&r, &n);
	if (r.bad) { disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: bad channel data"); return; }
	if (n > s->local_window) {
		disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: channel data beyond the window");
		return;
	}
	s->local_window -= n;
	if (n) s->event(s->user, SSHS_EV_DATA, d, n, 0);
}

// -- dispatch --

static bool kex_message(uint8_t m) {
	return m == MSG_KEXINIT || m == MSG_NEWKEYS || m == MSG_KEX_ECDH_INIT;
}

static void unimplemented(ssh_server_t *s, uint32_t seq) {
	uint8_t buf[8];
	ssh_wr w;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_UNIMPLEMENTED);
	ssh_wr_u32(&w, seq);
	send_packet(s, buf, ssh_wr_len(&w));
}

static void handle_packet(ssh_server_t *s, const uint8_t *pl, uint32_t len, uint32_t seq) {
	uint8_t m;

	if (!len) { disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: empty packet"); return; }
	m = pl[0];

	// Strict KEX: until the first exchange is over, only its messages.
	// That is what shuts the Terrapin attack out -- nothing can be
	// slipped into, or deleted from, the unauthenticated handshake.
	if (s->strict && !s->first_kex_done && !kex_message(m)) {
		disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: strict kex: unexpected message");
		return;
	}

	if (s->ignore_next && m == MSG_KEX_ECDH_INIT) { s->ignore_next = false; return; }

	switch (m) {
	case MSG_DISCONNECT:
		closed(s, "ssh: the client disconnected");
		return;
	case MSG_IGNORE:
	case MSG_DEBUG:
	case MSG_UNIMPLEMENTED:
		if (s->strict && !s->first_kex_done) break;         // (refused above)
		return;
	case MSG_KEXINIT:
		if (s->state != ST_KEXINIT && s->state != ST_SERVICE && s->state != ST_AUTH &&
				s->state != ST_OPEN) break;
		handle_kexinit(s, pl, len, seq);
		return;
	case MSG_KEX_ECDH_INIT:
		if (s->state != ST_ECDH) break;
		handle_ecdh_init(s, pl, len);
		return;
	case MSG_NEWKEYS:
		if (s->state != ST_NEWKEYS) break;
		handle_newkeys(s);
		return;
	}

	// Anything else during a rekey is a protocol error on the client's
	// part (RFC 4253 section 7.1).
	if (s->kexing) {
		disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: unexpected message during key exchange");
		return;
	}

	switch (m) {
	case MSG_SERVICE_REQUEST: {
		ssh_rd r;
		const uint8_t *name;
		uint32_t n;
		uint8_t buf[32];
		ssh_wr w;
		if (s->state != ST_SERVICE) break;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		name = ssh_rd_string(&r, &n);
		if (r.bad || !ssh_str_eq(name, n, "ssh-userauth")) {
			disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: only ssh-userauth is offered");
			return;
		}
		ssh_wr_init(&w, buf, sizeof(buf));
		ssh_wr_u8(&w, MSG_SERVICE_ACCEPT);
		ssh_wr_cstr(&w, "ssh-userauth");
		if (send_packet(s, buf, ssh_wr_len(&w))) s->state = ST_AUTH;
		return;
	}
	case MSG_USERAUTH_REQUEST:
		if (s->state == ST_OPEN) return;       // already in: ignored (RFC 4252 5.1)
		if (s->state != ST_AUTH) break;
		handle_userauth(s, pl, len);
		return;
	case MSG_GLOBAL_REQUEST: {
		ssh_rd r;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		ssh_rd_skip_string(&r);
		if (ssh_rd_bool(&r) && !r.bad) {
			uint8_t f = MSG_REQUEST_FAILURE;
			send_packet(s, &f, 1);
		}
		return;
	}
	}

	// the connection protocol: only once authenticated
	if (s->state != ST_OPEN) {
		disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: message out of order");
		return;
	}
	switch (m) {
	case MSG_CHANNEL_OPEN:
		handle_channel_open(s, pl, len);
		return;
	case MSG_CHANNEL_REQUEST:
		if (s->chan_open) handle_channel_request(s, pl, len);
		return;
	case MSG_CHANNEL_DATA:
		if (s->chan_open) handle_channel_data(s, pl, len);
		return;
	case MSG_CHANNEL_EXTENDED_DATA:
		return;                                 // stderr from a client: nothing to do
	case MSG_CHANNEL_WINDOW_ADJUST: {
		ssh_rd r;
		ssh_rd_init(&r, pl, len);
		ssh_rd_u8(&r);
		ssh_rd_u32(&r);
		uint32_t add = ssh_rd_u32(&r);
		if (!r.bad) {
			uint64_t w = (uint64_t)s->remote_window + add;
			s->remote_window = w > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w;
		}
		return;
	}
	case MSG_CHANNEL_EOF:
		s->event(s->user, SSHS_EV_EOF, 0, 0, 0);
		return;
	case MSG_CHANNEL_CLOSE:
		if (s->chan_open && !s->chan_close_sent) {
			chan_reply(s, MSG_CHANNEL_CLOSE);
			s->chan_close_sent = true;
		}
		s->chan_open = false;
		closed(s, "ssh: the client closed the session");
		return;
	case MSG_CHANNEL_SUCCESS:
	case MSG_CHANNEL_FAILURE:
		return;                                 // answers to nothing we ask
	}
	unimplemented(s, seq);
}

// -- receive --

static void feed_version(ssh_server_t *s, const uint8_t **pp, uint32_t *pn) {
	const uint8_t *p = *pp;
	uint32_t n = *pn;

	while (n) {
		if (s->v_c_len >= sizeof(s->v_c) - 1) {
			closed(s, "ssh: client identification line too long");
			return;
		}
		if (*p == '\n') {
			s->v_c[s->v_c_len] = 0;
			if (s->v_c_len && s->v_c[s->v_c_len - 1] == '\r') s->v_c[--s->v_c_len] = 0;
			p++; n--;
			// A client sends its identification first (RFC 4253 4.2:
			// only a server may send other lines before it).
			if (s->v_c_len < 8 || memcmp(s->v_c, "SSH-2.0-", 8)) {
				closed(s, "ssh: not an SSH-2.0 client");
				return;
			}
			{
				char line[96];
				snprintf(line, sizeof(line), "ssh: client %.60s", s->v_c);
				log_line(s, line);
			}
			s->state = ST_KEXINIT;
			*pp = p; *pn = n;
			return;
		}
		s->v_c[s->v_c_len++] = (char)*p;
		p++; n--;
	}
	*pp = p; *pn = n;
}

void sshs_feed(ssh_server_t *s, const uint8_t *data, uint32_t len) {
	uint32_t take;

	while (len && s->state != ST_CLOSED) {
		if (s->state == ST_VERSION) {
			feed_version(s, &data, &len);
			continue;
		}
		if (!s->rx_header_done) {
			take = SSH_AEAD_LEN_LEN - s->rx_len;
			if (take > len) take = len;
			memcpy(s->rx + s->rx_len, data, take);
			s->rx_len += take;
			data += take; len -= take;
			if (s->rx_len < SSH_AEAD_LEN_LEN) return;
			s->rx_payload = ssh_aead_peek_len(&s->rx_cipher, s->rx);
			// Unauthenticated, so hostile until proven otherwise. A
			// server must take a 35000-byte packet (RFC 4253 6.1), but
			// nothing a client of this server needs to send is near
			// SSHS_MAX_PACKET -- the channel's max packet is 1 KB.
			if (s->rx_payload < 8 || s->rx_payload > SSHS_MAX_PACKET) {
				disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: packet too large");
				return;
			}
			s->rx_need = SSH_AEAD_LEN_LEN + s->rx_payload +
				(s->rx_cipher.active ? SSH_AEAD_TAG_LEN : 0);
			s->rx_header_done = true;
		}
		take = s->rx_need - s->rx_len;
		if (take > len) take = len;
		memcpy(s->rx + s->rx_len, data, take);
		s->rx_len += take;
		data += take; len -= take;
		if (s->rx_len < s->rx_need) return;

		uint32_t seq = (uint32_t)s->rx_cipher.seq;
		if (!ssh_aead_open(&s->rx_cipher, s->rx, s->rx_payload)) {
			closed(s, "ssh: packet authentication failed");
			return;
		}
		uint8_t pad = s->rx[SSH_AEAD_LEN_LEN];
		if ((uint32_t)pad + 1 > s->rx_payload) {
			disconnect_code(s, DISC_PROTOCOL_ERROR, "ssh: bad padding");
			return;
		}
		s->rx_len = 0;
		s->rx_header_done = false;
		handle_packet(s, s->rx + SSH_AEAD_LEN_LEN + 1, s->rx_payload - pad - 1, seq);
	}
}

// -- the owner's side --

void sshs_init(ssh_server_t *s, const uint8_t host_seed[32], void *user,
		sshs_write_fn write, sshs_event_fn event, sshs_random_fn random, sshs_auth_fn auth) {
	uint8_t seed[32], pub[32];
	ssh_wr w;

	memset(s, 0, sizeof(*s));
	s->user = user;
	s->write = write;
	s->event = event;
	s->random = random;
	s->auth = auth;
	s->methods = SSHS_AUTH_PASSWORD;
	ssh_cipher_init(&s->tx_cipher);
	ssh_cipher_init(&s->rx_cipher);

	memcpy(seed, host_seed, 32);                // key_pair() wipes its seed
	crypto_ed25519_key_pair(s->host_secret, pub, seed);
	ssh_wr_init(&w, s->host_blob, sizeof(s->host_blob));
	ssh_wr_cstr(&w, ALG_HOSTKEY);
	ssh_wr_string(&w, pub, 32);

	s->state = ST_VERSION;
	if (!s->write(s->user, (const uint8_t *)SERVER_ID "\r\n", (uint32_t)strlen(SERVER_ID) + 2)) {
		closed(s, "ssh: could not send");
		return;
	}
	send_kexinit(s);
}

void sshs_set_auth(ssh_server_t *s, uint32_t methods,
		sshs_pk_check_fn check, sshs_pk_verify_fn verify) {
	s->methods = methods;
	s->pk_check = check;
	s->pk_verify = verify;
}

uint32_t sshs_send(ssh_server_t *s, const uint8_t *data, uint32_t len, uint32_t room) {
	uint32_t n = len;
	uint8_t hdr[9];

	if (s->state != ST_OPEN || !s->chan_open || s->kexing || s->chan_eof_sent) return 0;
	if (room <= SSHS_SEND_OVERHEAD) return 0;
	if (n > room - SSHS_SEND_OVERHEAD) n = room - SSHS_SEND_OVERHEAD;
	if (n > s->remote_window) n = s->remote_window;
	if (n > s->remote_max) n = s->remote_max;
	if (n > SSHS_TX_MAX - 64) n = SSHS_TX_MAX - 64;
	if (!n) return 0;

	// Built in place in tx[] after the 5-byte header send_packet() adds.
	hdr[0] = MSG_CHANNEL_DATA;
	put_u32(hdr + 1, s->remote_chan);
	memcpy(s->tx + 5, hdr, 5);
	put_u32(s->tx + 10, n);
	memcpy(s->tx + 14, data, n);
	if (!send_packet(s, s->tx + 5, 9 + n)) return 0;
	s->remote_window -= n;
	return n;
}

void sshs_consumed(ssh_server_t *s, uint32_t n) {
	uint8_t buf[12];
	ssh_wr w;
	if (s->state != ST_OPEN || !s->chan_open) return;
	s->local_credit += n;
	// Re-advertised in halves, not byte by byte; never during a rekey.
	if (s->kexing || s->local_credit < SSHS_LOCAL_WINDOW / 2) return;
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_WINDOW_ADJUST);
	ssh_wr_u32(&w, s->remote_chan);
	ssh_wr_u32(&w, s->local_credit);
	if (send_packet(s, buf, ssh_wr_len(&w))) {
		s->local_window += s->local_credit;
		s->local_credit = 0;
	}
}

void sshs_end(ssh_server_t *s, uint32_t exit_status) {
	uint8_t buf[40];
	ssh_wr w;
	if (s->state == ST_CLOSED) return;
	if (!s->chan_open || s->kexing) { sshs_disconnect(s, "session ended"); return; }
	ssh_wr_init(&w, buf, sizeof(buf));
	ssh_wr_u8(&w, MSG_CHANNEL_REQUEST);
	ssh_wr_u32(&w, s->remote_chan);
	ssh_wr_cstr(&w, "exit-status");
	ssh_wr_bool(&w, false);
	ssh_wr_u32(&w, exit_status);
	if (!send_packet(s, buf, ssh_wr_len(&w))) return;
	if (!s->chan_eof_sent) { chan_reply(s, MSG_CHANNEL_EOF); s->chan_eof_sent = true; }
	if (!s->chan_close_sent) { chan_reply(s, MSG_CHANNEL_CLOSE); s->chan_close_sent = true; }
	// Over now, without waiting for the client's CLOSE in answer:
	// OpenSSH sends one, our own client (ssh_proto.c) does not, and
	// either way the owner closes the connection once these are out.
	s->chan_open = false;
	closed(s, "ssh: session ended");
}

bool sshs_is_closed(const ssh_server_t *s) {
	return s->state == ST_CLOSED;
}

void sshs_host_public(const uint8_t host_seed[32], uint8_t pub[32], char *fp, uint32_t cap) {
	uint8_t seed[32], secret[64], blob[51];
	ssh_wr w;
	memcpy(seed, host_seed, 32);
	crypto_ed25519_key_pair(secret, pub, seed);
	crypto_wipe(secret, sizeof(secret));
	ssh_wr_init(&w, blob, sizeof(blob));
	ssh_wr_cstr(&w, ALG_HOSTKEY);
	ssh_wr_string(&w, pub, 32);
	ssh_fingerprint(fp, cap, blob, sizeof(blob));
}
