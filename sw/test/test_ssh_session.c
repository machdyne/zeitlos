/*
 * Zeitlos -- host test for sw/apps/net/ssh/ssh_proto.c
 *
 * Runs the real client engine against a minimal SSH-2 SERVER
 * implemented here, in the same process, over a pair of byte queues.
 * Nothing is mocked: the server does a real curve25519-sha256 key
 * exchange, signs a real exchange hash with a real Ed25519 host key,
 * and speaks real chacha20-poly1305@openssh.com from NEWKEYS onward.
 *
 * The point is that a wrong byte here prints a diagnosis instead of
 * costing a flash cycle and producing "authentication failed". See
 * ssh_proto.h on why the engine has no TCP dependency -- this file is
 * the reason.
 *
 * Build (from sw/test):
 *   gcc -O2 -I../apps/net/ssh -I../ext/monocypher -o t_session \
 *       test_ssh_session.c ../apps/net/ssh/ssh_proto.c \
 *       ../apps/net/ssh/ssh_wire.c ../apps/net/ssh/ssh_crypto.c \
 *       ../apps/net/ssh/ssh_sha256.c ../ext/monocypher/monocypher.c \
 *       ../ext/monocypher/monocypher-ed25519.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ssh_proto.h"
#include "ssh_wire.h"
#include "ssh_crypto.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

static int failures = 0;
static void ok(const char *name, int cond) {
	printf("  %s %s\n", cond ? "ok  " : "FAIL", name);
	if (!cond) failures++;
}

/* ------------------------------------------------------------------ */
/* byte pipes                                                          */
/* ------------------------------------------------------------------ */

#define PIPE_CAP 65536
typedef struct { uint8_t b[PIPE_CAP]; uint32_t len; } pipe_t;
static pipe_t c2s, s2c;

static void pipe_put(pipe_t *p, const uint8_t *d, uint32_t n) {
	if (p->len + n > PIPE_CAP) { printf("pipe overflow\n"); exit(2); }
	memcpy(p->b + p->len, d, n); p->len += n;
}
static uint32_t pipe_take(pipe_t *p, uint8_t *out, uint32_t max) {
	uint32_t n = p->len < max ? p->len : max;
	memcpy(out, p->b, n);
	memmove(p->b, p->b + n, p->len - n);
	p->len -= n;
	return n;
}

/* ------------------------------------------------------------------ */
/* client side callbacks                                               */
/* ------------------------------------------------------------------ */

static ssh_proto_t cli;
static char last_status[256];
static char last_close[256];
static char last_fp[128];
static int  ready = 0, closed = 0, asked_pw = 0, hostkey_events = 0;
static uint8_t rx_accum[65536]; static uint32_t rx_accum_len;
static uint8_t banner_accum[4096]; static uint32_t banner_len;

/* Deterministic "random" so a failing run reproduces exactly. Not a
   security shortcut -- ssh_proto.h takes randomness as a callback
   precisely so a test can pin it. */
static uint64_t prng = 0x123456789abcdefULL;
static void cli_random(void *u, uint8_t *out, uint32_t n) {
	(void)u;
	for (uint32_t i = 0; i < n; i++) {
		prng = prng * 6364136223846793005ULL + 1442695040888963407ULL;
		out[i] = (uint8_t)(prng >> 33);
	}
}

static bool cli_write(void *u, const uint8_t *d, uint32_t n) {
	(void)u; pipe_put(&c2s, d, n); return true;
}

static void cli_event(void *u, ssh_event_t ev, const uint8_t *d,
                      uint32_t n, const char *text) {
	(void)u;
	switch (ev) {
	case SSH_EV_STATUS:
		snprintf(last_status, sizeof last_status, "%s", text ? text : "");
		break;
	case SSH_EV_HOSTKEY:
		hostkey_events++;
		snprintf(last_fp, sizeof last_fp, "%s", text ? text : "");
		break;
	case SSH_EV_NEED_PASSWORD:
		asked_pw++;
		ssh_proto_password(&cli, "hunter2");
		break;
	case SSH_EV_READY: ready = 1; break;
	case SSH_EV_DATA:
		if (rx_accum_len + n < sizeof rx_accum) {
			memcpy(rx_accum + rx_accum_len, d, n); rx_accum_len += n; }
		break;
	case SSH_EV_BANNER:
		if (banner_len + n < sizeof banner_accum) {
			memcpy(banner_accum + banner_len, d, n); banner_len += n; }
		break;
	case SSH_EV_CLOSED:
		closed = 1;
		snprintf(last_close, sizeof last_close, "%s", text ? text : "");
		break;
	}
}

/* ------------------------------------------------------------------ */
/* server side                                                         */
/* ------------------------------------------------------------------ */

#define SRV_ID "SSH-2.0-TestServer_1.0"

static uint8_t srv_sk[64], srv_pk[32];       /* ed25519 host key */
static uint8_t srv_eph_sk[32], srv_eph_pk[32];
static uint8_t srv_keyblob[4 + 11 + 4 + 32];
static uint32_t srv_keyblob_len;
static ssh_cipher srv_tx, srv_rx;
static ssh_sha256_ctx srv_hash;
static uint8_t srv_H[32], srv_sid[32];
static uint8_t srv_k_mpint[40];  /* 40 not 36 -- see ssh_proto.h */ static uint32_t srv_k_mpint_len;
static uint8_t srv_in[65536]; static uint32_t srv_in_len;
static int srv_state = 0;   /* 0 version, 1 kexinit, 2 ecdh, 3 newkeys, 4 running */
static uint32_t srv_remote_chan;
static int srv_auth_none_seen = 0, srv_pw_ok = 0;
static const char *srv_offer_cipher = "chacha20-poly1305@openssh.com";
static int srv_send_big_banner = 0;
static char srv_v_c[256]; static uint32_t srv_v_c_len;
static int srv_have_sid = 0;

static void srv_out(const uint8_t *d, uint32_t n) { pipe_put(&s2c, d, n); }

static void srv_hash_string(const void *d, uint32_t n) {
	uint8_t l[4] = { (uint8_t)(n>>24),(uint8_t)(n>>16),(uint8_t)(n>>8),(uint8_t)n };
	ssh_sha256_update(&srv_hash, l, 4);
	ssh_sha256_update(&srv_hash, d, n);
}

static void srv_send_packet(const uint8_t *payload, uint32_t len) {
	static uint8_t pkt[70000];
	/* Same phase-dependent rule as the client: before NEWKEYS the
	   4-byte length field counts toward the block alignment; with
	   chacha20-poly1305 it is AAD and does not. The first version of
	   this server used the AEAD rule unconditionally -- exactly the
	   client's bug -- so the two agreed with each other and both were
	   wrong. A real OpenSSH server closed the connection. */
	uint32_t aad = srv_tx.active ? 4 : 0;
	uint32_t pad = 8 - (((4 - aad) + 1 + len) % 8);
	if (pad < 4) pad += 8;
	uint32_t total = 1 + len + pad;
	pkt[0]=(uint8_t)(total>>24);pkt[1]=(uint8_t)(total>>16);
	pkt[2]=(uint8_t)(total>>8);pkt[3]=(uint8_t)total;
	pkt[4]=(uint8_t)pad;
	memcpy(pkt+5, payload, len);
	memset(pkt+5+len, 0x5a, pad);
    ssh_aead_seal(&srv_tx, pkt, total);
	srv_out(pkt, 4 + total + (srv_tx.active ? 16 : 0));
}

static uint8_t srv_kexinit_payload[512];
static uint32_t srv_kexinit_len;

/* The exchange hash covers V_C || V_S || I_C || I_S in that order, so
   the server cannot hash its OWN kexinit at send time -- the client's
   has not arrived yet. It is buffered here and hashed in the right
   place, after I_C, when the client's kexinit turns up. Getting this
   wrong is invisible until the signature check, which is exactly what
   happened on the first run of this test: the CLIENT was right and the
   server was hashing I_S before I_C. */
static void srv_send_kexinit(void) {
	uint8_t buf[512]; ssh_wr w;
	ssh_wr_init(&w, buf, sizeof buf);
	ssh_wr_u8(&w, 20);
	uint8_t cookie[16]; memset(cookie, 0xAB, 16);
	ssh_wr_bytes(&w, cookie, 16);
	ssh_wr_cstr(&w, "curve25519-sha256,diffie-hellman-group14-sha256");
	ssh_wr_cstr(&w, "ssh-ed25519,rsa-sha2-256");
	ssh_wr_cstr(&w, srv_offer_cipher);
	ssh_wr_cstr(&w, srv_offer_cipher);
	ssh_wr_cstr(&w, "hmac-sha2-256"); ssh_wr_cstr(&w, "hmac-sha2-256");
	ssh_wr_cstr(&w, "none"); ssh_wr_cstr(&w, "none");
	ssh_wr_cstr(&w, ""); ssh_wr_cstr(&w, "");
	ssh_wr_bool(&w, false); ssh_wr_u32(&w, 0);
	memcpy(srv_kexinit_payload, buf, ssh_wr_len(&w));
	srv_kexinit_len = ssh_wr_len(&w);       /* I_S, hashed later */
	srv_send_packet(buf, ssh_wr_len(&w));
}

static void srv_handle_payload(const uint8_t *pl, uint32_t len);

/* Server-initiated rekey. OpenSSH forces one after an hour or a
   gigabyte, so a client that mishandles it works flawlessly in every
   short test and then dies in production. Exercising it here is the
   only way that path is ever run before a board does it. */
static void srv_start_rekey(void) {
	ssh_sha256_init(&srv_hash);
	srv_hash_string(srv_v_c, srv_v_c_len);          /* V_C */
	srv_hash_string(SRV_ID, strlen(SRV_ID));        /* V_S */
	srv_send_kexinit();
}

/* Parse whatever the client has sent so far. */
static void srv_pump(void) {

	uint8_t tmp[4096];
	uint32_t n;
	while ((n = pipe_take(&c2s, tmp, sizeof tmp)) > 0) {
		memcpy(srv_in + srv_in_len, tmp, n); srv_in_len += n;
	}

	if (srv_state == 0) {
		uint8_t *nl = memchr(srv_in, '\n', srv_in_len);
		if (!nl) return;
		uint32_t linelen = (uint32_t)(nl - srv_in);
		uint32_t vlen = linelen;
		if (vlen && srv_in[vlen-1] == '\r') vlen--;
		memcpy(srv_v_c, srv_in, vlen); srv_v_c_len = vlen;
		ssh_sha256_init(&srv_hash);
		srv_hash_string(srv_in, vlen);                 /* V_C */
		srv_hash_string(SRV_ID, strlen(SRV_ID));       /* V_S */
		memmove(srv_in, nl+1, srv_in_len - linelen - 1);
		srv_in_len -= linelen + 1;
		srv_state = 1;
		srv_send_kexinit();
	}

	for (;;) {
		if (srv_in_len < 4) return;
		uint32_t plen = ssh_aead_peek_len(&srv_rx, srv_in);
		uint32_t need = 4 + plen + (srv_rx.active ? 16 : 0);
		if (plen > 60000) {
			printf("server: implausible len %u at state %d, srv_in_len=%u\n",
			       plen, srv_state, srv_in_len);
			printf("  first 32 bytes: ");
			for (uint32_t i = 0; i < (srv_in_len < 32 ? srv_in_len : 32); i++)
				printf("%02x", srv_in[i]);
			printf("\n  as text: ");
			for (uint32_t i = 0; i < (srv_in_len < 32 ? srv_in_len : 32); i++)
				printf("%c", srv_in[i] >= 32 && srv_in[i] < 127 ? srv_in[i] : '.');
			printf("\n");
			exit(2); }
		if (srv_in_len < need) return;
		if (!ssh_aead_open(&srv_rx, srv_in, plen)) {
			printf("server: TAG FAILURE on inbound packet\n"); exit(2);
		}
		/* RFC 4253 section 6 alignment, checked rather than assumed.
		   A real server enforces this and simply closes the connection
		   when it fails, which is a miserable thing to debug from the
		   client end -- so it is an explicit, loud failure here. */
		{
			uint32_t aad_in = srv_rx.active ? 4 : 0;
			uint32_t onwire = (4 - aad_in) + plen;
			if (onwire % 8) {
				printf("server: MISALIGNED inbound packet: plen=%u, "
				       "%u bytes to align (must be a multiple of 8)\n",
				       plen, onwire % 8);
				exit(2);
			}
		}
		uint8_t pad = srv_in[4];
		srv_handle_payload(srv_in + 5, plen - pad - 1);
		memmove(srv_in, srv_in + need, srv_in_len - need);
		srv_in_len -= need;
	}
}

static void srv_handle_payload(const uint8_t *pl, uint32_t len) {

	ssh_rd r;
	if (!len) return;
	uint8_t msg = pl[0];

	if (msg == 20) {                              /* client KEXINIT */
		srv_hash_string(pl, len);                             /* I_C */
		srv_hash_string(srv_kexinit_payload, srv_kexinit_len); /* I_S */
		return;
	}

	if (msg == 30) {                              /* KEX_ECDH_INIT */
		uint32_t qn; ssh_rd_init(&r, pl, len); ssh_rd_u8(&r);
		const uint8_t *q_c = ssh_rd_string(&r, &qn);
		if (qn != 32) { printf("server: bad Q_C\n"); exit(2); }

		uint8_t shared[32];
		crypto_x25519(shared, srv_eph_sk, q_c);

		ssh_wr w; ssh_wr_init(&w, srv_k_mpint, sizeof srv_k_mpint);
		ssh_wr_mpint(&w, shared, 32);
		srv_k_mpint_len = ssh_wr_len(&w);

		srv_hash_string(srv_keyblob, srv_keyblob_len);  /* K_S */
		srv_hash_string(q_c, 32);                       /* Q_C */
		srv_hash_string(srv_eph_pk, 32);                /* Q_S */
		ssh_sha256_update(&srv_hash, srv_k_mpint, srv_k_mpint_len);
		ssh_sha256_final(&srv_hash, srv_H);
		if (!srv_have_sid) { memcpy(srv_sid, srv_H, 32); srv_have_sid = 1; }

		uint8_t sig[64];
		crypto_ed25519_sign(sig, srv_sk, srv_H, 32);

		uint8_t sigblob[4+11+4+64]; ssh_wr sw;
		ssh_wr_init(&sw, sigblob, sizeof sigblob);
		ssh_wr_cstr(&sw, "ssh-ed25519");
		ssh_wr_string(&sw, sig, 64);

		uint8_t buf[512]; ssh_wr_init(&w, buf, sizeof buf);
		ssh_wr_u8(&w, 31);
		ssh_wr_string(&w, srv_keyblob, srv_keyblob_len);
		ssh_wr_string(&w, srv_eph_pk, 32);
		ssh_wr_string(&w, sigblob, ssh_wr_len(&sw));
		srv_send_packet(buf, ssh_wr_len(&w));
		return;
	}

	if (msg == 21) {                              /* client NEWKEYS */
		uint8_t nk = 21; srv_send_packet(&nk, 1);
		uint8_t kc[64], kd[64];
		ssh_derive_key(kc, srv_k_mpint, srv_k_mpint_len, srv_H, 'C', srv_sid);
		ssh_derive_key(kd, srv_k_mpint, srv_k_mpint_len, srv_H, 'D', srv_sid);
		ssh_cipher_set_key(&srv_rx, kc);   /* client->server */
		ssh_cipher_set_key(&srv_tx, kd);   /* server->client */
		srv_state = 4;
		return;
	}

	if (msg == 5) {                               /* SERVICE_REQUEST */
		uint8_t buf[64]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
		ssh_wr_u8(&w, 6); ssh_wr_cstr(&w, "ssh-userauth");
		srv_send_packet(buf, ssh_wr_len(&w));
		if (srv_send_big_banner) {
			/* deliberately larger than SSH_MAX_PACKET */
			static uint8_t big[9000]; ssh_wr bw;
			ssh_wr_init(&bw, big, sizeof big);
			ssh_wr_u8(&bw, 53);
			static char txt[8000]; memset(txt, 'B', sizeof txt - 1);
			txt[sizeof txt - 1] = 0;
			ssh_wr_cstr(&bw, txt); ssh_wr_cstr(&bw, "en");
			srv_send_packet(big, ssh_wr_len(&bw));
		}
		return;
	}

	if (msg == 50) {                              /* USERAUTH_REQUEST */
		uint32_t un, sn, mn;
		ssh_rd_init(&r, pl, len); ssh_rd_u8(&r);
		const uint8_t *user = ssh_rd_string(&r, &un);
		ssh_rd_string(&r, &sn);
		const uint8_t *meth = ssh_rd_string(&r, &mn);
		(void)user;
		if (ssh_str_eq(meth, mn, "none")) {
			srv_auth_none_seen = 1;
			uint8_t buf[64]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
			ssh_wr_u8(&w, 51); ssh_wr_cstr(&w, "publickey,password");
			ssh_wr_bool(&w, false);
			srv_send_packet(buf, ssh_wr_len(&w));
			return;
		}
		if (ssh_str_eq(meth, mn, "password")) {
			uint32_t pn; ssh_rd_bool(&r);
			const uint8_t *pw = ssh_rd_string(&r, &pn);
			if (pn == 7 && !memcmp(pw, "hunter2", 7)) {
				srv_pw_ok = 1;
				uint8_t b = 52; srv_send_packet(&b, 1);
			} else {
				uint8_t buf[64]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
				ssh_wr_u8(&w, 51); ssh_wr_cstr(&w, "password");
				ssh_wr_bool(&w, false);
				srv_send_packet(buf, ssh_wr_len(&w));
			}
			return;
		}
		return;
	}

	if (msg == 90) {                              /* CHANNEL_OPEN */
		uint32_t tn; ssh_rd_init(&r, pl, len); ssh_rd_u8(&r);
		ssh_rd_string(&r, &tn);
		srv_remote_chan = ssh_rd_u32(&r);
		uint8_t buf[64]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
		ssh_wr_u8(&w, 91);
		ssh_wr_u32(&w, srv_remote_chan);
		ssh_wr_u32(&w, 7);            /* our channel id */
		ssh_wr_u32(&w, 2097152);      /* window */
		ssh_wr_u32(&w, 32768);        /* max packet */
		srv_send_packet(buf, ssh_wr_len(&w));
		return;
	}

	if (msg == 98) {                              /* CHANNEL_REQUEST */
		uint32_t tn; ssh_rd_init(&r, pl, len); ssh_rd_u8(&r);
		ssh_rd_u32(&r);
		const uint8_t *type = ssh_rd_string(&r, &tn);
		bool want = ssh_rd_bool(&r);
		if (ssh_str_eq(type, tn, "shell")) {
			/* A window adjust and some stderr BEFORE the reply to the
			   shell request. Both are legal and both are things a real
			   OpenSSH session sends; the client must keep waiting for
			   CHANNEL_SUCCESS rather than treating the first thing it
			   sees as the answer.
			   Placed HERE, after the pty reply, on purpose. An earlier
			   version put the window adjust right after
			   CHANNEL_OPEN_CONFIRMATION, where the buggy client sent
			   its shell request early and then mistook the PTY reply
			   for the shell reply -- two errors cancelling out, and a
			   test that passed for the wrong reason. */
			uint8_t wa[16]; ssh_wr ww; ssh_wr_init(&ww, wa, sizeof wa);
			ssh_wr_u8(&ww, 93); ssh_wr_u32(&ww, srv_remote_chan);
			ssh_wr_u32(&ww, 65536);
			srv_send_packet(wa, ssh_wr_len(&ww));

			uint8_t xd[128]; ssh_wr xw; ssh_wr_init(&xw, xd, sizeof xd);
			ssh_wr_u8(&xw, 95); ssh_wr_u32(&xw, srv_remote_chan);
			ssh_wr_u32(&xw, 1);            /* SSH_EXTENDED_DATA_STDERR */
			ssh_wr_cstr(&xw, "motd\r\n");
			srv_send_packet(xd, ssh_wr_len(&xw));
		}
		if (want) {
			uint8_t buf[16]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
			ssh_wr_u8(&w, 99); ssh_wr_u32(&w, srv_remote_chan);
			srv_send_packet(buf, ssh_wr_len(&w));
		}
		if (ssh_str_eq(type, tn, "shell")) {
			/* greet the shell */
			uint8_t buf[128]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
			ssh_wr_u8(&w, 94); ssh_wr_u32(&w, srv_remote_chan);
			ssh_wr_cstr(&w, "hello from the test server\r\n$ ");
			srv_send_packet(buf, ssh_wr_len(&w));
		}
		return;
	}

	if (msg == 94) {                              /* CHANNEL_DATA (echo) */
		uint32_t dn; ssh_rd_init(&r, pl, len); ssh_rd_u8(&r);
		ssh_rd_u32(&r);
		const uint8_t *d = ssh_rd_string(&r, &dn);
		uint8_t buf[2048]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
		ssh_wr_u8(&w, 94); ssh_wr_u32(&w, srv_remote_chan);
		ssh_wr_string(&w, d, dn);
		srv_send_packet(buf, ssh_wr_len(&w));
		return;
	}
}

/* The client hashes I_C at send time and I_S on arrival, giving
   V_C,V_S,I_C,I_S. The server above hashes I_S when it sends and I_C
   on arrival. Those agree only if the server's KEXINIT is hashed
   AFTER the client's. Rather than complicate the server, the exchange
   is driven so the client's KEXINIT reaches the server first -- which
   is also what happens on a real connection, since the client sends
   its KEXINIT immediately on seeing the server's version line. */

static void run_exchange(void) {
	uint8_t tmp[4096]; uint32_t n; int spins = 0;
	while (spins++ < 20000) {
		srv_pump();
		if (s2c.len) {
			/* deliberately awkward chunking: SSH packets routinely
			   straddle TCP segments and the framing must not care */
			n = pipe_take(&s2c, tmp, 13);
			ssh_proto_feed(&cli, tmp, n);
			continue;
		}
		if (c2s.len) continue;
		break;
	}
}

int main(void) {

	/* host key */
	uint8_t seed[32];
	for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 5 + 1);
	crypto_ed25519_key_pair(srv_sk, srv_pk, seed);
	{ ssh_wr w; ssh_wr_init(&w, srv_keyblob, sizeof srv_keyblob);
	  ssh_wr_cstr(&w, "ssh-ed25519"); ssh_wr_string(&w, srv_pk, 32);
	  srv_keyblob_len = ssh_wr_len(&w); }

	for (int i = 0; i < 32; i++) srv_eph_sk[i] = (uint8_t)(i * 9 + 7);
	crypto_x25519_public_key(srv_eph_pk, srv_eph_sk);

	printf("full handshake:\n");
	ssh_cipher_init(&srv_tx); ssh_cipher_init(&srv_rx);
	srv_out((const uint8_t *)SRV_ID "\r\n", strlen(SRV_ID) + 2);
	ssh_proto_init(&cli, NULL, cli_write, cli_event, cli_random, "alice");

	/* pump until the client stalls on the host key */
	run_exchange();
	ok("host key event raised", hostkey_events == 1);
	ok("fingerprint looks right", !strncmp(last_fp, "SHA256:", 7)
	   && strlen(last_fp) == 50);
	ok("engine stalls until host key accepted", !ready && !closed);
	printf("      %s\n", last_fp);

	ssh_proto_accept_host(&cli);
	run_exchange();

	ok("server saw auth method probe", srv_auth_none_seen == 1);
	ok("client asked for a password", asked_pw == 1);
	ok("server accepted the password", srv_pw_ok == 1);
	ok("session reached SSH_EV_READY", ready == 1);
	ok("no close event", closed == 0);
	ok("shell greeting arrived encrypted",
	   rx_accum_len >= 26 && !memcmp(rx_accum, "hello from the test server", 26));

	/* keystrokes round trip */
	rx_accum_len = 0;
	ok("send accepted", ssh_proto_send(&cli, (const uint8_t *)"ls -la\r", 7));
	run_exchange();
	ok("echo returned intact", rx_accum_len == 7
	   && !memcmp(rx_accum, "ls -la\r", 7));

	/* a burst, to exercise sequence numbers and windowing */
	rx_accum_len = 0;
	for (int i = 0; i < 50; i++) {
		char c = (char)('a' + (i % 26));
		if (!ssh_proto_send(&cli, (const uint8_t *)&c, 1)) {
			ok("burst send failed", 0); break;
		}
		run_exchange();
	}
	ok("50 sequential packets round trip", rx_accum_len == 50);
	{ int good = 1;
	  for (int i = 0; i < 50; i++)
	      if (rx_accum[i] != 'a' + (i % 26)) good = 0;
	  ok("burst contents intact", good); }

	ok("still open after burst", ssh_proto_is_open(&cli));

	/* ---- negotiation failure gets a specific message ---- */
	printf("cipher mismatch:\n");
	memset(&c2s, 0, sizeof c2s); memset(&s2c, 0, sizeof s2c);
	srv_in_len = 0; srv_state = 0; closed = 0; ready = 0;
	hostkey_events = 0; asked_pw = 0; srv_have_sid = 0;
	ssh_cipher_init(&srv_tx); ssh_cipher_init(&srv_rx);
	srv_offer_cipher = "aes128-ctr,aes256-gcm@openssh.com";
	srv_out((const uint8_t *)SRV_ID "\r\n", strlen(SRV_ID) + 2);
	ssh_proto_init(&cli, NULL, cli_write, cli_event, cli_random, "alice");
	run_exchange();
	ok("closed on cipher mismatch", closed == 1);
	ok("error names the cipher", strstr(last_close, "chacha20-poly1305") != NULL);
	printf("      %s\n", last_close);

	/* ---- a tampered packet must not authenticate ---- */
	printf("tamper detection:\n");
	memset(&c2s, 0, sizeof c2s); memset(&s2c, 0, sizeof s2c);
	srv_in_len = 0; srv_state = 0; closed = 0; ready = 0;
	hostkey_events = 0; asked_pw = 0; rx_accum_len = 0; srv_have_sid = 0;
	ssh_cipher_init(&srv_tx); ssh_cipher_init(&srv_rx);
	srv_offer_cipher = "chacha20-poly1305@openssh.com";
	srv_out((const uint8_t *)SRV_ID "\r\n", strlen(SRV_ID) + 2);
	ssh_proto_init(&cli, NULL, cli_write, cli_event, cli_random, "alice");
	run_exchange();
	ssh_proto_accept_host(&cli);
	run_exchange();
	ok("second session opened", ready == 1);
	/* corrupt one byte of the next server packet in flight */
	{
		uint8_t buf[128]; ssh_wr w; ssh_wr_init(&w, buf, sizeof buf);
		ssh_wr_u8(&w, 94); ssh_wr_u32(&w, srv_remote_chan);
		ssh_wr_cstr(&w, "this will be corrupted");
		srv_send_packet(buf, ssh_wr_len(&w));
		s2c.b[10] ^= 0x40;
		uint8_t tmp[4096]; uint32_t n;
		while ((n = pipe_take(&s2c, tmp, sizeof tmp)) > 0)
			ssh_proto_feed(&cli, tmp, n);
	}
	ok("corrupted packet closes the session", closed == 1);
	ok("error says authentication failed",
	   strstr(last_close, "authentication failed") != NULL);
	printf("      %s\n", last_close);

	/* ---- oversize packet is survivable, not fatal ---- */
	printf("oversize banner:\n");
	memset(&c2s, 0, sizeof c2s); memset(&s2c, 0, sizeof s2c);
	srv_in_len = 0; srv_state = 0; closed = 0; ready = 0;
	hostkey_events = 0; asked_pw = 0; rx_accum_len = 0; banner_len = 0;
	srv_have_sid = 0;
	ssh_cipher_init(&srv_tx); ssh_cipher_init(&srv_rx);
	srv_send_big_banner = 1;
	srv_out((const uint8_t *)SRV_ID "\r\n", strlen(SRV_ID) + 2);
	ssh_proto_init(&cli, NULL, cli_write, cli_event, cli_random, "alice");
	run_exchange();
	ssh_proto_accept_host(&cli);
	run_exchange();
	if (closed) printf("      close reason: %s\n", last_close);
	ok("8KB banner discarded without killing the session", ready == 1 && !closed);
	ok("session still usable afterwards",
	   rx_accum_len >= 26 && !memcmp(rx_accum, "hello from the test server", 26));

	/* ---- server-initiated rekey mid-session ---- */
	printf("rekey:\n");
	memset(&c2s, 0, sizeof c2s); memset(&s2c, 0, sizeof s2c);
	srv_in_len = 0; srv_state = 0; closed = 0; ready = 0;
	hostkey_events = 0; asked_pw = 0; rx_accum_len = 0;
	srv_have_sid = 0; srv_send_big_banner = 0;
	ssh_cipher_init(&srv_tx); ssh_cipher_init(&srv_rx);
	srv_out((const uint8_t *)SRV_ID "\r\n", strlen(SRV_ID) + 2);
	ssh_proto_init(&cli, NULL, cli_write, cli_event, cli_random, "alice");
	run_exchange();
	ssh_proto_accept_host(&cli);
	run_exchange();
	ok("session open before rekey", ready == 1 && !closed);

	{
		uint8_t sid_before[32];
		memcpy(sid_before, srv_sid, 32);

		srv_start_rekey();
		run_exchange();

		ok("host key not re-confirmed on rekey", hostkey_events == 1);
		ok("session survived the rekey", !closed && ssh_proto_is_open(&cli));
		ok("session id unchanged across rekey",
		   !memcmp(sid_before, srv_sid, 32));

		/* the real question: does traffic still work under the NEW keys */
		rx_accum_len = 0;
		ok("send accepted after rekey",
		   ssh_proto_send(&cli, (const uint8_t *)"after-rekey", 11));
		run_exchange();
		ok("echo intact under new keys",
		   rx_accum_len == 11 && !memcmp(rx_accum, "after-rekey", 11));

		/* and a second rekey, to catch anything that only works once */
		srv_start_rekey();
		run_exchange();
		rx_accum_len = 0;
		ssh_proto_send(&cli, (const uint8_t *)"twice", 5);
		run_exchange();
		ok("survives a second rekey",
		   rx_accum_len == 5 && !memcmp(rx_accum, "twice", 5));
	}

	printf("\n%s\n", failures ? "SESSION: FAIL" : "SESSION: PASS");
	return failures != 0;
}
