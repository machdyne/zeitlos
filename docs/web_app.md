# web -- an experimental web browser

`sw/apps/web` is a text-mode web browser for Zeitlos. It renders
documents, follows links, and acts as a **fetch service** so that
other apps can make HTTP and HTTPS requests without containing any
network code of their own.

It is roughly a Lynx-class browser: no JavaScript, no CSS, no images
in the page flow. That is a smaller thing than a modern browser by
several orders of magnitude, and it is still enough to read
Wikipedia, documentation, mailing list archives, RFCs, plain-text
news, and the parts of the web that are still documents.

**Status: partially implemented.** See "Where this stands" at the end
for exactly what exists today.

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
      |                       | TLS, HTTP, spool     |
      |                       | to the card          |
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

### Everything fetched is spooled to the card

`web` writes every response body to a cache file as it arrives, and
serves the stream to its client from that file. Three things fall out
of it:

- The renderer and an external client use the same path, so the
  public API is the one the browser itself exercises.
- Back and forward are free, and re-rendering at a new window width
  costs no network.
- The connection is never stalled waiting for a slow consumer, which
  matters when the window is 2048 bytes.

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
| 4a | HTTP keep-alive, RAM disk, indexed trust store | **done** |
| 4b | Progressive rendering, reader mode, anchors, better tables, GET forms, bookmarks, find-in-page, a real cache | |
| 5 | gzip inflate, TCP throughput work, TLS session resumption | |
| 6 | Optional off-board proxy for the scripted web | |

### Where the time goes

Measured on Lakritz loading `en.wikipedia.org/wiki/Main_Page`, 258KB:

| | |
|---|---|
| body transfer | ~23s -- see [networking.md](networking.md) |
| HTML parse and index | ~12s |
| TLS handshake | ~15s, of which ~10s is certificate verification |
| storage, DNS, connect | under 1s |
| **total** | **~52s** |

It started at 100s. The three that remain are independent of each
other: TCP reassembly, the HTML parser, and RSA verification.

### Known risks, ranked

1. **`net`'s TCP was written for a LAN.** Stop-and-wait, no
   out-of-order reassembly, a 2048-byte window. Over the open
   internet with 100ms of round-trip time that is roughly 20KB/s at
   best, and a single reordered segment costs a retransmit timeout.
   This should be measured early in Phase 1b, before anything is
   built on top of it.
2. **One TCB.** No browsing while an SSH session is open, and no
   parallel fetches.
3. **X.509 parsing is attacker-controlled DER.** It needs host
   fuzzing before it is trusted, not after.
4. **Card I/O may dominate page load.** Everything is spooled through
   a bit-banged SD interface.

---

## Where this stands

Phase 1 is complete as source. **It has never been run.** There is no
bare-metal RISC-V toolchain in the environment it was written in, so
nothing here has been compiled for the target, linked, or executed.
What has been done instead:

- Every module with logic in it is compiled and tested on the build
  machine: 780 assertions across five suites, plus an off-device
  renderer that draws a real page to a PBM.
- `net.c` syntax-checks clean with the host compiler in all four
  combinations of `NET_SOCK` and `SSH_ENABLE`.
- `web.c` **links** against the real `sw/common` objects with no
  undefined symbols, which verifies every API call it makes exists
  with the right name and signature. It does not verify that it
  works.

| File | What it is | Tested by |
|---|---|---|
| `url.c` | RFC 3986 parsing and resolution | `test_url.c`, 94 checks |
| `http.c` | request building, response parsing | `test_http.c`, 173 checks |
| `uni.c` | UTF-8 decoding and folding to ASCII | `test_html.c` |
| `html.c` | streaming HTML parser | `test_html.c`, 90 + invariants |
| `layout.c` | wrapping, styles, hit testing | `test_layout.c`, 230 checks |
| `page.c` | checkpoint index, scroll arithmetic | `test_page.c`, 193 checks |
| `tls_crypto.c` | HMAC, HKDF, key schedule, AEAD | `test_tls_crypto.c`, 38 checks vs generated vectors |
| `tls.c` | record layer, TLS 1.3 handshake | `test_tls.c`, a live handshake against OpenSSL |
| `der.c` | DER reader | `test_x509.c`, plus a mutation sweep under ASan |
| `x509.c` | certificate parsing, hostname matching | `test_x509.c`, 54 checks vs real certificates |
| `web.c` | window, transport, fetch service | nothing. It needs the machine. |

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
link, Enter follows it, `g` or Ctrl+L opens a URL, `b` or Backspace
goes back, `f` forward, `r` reloads. The mouse works on links and the
scrollbar.

`/web` must exist on the card, or the first fetch reports that it
cannot write the spool.

Things that are known-missing rather than broken:

- **No progressive rendering.** A page downloads completely, then
  appears. `page.c` is written and tested for the streaming case, so
  this is a change in `web.c` rather than a redesign.
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

1. **`tcp.c` over the open internet.** Stop-and-wait, a 2048-byte
   window, no fast retransmit. Roughly 20KB/s at 100ms RTT if
   everything goes well. This is the thing to measure first, before
   anything is built on top of it, because if it is the real ceiling
   it changes what Phase 5 has to be.
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
