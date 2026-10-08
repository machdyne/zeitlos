# Gopher and Gemini in `web`

`sw/apps/web` opens `gopher://` and `gemini://` URLs as well as
`http://` and `https://`. Type one into the URL bar, or follow a link to
one from any page:

```
gopher://gopher.floodgap.com/
gemini://geminiprotocol.net/
```

Both protocols are the small internet: documents that are mostly text,
served by one request and one response over a connection that closes.
That is what a 640x480 monochrome screen and a 48MHz CPU are good at,
and it is where a lot of writing that suits this machine lives.

| | Gopher | Gemini |
|---|---|---|
| port | 70 | 1965 |
| transport | plain TCP | TLS 1.3 |
| documents | menus, text | text/gemini, any text/* |
| certificates | none | trust on first use (below) |
| input | search items (type 7) | status 1x prompts |

## How it works

`sw/apps/web/smallweb.c` builds the request, reads Gemini's status
line, and **converts what arrives into HTML**, a line at a time. That
HTML is spooled exactly as an HTTP body is, and from there on it is the
web page path unchanged: the index that lets a page of any size be read
without holding it, wrapping, links and selecting them, scrolling, and
history. A link in a Gemini page is followed by the same code as a
link in a web page.

The alternative, a second renderer, would have been a second place for
every one of those bugs to live. The conversion needs one line of
memory whatever the document's size.

Like `url.c` and `html.c`, `smallweb.c` has no Zeitlos dependencies.
Everything above the socket is tested on the build machine (below);
`web.c` only decides which protocol a fetch is and feeds bytes through.

### Gemini documents

| gemtext | shown as |
|---|---|
| `# ## ###` | headings |
| text lines | a paragraph; **consecutive lines stay together**, a blank line separates paragraphs |
| `=> URL label` | a link, shown as `=> label` (the URL when there is no label) |
| `* item` | a list |
| `> quote` | a quotation |
| ```` ``` ```` | preformatted text, literally, until the next ```` ``` ```` |

Consecutive text lines are kept as one paragraph with line breaks
because that is how gemtext is written: either one long line per
paragraph with blank lines between, or several short lines that belong
together (a poem, an address). Either way the page looks as its author
wrote it.

A response of `text/gemini` (or an empty type, which the protocol says
means the same) is converted; `text/html` is passed through to the HTML
engine; any other `text/*` is shown preformatted. Anything else (an
image, a binary) is refused with its type named, as HTTP responses the
browser cannot show are.

### Gemini status codes

| status | what happens |
|---|---|
| 1x input | the URL bar gets `URL?` with the caret after it, and the server's prompt goes on the status line: type after the `?`, press Enter |
| 2x success | the page |
| 3x redirect | followed, through the same redirect limit and loop check as HTTP; a target too long to hold is refused rather than truncated |
| 4x, 5x | the error, with its number and the server's text |
| 6x | refused: client certificates are not implemented |

A query typed into the bar is percent-encoded on the way out, so spaces
and non-ASCII letters reach the server as the protocol requires.

### Gopher

A Gopher URL carries the item type as the first character of its path:
`gopher://host/1/phlog` is a menu (`1`) whose selector is `/phlog`. The
root, `gopher://host/`, is the server's main menu.

| type | shown as |
|---|---|
| `1` menu | a menu (below) |
| `0` text | preformatted text |
| `7` search | the URL bar asks for the terms first, as for a Gemini prompt |
| `h` HTML | passed to the HTML engine |
| other | refused, with the type named |

A menu is laid out as text in one preformatted block, because Gopher
menus are designed in columns and ASCII art and reflowing them would
wreck both. Each item gets a marker for what it is:

```
      Welcome to the Zeitlos gopher hole
[dir] Phlog
[txt] About this server
[ ? ] Search Veronica
[www] Project page
[bin] Firmware image
```

Menus, text, searches and web links (`URL:` selectors) are links.
Binaries, images, sounds and telnet sessions are shown for what they
are but are not links, since there is nothing here that could open
them. A menu or text item ends at its `.` line; `..` at the start of a
text line stands for `.`.

## Certificates: trust on first use

This browser's TLS is built on one rule, from [tls.md](tls.md): refuse,
do not warn. There is no way to connect with authentication switched
off. Gemini tests that rule, because **almost every Gemini server
presents a self-signed certificate**: the CA check that protects https
would refuse nearly every capsule there is.

The protocol's own answer is trust on first use (TOFU), and that is
what `web` does, as a second *kind* of authentication rather than a
hole in the first:

- **The server must still prove it holds its key.** CertificateVerify
  is checked against the certificate's key exactly as for https. What
  changes is only the question asked of that key: not "does a trusted
  CA vouch for it" but "is it the key this server had before".
- **The first visit pins the key.** Its fingerprint, the SHA-256 of the
  certificate's public key, goes into `/data/web/pins.txt`. The console says
  `web: gemini: pinned host:1965 ...`.
- **Every later visit must present the same key**, or the connection is
  refused: *this capsule's key has changed since it was first seen*.
  If the change is expected (the server was rebuilt), delete that
  host's line from `/data/web/pins.txt` in `text`, and the next visit pins
  the new key.
- **The pin is the key, not the certificate**, so renewing a
  certificate with the same key is not a change. Dates are not checked,
  which is Gemini practice for self-signed certificates, and so a pin
  needs no clock: unlike https, Gemini works before NTP has set the
  time.
- **No card, no file.** A pin that cannot be written is kept for the
  session (up to eight), so it still protects every later connection
  until the machine restarts.

TOFU does not protect the first visit: whoever answers first is who
gets pinned. That is the known trade-off of the model and the reason
it is used only for Gemini, where there is no alternative, and never
for https.

`tls_set_pin()` (`sw/apps/web/tls.h`) is the mechanism. Exactly one of
`tls_set_verify()` and `tls_set_pin()` must be called before
`tls_start()`, which refuses with neither and with both. In pin mode the
ClientHello also leaves out ALPN, which names `http/1.1` for https:
Gemini defines no ALPN value, and offering an HTTP protocol to a Gemini
server is wrong.

Capsules with RSA (to 4096 bits), ECDSA P-256, ECDSA P-384 and
Ed25519 keys are all reachable ([tls.md](tls.md#signature-schemes)).
P-384 and Ed25519 were added after the first real capsules were tried:
a server can only sign with a scheme matching its key, and before those
two were offered such a capsule ended the handshake with alert 40.

**Gemini servers ask for a client certificate**, since that is how a
capsule identifies a visitor, and this browser has none: it answers
with an empty one, which is how a client says so. It once answered in
the wrong place in the handshake, and every capsule failed with *"the
server did not prove it holds its own key"* -- [tls.md](tls.md#answering-a-certificaterequest-in-order)
has what went wrong. A capsule that *requires* a certificate still
refuses the connection (status 6x is the polite form of the same).

## Testing

```
make -C sw/apps/web test         # includes test_smallweb
make -C sw/apps/web test-tls     # live handshakes, including pin mode
```

`tests/test_smallweb.c` also **fuzzes** the converter: 3,000 random
Gemtext and Gopher documents -- every line type, blank lines, CR and
LF, long lines, stray bytes -- fed whole and in random pieces. The
output must stay within eight times the input, the two must agree, and
under `-fsanitize=address` any read past a line stops the run. It was
added after skyjake.fi's Cosmos filled 20MB of spool from 36KB of
page: on a blank line the reused line buffer still started with the
previous line's `#`, the heading branch took `0 - 1` of an unsigned
length, and four billion bytes of memory went into the page from inside
one call -- which is also why Stop could not stop it. The old converter
fails the fuzz test on its first bad read.

`tests/test_smallweb.c` checks the requests byte for byte (selectors
with spaces, searches, non-default ports, percent-encoding a typed
query), every Gemini status class, and the HTML each kind of document
becomes. Every conversion is run twice, whole and **fed one byte at a
time**, and the two must agree: on the device a body arrives in
whatever pieces TCP and TLS produce.

`make test-tls` runs the real client against a real OpenSSL server
(see [tls.md](tls.md#make-test-tls--a-live-handshake-verified)). Pin
mode adds two runs:

| mode | root store | expected |
|---|---|---|
| `pin` | **none** | accepted: the pin replaces the chain check rather than running beside it; the pin callback runs exactly once, with the host |
| `pinwrong` | none | refused by the pin |

To look at the result rather than assert it, convert a document and
render it with the browser's own layout engine:

```
make -C sw/apps/web render DOC=page.html OUT=/tmp/page.pbm
```

## Size, and where a page goes

Neither protocol says how long a response is -- it ends when the server
closes the connection -- so the client has to set a limit, and **web
stops a Gopher or Gemini response at 4 MB** (`SW_MAX_BYTES`, `web.c`)
and shows what arrived, with a note at the end saying it was cut short.
A capsule that streams, or never closes, would otherwise fill the
spool without end.

The converted page is written to the **spool**, like any web page:
`/tmp/web.spool`, which is on the RAM disk when there is one and on the
card when there is not ([layout.md](layout.md)). The status line's "receiving... N KB" is the
size of that file.

The console says what happened, for a report or for looking into an
odd capsule:

```
web: gemini request gemini://example.org/
web: gemini: 20 text/gemini;lang=en                 <- the response header
web: gemini: 48213 bytes received, 61874 written to the spool
web: gemini response passed 4 MB -- stopped there   <- only at the limit
web: stop (state 3, stopping)                       <- the Stop button or Escape
```

Converted output runs somewhat larger than what was received (markup
around each line); far larger would be a bug in the conversion -- one
was, below -- and "received" growing without end is the server.

A converted Gopher menu is one `<pre>`, which once hung the page
indexer at *"reading... 0 blocks"* and blanked short menus; the cause
was in `html.c`, and [html_layout.md](html_layout.md#where-a-mark-is-legal)
has it.

## Limits

- **No client certificates** (Gemini status 6x), so capsules that use
  them to identify a user cannot be logged into.
- **Gopher binaries, images and sounds are not opened**, and Gopher+
  is not spoken.
- **Gemini images are not shown**: a Gemini link to a `.png` is a
  separate response with an image type, which the browser refuses
  rather than decoding. HTTP images are loaded in place from an HTML
  page's placeholders, and Gemini has no such placeholders.
- **The first Gemini visit is trusted**, as described above.
