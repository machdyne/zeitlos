#ifndef ZWEB_H
#define ZWEB_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The fetch service: how any process asks `web` (sw/apps/web) to
 * retrieve a URL. See docs/web_app.md and docs/http.md.
 *
 * -- why this lives in `web` and not in `net` --
 *
 * Fetching a URL is at least as useful to the rest of the system as
 * rendering one. The obvious place to put it is `net`, and that is
 * the wrong place: `net` is a core app in the ZAR archive
 * (sw/os/zar.h) that has to stay small enough for a 1MB board, and
 * HTTP plus TLS plus X.509 is roughly 90KB that nothing else will
 * ever call.
 *
 * So all of it lives in `web`, which registers as a service. `net`
 * gains only a raw TCP socket provider (sw/apps/net/sock.h, about
 * 100 lines, behind NET_SOCK). An app that wants an HTTPS request
 * runs `net` and `web` and asks; it contains no TLS, no certificate
 * parsing and no HTTP of its own.
 *
 * The cost, stated plainly: a client that wants only to fetch a URL
 * loads all of `web`, renderer included. If a second serious consumer
 * ever appears, the fix is to split http+tls into their own service
 * app -- which is why everything below is addressed BY REGISTERED
 * NAME rather than by a fixed pid. That split then costs no client
 * any change at all.
 *
 * -- finding the service --
 *
 *     uint32_t web_pid;
 *     if (!z_pid_lookup(Z_WEB_SERVICE_NAME, &web_pid)) {
 *         // web is not running
 *     }
 *
 * There is deliberately no Z_PID_WEB constant. Z_PID_NET exists
 * because it predates the registry (sw/os/pidreg.h) and znet.h says
 * as much; there is no reason to add a second one now that
 * z_pid_lookup() works.
 *
 * -- the shape of a fetch --
 *
 * Two steps, then a stream:
 *
 *   1. Z_WEB_FETCH  ->  Z_WEB_FETCH_REPLY
 *      The request is made, redirects are followed, and the response
 *      HEADERS come back: status, content type, length, final URL.
 *      The body is not sent yet.
 *
 *   2. zstream_open(web_pid, Z_MAP{"handle": <from the reply>})
 *      The body, as a normal zstream (sw/common/zstream.h).
 *
 * Two steps rather than one because the headers are what a caller
 * needs in order to decide whether it wants the body at all. A 404
 * has a body; so does a 20MB video. A client that streamed first and
 * asked later would have to receive both to find out.
 *
 * This costs one extra round trip and no waiting: `web` spools every
 * response body to a file on the card as it arrives, whether or not
 * anyone is reading yet, and serves the stream from that spool. So
 * the connection is never stalled on a slow consumer -- which matters
 * when tcp.c's receive window is 2048 bytes -- and the renderer and
 * an external client end up using exactly the same path.
 *
 * -- what a client does NOT have to think about --
 *
 * DNS, TCP, TLS, redirects, chunked transfer-encoding, and the fact
 * that there is only one TCB in the system. All of that is on the
 * other side of Z_WEB_FETCH.
 *
 * What it DOES have to think about: `web` handles one fetch at a
 * time, because `net` has one TCP connection. A second request while
 * one is in flight is queued, not refused -- see Z_WEB_FETCH below.
 */

#include <stdint.h>

// The name to pass to z_pid_lookup(). `web` registers the basename
// "web" (z_pid_register(), sw/os/pidreg.h), so the first instance is
// "web0" -- same convention as "net0", "wm0", "repl0".
#define Z_WEB_SERVICE_NAME  "web0"

// -- message subjects --
//
// 310-312, continuing the ONE shared sequence that znet.h (302-305,
// 308-309) and zntp.h (306-307) also draw from. Nothing enforces
// this; the numbers are global by convention only, and reusing one
// does not fail to compile and does not look wrong at runtime -- a
// dispatch chain of `else if` just delivers the message to the first
// handler that matches. See net.c's SUBJECT COLLISION CHECK, which
// exists because exactly that happened once.
//
// The next subject added anywhere starts at 313.

// requester -> web:
//   Z_MAP {
//     "url":     Z_STR     required, absolute (http:// or https://)
//     "method":  Z_STR     optional, default "GET". Only "GET" and
//                          "HEAD" are accepted in Phase 1.
//     "accept":  Z_STR     optional, the Accept header to send
//     "nocache": Z_UINT32  optional, non-zero to bypass the spool and
//                          re-fetch. A reload button, essentially.
//   }
//
// Queued if a fetch is already in flight -- `web` has one TCP
// connection to work with and serialises requests rather than
// refusing them, since a client that got "busy" could only sit in a
// retry loop that does the same thing worse. A client that does not
// want to wait can send Z_WEB_CANCEL.
#define Z_WEB_FETCH          310

// web -> requester, reply to Z_WEB_FETCH (same tag):
//   Z_MAP {
//     "ok":           Z_UINT32   0 or 1
//     "status":       Z_UINT32   HTTP status, when ok
//     "content_type": Z_STR      lowercased, parameters stripped,
//                                e.g. "text/html"
//     "charset":      Z_STR      from Content-Type, "" if absent
//     "length":       Z_UINT32   body length if known, else 0
//     "url":          Z_STR      the FINAL url after redirects
//     "handle":       Z_UINT32   pass this to zstream_open()
//     "error":        Z_STR      only when ok == 0
//   }
//
// `ok` is about the FETCH, not about the HTTP status: a 404 that came
// back cleanly is ok == 1 with status == 404, and it has a body worth
// showing. ok == 0 means there is no response at all -- DNS failed,
// the connection was refused, TLS could not be established, the
// redirect limit was hit.
//
// "url" is the final URL and clients should use it as the base for
// resolving anything relative. Getting this wrong is how a redirect
// to a different directory makes every link on the resulting page
// point somewhere that does not exist.
//
// The handle is valid until the body stream ends, is cancelled, or
// the client process exits. Opening a stream with a stale handle is
// rejected rather than silently serving the wrong body.
#define Z_WEB_FETCH_REPLY    311

// requester -> web: Z_UINT32, a handle from a FETCH_REPLY.
//
// Abandons the fetch: stops the transfer if it is still running,
// drops the queued request if it has not started, and releases the
// handle. No reply. Sending this for a handle that has already
// finished is harmless and is the normal way to say "I am done with
// the body and you can drop the spool".
#define Z_WEB_CANCEL         312

// -- opening the body stream --
//
// A Z_STREAM_OPEN arriving at `web` always means "send me the body
// for this handle": Z_MAP{"handle": Z_UINT32}. There is currently
// only one thing an open to `web` can mean, the same reasoning
// znet.h gives for TFTP GET at `net`; if `web` gains other producer
// roles later, an "op" key can disambiguate then.
//
// Z_STREAM_EOF marks the end of the body. Z_STREAM_ERROR carries a
// failure that happened mid-body -- a connection that died after the
// headers were already reported, most likely.

#endif
