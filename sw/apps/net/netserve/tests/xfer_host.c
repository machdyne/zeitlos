/*
 * sftp and scp against REAL OpenSSH clients, on the build machine.
 *
 *   xfer_host PORT ROOT [read]
 *
 * Listens on 127.0.0.1:PORT and serves one connection at a time: the
 * SSH server engine (../../ssh/ssh_server.c) and the transfer code
 * (../xfer.c, sftp.c, scp.c) exactly as netserve runs them, with ROOT
 * standing in for the card (nsfs_posix.c). Logins are noauth -- any
 * user, no password: what is under test is the copying, not the login.
 *
 * The plumbing copies netserve.c's: the engine's output held to
 * SSH_OUT_MAX and fed only with 600 bytes of room, a transfer's input
 * as the window, XFER_BUDGET requests a pass. tests/test_openssh.sh
 * drives it with sftp, scp and scp -O.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/random.h>

#include "../../ssh/ssh_server.h"
#include "../xfer.h"
#include "../nsfs.h"

#define SSH_OUT_MAX 2048
#define XFER_BUDGET 8

void nsfs_posix_root(const char *r);

static ssh_server_t eng;
static uint8_t out[SSH_OUT_MAX];
static uint32_t out_len;
static xfer_t *xf;
static uint32_t flags;
static bool want_close;
static time_t linger;               // as netserve's LINGER_TICKS

static bool w(void *u, const uint8_t *d, uint32_t n) {
	(void)u;
	if (n > SSH_OUT_MAX - out_len) return false;
	memcpy(out + out_len, d, n);
	out_len += n;
	return true;
}

static void rnd(void *u, uint8_t *o, uint32_t n) {
	(void)u;
	while (n) { ssize_t r = getrandom(o, n, 0); if (r > 0) { o += r; n -= (uint32_t)r; } }
}

static int auth(void *u, const char *user, const uint8_t *pw, uint32_t n) {
	(void)u; (void)user; (void)pw; (void)n;
	return SSHS_AUTH_OK;
}

static void xlog(void *u, const char *line) { (void)u; fprintf(stderr, "xfer_host: %s\n", line); }

static void ev(void *u, sshs_event_t e, const uint8_t *d, uint32_t n, const char *text) {
	(void)u;
	switch (e) {
	case SSHS_EV_LOG: fprintf(stderr, "xfer_host: %s\n", text); break;
	case SSHS_EV_SHELL: {
		static const char m[] = "no shell here\r\n";
		sshs_send(&eng, (const uint8_t *)m, sizeof(m) - 1, SSH_OUT_MAX - out_len);
		sshs_end(&eng, 1);
		break;
	}
	case SSHS_EV_SFTP:
	case SSHS_EV_EXEC: {
		char why[96] = "";
		xf = e == SSHS_EV_EXEC ? xfer_scp(text, "/home", flags, xlog, NULL, why, sizeof(why))
		                       : xfer_sftp("/home", flags, xlog, NULL);
		if (!xf) {
			char m[112];
			int l = snprintf(m, sizeof(m), "\1%s\n", why);
			fprintf(stderr, "xfer_host: %s\n", why);
			sshs_send(&eng, (const uint8_t *)m, (uint32_t)l, SSH_OUT_MAX - out_len);
			sshs_end(&eng, 1);
			break;
		}
		sshs_grow_window(&eng, XFER_IN - SSHS_LOCAL_WINDOW);
		break;
	}
	case SSHS_EV_DATA:
		if (xf) xfer_put(xf, d, n);
		break;
	case SSHS_EV_EOF:
		if (xf) xfer_eof(xf);
		break;
	case SSHS_EV_CLOSED:
		fprintf(stderr, "xfer_host: %s\n", text ? text : "closed");
		want_close = true;
		break;
	}
}

static void serve(int c) {
	uint8_t seed[32];
	memset(seed, 7, sizeof(seed));             // a fixed host key: tests skip the check anyway
	out_len = 0;
	xf = NULL;
	want_close = false;
	linger = 0;
	sshs_init(&eng, seed, NULL, w, ev, rnd, auth);
	sshs_set_auth(&eng, SSHS_AUTH_ANY, NULL, NULL);
	sshs_set_services(&eng, SSHS_SVC_SFTP | SSHS_SVC_SCP);

	for (;;) {
		// as netserve's ssh_in(): fed only when its answer has room
		if (!want_close && SSH_OUT_MAX - out_len >= 600) {
			struct pollfd p = { c, POLLIN, 0 };
			if (poll(&p, 1, (xf || out_len) ? 0 : 50) > 0) {
				uint8_t b[1460];
				ssize_t r = read(c, b, sizeof(b));
				if (r <= 0) break;
				sshs_feed(&eng, b, (uint32_t)r);
			}
		}
		// as xfer_pump()
		if (xf && !sshs_is_closed(&eng)) {
			uint32_t n, st;
			uint32_t used = xfer_step(xf, XFER_BUDGET);
			if (used) sshs_consumed(&eng, used);
			const uint8_t *p = xfer_out(xf, &n);
			while (n) {
				uint32_t k = sshs_send(&eng, p, n, SSH_OUT_MAX - out_len);
				if (!k) break;
				xfer_sent(xf, k);
				p = xfer_out(xf, &n);
			}
			if (xfer_done(xf, &st)) {
				xfer_free(xf);
				xf = NULL;
				sshs_end_wait(&eng, st);
				linger = time(NULL) + 5;
			}
		}
		if (out_len) {
			ssize_t r = write(c, out, out_len);
			if (r < 0) break;
			memmove(out, out + r, out_len - (uint32_t)r);
			out_len -= (uint32_t)r;
		}
		if (linger && time(NULL) >= linger && !sshs_is_closed(&eng)) sshs_disconnect(&eng, "session ended");
		if (want_close && !out_len) break;
	}
	if (xf) { xfer_free(xf); xf = NULL; }      // as end(): files closed, a .part removed
	close(c);
	fprintf(stderr, "xfer_host: connection over\n");
}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: xfer_host PORT ROOT [read]\n"); return 2; }
	nsfs_posix_root(argv[2]);
	if (argc > 3 && !strcmp(argv[3], "read")) flags = XFER_READONLY;
	signal(SIGPIPE, SIG_IGN);
	int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 4)) { perror("xfer_host"); return 1; }
	fprintf(stderr, "xfer_host: listening on %s\n", argv[1]);
	for (;;) {
		int c = accept(l, NULL, NULL);
		if (c >= 0) serve(c);
	}
}
