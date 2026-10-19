/*
 * print's transport over POSIX sockets, for the host tests. See
 * host_net.h.
 */

#include "host_net.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>

static int fd = -1;
static out_t sock_out;

static bool sock_write(out_t *o, const void *d, uint32_t n) {
	const uint8_t *p = d;
	(void)o;
	while (n) {
		ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
		if (k <= 0) return false;
		p += k;
		n -= (uint32_t)k;
	}
	return true;
}

static int sock_read(void *ctx, uint8_t *buf, int max) {
	struct pollfd pf = { fd, POLLIN, 0 };
	(void)ctx;
	if (poll(&pf, 1, 30000) <= 0) return -1;
	return (int)recv(fd, buf, (size_t)max, 0);
}

static bool sock_connect(void *ctx, const ipp_url_t *u, char *err, int n) {
	struct addrinfo hints, *res = NULL;
	char port[8];
	(void)ctx;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof(port), "%d", u->port);
	if (getaddrinfo(u->host, port, &hints, &res) || !res) {
		snprintf(err, (size_t)n, "cannot find %s", u->host);
		return false;
	}
	fd = socket(res->ai_family, res->ai_socktype, 0);
	if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen)) {
		snprintf(err, (size_t)n, "cannot connect to %s:%d", u->host, u->port);
		freeaddrinfo(res);
		if (fd >= 0) close(fd);
		fd = -1;
		return false;
	}
	freeaddrinfo(res);
	memset(&sock_out, 0, sizeof(sock_out));
	sock_out.write = sock_write;
	return true;
}

static void sock_close(void *ctx) {
	(void)ctx;
	if (fd >= 0) close(fd);
	fd = -1;
}

void host_transport(transport_t *t) {
	memset(t, 0, sizeof(*t));
	t->connect = sock_connect;
	t->close = sock_close;
	t->sock = &sock_out;
	t->read = sock_read;
}
