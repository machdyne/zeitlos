/*
 * The SSH server engine (ssh_server.c) against the REAL client engine
 * (ssh_proto.c), in one process, each one's output fed to the other.
 * Both were written to the RFCs and OpenSSH's behaviour separately; a
 * byte either disagrees about shows up here, not on a board.
 *
 *   cd sw/apps/net/ssh && cc -std=gnu99 -O2 -Wall -I. -o /tmp/t \
 *     tests/test_ssh_server.c ssh_server.c ssh_proto.c ssh_wire.c ssh_crypto.c \
 *     ../../../common/zsha256.c ../../../ext/monocypher/monocypher.c \
 *     ../../../ext/monocypher/monocypher-ed25519.c && /tmp/t
 *
 * (Also in `make test` in sw/apps/net/netserve.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../ssh_server.h"
#include "../ssh_proto.h"
#include "../ssh_wire.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* -- two byte pipes -- */
typedef struct { uint8_t b[1 << 18]; uint32_t n; } pipe_t;
static pipe_t c2s, s2c;

static bool w_client(void *u, const uint8_t *d, uint32_t n) {
	(void)u;
	if (c2s.n + n > sizeof(c2s.b)) return false;
	memcpy(c2s.b + c2s.n, d, n); c2s.n += n;
	return true;
}
static bool w_server(void *u, const uint8_t *d, uint32_t n) {
	(void)u;
	if (s2c.n + n > sizeof(s2c.b)) return false;
	memcpy(s2c.b + s2c.n, d, n); s2c.n += n;
	return true;
}

/* two independent generators, so neither side's randomness is the other's */
static uint64_t rs_c = 0x1234567, rs_s = 0x9abcdef;
static void rnd(uint64_t *st, uint8_t *o, uint32_t n) {
	while (n--) { *st ^= *st << 13; *st ^= *st >> 7; *st ^= *st << 17; *o++ = (uint8_t)*st; }
}
static void r_client(void *u, uint8_t *o, uint32_t n) { (void)u; rnd(&rs_c, o, n); }
static void r_server(void *u, uint8_t *o, uint32_t n) { (void)u; rnd(&rs_s, o, n); }

/* -- the client's side -- */
static ssh_proto_t cli;
static struct {
	int hostkey, ready, closed, pw_asked;
	char fp[64], why[128];
	uint8_t got[65536]; uint32_t got_n;
	const char *password;
} C;

static void ev_client(void *u, ssh_event_t ev, const uint8_t *d, uint32_t n, const char *t) {
	(void)u;
	switch (ev) {
	case SSH_EV_HOSTKEY:
		C.hostkey++;
		snprintf(C.fp, sizeof(C.fp), "%s", t);
		ssh_proto_accept_host(&cli);
		break;
	case SSH_EV_NEED_PASSWORD:
		C.pw_asked++;
		if (C.password) ssh_proto_password(&cli, C.password);
		break;
	case SSH_EV_READY: C.ready++; break;
	case SSH_EV_DATA:
		if (C.got_n + n <= sizeof(C.got)) { memcpy(C.got + C.got_n, d, n); C.got_n += n; }
		break;
	case SSH_EV_CLOSED:
		C.closed++;
		snprintf(C.why, sizeof(C.why), "%s", t ? t : "");
		break;
	default: break;
	}
}

/* -- the server's side -- */
static ssh_server_t srv;
static struct {
	int shell, closed, eof, auth_calls;
	char why[128], user[40];
	uint8_t got[65536]; uint32_t got_n;
	int auth_result_override;       // -1: check the password
	bool consume;                   // pass data on at once (sshs_consumed)
} S;
static const char *PASSWORD = "correct horse";

static int auth_server(void *u, const char *user, const uint8_t *pw, uint32_t n) {
	(void)u;
	S.auth_calls++;
	snprintf(S.user, sizeof(S.user), "%s", user);
	if (S.auth_result_override >= 0) return S.auth_result_override;
	return (n == strlen(PASSWORD) && !memcmp(pw, PASSWORD, n)) ? SSHS_AUTH_OK : SSHS_AUTH_BAD;
}
static void ev_server(void *u, sshs_event_t ev, const uint8_t *d, uint32_t n, const char *t) {
	(void)u;
	switch (ev) {
	case SSHS_EV_SHELL: S.shell++; break;
	case SSHS_EV_DATA:
		if (S.got_n + n <= sizeof(S.got)) { memcpy(S.got + S.got_n, d, n); S.got_n += n; }
		if (S.consume) sshs_consumed(&srv, n);
		break;
	case SSHS_EV_EOF: S.eof++; break;
	case SSHS_EV_CLOSED: S.closed++; snprintf(S.why, sizeof(S.why), "%s", t ? t : ""); break;
	default: break;
	}
}

/* Moves bytes both ways until neither side says anything more. */
static void pump(void) {
	for (int i = 0; i < 1000 && (c2s.n || s2c.n); i++) {
		static uint8_t tmp[1 << 18];
		uint32_t n;
		if (c2s.n) { n = c2s.n; memcpy(tmp, c2s.b, n); c2s.n = 0; sshs_feed(&srv, tmp, n); }
		if (s2c.n) { n = s2c.n; memcpy(tmp, s2c.b, n); s2c.n = 0; ssh_proto_feed(&cli, tmp, n); }
	}
}

static const uint8_t host_seed[32] = { 7, 7, 7, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
	14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 };

static void start(const char *password) {
	memset(&C, 0, sizeof(C));
	memset(&S, 0, sizeof(S));
	S.auth_result_override = -1;
	S.consume = true;
	c2s.n = s2c.n = 0;
	C.password = password;
	ssh_proto_init(&cli, 0, w_client, ev_client, r_client, "phil");
	sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
	pump();
}

/* -- building raw plaintext packets, for the strict-KEX cases -- */
static uint32_t raw_packet(uint8_t *out, const uint8_t *pl, uint32_t n) {
	uint32_t base = 4 + 1 + n, pad = 8 - (base % 8);
	if (pad < 4) pad += 8;
	uint32_t total = 1 + n + pad;
	out[0] = (uint8_t)(total >> 24); out[1] = (uint8_t)(total >> 16);
	out[2] = (uint8_t)(total >> 8); out[3] = (uint8_t)total;
	out[4] = (uint8_t)pad;
	memcpy(out + 5, pl, n);
	memset(out + 5 + n, 0, pad);
	return 4 + total;
}
static uint32_t client_kexinit(uint8_t *out, const char *kex) {
	uint8_t pl[600];
	ssh_wr w;
	ssh_wr_init(&w, pl, sizeof(pl));
	ssh_wr_u8(&w, 20);
	for (int i = 0; i < 16; i++) ssh_wr_u8(&w, (uint8_t)i);
	ssh_wr_cstr(&w, kex);
	ssh_wr_cstr(&w, "ssh-ed25519");
	ssh_wr_cstr(&w, "chacha20-poly1305@openssh.com");
	ssh_wr_cstr(&w, "chacha20-poly1305@openssh.com");
	ssh_wr_cstr(&w, "hmac-sha2-256");
	ssh_wr_cstr(&w, "hmac-sha2-256");
	ssh_wr_cstr(&w, "none");
	ssh_wr_cstr(&w, "none");
	ssh_wr_cstr(&w, "");
	ssh_wr_cstr(&w, "");
	ssh_wr_bool(&w, false);
	ssh_wr_u32(&w, 0);
	return raw_packet(out, pl, ssh_wr_len(&w));
}

int main(void) {

	/* -- 1. the whole session -- */
	start(PASSWORD);
	CK(C.hostkey == 1 && !strncmp(C.fp, "SHA256:", 7), "the client saw and accepted the host key");
	{
		uint8_t pub[32]; char fp[64];
		sshs_host_public(host_seed, pub, fp, sizeof(fp));
		CK(!strcmp(fp, C.fp), "the fingerprint the server prints is the one the client sees");
	}
	CK(S.auth_calls == 1 && !strcmp(S.user, "phil"), "one password check, for user phil");
	CK(C.ready == 1 && S.shell == 1, "authenticated, and a shell requested and granted");
	if (!C.ready) {
		printf("FAIL: the handshake did not complete (client: %s; server: %s) -- stopping\n", C.why, S.why);
		return 1;
	}
	CK(!srv.strict, "our own client does not ask for strict kex (OpenSSH does: see 4)");

	/* client -> server */
	CK(ssh_proto_send(&cli, (const uint8_t *)"ls\r", 3), "the client sends a line");
	pump();
	CK(S.got_n == 3 && !memcmp(S.got, "ls\r", 3), "the server gets it");

	/* server -> client, much more than any window or packet */
	{
		static uint8_t big[40000];
		for (uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 13 + 1);
		uint32_t sent = 0, stuck = 0;
		while (sent < sizeof(big) && stuck < 50) {
			uint32_t n = sshs_send(&srv, big + sent, sizeof(big) - sent, 600);
			if (!n) stuck++; else stuck = 0;
			sent += n;
			pump();
		}
		CK(sent == sizeof(big) && C.got_n == sizeof(big) && !memcmp(C.got, big, sizeof(big)),
			"40000 bytes to the client, in order, within its window (%u sent, %u got)", sent, C.got_n);
	}

	/* the server's window: it is what the owner holds, topped up as it passes data on */
	S.consume = false;
	S.got_n = 0;
	{
		uint8_t chunk[100];
		memset(chunk, 'k', sizeof(chunk));
		for (int i = 0; i < 20; i++) { ssh_proto_send(&cli, chunk, sizeof(chunk)); pump(); }
		uint32_t first = S.got_n;
		CK(first <= SSHS_LOCAL_WINDOW && first >= SSHS_LOCAL_WINDOW - 100,
			"the client stops at the server's window (%u of %u)", first, SSHS_LOCAL_WINDOW);
		for (int i = 0; i < 5; i++) { ssh_proto_send(&cli, chunk, sizeof(chunk)); pump(); }
		CK(S.got_n == first, "and stays stopped while the owner holds the data");
		sshs_consumed(&srv, first + 3);         /* + the earlier "ls\r" */
		pump();
		for (int i = 0; i < 5; i++) { ssh_proto_send(&cli, chunk, sizeof(chunk)); pump(); }
		CK(S.got_n == first + 500, "and goes on once the owner has passed it on (%u)", S.got_n);
	}
	S.consume = true;

	/* the server never sends past the client's window, whatever it is given */
	{
		static uint8_t lots[40000];
		uint32_t total = 0, n;
		uint32_t before = srv.remote_window;
		while ((n = sshs_send(&srv, lots, sizeof(lots), 1200)) > 0) total += n;   /* no pump: no adjusts */
		CK(total == before && srv.remote_window == 0,
			"the server stops exactly at the client's window (%u of %u)", total, before);
		pump();
	}

	/* the port closes: exit-status, EOF, CLOSE; the client's CLOSE ends it */
	sshs_end(&srv, 0);
	pump();
	CK(S.closed == 1 && sshs_is_closed(&srv), "the session ends");
	CK(C.closed == 1, "and the client knows (%s)", C.why);

	/* -- 2. wrong passwords -- */
	start("wrong one");
	CK(S.auth_calls >= 1 && C.ready == 0, "a wrong password: no shell");
	{
		/* the client engine asks again on failure; three wrong in all */
		for (int i = 0; i < 5 && !S.closed; i++) { ssh_proto_password(&cli, "still wrong"); pump(); }
		CK(S.closed == 1 && S.auth_calls == 3 && strstr(S.why, "Too many"),
			"three failures: disconnected (%d checks: %s)", S.auth_calls, S.why);
	}

	/* -- the kernel's backoff -- */
	start(NULL);
	S.auth_result_override = SSHS_AUTH_REFUSE;
	ssh_proto_password(&cli, "anything");
	pump();
	CK(S.closed == 1 && C.ready == 0, "the kernel refusing further checks disconnects at once");

	/* -- 3. a tampered packet -- */
	start(PASSWORD);
	CK(C.ready == 1, "(a session)");
	ssh_proto_send(&cli, (const uint8_t *)"hello", 5);
	c2s.b[c2s.n - 20] ^= 1;
	pump();
	CK(S.closed == 1 && strstr(S.why, "authentication failed") && S.got_n == 0,
		"one flipped bit: rejected, nothing delivered (%s)", S.why);

	/* -- 4. strict kex, as OpenSSH 9.6+ offers it -- */
	{
		static uint8_t buf[4096];
		uint32_t n;
		memset(&S, 0, sizeof(S));
		S.auth_result_override = -1;
		s2c.n = 0;
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"SSH-2.0-OpenSSH_9.9\r\n", 21);
		n = client_kexinit(buf, "sntrup761x25519-sha512,curve25519-sha256,kex-strict-c-v00@openssh.com");
		sshs_feed(&srv, buf, n);
		CK(srv.strict && !S.closed, "a client offering kex-strict-c gets strict kex");

		/* Terrapin's opening move: an IGNORE before the client's KEXINIT */
		memset(&S, 0, sizeof(S));
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"SSH-2.0-OpenSSH_9.9\r\n", 21);
		uint8_t ign[8] = { 2, 0, 0, 0, 0 };
		n = raw_packet(buf, ign, 5);
		sshs_feed(&srv, buf, n);
		n = client_kexinit(buf, "curve25519-sha256,kex-strict-c-v00@openssh.com");
		sshs_feed(&srv, buf, n);
		CK(S.closed == 1 && strstr(S.why, "first packet"), "strict: a packet before KEXINIT is refused (%s)", S.why);

		/* ... and without strict, the same IGNORE is fine */
		memset(&S, 0, sizeof(S));
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"SSH-2.0-OpenSSH_9.9\r\n", 21);
		n = raw_packet(buf, ign, 5);
		sshs_feed(&srv, buf, n);
		n = client_kexinit(buf, "curve25519-sha256");
		sshs_feed(&srv, buf, n);
		CK(!S.closed && !srv.strict, "without strict kex the same IGNORE is accepted");

		/* a service request in the middle of a strict first kex */
		memset(&S, 0, sizeof(S));
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"SSH-2.0-OpenSSH_9.9\r\n", 21);
		n = client_kexinit(buf, "curve25519-sha256,kex-strict-c-v00@openssh.com");
		sshs_feed(&srv, buf, n);
		uint8_t sreq[32]; ssh_wr w; ssh_wr_init(&w, sreq, sizeof(sreq));
		ssh_wr_u8(&w, 5); ssh_wr_cstr(&w, "ssh-userauth");
		n = raw_packet(buf, sreq, ssh_wr_len(&w));
		sshs_feed(&srv, buf, n);
		CK(S.closed == 1 && strstr(S.why, "strict"), "strict: a non-kex message mid-kex is refused");

		/* no algorithm in common */
		memset(&S, 0, sizeof(S));
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"SSH-2.0-OpenSSH_9.9\r\n", 21);
		n = client_kexinit(buf, "diffie-hellman-group14-sha256");
		sshs_feed(&srv, buf, n);
		CK(S.closed == 1 && strstr(S.why, "no algorithms"), "no kex in common: a clear refusal");

		/* not SSH at all */
		memset(&S, 0, sizeof(S));
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"GET / HTTP/1.0\r\n", 16);
		CK(S.closed == 1, "an HTTP request on the SSH port: closed");

		/* a hostile length */
		memset(&S, 0, sizeof(S));
		sshs_init(&srv, host_seed, 0, w_server, ev_server, r_server, auth_server);
		sshs_feed(&srv, (const uint8_t *)"SSH-2.0-x\r\n", 11);
		uint8_t huge[4] = { 0x7f, 0xff, 0xff, 0xff };
		sshs_feed(&srv, huge, 4);
		CK(S.closed == 1, "an absurd packet length: closed, nothing allocated");
	}

	/* -- a client that sends past the server's window -- */
	start(PASSWORD);
	{
		uint8_t chunk[1000];
		memset(chunk, 'w', sizeof(chunk));
		S.consume = false;
		cli.remote_window += 5000;              /* a client ignoring the window we gave it */
		for (int i = 0; i < 3 && !S.closed; i++) { ssh_proto_send(&cli, chunk, sizeof(chunk)); pump(); }
		CK(S.closed == 1 && strstr(S.why, "window") && S.got_n <= SSHS_LOCAL_WINDOW,
			"data past the window: disconnected, nothing past it delivered (%u, %s)", S.got_n, S.why);
	}

	/* -- channel traffic before authenticating -- */
	start(NULL);                                /* stops at the password prompt */
	CK(C.pw_asked == 1 && !C.ready, "(waiting for a password)");
	{
		cli.state = 11;                         /* ssh_proto.c's ST_OPEN: pretend */
		cli.remote_window = 1000;
		ssh_proto_send(&cli, (const uint8_t *)"id\r", 3);
		pump();
		CK(S.closed == 1 && S.got_n == 0 && !S.shell,
			"channel data before authentication: disconnected, nothing delivered (%s)", S.why);
	}

	/* -- 5. a second, independent session from the same host key -- */
	rs_s ^= 0x5555;
	start(PASSWORD);
	CK(C.ready == 1 && C.hostkey == 1, "a second session, fresh ephemeral keys");

	printf("ssh server: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
