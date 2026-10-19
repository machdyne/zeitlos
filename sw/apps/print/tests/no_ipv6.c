/*
 * For tests/run_tests.sh on a build machine without IPv6 (a container):
 * ippeveprinter, CUPS's reference IPP Everywhere printer, will not start
 * there, because it insists on an IPv6 listener as well as an IPv4 one.
 * Preloaded into it, this makes the IPv6 listener an IPv4 socket that
 * is never called on; everything else is the real printer. Not part of
 * Zeitlos; only used when `ippeveprinter` fails without it.
 *
 *   cc -shared -fPIC -o /tmp/no_ipv6.so tests/no_ipv6.c -ldl
 *   LD_PRELOAD=/tmp/no_ipv6.so ippeveprinter -n 127.0.0.1 ...
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
static char fake[4096];
int socket(int d, int t, int p) {
	static int (*real)(int, int, int);
	int fd;
	if (!real) real = dlsym(RTLD_NEXT, "socket");
	if (d != AF_INET6) return real(d, t, p);
	fd = real(AF_INET, t, p);
	if (fd >= 0 && fd < 4096) fake[fd] = 1;
	return fd;
}
int bind(int fd, const struct sockaddr *a, socklen_t l) {
	static int (*real)(int, const struct sockaddr *, socklen_t);
	if (!real) real = dlsym(RTLD_NEXT, "bind");
	if (fd >= 0 && fd < 4096 && fake[fd]) return 0;
	{
		// the "IPv6" listener, made as IPv4 by libcups: the port is the
		// IPv4 listener's already -- a socket that never gets a call
		int e = real(fd, a, l);
		if (e && a && a->sa_family == AF_INET) return 0;
		return e;
	}
}
int setsockopt(int fd, int lv, int on, const void *v, socklen_t l) {
	static int (*real)(int, int, int, const void *, socklen_t);
	if (!real) real = dlsym(RTLD_NEXT, "setsockopt");
	if (fd >= 0 && fd < 4096 && fake[fd]) return 0;
	return real(fd, lv, on, v, l);
}
int close(int fd) {
	static int (*real)(int);
	if (!real) real = dlsym(RTLD_NEXT, "close");
	if (fd >= 0 && fd < 4096) fake[fd] = 0;
	return real(fd);
}
#include <netdb.h>
#include <string.h>
int getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r) {
	static int (*real)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
	struct addrinfo h2, *p;
	int e;
	if (!real) real = dlsym(RTLD_NEXT, "getaddrinfo");
	if (!h || h->ai_family != AF_INET6) return real(n, s, h, r);
	h2 = *h;
	h2.ai_family = AF_INET;
	e = real(n && strchr(n, ':') ? NULL : n, s, &h2, r);
	if (!e) for (p = *r; p; p = p->ai_next) p->ai_family = AF_INET6;
	return e;
}
