#ifndef CINEMA_HTTPC_H
#define CINEMA_HTTPC_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * httpc -- the little of HTTP/1.1 that cinema needs to play a film from
 * a web server (docs/cinema_app.md, "From a web server"): one GET of a
 * byte range at a time, on a connection kept open between them.
 *
 * Nothing of Zeitlos is in here and no socket: the caller sends the
 * request's bytes and feeds what arrives, so the whole of it runs in a
 * test on the host (tests/test_httpc.c), against made-up answers and
 * against a real server.
 *
 * What it knows: 206 with a Content-Length (and a Content-Range, read
 * for the file's size); 200 to a range from 0, of which it takes the
 * first bytes asked for and says the connection must be closed; 416 as
 * "nothing there" (past the end). What it refuses: a 200 to a range
 * that does not start at 0 (a server without ranges), chunked bodies
 * (no file server sends a range that way), any other status.
 * sw/apps/web/http.c is the whole client; this is not it.
 */

#include <stdint.h>
#include <stdbool.h>

#define HC_LINE_MAX 160     /* a header line kept; a longer one is one it does not read */

enum {
	HC_IDLE = 0,
	HC_HEAD,            /* the request is sent; the status line and headers */
	HC_BODY,
	HC_DONE,            /* the answer is whole */
	HC_FAIL
};

enum {
	HC_E_NONE = 0,
	HC_E_STATUS,        /* a status it has no use for (hc_t.status) */
	HC_E_NORANGE,       /* the server answered a range with the whole file */
	HC_E_CHUNKED,
	HC_E_HEADER,        /* not HTTP, or headers without end */
	HC_E_SHORT          /* the connection closed inside a body with a length */
};

typedef struct {
	uint8_t state, err;
	uint16_t status;
	uint32_t off, want;         /* the range asked for */
	uint32_t got;               /* body bytes given to the caller */
	uint32_t left;              /* of the body, still to come (0xffffffff: until the close) */
	uint32_t take;              /* of those, how many are wanted */
	uint32_t total;             /* the file's size, when a Content-Range said (else 0) */
	uint32_t hbytes;
	bool has_len, to_close;     /* to_close: no second request on this connection */
	bool skip;
	uint16_t ll;
	char line[HC_LINE_MAX];
} hc_t;

typedef void (*hc_body_fn)(void *user, const uint8_t *p, uint32_t n);

/* The request for bytes [off, off + len) of `path` into out; its length,
 * or 0 if it does not fit. `host` is the Host header's value (with the
 * port when it is not 80). Starts the answer's parse. */
uint32_t hc_request(hc_t *h, char *out, uint32_t outmax, const char *host,
	const char *path, uint32_t off, uint32_t len);

/* What arrived. Body bytes go to `body` as they are found. Bytes after
 * the answer's end are not looked at (there is one request at a time). */
void hc_feed(hc_t *h, const uint8_t *p, uint32_t n, hc_body_fn body, void *user);

/* The connection closed: an answer without a length ends here; one with
 * a length that is not all there has failed. */
void hc_closed(hc_t *h);

const char *hc_error(const hc_t *h);

/* "http://host[:port]/path": host and path copied out, port 80 unless
 * given. False for anything else (https is not here). */
bool hc_url(const char *url, char *host, uint32_t hostmax, uint16_t *port,
	char *path, uint32_t pathmax);

#endif
