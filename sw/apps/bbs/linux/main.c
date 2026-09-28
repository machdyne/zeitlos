/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- the server, on Linux. docs/bbs_linux.md.
 *
 *   bbs -d /var/lib/bbs [-t 23] [-s /var/lib/bbs/bbs.sock]     the server
 *   bbs -d /var/lib/bbs --connect                              a caller, via SSH
 *   bbs -d /var/lib/bbs --connect --local                      the sysop, here
 *
 * The server is one process and one poll() loop, as on Zeitlos: telnet
 * callers directly, and everyone else through a Unix socket. SSH is
 * OpenSSH's job -- a `bbs` account whose ForceCommand is `bbs
 * --connect`, which relays the caller's terminal to the socket -- so
 * there is no SSH server here to trust, only the one the machine
 * already runs.
 *
 * Telnet as netserve speaks it on Zeitlos: WILL ECHO and WILL SGA, every
 * other option refused once, IAC IAC for a literal 0xFF both ways, and
 * Enter delivered as one CR however the client sends it.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../core/bbs.h"

#define MAX_CONN 64
#define TX_MAX 4096
#define HELLO_MAX 256

enum { C_FREE = 0, C_TELNET, C_SOCK };
enum { TN_DATA = 0, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC };
#define IAC 255
#define DONT 254
#define DO 253
#define WONT 252
#define WILL 251
#define SB 250
#define SE 240

typedef struct {
	int kind, fd, node;
	bool hello_done;            // C_SOCK: the identity line has arrived
	char hello[HELLO_MAX];
	int hello_len;
	uint8_t tn, tn_cmd;
	bool last_cr;
	uint32_t answered;          // options already answered, by bit
	uint8_t tx[TX_MAX];         // escaped for telnet, waiting for the socket
	int tx_len;
	bool closing;               // send what is left, then close
} conn_t;

static conn_t conns[MAX_CONN];
static volatile sig_atomic_t stop;

static void on_signal(int s) { (void)s; stop = 1; }

static void nonblock(int fd) {
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void conn_close(conn_t *c) {
	if (c->node >= 0) bbs_hangup(c->node);
	close(c->fd);
	memset(c, 0, sizeof(*c));
	c->node = -1;
}

static void tx_add(conn_t *c, const void *d, int n) {
	if (c->tx_len + n > TX_MAX) n = TX_MAX - c->tx_len;
	memcpy(c->tx + c->tx_len, d, (size_t)n);
	c->tx_len += n;
}

static void tn_reply(conn_t *c, uint8_t cmd, uint8_t opt) {
	uint8_t b[3] = { IAC, cmd, opt };
	tx_add(c, b, 3);
}

static void tn_option(conn_t *c, uint8_t cmd, uint8_t opt) {
	uint32_t bit = opt < 32 ? (1u << opt) : 0;
	if (bit && (c->answered & bit)) return;
	if (cmd == DO) { if (opt != 1 && opt != 3) tn_reply(c, WONT, opt); }
	else if (cmd == WILL) tn_reply(c, DONT, opt);
	else if (cmd == DONT && opt == 1) tn_reply(c, WONT, opt);
	c->answered |= bit;
}

// Telnet to plain bytes (see netserve.c's tn_decode(), the same rules).
static int tn_decode(conn_t *c, const uint8_t *in, int n, uint8_t *out) {
	int k = 0;
	for (int i = 0; i < n; i++) {
		uint8_t b = in[i];
		switch (c->tn) {
		case TN_DATA:
			if (b == IAC) { c->tn = TN_IAC; break; }
			if (c->last_cr && (b == '\n' || b == 0)) { c->last_cr = false; break; }
			c->last_cr = (b == '\r');
			out[k++] = (b == '\n') ? '\r' : b;
			break;
		case TN_IAC:
			c->tn = TN_DATA;
			if (b == IAC) { c->last_cr = false; out[k++] = IAC; }
			else if (b == DO || b == DONT || b == WILL || b == WONT) { c->tn_cmd = b; c->tn = TN_OPT; }
			else if (b == SB) c->tn = TN_SB;
			break;
		case TN_OPT: tn_option(c, c->tn_cmd, b); c->tn = TN_DATA; break;
		case TN_SB: if (b == IAC) c->tn = TN_SB_IAC; break;
		case TN_SB_IAC: c->tn = (b == SE) ? TN_DATA : TN_SB; break;
		}
	}
	return k;
}

static void start_node(conn_t *c, const char *transport, const char *peer, const char *user) {
	bbs_conn_t w = { transport, peer, user };
	c->node = bbs_connect(&w);
	if (c->node < 0) {
		const char *t = bbs_busy_text();
		tx_add(c, t, (int)strlen(t));
		c->closing = true;
	}
}

// "ZBBS1 <transport> <peer> <user>" from `bbs --connect`.
static void hello_line(conn_t *c) {
	char tr[16] = "", peer[64] = "", user[64] = "";
	char *s = c->hello, *f[4] = { 0 };
	int k = 0;
	for (char *t = strtok(s, " "); t && k < 4; t = strtok(NULL, " ")) f[k++] = t;
	if (k < 2 || strcmp(f[0], "ZBBS1") || (strcmp(f[1], "ssh") && strcmp(f[1], "local"))) {
		c->closing = true;
		return;
	}
	snprintf(tr, sizeof(tr), "%s", f[1]);
	if (k > 2 && strcmp(f[2], "-")) snprintf(peer, sizeof(peer), "%s", f[2]);
	if (k > 3 && strcmp(f[3], "-")) snprintf(user, sizeof(user), "%s", f[3]);
	c->hello_done = true;
	start_node(c, tr, peer, user);
}

static void conn_read(conn_t *c) {
	uint8_t buf[2048], out[2048];
	ssize_t n = read(c->fd, buf, sizeof(buf));
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { conn_close(c); return; }
	if (n < 0) return;
	int i = 0;
	if (c->kind == C_SOCK && !c->hello_done) {
		for (; i < n; i++) {
			if (buf[i] == '\n') { c->hello[c->hello_len] = 0; hello_line(c); i++; break; }
			if (c->hello_len < HELLO_MAX - 1) c->hello[c->hello_len++] = (char)buf[i];
			else { c->closing = true; return; }
		}
		if (!c->hello_done) return;
	}
	if (c->node < 0 || c->closing) return;
	if (c->kind == C_TELNET) {
		int k = tn_decode(c, buf + i, (int)n - i, out);
		if (k) bbs_input(c->node, out, (uint32_t)k);
	} else if (n - i > 0) bbs_input(c->node, buf + i, (uint32_t)(n - i));
}

// The core's output into tx (escaping 0xFF for telnet), then the socket.
static void conn_flush(conn_t *c) {
	if (c->node >= 0) {
		const uint8_t *p;
		uint32_t n;
		while ((n = bbs_output(c->node, &p)) > 0 && c->tx_len < TX_MAX - 2) {
			uint32_t used = 0;
			while (used < n && c->tx_len < TX_MAX - 2) {
				c->tx[c->tx_len++] = p[used];
				if (c->kind == C_TELNET && p[used] == IAC) c->tx[c->tx_len++] = IAC;
				used++;
			}
			bbs_consumed(c->node, used);
		}
		if (bbs_wants_close(c->node)) c->closing = true;
	}
	if (c->tx_len) {
		ssize_t w = write(c->fd, c->tx, (size_t)c->tx_len);
		if (w < 0 && errno != EAGAIN && errno != EINTR) { conn_close(c); return; }
		if (w > 0) { memmove(c->tx, c->tx + w, (size_t)(c->tx_len - w)); c->tx_len -= (int)w; }
	}
	if (c->closing && !c->tx_len) conn_close(c);
}

// IPv4 alone, for a machine with no IPv6.
static int listen_tcp4(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	if (fd < 0) return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) { close(fd); return -1; }
	nonblock(fd);
	return fd;
}

// IPv6 and IPv4 on one socket where the machine has IPv6.
static int listen_tcp(int port) {
	int fd = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0;
	if (fd < 0) return listen_tcp4(port);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
	struct sockaddr_in6 a;
	memset(&a, 0, sizeof(a));
	a.sin6_family = AF_INET6;
	a.sin6_port = htons((uint16_t)port);
	a.sin6_addr = in6addr_any;
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) { close(fd); return -1; }
	nonblock(fd);
	return fd;
}

static int listen_unix(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(a.sun_path)) { close(fd); return -1; }
	strcpy(a.sun_path, path);
	unlink(path);
	// Only this user (the `bbs` account) may connect: the socket trusts
	// what a --connect client says about where its caller came from.
	mode_t old = umask(0077);
	int r = bind(fd, (struct sockaddr *)&a, sizeof(a));
	umask(old);
	if (r < 0 || listen(fd, 16) < 0) { close(fd); return -1; }
	nonblock(fd);
	return fd;
}

static void accept_one(int lfd, int kind) {
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	int fd = accept(lfd, (struct sockaddr *)&ss, &sl);
	if (fd < 0) return;
	conn_t *c = NULL;
	for (int i = 0; i < MAX_CONN; i++) if (conns[i].kind == C_FREE) { c = &conns[i]; break; }
	if (!c) { close(fd); return; }
	nonblock(fd);
	memset(c, 0, sizeof(*c));
	c->kind = kind;
	c->fd = fd;
	c->node = -1;
	if (kind == C_TELNET) {
		char peer[64] = "";
		if (ss.ss_family == AF_INET6) {
			struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
			inet_ntop(AF_INET6, &a6->sin6_addr, peer, sizeof(peer));
			if (!strncmp(peer, "::ffff:", 7)) memmove(peer, peer + 7, strlen(peer + 7) + 1);
		} else if (ss.ss_family == AF_INET) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, peer, sizeof(peer));
		}
		static const uint8_t offer[] = { IAC, WILL, 1, IAC, WILL, 3 };   // ECHO, SGA
		tx_add(c, offer, sizeof(offer));
		start_node(c, "telnet", peer, "");
	}
}

// -- the link to this node's fed (docs/bbs.md, "Federation") --

static int fed_fd = -1;
static time_t fed_next;			// when to try again

static void fed_down(void) {
	if (fed_fd >= 0) { close(fed_fd); fed_fd = -1; bbs_fed_up(false); }
	fed_next = time(NULL) + 5;
}

static void fed_try(void) {
	const char *path = bbs_fed_target();
	if (fed_fd >= 0 || !path[0] || time(NULL) < fed_next) return;
	struct sockaddr_un ua;
	memset(&ua, 0, sizeof(ua));
	ua.sun_family = AF_UNIX;
	snprintf(ua.sun_path, sizeof(ua.sun_path), "%s", path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&ua, sizeof(ua))) {
		if (fd >= 0) close(fd);
		fed_next = time(NULL) + 5;
		return;
	}
	nonblock(fd);
	fed_fd = fd;
	bbs_fed_up(true);
}

static void fed_io(short revents) {
	if (fed_fd < 0) return;
	if (revents & (POLLIN | POLLHUP | POLLERR)) {
		uint8_t b[4096];
		ssize_t r = read(fed_fd, b, sizeof(b));
		if (r > 0) bbs_fed_input(b, (uint32_t)r);
		else if (r == 0 || (errno != EAGAIN && errno != EINTR)) { fed_down(); return; }
	}
	const uint8_t *p;
	uint32_t n;
	while (fed_fd >= 0 && (n = bbs_fed_output(&p)) > 0) {
		ssize_t w = write(fed_fd, p, n);
		if (w <= 0) { if (w < 0 && errno != EAGAIN && errno != EINTR) fed_down(); break; }
		bbs_fed_consumed((uint32_t)w);
	}
}

static int serve(const char *datadir, int port, const char *sock) {
	int tfd = -1, ufd;
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	if (!bbs_init(datadir)) return 1;
	for (int i = 0; i < MAX_CONN; i++) conns[i].node = -1;
	if (port > 0 && (tfd = listen_tcp(port)) < 0) {
		fprintf(stderr, "bbs: cannot listen on port %d: %s\n", port, strerror(errno));
		return 1;
	}
	if ((ufd = listen_unix(sock)) < 0) {
		fprintf(stderr, "bbs: cannot listen on %s: %s\n", sock, strerror(errno));
		return 1;
	}
	fprintf(stderr, "bbs: telnet on %d, connect at %s\n", port, sock);
	while (!stop) {
		struct pollfd pf[MAX_CONN + 3];
		conn_t *who[MAX_CONN + 3];
		int np = 0, fi = -1;
		fed_try();
		if (fed_fd >= 0) {
			const uint8_t *p;
			fi = np; pf[np].fd = fed_fd; pf[np].events = POLLIN | (bbs_fed_output(&p) ? POLLOUT : 0); who[np++] = NULL;
		}
		if (tfd >= 0) { pf[np].fd = tfd; pf[np].events = POLLIN; who[np++] = NULL; }
		pf[np].fd = ufd; pf[np].events = POLLIN; who[np++] = NULL;
		for (int i = 0; i < MAX_CONN; i++) {
			conn_t *c = &conns[i];
			if (c->kind == C_FREE) continue;
			pf[np].fd = c->fd;
			pf[np].events = POLLIN | (c->tx_len ? POLLOUT : 0);
			who[np++] = c;
		}
		poll(pf, (nfds_t)np, 100);
		for (int i = 0; i < np; i++) {
			if (i == fi) { fed_io(pf[i].revents); continue; }
			if (!who[i]) {
				if (pf[i].revents & POLLIN) accept_one(pf[i].fd, pf[i].fd == tfd ? C_TELNET : C_SOCK);
				continue;
			}
			if (who[i]->kind == C_FREE) continue;
			if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) conn_read(who[i]);
		}
		bbs_poll();
		for (int i = 0; i < MAX_CONN; i++) if (conns[i].kind != C_FREE) conn_flush(&conns[i]);
		fed_io(0);				// what the callers' posts queued
	}
	for (int i = 0; i < MAX_CONN; i++) if (conns[i].kind != C_FREE) conn_close(&conns[i]);
	unlink(sock);
	fprintf(stderr, "bbs: stopped\n");
	return 0;
}

// -- `bbs --connect`: the caller's terminal, relayed to the server --

static struct termios saved;
static bool raw_on;

static void raw_off(void) {
	if (raw_on) tcsetattr(0, TCSANOW, &saved);
}

static int connect_client(const char *sock, bool local) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("The BBS is not running just now -- please try again later.\r\n");
		return 1;
	}
	// Where the caller came from: sshd says, in SSH_CONNECTION
	// ("client-ip client-port server-ip server-port").
	char peer[64] = "-", hello[160];
	const char *sc = getenv("SSH_CONNECTION");
	if (!local && sc) { sscanf(sc, "%63s", peer); }
	snprintf(hello, sizeof(hello), "ZBBS1 %s %s -\n", local ? "local" : "ssh", local ? "-" : peer);
	if (write(fd, hello, strlen(hello)) < 0) return 1;
	if (isatty(0) && tcgetattr(0, &saved) == 0) {
		struct termios t = saved;
		cfmakeraw(&t);
		tcsetattr(0, TCSANOW, &t);
		raw_on = true;
		atexit(raw_off);
	}
	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		struct pollfd pf[2] = { { 0, POLLIN, 0 }, { fd, POLLIN, 0 } };
		if (poll(pf, 2, -1) < 0 && errno != EINTR) break;
		char b[4096];
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(0, b, sizeof(b));
			if (n <= 0) break;
			if (write(fd, b, (size_t)n) != n) break;
		}
		if (pf[1].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(fd, b, sizeof(b));
			if (n <= 0) break;
			for (ssize_t o = 0; o < n; ) {
				ssize_t w = write(1, b + o, (size_t)(n - o));
				if (w <= 0) { if (errno == EINTR) continue; goto done; }
				o += w;
			}
		}
	}
done:
	raw_off();
	return 0;
}

static void usage(void) {
	fprintf(stderr,
		"usage: bbs -d DATADIR [-t PORT] [-s SOCKET]       run the BBS\n"
		"       bbs -d DATADIR [-s SOCKET] --connect [--local]\n"
		"  -t PORT    telnet port (0: none; default 2323)\n"
		"  -s SOCKET  where --connect finds the server (default DATADIR/bbs.sock)\n"
		"  --connect  relay this terminal to the server: the ForceCommand for SSH\n"
		"  --local    ... as a local caller rather than an SSH one\n");
}

int main(int argc, char **argv) {
	const char *dir = NULL, *sock = NULL;
	int port = 2323;
	bool connect_mode = false, local = false;
	char sockbuf[BBS_PATH_MAX];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) sock = argv[++i];
		else if (!strcmp(argv[i], "--connect")) connect_mode = true;
		else if (!strcmp(argv[i], "--local")) local = true;
		else { usage(); return 2; }
	}
	if (!dir) { usage(); return 2; }
	if (!sock) { snprintf(sockbuf, sizeof(sockbuf), "%s/bbs.sock", dir); sock = sockbuf; }
	if (connect_mode) return connect_client(sock, local);
	return serve(dir, port, sock);
}
