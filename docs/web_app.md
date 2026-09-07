# web -- an experimental web browser

`sw/apps/web` is a text-mode web browser for Zeitlos. It renders
documents, follows links, and acts as a **fetch service** so that
other apps can make HTTP and HTTPS requests without containing any
network code of their own.

It is roughly a Lynx-class browser: no JavaScript, no CSS, no layout
beyond a single column. That is a smaller thing than a modern browser
by several orders of magnitude, and it is still enough to read
Wikipedia, documentation, mailing list archives, RFCs, plain-text
news, and the parts of the web that are still documents.

**Images are drawn**, as placeholder boxes that load in place when
clicked -- see [html_layout.md](html_layout.md).

**Status: it runs.** On a Lakritz board it fetches, verifies and
renders real pages from the public internet over TLS 1.3 with a
verified certificate chain. See "Where this stands" at the end.

| | |
|---|---|
| HTTP/1.1 | keep-alive, chunked, redirects, `Content-Encoding: gzip` |
| TLS 1.3 | ChaCha20-Poly1305, X.509 chain verification against a root store on the card |
| Signatures | RSA PKCS#1 and PSS, ECDSA P-256 and P-384, optionally hardware-accelerated |
| Documents | streaming HTML parser, sparse checkpoint index, unlimited page size |
| Images | BMP, PNM, GIF and JPEG, decoded and dithered to 1bpp |

---

## Why no JavaScript

This is the first question anyone asks, so it is settled here rather
than left implied.

A JavaScript engine is not a feature that could be added later if
someone found the time. The smallest credible interpreters are
comparable in size to this entire operating system, and they are the
easy part. A page that runs JavaScript expects a DOM to mutate, a CSS
box model to lay out against, a layout engine that can be invalidated
and re-run, `fetch()` with an origin policy behind it, timers,
promises, and a garbage collector with pauses short enough not to be
noticed. Each of those is a subsystem. Together they are not a
browser feature, they are a different project.

The board this is aimed at has 32MB of RAM and a 1MB kernel pool by
default. A page that needs JavaScript to show its text is a page this
machine cannot show, and pretending otherwise by shipping a partial
engine would produce something that fails in complicated ways instead
of simple ones.

So the decision is permanent, and the design leans into it. Since
there is no script, there is no DOM: the HTML parser (`html.c`)
maintains an element stack and streams out blocks, never building a
tree. That single consequence is what makes it possible to read a
350KB page on a machine with a 1MB pool.

One deliberate consequence in the other direction: `<noscript>`
content **is** rendered. It is the fallback authors wrote for
browsers exactly like this one, and hiding it would be perverse.

For the parts of the web that genuinely require script, the intended
answer is Phase 6 -- an optional proxy running on another machine
that renders a page and sends back something this browser can
display. That keeps the complexity off the board rather than
pretending it does not exist.

---

## Architecture

### `web` is a service, not just a window

Fetching a URL is at least as useful to the rest of the system as
rendering one. Rather than putting HTTP and TLS into `net` -- which
is a core app that has to stay small enough for a 1MB board -- all of
it lives in `web`, which registers itself as a fetch service.

```
  other app                  web                    net
      |                       |                      |
      |-- Z_STREAM_OPEN ----->|                      |
      |   {url: "https://..."}|                      |
      |                       |-- Z_PORT_CONNECT --->|   raw TCP
      |                       |   {ip, port}         |   (sock.c)
      |                       |<-- Z_PORT_DATA ----->|
      |                       |                      |
      |                       | TLS, HTTP, gzip,     |
      |                       | spool to /ram or card|
      |<-- Z_STREAM_CHUNK ----|                      |
```

So an app that wants an HTTPS request runs `net` and `web` and asks.
It contains no TLS, no certificate parsing, and no HTTP.

The cost is real and worth naming: an app that wants only to fetch a
URL loads all of `web`, including the renderer. `web` is therefore a
32MB-board app; it is unlikely to fit comfortably on Obst. If a
second serious consumer of the fetch service ever appears, the fix is
to split `http` and `tls` out into their own service app and leave
`web` as a renderer that calls it. **The service API is deliberately
addressed by registered name rather than by a hardcoded pid, so that
split costs zero changes in any client.**

### What `net` gains, and what it does not

`net` needs exactly one addition: a **raw TCP socket provider**, so
that another process can open an arbitrary TCP connection through the
single TCB in `tcp.c`. It is a generalisation of the existing
`handle_telnet_port_connect()` path -- the port state machine, the
transmit queue and the ack-based flow control are already there and
are reused -- and it is behind `NET_SOCK ?= 1` so a small board can
compile it out.

It cannot simply reuse the telnet provider. `telnet.c` escapes a
literal `0xFF` byte as `IAC IAC`, which would corrupt roughly one
byte in 256 of a TLS record.

One TCB means one connection at a time, system-wide. Browsing and an
SSH session are mutually exclusive, exactly as telnet and SSH already
are.

### Everything fetched is spooled

`web` writes every response body to a spool file as it arrives, and
serves the stream to its client from that file.

The spool goes on the **RAM disk** when there is one and the card
otherwise -- `/ram/webspool`, falling back to `/web/spool`. That is
worth thirty times the read speed when re-reading a page to index it;
see [ramdisk.md](ramdisk.md). `web` prints which it chose.

Three things fall out of spooling at all:

- The renderer and an external client use the same path, so the
  public API is the one the browser itself exercises.
- Back and forward are free, and re-rendering at a new window width
  costs no network.
- The connection is never stalled waiting for a slow consumer, which
  matters when the receive window is only as large as the NIC's
  buffer -- see [networking.md](networking.md).

### Reading a page too large to hold

The document pipeline is modelled directly on `sw/apps/read`, which
already solved this problem for Markdown: keep a **sparse index** of
checkpoints, each a byte offset plus the parser state that went with
it, and reproduce any screen by seeking to the nearest checkpoint and
replaying forward. `html.c` is built to support exactly that. The
mechanism, and the one contract in it that is easy to get wrong, are
described in [html_layout.md](html_layout.md).

---

## TLS, and how it is gated

TLS 1.3 needs `TLS_CHACHA20_POLY1305_SHA256`, and every primitive for
it is already in this tree: X25519, ChaCha20, Poly1305 and BLAKE2b in
`sw/ext/monocypher`, SHA-256 in `sw/common/zsha256.c`, and X.509
parsing and signature verification in `web` itself.

During development there was a window where the record layer worked
and certificate verification did not, gated by a `WEB_TLS_INSECURE`
build flag. **That flag is gone**, along with every code path it
guarded: there is no build of this app that speaks TLS without
checking certificates. `make WEB_TLS=0` builds without TLS at all,
which is the option for a board where the crypto does not fit -- not
a way to switch checking off.

`docs/ssh.md` sets this tree's standard and it is followed here:
refuse, do not warn. A connection that looks encrypted and
authenticates nothing is worse than plain HTTP, because it looks
safe.

## `make check-sources`

Compares the `.c` files in the app directory against `OBJS` and fails
if any is missing.

It exists because `toolbar.c` was written, tested and left out of the
link, and **nothing noticed until the target linker did**. A file that
is compiled by nothing and referenced by nothing produces no error
anywhere: the host tests build their own file lists, and a dry run of
the Makefile happily compiles whatever it is told to.

The same check immediately found three dead files -- `p256.c`,
`p256.h` and a stray duplicate of `tests/test_page.c` -- that had been
unreferenced for many drops.

Host-only sources (`render.c`, `*_test.c`) are skipped by name.

## The toolbar

An editable URL field with Back and Forward beside it.

```
+------------------------------------------+  +--+ +--+
| https://en.wikipedia.org/wiki/Main_Page   |  | <| |> |
+------------------------------------------+  +--+ +--+
```

The URL used to be a label, and opening a page meant `g` and a modal
dialog over the top of it — a step that existed only because there
was nowhere to type. Now `g` (or Ctrl+L, or a click) focuses the
field; Return navigates, Escape puts back what was there.

**While the field has focus, every key goes to it.** Otherwise `g` in
the middle of a hostname would open a dialog and space would scroll
the page out from under what is being typed.

`b` and `f` still work, so the buttons are a convenience rather than
the only way — which is why a window too narrow for everything drops
the **buttons** and keeps the field.

### The field is a shared widget

`sw/common/zedit.c`. It was private to `zdialog.c`, whose own comment
set the condition for moving it:

> *"There is exactly one caller ... If a second caller appears, that
> is the moment to move it, not before."*

This is that caller. Moving rather than copying means the next
improvement lands in both — and one arrived immediately: the dialog's
version could not scroll, so a filename wider than the box ran off
the end of its own frame. `tests/test_edit.c` covers the editing and
the scrolling.

### The button glyphs are testable too, for a reason

The first version of the arrows drew **nothing**. The buttons appeared
as empty frames, and the code -- a loop computing columns and
half-heights inline -- read as though it should work.

So the shape is now `toolbar_arrow_px()`, a pure function, and
`tests/test_toolbar.c` checks that it produces a visible number of
pixels, stays inside the frame, and that the two arrows are **mirror
images**. That last one failed on the first attempt: each triangle was
correct on its own and they were not mirrors, because the span they
occupied was not symmetric about the button's centre line. The fix is
to define one arrow and mirror the coordinate, which makes it true by
construction rather than by two pieces of arithmetic agreeing.

A **disabled** button still shows its arrow, stippled every other
pixel. It used to show an empty frame, on the reasoning that greying
is unavailable on one bit -- but an empty frame does not say "you
cannot go back", it says "this button is broken", which is exactly
how it was reported. On a fresh page both buttons are disabled, so
that was the state a user saw first.

### The geometry is testable

`toolbar.c` is arithmetic with no window, no framebuffer and no
fonts, so `tests/test_toolbar.c` can sweep it across window widths
from 20 to 800 and check that nothing overlaps, nothing leaves the
window, and `toolbar_hit()` agrees with the drawn rectangles at every
boundary.

That matters because the failure is quiet: **a button whose drawn
rectangle and whose clickable rectangle differ by a pixel works
everywhere except at its own edge**, and nobody reports that. Both
come from `toolbar_geom()`, so they agree by construction rather than
by being kept in step.

### History

Eight entries, not thirty-two. Each is a `URL_MAX` buffer, so the old
depth was 32KB of `.bss` for something nobody reaches with two
buttons and no menu. The oldest is dropped rather than refusing to
record a new one.

---

## Phases

| Phase | Contents | State |
|---|---|---|
| 1a | URL, UTF-8 folding, HTML parser, layout engine, host tests, off-device renderer | **done** |
| 1b | Raw socket provider in `net`, fetch-service protocol, HTTP/1.1 client | **done** |
| 1c | The app: window, URL bar, checkpoint index, scrolling, link navigation, the fetch service | **done** |
| 2 | TLS 1.3 record layer and handshake | **done** |
| 3a | DER reader and X.509 parsing, hostname matching | **done** -- see [x509.md](x509.md) |
| 3b | RSA, ECDSA P-256 and P-384, SHA-384 | **done** -- see [x509.md](x509.md) |
| 3c | Chain validation, root store on the card | **done**. `WEB_TLS_INSECURE` is gone |
| 3d | Hardware Montgomery multiplier | **done** -- `rtl/montmul.v`, see [crypto_hw_options.md](crypto_hw_options.md) |
| 4a | HTTP keep-alive, RAM disk, indexed trust store, editable URL bar with history | **done** |
| 4b | gzip (`Content-Encoding`) | **done** -- `sw/common/zinflate.c`, see [http.md](http.md) |
| 4c | Image placeholders, loaded in place on click | **done** -- see [html_layout.md](html_layout.md) |
| 5a | PNG decoding | `zinflate.c` exists; the chunk and filter layers do not |
| 5b | TCP out-of-order reassembly | written, tested, and OFF -- see [networking.md](networking.md) |
| 5c | TLS session resumption | see [tls_resumption.md](tls_resumption.md) |
| 5d | Progressive rendering, reader mode, anchors, GET forms, bookmarks, find-in-page, a real cache | |
| 6 | Optional off-board proxy for the scripted web | |

### Where the time goes

Measured on Lakritz loading `en.wikipedia.org/wiki/Main_Page`, 258KB
of HTML:

| | |
|---|---|
| HTML parse and index | ~12.5s |
| body transfer | ~10s (62KB on the wire, gzipped) |
| TLS handshake | ~14s, of which ~10s is certificate verification |
| storage, DNS, connect | under 1s |
| **total** | **~36s** |

It started at **100s**. Three changes did most of that: a hardware
Montgomery multiplier (verification 36s -> 10s), gzip (body 23s ->
10s), and matching the TCP window to the NIC's buffer, which stopped
the transfer failing outright.

What is left is independent of each other: the **HTML parser** (pure
CPU, no data cache), **TCP reassembly** (which would let the window
exceed the NIC buffer), and **RSA** verification on sites that use it.

### Known risks, ranked

Written before any of this ran. Kept, with what actually happened.

1. **`net`'s TCP was written for a LAN.** *Correct, and the biggest
   one.* But the mechanism was not what this predicted: throughput is
   bounded by the NIC's receive buffer, not by round-trip time, because
   there is no out-of-order reassembly and a frame the hardware cannot
   hold is discarded along with everything behind it. See
   [networking.md](networking.md).
2. **One TCB.** *Correct and unchanged.* No browsing while an SSH
   session is open, and no parallel fetches. `web` now keeps that one
   connection alive across same-host requests rather than reopening
   it.
3. **X.509 parsing is attacker-controlled DER.** *Still true, still
   the right worry.* `der.c` has a mutation sweep under ASan;
   `sw/common/zinflate.c` joined it as a second attacker-controlled
   parser and got the same treatment.
4. **Card I/O may dominate page load.** *Wrong.* Storage is well under
   a second. The HTML parser is 12s and the body transfer is 10s;
   spooling never showed up.

The one nobody wrote down: **certificate verification**, which was 36
of the first 85 seconds and needed a hardware multiplier to fix.

---

## Where this stands

**It runs.** On a Lakritz board it fetches, verifies and renders real
pages from the public internet -- en.wikipedia.org and google.com
among them -- over TLS 1.3 with a verified certificate chain.

This section used to say "Phase 1 is complete as source. It has never
been run", because there was no bare-metal RISC-V toolchain where it
was written. Everything below that line is still true and still how
the code is developed; what changed is that the result is now checked
against hardware as well.

- Every module with logic in it is compiled and tested on the build
  machine: **1,006 assertions across fourteen suites**, plus an
  off-device renderer that draws a real page to a PBM.
- `net.c` syntax-checks clean with the host compiler in all four
  combinations of `NET_SOCK` and `SSH_ENABLE`.
- `make check-sources` verifies every `.c` in the app is actually
  linked -- added after `toolbar.c` was written, tested, and left out
  of `OBJS`, which nothing noticed until the target linker did.

| File | What it is | Tested by |
|---|---|---|
| `url.c` | RFC 3986 parsing and resolution | `test_url.c`, 94 checks |
| `toolbar.c` | URL bar and button geometry | `test_toolbar.c`, 23 checks over a width sweep |
| `http.c` | request building, response parsing, gzip | `test_http.c`, 214 checks |
| `uni.c` | UTF-8 decoding and folding to ASCII | `test_html.c` |
| `html.c` | streaming HTML parser | `test_html.c`, 97 + invariants |
| `layout.c` | wrapping, styles, images, hit testing | `test_layout.c`, 249 checks |
| `page.c` | checkpoint index, scroll arithmetic | `test_page.c`, 193 checks |
| `tls_crypto.c` | HMAC, HKDF, key schedule, AEAD | `test_tls_crypto.c`, 38 checks |
| `tls.c` | record layer, TLS 1.3 handshake | `test_tls.c`, live handshakes against OpenSSL |
| `der.c` | DER reader | `test_x509.c`, plus a mutation sweep under ASan |
| `x509.c` | certificate parsing, hostname matching | `test_x509.c`, 54 checks vs real certificates |
| `rsa.c` | PKCS#1 v1.5 and PSS | `test_rsa.c`, 20 checks |
| `ecdsa.c` | P-256 and P-384, software and hardware paths | `test_ecdsa.c` 10, plus 10 through a model of `rtl/montmul.v` |
| `sha384.c` | SHA-384 | `test_sha384.c`, 3 published vectors |
| `verify.c` | chain building and validation | `test_verify.c`, 44 checks |
| `web.c` | window, transport, fetch service | nothing. It needs the machine. |

Also outside this directory but load-bearing here:
`sw/common/zinflate.c` (30 checks against real gzip data),
`sw/common/zedit.c` (17), and `rtl/tests/tb_montmul.v` (51 products
in simulation).

**`web.c` remains untested by anything but the board**, and that is
where most of the bugs found on hardware have been -- spool handles,
fetch state, and which buffer a callback writes to.

```
cd sw/apps/web
make test                                  offline, always runs
make test-tls                              live handshake against OpenSSL
make test PAGES="$(ls ~/pages/*.html)"     invariants on real pages
make render DOC=~/pages/wikipedia.html && display /tmp/web.pbm
```

### What to expect on first run

`run net`, then `run web`, then press `g`.

Keys: arrows and PageUp/PageDown scroll, Home/End jump, Tab selects a
link, Enter follows it, `g` or Ctrl+L focuses the URL bar, `b` or
Backspace goes back, `f` forward, `r` reloads. In the URL bar, Enter
navigates and Escape restores what was there.

The mouse works on links, images, the URL bar, the Back and Forward
buttons, and the scrollbar.

`/web` must exist on the card, and `/web/roots.der` must be there for
HTTPS -- `make roots` builds it, see `sw/apps/web/roots/README.md`.
The clock must be set, which `net` does over NTP; a certificate
cannot be checked for expiry without one and `web` refuses rather
than skipping the check.

Clicking an image box loads the picture in place. Clicking a link
follows it.

Things that are known-missing rather than broken:

- **No progressive rendering.** A page downloads completely, then
  appears. `page.c` is written and tested for the streaming case, so
  this is a change in `web.c` rather than a redesign.
- **No PNG.** BMP, PNM, GIF and JPEG decode; PNG is recognised and
  refused. `sw/common/zinflate.c` is what its compressed stream
  needs and already exists for gzip, so what remains is the chunk
  and unfilter layers.
- **One image at a time.** A decoded bitmap is the size of its box
  and there is no dynamic memory, so clicking a second image
  replaces the first.
- **Fragment links jump to the top**, because there is no anchor
  index yet.
- **One TCB**, so browsing and an SSH session are mutually exclusive,
  and `net` will say so. `web` now keeps that one connection alive
  across same-host requests rather than reopening it, and gives it
  back after ten idle seconds -- see [http.md](http.md).
- **No TLS session resumption**, so a redirect to a DIFFERENT host
  pays for a full handshake. `google.com` to `www.google.com` is two.
  See [tls_resumption.md](tls_resumption.md).
- **RSA chains are slow.** The hardware multiplier is 384-bit and
  covers ECDSA; an RSA-4096 root takes about 5.5 seconds and some
  servers hang up first.

### What is most likely to be wrong

In rough order of my own confidence:

1. **`tcp.c` over the open internet.** *Measured since: roughly
   25KB/s against a real host, about 1% of the 10Mbit link.* The
   window is now matched to the NIC's receive buffer, which stopped
   transfers failing; out-of-order reassembly is what would raise the
   ceiling. See [networking.md](networking.md).
2. **The blocking DNS resolve in `start_fetch()`.** It draws the
   status line first and then stops reading messages, which is the
   shape that made wm report a missed redraw ack for `term`.
   Bounded, but not free.
3. **Card I/O.** Every byte goes to the spool and comes back out of
   it over a bit-banged SD interface. `page.c` seeks only when the
   handle is not already in the right place, which is the same
   optimisation `read` needed, but it has not been measured here.
4. **The service provider's stream path**, which is the least
   exercised code in the drop -- the browser itself does not use it.

Three of the bugs found while writing this were found only by the
invariants in `test_html.c` and `test_page.c`, and none by a unit
case: the parse from the start was correct every time. A fourth --
a checkpoint table that filled with duplicates until the replay
stride reached 128 -- was not caught by any assertion at all, only by
printing the numbers. That one is now asserted.
