# HTTP and the fetch service

How `sw/apps/web` makes a request, and how any other app can ask it
to make one.

See [web_app.md](web_app.md) for the browser as a whole and
[networking.md](networking.md#raw-tcp-sockets) for the socket
provider underneath.

---

## The layers

```
  web.c        decides WHAT to fetch, follows redirects, spools
    |
  http.c       builds the request, parses the response
    |
  tls.c        Phase 2 -- absent today
    |
  zport        Z_PORT_CONNECT {ip, port} / Z_PORT_DATA
    |
  net/sock.c   raw TCP, nothing added or removed
    |
  net/tcp.c    one TCB, stop-and-wait
```

`http.c` includes nothing from Zeitlos and touches no socket. Bytes
go in through `http_feed()`, headers and body come out through two
callbacks. That is what lets `tests/test_http.c` run the whole parser
on the build machine against hand-written responses — including the
malformed ones, which are the ones worth testing and the ones a real
server will not produce on request.

It also means the same parser sits behind plain TCP and behind TLS.
If it knew which, there would be two paths through it and only one
would get exercised.

---

## What `http.c` handles

GET and HEAD. The three ways a response body can end — `Content-Length`,
chunked transfer-encoding, and connection close. Redirects are
**parsed** here (`Location` is reported) and **followed** a layer up,
because how many to follow and whether `https` may become `http` are
policy, and policy does not belong in a parser.

Some specifics that are easy to get wrong and are checked directly:

- **`Transfer-Encoding` wins over `Content-Length`**, per RFC 7230
  §3.3.3, and the length is *erased* rather than merely
  deprioritised. Both being present is how request smuggling is
  built; nothing downstream of this browser can be desynchronised by
  it, but a response whose own length is ambiguous is not one to
  guess about.
- **Two disagreeing `Content-Length` headers are a hard error.** Two
  identical ones are fine.
- **A `Content-Length` that does not fit in 32 bits is a lie**, not a
  large file, and wrapping it produces a small number that looks
  entirely reasonable.
- **204, 304 and 1xx never have a body**, whatever their headers
  claim. They routinely carry a `Content-Length` describing a body
  they are not sending, and waiting for it hangs the connection.
- **A 1xx is followed by another complete response.** Some servers
  send `100 Continue` unsolicited; treating it as the real response
  leaves the actual one unparsed.
- **`HEAD` has to be told to the parser**, because a HEAD response
  carries a real `Content-Length` describing a body that will never
  arrive.
- **A short `Content-Length` body is an error.** Silently accepting
  one renders half a page as though it were the whole page, which is
  the failure a reader cannot detect.
- **The fragment is never sent.** It is a client-side concept.
- **The port appears in `Host:` only when it is not the scheme
  default.** Sending `example.com:80` is legal and a surprising
  number of virtual-host configurations fail to match on it.

### What it does not handle, and why

- **No compression.** No `Accept-Encoding` is sent, so nothing
  arrives gzipped. That costs bandwidth on a link with little to
  spare and saves an inflate implementation plus a decompression
  buffer. Phase 5.
- **No pipelining.** One request is outstanding at a time.
  Connections ARE reused across same-host requests -- see "Persistent
  connections" below.
- **No cookies or authentication.** Nothing here has a user to be
  logged in as.
- **No HTTP/2 or /3.** Both need TLS ALPN and a different framing
  layer; every server that speaks them still speaks 1.1.

---

## The fetch service

`web` registers as `web0` (`z_pid_register()`, `sw/os/pidreg.h`) and
answers `Z_WEB_FETCH`. The protocol is in `sw/common/zweb.h`.

```c
uint32_t web_pid;
if (!z_pid_lookup(Z_WEB_SERVICE_NAME, &web_pid)) { /* not running */ }

z_obj_t req = z_obj_map(1);
z_map_set(&req, "url", z_obj_str("https://example.com/"));
z_msg_new_send(web_pid, Z_WEB_FETCH, tag, req);

// ... Z_WEB_FETCH_REPLY arrives with status, content_type,
//     the final URL after redirects, and a handle ...

z_obj_t open = z_obj_map(1);
z_map_set(&open, "handle", z_obj_uint32(handle));
zstream_open(web_pid, open);        // the body
```

There is deliberately **no `Z_PID_WEB` constant**. `Z_PID_NET` exists
because it predates the registry and `znet.h` says as much; there is
no reason to add a second fixed pid now that `z_pid_lookup()` works.
Addressing by name is also what makes it possible to split `http` and
`tls` out of `web` later without any client changing.

### Why two steps and not one

The headers are what a caller needs in order to decide whether it
wants the body at all. A 404 has a body; so does a 20MB video. A
client that streamed first and asked later would have to receive both
to find out.

The extra round trip costs no waiting, because of the spool.

### The spool

`web` writes every response body to a file on the card **as it
arrives**, whether or not anyone is reading yet, and serves the
stream from that file. Three things fall out of it:

- The connection is never stalled on a slow consumer, which matters
  when `tcp.c`'s receive window is 2048 bytes.
- Back, forward, and re-wrapping the page at a new window width cost
  no network.
- The renderer and an external client use exactly the same path, so
  the public API is the one the browser itself exercises rather than
  a second-class one beside it.

### One fetch at a time

`net` has one TCP connection, so `web` has one fetch. A second
request is **queued, not refused** — a client that got "busy" could
only sit in a retry loop doing the same thing worse. `Z_WEB_CANCEL`
is how a client that does not want to wait gets out.

And because it is the same TCB: a fetch cannot start while an SSH or
telnet session is open, and will be refused by `net` with a message
saying so.

---

## Testing

```
cd sw/apps/web
make test
```

`tests/test_http.c` is 173 checks, and most of them are malformed
input. Every response is also fed at chunk sizes of 1, 3, 17 and 512
bytes as well as whole, and all of them must produce identical
output — the same invariant `tests/test_html.c` uses, for the same
reason. A parser that works on whole responses and breaks on 1-byte
feeds is one that works in the test file and fails on hardware, where
the transport delivers whatever a 536-byte segment happened to
contain.

What is **not** tested here is the thing that will actually bite:
`tcp.c` against a real internet host. See
[networking.md](networking.md#still-unmeasured).

---

## Persistent connections

`http.c` used to send `Connection: close` unconditionally, with the
reason stated in a comment: *"because `net` has one TCB: a kept-alive
connection would hold the only socket in the system open doing
nothing."*

The first half of that had expired. Keeping a connection open uses
the **same** one TCB — it simply does not close it between requests.
And closing is expensive here in a way it is not on a desktop: a
same-host redirect meant a second full TLS handshake, about fifteen
seconds on this hardware, to fetch a page from a server we were
already connected to. `en.wikipedia.org/` to `/wiki/Main_Page` paid
that twice.

The second half was real, and `web.c` answers it with an idle timer
rather than by closing after every request: ten seconds, long enough
for the redirects and sub-fetches that follow a page load, after
which net's only socket goes back.

### When a connection may be reused

`http_response_t.keep_alive` is true only when **both** hold:

- the server did not say `Connection: close`, and
- the response is **self-delimiting** — a `Content-Length`, chunked
  framing, a HEAD response, or a status that never has a body.

The second is the one that matters. A response whose body ends when
the connection ends cannot be followed by anything, and treating it
as reusable would leave the next request reading the tail of this
body as its status line. **That is response smuggling against
oneself**, and it is the failure that a permissive answer here would
cause — so `tests/test_http.c` checks the close-delimited case
explicitly, not just the reusable ones.

### The retry

A server may close an idle connection at any moment, including
between our last read and our next write. A reused connection that
dies without answering is therefore **normal, not an error**, and
`web.c` retries once on a fresh socket.

Once, and only when nothing was received. A connection that died
mid-response is a real failure, and silently retrying a request that
may already have been acted on is not something to do quietly.

### What it does not do

Nothing is pipelined: one request is outstanding at a time. And a
redirect to a **different** host still pays for a new connection —
`google.com` to `www.google.com` is two handshakes however this is
arranged. TLS session resumption is what covers that case; see
docs/tls_resumption.md.

---

## Compressed responses

`sw/common/zinflate.c` -- DEFLATE (RFC 1951) with zlib and gzip
wrappers.

It is here for `Content-Encoding: gzip`, and it is the single change
that most affects how long a page takes: en.wikipedia.org's front page
is 258KB uncompressed and about a fifth of that gzipped, against a
body transfer that is currently the largest cost of a load. The same
decoder serves PNG's IDAT stream, which is why it lives in
`sw/common` rather than in the browser.

### The memory

**32KB, and it is not optional.** DEFLATE back-references reach 32768
bytes, so a decoder must keep that much history.

The window is caller-owned rather than embedded in `z_inflate_t`, for
two reasons: an app that never decompresses does not pay for it, and
`sw/common/zimg.c` can put it in the union it already shares between
decoders -- where GIF's LZW dictionary is 17.6KB of the same bytes, so
the net cost there is far less than 32KB.

### Suspendable at any byte

A network callback delivers what arrived, not what the decoder wants,
so every point that can block on more input or more output room is a
state. `tests/test_inflate.c` runs every case at input and output
chunk sizes of 1, 3, 7, 512 and everything.

That sweep earned its place immediately -- **three bugs, all of the
same shape**, and all invisible when the whole stream is handed over
at once:

- a **repeat code** (16/17/18) was decoded and *then* its extra bits
  read, so running out of input between them lost the repeat and
  resumed by decoding a different symbol;
- a **literal** was decoded and *then* checked for output room, so a
  full output buffer dropped the byte;
- the **trailer** countdown was a local, so a trailer arriving in
  pieces restarted at eight bytes every call and the stream stalled a
  few bytes from the end.

Each consumed something from the bit stream before checking it could
finish the job. That is the hazard in any resumable decoder, and it
is why the chunk sweep is not optional.

### Untrusted input

This parses attacker-controlled data with nothing authenticating it,
which makes it the highest-risk code in the tree after TLS.

- Every back-reference distance is checked against what has actually
  been written. Trusting one reads uninitialised window bytes, which
  is a **memory disclosure**, not a wrong character.
- Over-subscribed and incomplete Huffman codes are rejected rather
  than tolerated.
- `z_inflate_init()` takes an output **limit**. A few hundred bytes
  can legitimately expand to megabytes, and "legitimately" is exactly
  what a decompression bomb looks like.

### What it does not do

The gzip and zlib trailers are **consumed but not verified**. A CRC-32
over every byte costs more than this machine can spare on a body it
is about to render, and every framing error the trailer would catch
has already been caught by the stream itself. Stated plainly because
"has a CRC" and "checks the CRC" are easy to confuse.

gzip header flags (FEXTRA/FNAME/FCOMMENT/FHCRC) are refused rather
than skipped: HTTP servers do not set them, and a skipper is more
code paths on untrusted input for no real case.
