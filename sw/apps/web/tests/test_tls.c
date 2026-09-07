/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Runs tls.c against a real TLS 1.3 server and completes a real
 * handshake.
 *
 *   make test-tls
 *
 * -- what this actually proves --
 *
 * That the ClientHello is well formed enough for OpenSSL to accept,
 * that the key schedule agrees with an independent implementation,
 * that the transcript hash is right, that the record framing and
 * nonce sequencing are right, and that application data flows both
 * ways. Every one of those fails identically on the wire -- the
 * server sends `decrypt_error` and nothing else -- so having them
 * fail HERE, on a machine with a debugger, is the difference between
 * an afternoon and a week.
 *
 * A recorded trace cannot do this. The client's key share is fresh
 * every run, so every secret differs and nothing past the ServerHello
 * would decrypt. Testing a TLS client means talking to something that
 * can do the key exchange.
 *
 * -- certificates ARE part of this now --
 *
 * The server generates a self-signed CA certificate for localhost and
 * writes out its DER; the client is given that one certificate as its
 * entire root store. So a successful run exercises the chain walk,
 * the hostname match, the validity dates and the RSA-PSS
 * CertificateVerify -- not just the record layer.
 *
 * The run with "none" gives the client an EMPTY store and requires
 * the handshake to be refused. That is the case that proves
 * verification is actually reached, rather than being code that
 * happens to be linked in and never consulted.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../tls.h"
#include "../x509.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

// -- randomness for the host build --
//
// tls.c calls these; on the board they come from sw/common/zrng.h,
// backed by rtl/trng.v. Here they come from /dev/urandom, which is
// the same contract: real entropy, and z_rng_secure() true only when
// that is genuinely so.

void z_rng_bytes(void *buf, uint32_t len) {
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f || fread(buf, 1, len, f) != len) {
		fprintf(stderr, "test_tls: no /dev/urandom\n");
		exit(2);
	}
	fclose(f);
}

bool z_rng_secure(void) { return true; }

// -- transport --

static int sock = -1;
// Large enough for the server's whole response. The point of the test
// is that the body spans many TLS records.
static uint8_t app[400000];
static uint32_t app_len;

static void on_send(void *user, const uint8_t *data, uint32_t len) {
	(void)user;
	while (len) {
		ssize_t n = write(sock, data, len);
		if (n <= 0) { perror("write"); exit(2); }
		data += n;
		len -= (uint32_t)n;
	}
}

static void on_data(void *user, const uint8_t *data, uint32_t len) {
	(void)user;
	if (app_len + len < sizeof(app)) {
		memcpy(app + app_len, data, len);
		app_len += len;
		app[app_len] = '\0';
	}
}

static tls_ctx_t tls;

// -- the root store --
//
// One certificate: the one the test server generated for itself. The
// server is its own CA, so a successful handshake here proves the
// whole path -- chain walk, hostname match, dates, RSA-PSS
// CertificateVerify -- and not merely that the record layer works.
static uint8_t root_der[8192];
static uint32_t root_len;

static bool find_root(void *user, const der_t *issuer, x509_cert_t *out) {
	const char *e = NULL;
	(void)user;
	if (root_len == 0) return false;
	if (!x509_parse(root_der, root_len, out, &e)) return false;
	return out->subject.len == issuer->len &&
		!memcmp(out->subject.p, issuer->p, issuer->len);
}

// A store with nothing in it. The handshake MUST fail -- this is the
// case that proves verification is actually reached, rather than
// being code that happens to be linked in.
static bool no_roots(void *user, const der_t *issuer, x509_cert_t *out) {
	(void)user; (void)issuer; (void)out;
	return false;
}

int main(int argc, char **argv) {

	struct sockaddr_in sa;
	int port = (argc > 1) ? atoi(argv[1]) : 44330;
	uint32_t chunk = (argc > 2) ? (uint32_t)atoi(argv[2]) : 0;
	// Mode: "verify" expects success; "none" and "wronghost" expect
	// the handshake to be REFUSED. The two failure modes are
	// separate because they exercise different checks -- an empty
	// store fails at the anchor, a wrong hostname fails before any
	// signature work happens at all.
	const char *mode = (argc > 3) ? argv[3] : "verify";
	bool empty_store = !strcmp(mode, "none");
	bool wrong_host = !strcmp(mode, "wronghost");
	bool expect_fail = empty_store || wrong_host;
	const char *cert_path = (argc > 4) ? argv[4] : "/tmp/tls_server_cert.der";
	const char *req =
		"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

	sock = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("connect");
		return 2;
	}

	{
		FILE *f = fopen(cert_path, "rb");
		if (!f) { fprintf(stderr, "no server cert at %s\n", cert_path); return 2; }
		root_len = (uint32_t)fread(root_der, 1, sizeof(root_der), f);
		fclose(f);
	}

	tls_init(&tls, wrong_host ? "wrong.example" : "localhost",
		on_send, on_data, NULL);

	// A fixed time inside the server certificate's one-day window.
	// Taken from the certificate itself so the test does not start
	// failing tomorrow.
	{
		x509_cert_t c;
		const char *e = NULL;
		int64_t now = 0;
		if (x509_parse(root_der, root_len, &c, &e)) now = c.not_before + 3600;
		tls_set_verify(&tls, now, empty_store ? no_roots : find_root, NULL);
	}

	if (expect_fail) {
		// tls_start still succeeds -- the refusal happens when the
		// chain arrives, not before.
		tls_start(&tls);
	} else {
		ck(tls_start(&tls), "ClientHello sent");
	}
	if (tls_failed(&tls)) {
		printf("FAIL: tls_start: %s\n", tls_error(&tls));
		return 1;
	}

	// Feed the socket in small pieces when asked.
	//
	// A record routinely spans several TCP segments on the real
	// transport -- `net`'s MSS is 536 bytes and a Certificate message
	// is several kilobytes -- so a record layer that only works when
	// whole records arrive at once is one that works here and fails
	// on hardware. Running the same handshake at chunk sizes of 1 and
	// 7 is the cheapest possible way to find that out.
	for (;;) {

		uint8_t buf[16384];
		ssize_t n = read(sock, buf, sizeof(buf));

		if (n < 0) { perror("read"); return 2; }
		if (n == 0) break;

		if (chunk) {
			for (ssize_t i = 0; i < n; i += chunk) {
				uint32_t take = (uint32_t)(n - i);
				if (take > chunk) take = chunk;
				tls_feed(&tls, buf + i, take);
			}
		} else {
			tls_feed(&tls, buf, (uint32_t)n);
		}

		if (tls_failed(&tls)) {
			if (expect_fail) {
				printf("ok (%s): refused -- %s\n", mode, tls_error(&tls));
				return 0;
			}
			printf("FAIL: handshake: %s\n", tls_error(&tls));
			return 1;
		}

		// A peer that has sent close_notify is finished, and so are
		// we -- blocking on read() until the socket drops instead is
		// what made the first version of this test hang after a
		// completely successful handshake.
		if (tls_closed(&tls)) break;

		// The request goes out the moment the handshake completes.
		static bool sent;
		if (!sent && tls_established(&tls)) {
			ck(true, "handshake completed");
			ck(tls_write(&tls, (const uint8_t *)req, (uint32_t)strlen(req)),
				"request written");
			sent = true;
		}

	}

	if (expect_fail) {
		printf("FAIL (%s): the handshake was ACCEPTED and should not "
			"have been\n", mode);
		return 1;
	}

	ck(tls_established(&tls) || app_len > 0, "connection reached data");
	ck(app_len > 0, "application data received");
	ck(strstr((char *)app, "HTTP/1.1 200") != NULL, "response is a 200");
	// The body must arrive COMPLETE and IN ORDER across every record.
	//
	// Checking only that some marker appears would pass while a
	// record in the middle was mangled -- which is exactly the
	// failure this test exists for.
	{
		const char *hdr_end = strstr((char *)app, "\r\n\r\n");
		uint32_t body_off = hdr_end ? (uint32_t)(hdr_end - (char *)app) + 4 : 0;
		uint32_t body_len = app_len - body_off;
		uint32_t bad = 0, first_bad = 0;

		ck(hdr_end != NULL, "response headers terminate");
		ck(body_len > 100000, "body spans many TLS records");

		for (uint32_t i = 0; i < body_len; i++) {
			uint8_t want = (uint8_t)(((i * 7) ^ (i >> 8)) & 0xFF);
			if (app[body_off + i] != want) {
				if (!bad) first_bad = i;
				bad++;
			}
		}

		checks++;
		if (bad) {
			fails++;
			printf("FAIL: %u body bytes wrong, first at offset %u of %u\n",
				(unsigned)bad, (unsigned)first_bad, (unsigned)body_len);
		}
	}

	close(sock);

	printf("%s: %d checks, %d failures (chunk=%u, %u bytes of app data)\n",
		fails ? "FAIL" : "ok", checks, fails,
		(unsigned)chunk, (unsigned)app_len);

	return fails ? 1 : 0;

}
