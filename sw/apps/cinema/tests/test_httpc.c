/*
 * test_httpc -- httpc.c on the build machine (make -C sw/apps/cinema test).
 *
 * Made-up answers, each fed whole and in pieces of 1, 3, 17 and 512
 * bytes, which must come to the same thing; then, with
 *     test_httpc --server HOST PORT /path LOCALFILE [PIECE]
 * a real server over a real socket: the file fetched in ranges on one
 * connection (reconnecting if the server closes) and compared with the
 * local copy -- the same calls, in the same order, as netsrc.c makes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "../httpc.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t got[1 << 16];
static uint32_t gotn;
static void body(void *u, const uint8_t *p, uint32_t n) {
	(void)u;
	if (gotn + n <= sizeof(got)) memcpy(got + gotn, p, n);
	gotn += n;
}

/* one answer, at one piece size; the state it ends in */
static int run(hc_t *h, uint32_t off, uint32_t len, const char *ans, uint32_t ansn, uint32_t piece, int closed) {
	char req[512];
	uint32_t i, rn;
	memset(h, 0, sizeof(*h));
	rn = hc_request(h, req, sizeof(req), "host:8000", "/a%20b.zv", off, len);
	CHECK(rn > 0 && rn == strlen(req), "request length");
	gotn = 0;
	for (i = 0; i < ansn; i += piece) {
		uint32_t k = ansn - i < piece ? ansn - i : piece;
		hc_feed(h, (const uint8_t *)ans + i, k, body, NULL);
	}
	if (closed) hc_closed(h);
	return h->state;
}

static const uint32_t pieces[] = { 1, 3, 17, 512, 1 << 20 };

static void same_at_every_size(const char *name, uint32_t off, uint32_t len, const char *ans, uint32_t ansn,
	int closed, int want_state, int want_err, const char *want_body, uint32_t want_n, int want_close) {
	unsigned i;
	for (i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
		hc_t h;
		int st = run(&h, off, len, ans, ansn, pieces[i], closed);
		CHECK(st == want_state, "%s: state %d, wanted %d (pieces of %u)", name, st, want_state, pieces[i]);
		CHECK(h.err == want_err, "%s: error %d, wanted %d (pieces of %u)", name, h.err, want_err, pieces[i]);
		if (want_state == HC_DONE) {
			CHECK(gotn == want_n && !memcmp(got, want_body, want_n), "%s: body, %u bytes for %u (pieces of %u)",
				name, gotn, want_n, pieces[i]);
			CHECK((int)h.to_close == want_close, "%s: to_close %d (pieces of %u)", name, h.to_close, pieces[i]);
		}
	}
}

#define A(s) s, (uint32_t)(sizeof(s) - 1)

static void test_request(void) {
	hc_t h;
	char req[512], small[40];
	memset(&h, 0, sizeof(h));
	CHECK(hc_request(&h, req, sizeof(req), "192.168.1.5:8000", "/films/general.zv", 4096, 8192) > 0, "request");
	CHECK(strstr(req, "GET /films/general.zv HTTP/1.1\r\n") == req, "request line: %s", req);
	CHECK(strstr(req, "\r\nHost: 192.168.1.5:8000\r\n") != NULL, "host");
	CHECK(strstr(req, "\r\nRange: bytes=4096-12287\r\n") != NULL, "range: %s", req);
	CHECK(!strcmp(req + strlen(req) - 4, "\r\n\r\n"), "ends with an empty line");
	CHECK(h.state == HC_HEAD, "waiting for the answer");
	CHECK(hc_request(&h, small, sizeof(small), "h", "/p", 0, 1) == 0, "a request that does not fit is none");
	CHECK(hc_request(&h, req, sizeof(req), "h", "/p", 0, 0) == 0, "nothing asked for");
	CHECK(hc_request(&h, req, sizeof(req), "h", "/p", 0xfffffff0u, 0x20) == 0, "a range past 4 GB");
}

static void test_answers(void) {
	hc_t h;
	same_at_every_size("206", 10, 5,
		A("HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
		  "Content-Length: 5\r\nContent-Range: bytes 10-14/100\r\n\r\nHELLO"),
		0, HC_DONE, HC_E_NONE, "HELLO", 5, 0);
	run(&h, 10, 5, A("HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\nContent-Range: bytes 10-14/100\r\n\r\nHELLO"), 7, 0);
	CHECK(h.total == 100, "the file's size from Content-Range: %u", h.total);
	same_at_every_size("206 cut at the file's end", 98, 8,
		A("HTTP/1.1 206 Partial Content\r\nContent-Length: 2\r\nContent-Range: bytes 98-99/100\r\n\r\nZZ"),
		0, HC_DONE, HC_E_NONE, "ZZ", 2, 0);
	same_at_every_size("header names in any case, bare LF", 0, 3,
		A("HTTP/1.1 206 OK\ncontent-LENGTH:3\nCONNECTION: Close\n\nabc"),
		0, HC_DONE, HC_E_NONE, "abc", 3, 1);
	same_at_every_size("416: past the end", 100, 8,
		A("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */100\r\nContent-Length: 0\r\n\r\n"),
		0, HC_DONE, HC_E_NONE, "", 0, 0);
	same_at_every_size("416 with a body", 100, 8,
		A("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 4\r\n\r\nnope"),
		0, HC_DONE, HC_E_NONE, "", 0, 0);
	same_at_every_size("200 to a range from 0: the first bytes, then close", 0, 4,
		A("HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n0123456789"),
		0, HC_DONE, HC_E_NONE, "0123", 4, 1);
	same_at_every_size("200 without a length, ended by the close", 0, 64,
		A("HTTP/1.0 200 OK\r\n\r\nshort file"),
		1, HC_DONE, HC_E_NONE, "short file", 10, 1);
	same_at_every_size("200 to a range from 100: no ranges here", 100, 4,
		A("HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n0123456789"),
		0, HC_FAIL, HC_E_NORANGE, "", 0, 1);
	same_at_every_size("404", 0, 4,
		A("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found"),
		0, HC_FAIL, HC_E_STATUS, "", 0, 1);
	same_at_every_size("chunked", 0, 4,
		A("HTTP/1.1 206 Partial Content\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\n\r\n"),
		0, HC_FAIL, HC_E_CHUNKED, "", 0, 1);
	same_at_every_size("not HTTP", 0, 4, A("SSH-2.0-OpenSSH_9.6\r\n"), 0, HC_FAIL, HC_E_HEADER, "", 0, 1);
	same_at_every_size("100 Continue first", 0, 2,
		A("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 206 Partial Content\r\nContent-Length: 2\r\n\r\nok"),
		0, HC_DONE, HC_E_NONE, "ok", 2, 0);
	same_at_every_size("two lengths that disagree", 0, 2,
		A("HTTP/1.1 206 Partial Content\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nok!"),
		0, HC_FAIL, HC_E_HEADER, "", 0, 1);
	same_at_every_size("a length that is not a number", 0, 2,
		A("HTTP/1.1 206 Partial Content\r\nContent-Length: lots\r\n\r\nok"),
		0, HC_FAIL, HC_E_HEADER, "", 0, 1);
	same_at_every_size("a length past 32 bits", 0, 2,
		A("HTTP/1.1 206 Partial Content\r\nContent-Length: 4294967296\r\n\r\nok"),
		0, HC_FAIL, HC_E_HEADER, "", 0, 1);
	same_at_every_size("closed inside the body", 0, 8,
		A("HTTP/1.1 206 Partial Content\r\nContent-Length: 8\r\n\r\nabc"),
		1, HC_FAIL, HC_E_SHORT, "", 0, 1);
	same_at_every_size("closed before any answer", 0, 8, A(""), 1, HC_FAIL, HC_E_SHORT, "", 0, 1);
	{
		/* a header far longer than a line kept, that is none of ours: read past */
		static char ans[4096];
		int n = snprintf(ans, sizeof(ans), "HTTP/1.1 206 Partial Content\r\nContent-Security-Policy: ");
		memset(ans + n, 'x', 2000); n += 2000;
		n += snprintf(ans + n, sizeof(ans) - (size_t)n, "\r\nContent-Length: 3\r\n\r\nabc");
		same_at_every_size("a long header that does not matter", 0, 3, ans, (uint32_t)n, 0, HC_DONE, HC_E_NONE, "abc", 3, 0);
		n = snprintf(ans, sizeof(ans), "HTTP/1.1 206 Partial Content\r\nContent-Length: 3");
		memset(ans + n, ' ', 400); n += 400;
		n += snprintf(ans + n, sizeof(ans) - (size_t)n, "\r\n\r\nabc");
		same_at_every_size("a long header that does", 0, 3, ans, (uint32_t)n, 0, HC_FAIL, HC_E_HEADER, "", 0, 1);
		/* headers without end */
		n = snprintf(ans, sizeof(ans), "HTTP/1.1 206 Partial Content\r\n");
		while (n < 4000) n += snprintf(ans + n, sizeof(ans) - (size_t)n, "X-Pad: yes\r\n");
		{
			hc_t h2;
			int i;
			char req[256];
			memset(&h2, 0, sizeof(h2));
			hc_request(&h2, req, sizeof(req), "h", "/p", 0, 3);
			for (i = 0; i < 4 && h2.state == HC_HEAD; i++) hc_feed(&h2, (const uint8_t *)ans, (uint32_t)n, body, NULL);
			CHECK(h2.state == HC_FAIL && h2.err == HC_E_HEADER, "headers without end are refused");
		}
	}
	{
		/* two answers on one connection, one after the other */
		hc_t h2;
		char req[256];
		memset(&h2, 0, sizeof(h2));
		hc_request(&h2, req, sizeof(req), "h", "/p", 0, 2);
		gotn = 0;
		hc_feed(&h2, (const uint8_t *)"HTTP/1.1 206 x\r\nContent-Length: 2\r\n\r\nab", 39, body, NULL);
		CHECK(h2.state == HC_DONE && gotn == 2 && !h2.to_close, "first of two");
		hc_request(&h2, req, sizeof(req), "h", "/p", 2, 2);
		hc_feed(&h2, (const uint8_t *)"HTTP/1.1 206 x\r\nContent-Length: 2\r\n\r\ncd", 39, body, NULL);
		CHECK(h2.state == HC_DONE && gotn == 4 && !memcmp(got, "abcd", 4), "second of two");
	}
}

static void test_url(void) {
	char host[64], path[128];
	uint16_t port;
	CHECK(hc_url("http://192.168.1.5:8000/films/a.zv", host, sizeof(host), &port, path, sizeof(path)) &&
		!strcmp(host, "192.168.1.5") && port == 8000 && !strcmp(path, "/films/a.zv"), "address, port and path");
	CHECK(hc_url("HTTP://nas.local/a.zv", host, sizeof(host), &port, path, sizeof(path)) &&
		!strcmp(host, "nas.local") && port == 80 && !strcmp(path, "/a.zv"), "a name, port 80");
	CHECK(hc_url("http://nas", host, sizeof(host), &port, path, sizeof(path)) && !strcmp(path, "/"), "no path is /");
	CHECK(hc_url("http://nas/the general.zv#t=3", host, sizeof(host), &port, path, sizeof(path)) &&
		!strcmp(path, "/the%20general.zv"), "a space is escaped, a fragment not sent: %s", path);
	CHECK(!hc_url("https://nas/a.zv", host, sizeof(host), &port, path, sizeof(path)), "https is not here");
	CHECK(!hc_url("/media/video/a.zv", host, sizeof(host), &port, path, sizeof(path)), "a path is not a URL");
	CHECK(!hc_url("http:///a.zv", host, sizeof(host), &port, path, sizeof(path)), "no host");
	CHECK(!hc_url("http://nas:0/a", host, sizeof(host), &port, path, sizeof(path)), "port 0");
	CHECK(!hc_url("http://nas:99999/a", host, sizeof(host), &port, path, sizeof(path)), "port past 65535");
	CHECK(!hc_url("http://nas:80x/a", host, sizeof(host), &port, path, sizeof(path)), "a port that is not a number");
	CHECK(!hc_url("http://a-very-long-host-name-indeed/a", host, 8, &port, path, sizeof(path)), "a host that does not fit");
	CHECK(!hc_url("http://nas/a-very-long-path", host, sizeof(host), &port, path, 8), "a path that does not fit");
}

/* -- a real server ---------------------------------------------------- */

static int dial(const char *host, const char *port) {
	struct addrinfo hints, *ai, *a;
	int fd = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &ai)) return -1;
	for (a = ai; a; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0) continue;
		if (!connect(fd, a->ai_addr, a->ai_addrlen)) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	return fd;
}

static uint8_t *want, *have;
static uint32_t haven;
static void body_file(void *u, const uint8_t *p, uint32_t n) {
	(void)u;
	memcpy(have + haven, p, n);
	haven += n;
}

static int against_server(const char *host, const char *port, const char *path, const char *local, uint32_t piece) {
	FILE *f = fopen(local, "rb");
	long size;
	uint32_t off = 0, connects = 0, requests = 0;
	int fd = -1;
	hc_t h;
	char hostport[128], req[512];
	if (!f) { printf("cannot open %s\n", local); return 1; }
	fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
	want = malloc((size_t)size + 1); have = malloc((size_t)size + piece + 1);
	if (fread(want, 1, (size_t)size, f) != (size_t)size) return 1;
	fclose(f);
	snprintf(hostport, sizeof(hostport), "%s:%s", host, port);
	memset(&h, 0, sizeof(h));
	for (;;) {
		uint32_t rn, before = haven;
		if (fd < 0) {
			fd = dial(host, port);
			if (fd < 0) { printf("cannot connect to %s\n", hostport); return 1; }
			connects++;
		}
		rn = hc_request(&h, req, sizeof(req), hostport, path, off, piece);
		if (write(fd, req, rn) != (ssize_t)rn) { close(fd); fd = -1; continue; }
		requests++;
		while (h.state == HC_HEAD || h.state == HC_BODY) {
			uint8_t buf[1500];
			ssize_t r = read(fd, buf, sizeof(buf));
			if (r <= 0) { hc_closed(&h); break; }
			hc_feed(&h, buf, (uint32_t)r, body_file, NULL);
		}
		if (h.state != HC_DONE) { printf("failed at %u: %s (status %u)\n", off, hc_error(&h), h.status); return 1; }
		if (h.to_close) { close(fd); fd = -1; }
		off += haven - before;
		if (haven - before < piece) break;      /* short: the end */
	}
	if (fd >= 0) close(fd);
	CHECK(haven == (uint32_t)size, "%u bytes fetched of %ld", haven, size);
	CHECK(haven == (uint32_t)size && !memcmp(have, want, (size_t)size), "the bytes are the file's");
	CHECK(h.total == 0 || h.total == (uint32_t)size, "the server's size for it: %u", h.total);
	printf("test_httpc: %s%s: %u bytes in %u requests of %u on %u connection%s\n", hostport, path, haven, requests,
		piece, connects, connects == 1 ? "" : "s");
	return 0;
}

int main(int argc, char **argv) {
	if (argc >= 6 && !strcmp(argv[1], "--server")) {
		uint32_t piece = argc > 6 ? (uint32_t)atoi(argv[6]) : 16384;
		if (against_server(argv[2], argv[3], argv[4], argv[5], piece)) return 1;
		printf("test_httpc: %d checks, %d failed\n", checks, fails);
		return fails ? 1 : 0;
	}
	test_request();
	test_answers();
	test_url();
	printf("test_httpc: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
