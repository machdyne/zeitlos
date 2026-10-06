# Remote desktop

The 640x480 1bpp framebuffer, in a browser. Point one at the board
and you get the desktop as it is on HDMI, with the keyboard and mouse
going back the other way. There is nothing to install.

Two ways, and only one of them at a time.

**Over the ESP32 link.** On a bitstream built with the
[ESP32 link](esp32link.md) (the ULX3S targets, including `ulx3s_85f`),
`net` scans the framebuffer and the ESP32 serves the page. Nothing
has to be started. That path is the one through "screend", below.

**Served by Zeitlos.** On a bitstream with a wired NIC and no ESP32
link, `run zerdesk` from `term` serves the same page over TCP. The
ULX3S build for that is `ulx3s_85f_langkatze`: an ENC28J60 in J1, the
onboard ESP32 left out of the gateware and held in reset. `zerdesk`
refuses to start when the ESP32 link is present. Both would read and
clear the same DIRTY register, and the ESP32 already serves the
desktop there. It does not start by itself.

The page, the stripe format, PackBits, the trailer and the colouring
are the same on both. What differs is who scans, who keeps a copy for
a browser that has just connected, and how the bytes travel. That is
"Served by Zeitlos", after the ESP32 path.

The scan itself, the hash, PackBits and the trailer live in
`sw/common/zscreen.c`, with no transport. The pointer and the keyboard
live in `sw/common/zinput.c`. `net`'s screen path and `zerdesk` both
call them, so a stripe is the same bytes either way.

```
net (sw/apps/net/screen.c)          the ESP32 (esp32/zeitlos-nic)
  read socctl's DIRTY: which stripes
    were written since last time
  snapshot + hash those stripes
  PackBits the ones that changed
  UDP to the gateway, port 7777 -->  screend.c: keep a shadow stripe
                                     relay the dirty ones over a
                                     WebSocket to every viewer
                                       |
  reg_vmouse / hid_inject     <--  ZNIC_MOUSE / ZNIC_INPUT  <--  the page
```

**Finding the address.** It is whatever the access point's DHCP gave
the ESP32, so nothing in this tree can know it. The console says it at
boot -- `esp_netif_handlers: sta ip: ...`, and `esp32link: LINK up
rssi=... ip=...` right after -- and `net` also writes it to `net.ip` at
the root of the sdcard when the link comes up. On this path,
everything else is `http://<that>/`. On the Ethernet path the address
is the wired one and the port is 8080; see "Served by Zeitlos".

## Stripes

The framebuffer is 30 stripes of 16 rows. A stripe is 1280 bytes
(640/8 x 16), which is one UDP datagram and one link frame, and 30 of
them is the whole screen.

Each pass reads the stripes that were written since the last one (see
"Only what was written" below), hashes them, and sends the ones that
differ from what was last copied. Every five seconds all thirty are
resent so that a stripe lost in flight heals itself -- this is UDP on
purpose, and the consumer, not the producer, keeps the shadow. That
full pass waits if the pointer is down or the screen is already busy:
thirty stripes is the longest the wire is ever held, and holding it
through a drag is what a viewer feels as seconds.

### Only what was written

Until rtl/socctl.v had a DIRTY register, a pass read the whole
framebuffer -- 9600 words, 14.3 ms -- to find out what had changed,
up to twenty times a second. With a viewer connected and nothing
moving, that was ~28% of the machine and nearly all of what `net` did.

DIRTY is one bit per stripe, set in hardware by every write the VRAM
accepts, so it sees the CPU, the line rasterizer and the blitter alike,
and things no software layer would report: a game poking VRAM through
a pointer, the kernel's `cls`. A pass reads it, clears exactly what it
read, and only then copies those stripes into the snapshot. A stripe
whose bit is clear has not been written since its last copy, so the
copy already is the screen, and a frame is still one instant -- the
stripes that are read are read in one tight pass, now a short one.
Clearing before copying is what makes the race safe: a write that
lands during the pass sets its bit again and is picked up next time.
Nothing on the wire changes.

The five-second resend now doubles as a check on the hardware. It
reads every stripe, and any stripe that changed without its bit set is
counted, printed, and summed up when the last viewer leaves:

```
screen: 186 full check(s), 0 stripe(s) changed with no DIRTY bit
```

On a bitstream without the register (FEATURES2 bit 14 clear) every
pass reads everything, as before.

Measured on the ULX3S 85F with one viewer, before and after:

| | before | after |
|---|---:|---|
| `net`, desktop still | 32.3% | **3.8%** |
| time in the snapshot, desktop still | 14.3 ms x 19.4/s = 27.7% | one 17.6 ms pass every 5 s = 0.35% |
| `net`, no viewer | 1.47% | 1.47% |
| `net` while dragging a window, cube open | 83.7% | **73.0%** |
| time in the snapshot, `gpu3d` animating | 30-35 ms a pass | ~9 stripes a pass, 7.9% of the clock |
| `net` with `gpu3d` animating (the machine is saturated; `net` uses its slice either way) | 59.1% | 58.8% |
| stripes reaching the browser, same scene | 44.0/s | 48.7/s |
| `gpu3d` frame rate, same scene | 81-84 fps | 79-83 fps |
| a stroke in `draw` with the cube animating, gap between updates | 30 / 34 / 30 ms | 27 / 24 / 28 ms |

What the browser shows was compared with VRAM read back over the
console, pixel for pixel, after each of nine scenes -- a still desktop,
opening windows, the cube animating for 40 s, a window drag, painting,
a focus change, the dock, typing, a page loading -- with one viewer held
for the whole scene and every pointer and key event sent over it: 0
pixels differ in each, and the check above counted 0 misses.

### One frame is one instant

The obvious loop is stripe by stripe: hash 16 rows, compress them,
clock them out, move on. A pass takes tens of milliseconds and the
screen does not hold still for it, so the thirty stripes were thirty
different moments. Dragging a window, the browser assembled a frame
holding the elastic band in several places at once -- and only over
the remote link, never on HDMI, where the XOR band is drawn and undone
in pairs. Measured: 91% of the frames a drag produced held the band at
up to five different x positions.

`snapshot()` fixes it by copying. One tight pass reads the whole
framebuffer into `net`'s own memory **and** hashes it on the way
through, and comparison, PackBits and transmission all read the copy.
Whatever `wm` does next cannot reach a scan already in flight. There
is no second page in VRAM to read instead; this is 38400 bytes of
`net`'s region standing in for one.

It is close to free because it replaces reads rather than adding them.
The hash pass already read all 9600 words out of VRAM, and every
stripe that got sent was read a second time into a staging buffer for
the encoder. Fusing copy and hash trades those second reads for the
writes of the copy.

### The hash has to see a vertical line

The first hash was `h ^= w[i]; h = rotl(h, 5);`, and it could not tell
a stripe with a full-height vertical line in it from a blank one. Not
a near miss -- the same value, exactly, for every column.

Xor and rotate are both linear, so that hash is the xor of every word
rotated by its distance from the end, and two words land on the same
rotation whenever their indices differ by a multiple of 32. A row is
20 words, so rows *r* and *r+8* alias (20*8 = 160), and a 16-row
stripe is exactly eight such pairs. A vertical line writes the same
bit in the same word of every row, so each pair cancels and the line
contributes nothing. Any rotate-and-xor has this hole.

What it cost on screen: a window dragged over the remote desktop left
the elastic band's two **vertical** edges behind, because the only
part of the picture made of full-height lines was the part the change
detector was blind to. The stripes holding the band's horizontal top
and bottom were resent; the leftovers survived until the five-second
full pass wiped them. The same drag on HDMI is spotless, which is what
kept the blame on compositing for so long.

Adding `h += w[i]` is the whole fix: addition carries across bit
positions, so the hash stops being linear over xor and the pairs no
longer cancel. Measured, 0 of 536 one-pixel moves of a vertical line
missed, against 536 of 536 before. One instruction per word, and no
multiply -- a board without the DSP option would pay a 32-step
sequential multiplier for it.

### PackBits, and what it costs

Stripes go out PackBits-encoded (`sw/apps/net/packbits.h`): a control
byte, then either a literal span or a repeated byte. A 1bpp desktop
stripe is mostly one repeated value, so it collapses hard.

The worst case is not small, though, and that once cost real bytes.
This encoder spends two bytes on a repeat of two and two on a literal
of one, so a stripe that alternates them -- `A BB A BB ...` -- grows by
a third. The dock's bottom stripe (icon outlines, row after row of
short runs) encodes to 1373 bytes. `net` used to build the datagram in
a buffer sized for 1280 + 64 and wrote past its end every time the dock
was sent, and the ESP32, which takes at most 1280 + 64 bytes of payload
(`STRIPE_MAX` in `screend.c`), cut the stripe short: the right half of
the dock's last row never reached the browser. `stripe_send()` now
sizes the buffer for the real worst case, and sends any stripe that
PackBits would grow past 1290 bytes as plain 128-byte literal runs
instead. That is still PackBits, every decoder already reads it, and it
fits the relay with the frame trailer on. The encoder itself is
unchanged: `tests/test_packbits.c` pins its output byte for byte.

It is also the second largest thing `net` does while a browser
watches. Measured on the board with `gpu3d` animating and ten stripes
changing per frame: 5.8 ms per stripe, 218 cycles per input byte,
27-37% of the whole CPU. The run counter is where nearly all the
iterations are, so it tests four bytes at a time against the value
splatted into a word wherever alignment allows. Note what did **not**
work: reading the source a word at a time and shifting bytes out of a
register was 23% *slower*. This is a PicoRV32, where the cost is
instructions retired, so hiding a load behind three arithmetic
operations is a bad trade.

### The frame trailer

Four bytes after the payload, and the reason a moving picture arrives
whole:

```
0x5A | fseq_lo | fseq_hi | flags        (bit 0 = last stripe of frame)
```

Behind the payload rather than in the 6-byte header because the header
does not survive the trip: the ESP32 relays `[idx, len, data]` to the
browser and drops everything else, so anything the page must see has
to travel inside `data`. Trailing bytes are the one place where that
is invisible to a decoder that does not know about them -- the decode
stops the moment it has produced its 1280 bytes -- so an ESP32 running
older firmware and a browser running an older page both keep working
byte for byte, with only the frame assembly missing.

A reader tells trailer from payload by arithmetic, not by the magic
byte alone: it is there only if exactly four bytes are left over after
the decode consumed what it needed.

### Who is watching, and how often

Two limits, and both were added after measuring what the streamer took
from the foreground: 80-120 us per character cell of a terminal
redraw.

**A viewer has to be connected.** Before, the only condition was
having a gateway, so a board with nobody watching still hashed the
whole framebuffer about 180 times a second -- roughly 1.3 MB/s of
reads across the same arbiter the GPU and the video scanout use.

`net` cannot see the WebSocket: it terminates on the ESP32. What does
cross the link is that firmware's own log, every `ESP_LOG` line framed
as `ZNIC_LOG`, and `screend` logs `viewer connected (fd N)` and
`viewer gone (fd N)` for exactly these two events. `esp32link.c` keeps
the count from those lines. That is a string match on another
program's log messages, which is not a contract, so there is a floor
under it: any keyboard or mouse event from the browser also counts as
a viewer for the next 30 seconds. A missed "connected" therefore means
a picture that is frozen until the pointer moves, never one that stays
frozen; a missed "gone" means streaming to nobody until the next
connect, which is what it did all the time before.

It keeps the fds rather than a count, because `screend` hands the same
fd to the next viewer and a browser that closes cleanly produces no
"gone" line at all -- its slot is freed a second later, when the
keepalive to it fails. Counting up on connect and down on gone
therefore drifted upwards, one per reconnect, until `net` believed
somebody was always watching.

**A pass is complete and paced.** Complete -- every dirty stripe in
one pass -- because a budget spread across several calls splits one
frame across several moments, which is the same comb of leftover edges
by another route. Paced because 180 fps was never useful: the browser
draws what it gets and the wire is 3 Mbaud. The ceiling is 20 passes a
second on a quiet screen and 15 under churn, where "churn" is more
than ten stripes in a pass -- a drag, or `gpu3d` animating -- and every
one of those stripes is bytes clocked out by hand.

`screen_idle_ticks()` reports when the next pass is due, and `net`'s
main loop sleeps on the nearest deadline anybody has, so a desktop
nobody is watching costs nothing at all.

## screend

`esp32/zeitlos-nic/main/screend.c`. An HTTP server on port 80 with two
handlers: `/` serves the page (compiled into the image), `/ws` is the
relay. `/ws` stays an ordinary GET until the origin check has passed.
The server would otherwise send the `101` before the handler ran, and
a refused page would already be attached. Who may open it is the same
rule as "Which page" under "Served by Zeitlos". There is no subnet
test here: the station is reached from other networks, and that test
would refuse the browser that is meant to connect.

A UDP task binds port 7777 and keeps one **shadow** per stripe --
still compressed, never decoded here -- plus a dirty mask. It is the
shadow that makes a reconnecting browser cheap: a new client is marked
as needing all thirty stripes and gets them from what is already held.

The relay task wakes the instant a stripe lands, with a 33 ms backstop
for anything missed, and sends each client **one** WebSocket frame
carrying every stripe it is still owed:

```
[ idx:u8 | len:u16le | data[len] ] repeated
```

Up to eight viewers. A ninth evicts the least recently served. Once a
second, a one-byte frame goes to every client that is up to date: the
page ignores it, and a send that fails is how a browser that vanished
without closing gets its slot back.

## The page

`esp32/zeitlos-nic/web/index.html`, one self-contained file -- it
cannot fetch a second one, so the wordmark is an inlined PNG used as a
CSS mask and everything else is in the file.

One file, two homes. The ESP32 firmware builds it in
(`EMBED_TXTFILES`). For the desktop Zeitlos serves itself, the card
image carries the same file as `/zerdesk/index.html` and `zerdesk`
reads it from there ("Served by Zeitlos", below), so that copy can be
edited on the machine.

**The canvas.** 640x480, `image-rendering: pixelated`, sized to the
window at 4:3. `fit()` prefers a whole number of framebuffer pixels
per screen pixel when that costs less than one CSS pixel of slack over
the 480 rows, and otherwise takes every remaining pixel. There is no
CSS border on it: `getBoundingClientRect()` is the mouse hit box, and
a border would put the pointer one pixel off.

**Frames, not stripes.** Stripes are painted onto an off-screen canvas
and the visible one is updated once per frame, in one `drawImage` of
just the rows that moved. The rule for when a frame is done has to
survive the relay: `screend` forwards whatever is pending in index
order, so a single WebSocket message can carry the tail of one frame
after the head of the next, and showing the picture whenever the frame
number moved on cut frames in half -- measured, 8-20% of the updates
were mid-frame. So the picture is shown when the **last** stripe of
the **newest** frame arrives. A stripe that turns up late, tagged with
an older frame, is still the freshest thing there is for those rows,
so it is painted; it just does not close anything. A timer covers the
case where the closing stripe never arrives at all: it fires on
silence, not on a deadline, so it cannot cut a frame that is still
streaming in. And a stream with no trailer at all is shown stripe by
stripe, which is what the page did before trailers existed.

**Keyboard.** `event.code` maps to a USB HID usage, modifiers pack
into one byte, and the pair goes back over the same WebSocket as
`[usage, mods, pressed]`. The firmware relays that as `ZNIC_INPUT` and
`net` pushes it into the kernel's shared HID ring with
`Z_SYS_HID_INJECT`, so it reads back through `HID_READ_KEY` like any
other key and `wm` cannot tell the difference. See
[user_input.md](user_input.md).

**Mouse.** The canvas box is scaled to framebuffer coordinates
(floor, not round: a CSS point sits in the pixel whose box contains
it) and sent as `[x_lo, x_hi, y_lo, y_hi, buttons]`, coalesced to
about 30 Hz for motion, immediately for a press or a release so a
click cannot hide behind the timer. The firmware relays it as
`ZNIC_MOUSE` and `net` writes `reg_vmouse` -- a software pointer
register (on every board now, `` `VMOUSE ``; scripted demos use it too,
[automate.md](automate.md)) that shares the layout of the USB one, so `wm` reads it the
same way -- then calls `Z_SYS_WM_WAKE`, because an MMIO write has no
interrupt behind it. Leaving the canvas, or hiding the tab, sends an
explicit release (`buttons` bit 7), and a second of silence drops the
register on its own, so the physical USB mouse is never left latched
out.

Note the pointer you see is the browser's own. Zeitlos composes its
cursor in the scanout, not in VRAM (`rtl/gpu/gpu_cursor.v`), so it is
on HDMI and not in the stream.

**Reconnecting.** A closed socket is retried every 1.5 s, and the
status line says which of connecting / connected / reconnecting it is,
with stripes per second and KiB/s beside it. Nothing has to be
restarted on the board for a browser to come back; the shadow is
already there.

## Colour: the stream does not carry any

The framebuffer is one bit per pixel. A pixel is set or clear, and
that is all that reaches the browser -- the stripes are the bits,
PackBits does not change that, and there is no palette anywhere in the
protocol. **So the two ends colour the picture separately, and they
are meant to.**

On HDMI the colour is the SOC's. `system.video.mode` in
[`/zeitlos.cfg`](config.md) -- also the `color` console command and
the Display row of the [settings](settings_app.md) app -- writes the
two low bits of socctl's VIDEO register (`0x7000_0208`,
[socctl.md](socctl.md)), which reach `rtl/gpu/gpu_video.v` as
`video_mode` and are applied in the scanout, at the last moment before
the pixel leaves the chip:

| mode | value | HDMI/VGA |
| --- | --- | --- |
| `white` | `00` | white on black (the reset default) |
| `amber` | `01` | amber on black |
| `green` | `10` | green on black |
| `paper` | `11` | black on white |

`paper` is not a colour: it is white with the pixel sense inverted, so
it reuses the white path exactly rather than defining a second white
that could drift out of step with the first.

The viewer cannot know any of that -- it is downstream of the
framebuffer and upstream of the scanout -- so the page has **its own**
selector, with the same four names, and tints the canvas as it
decodes: a set bit is the ink, a clear bit is the paper.

| mode | ink | paper |
| --- | --- | --- |
| Paper | `#000000` | `#ffffff` |
| White | `#ffffff` | `#000000` |
| Green | `#33ff6a` | `#070a07` |
| Amber | `#ffb000` | `#0a0806` |

The same two values colour the page's own chrome, so the masthead and
the desktop agree.

**The two are independent by construction, and that is not a fault.**
The board can be in green while the browser is in amber; two browsers
can disagree with each other. Out of the box they already do: the SOC
comes up `white` (or whatever the board's `GPU_*` defines set as the
power-on default) and the page opens on Paper. Changing the mode on
the board changes nothing in any browser, and vice versa, because
nothing in between carries a colour to change.

## Tools

`esp32/flash.py --from verify --host <ip>` checks both ends without
touching the board: that the desktop answers, that the page it serves
carries the marker of a viewer that assembles whole frames, and that
stripes are actually arriving, by counting how many of the thirty turn
up over the WebSocket. It is worth running on its own after a board
moves network, not only after flashing.

Anything else wants a browser. The protocol above is small enough to
speak from a script -- a WebSocket, binary frames, PackBits, and
five-byte mouse packets -- which is how the desktop gets driven with
no keyboard or monitor attached to the board at all.

## Served by Zeitlos

`sw/apps/zerdesk/`. An app of its own: not part of `net`, and not a
listener inside `netserve`. `net` accepts the TCP connections
(`Z_NET_LISTEN`) and relays them; `zerdesk` serves two paths.

| | |
|---|---|
| `GET /` | the viewer page, read from the card: `/zerdesk/index.html`. The card image puts `esp32/zeitlos-nic/web/index.html` there unchanged, so both paths serve one file |
| `GET /ws` | the WebSocket. One stripe is `[idx:u8 \| len:u16le \| PackBits + trailer]`, the bytes `screend` forwards today. Keys come back as three bytes, the pointer as five, and `zinput` injects them the same way `esp32link` does |

There is no password. The port is `apps.zerdesk.port`, **8080** unless
the file says otherwise.

**Who may connect.** `apps.zerdesk.allow` is `subnet` or `any`, the
same choice `netserve` makes, and `subnet` is the default. Anything
else, and a missing key, stays on the subnet. A peer off the subnet
is refused before it is given a connection.

**Which page.** The upgrade is taken when `Origin` is absent. A
browser always sends that header, so a page on another site cannot
leave it out; the programs that drive the desktop are not browsers
and do not send it. When the header is present it has to be `http://`
or `https://`, in either case, followed by the value of `Host` and
nothing more. `null`, an empty value, a path or a different host is
`403 Forbidden` and the body `Origin not allowed`. The `101` is not
sent. `screend` uses the same rule and the same body. The upgrade
also requires `Sec-WebSocket-Version: 13`.

**How many.** `apps.zerdesk.viewers` is how many browsers at once,
**6** unless the file says otherwise, and never more than 6 (that is
how many inbound relays `net` has). Whoever asks for `/` or `/ws`
past the number in the file, while a relay is still free, gets a
plain-text `503` whose body is
`Too many viewers connected`. The page is unchanged, so that is what
the browser shows. With the cap set to 3, that refusal is the fourth
browser. At the default of 6 the next connection does not reach the
app: `net` has no seventh relay, and the TCP connection is refused.
The ESP32 path is different: `screend` allows
eight, and a ninth evicts the least recently served.

**The page is on the card.** `zerdesk` carries no copy of it. Each
`GET /` opens `/zerdesk/index.html`, takes its size for
`Content-Length`, and sends it 1 KB at a time through the same buffer
the stripes use, so the page costs no memory of its own and may be any
size. An edited page is what the next browser gets, with no restart.
Reading 18 KB from the card makes the request about 65 ms slower than
a built-in page did (about 0.27 s instead of 0.21 s on a direct
cable), once per page load.

It is outside `/www` on purpose: `netserve` serves that directory, and
a viewer page served from there would open its WebSocket to
`netserve`. `/zerdesk` sits beside the other apps' data directories,
`/bbs`, `/fed` and `/web`.

Without the file there is no remote desktop, and both ends say why.
At start the console says

```
zerdesk: NO PAGE: /zerdesk/index.html is not on the card. Browsers get a plain-text error until it is there; the card image carries it.
```

and `zerdesk` keeps running. A browser asking for `/` then gets a
plain-text `503`:

```
No remote desktop: the viewer page /zerdesk/index.html is not on the card. The card image carries it; its source is esp32/zeitlos-nic/web/index.html.
```

and the console logs each refusal. Putting the file back is enough:
the next request is served.

**Started by hand.** From `term`, `run zerdesk`. It does not start
by itself. On the ESP32 bitstream it prints that the link is present
and exits, which is the whole of the refusal.

```
apps.zerdesk.port: 8080
apps.zerdesk.allow: subnet
apps.zerdesk.viewers: 6
```

**One consumer of DIRTY.** The register is cleared by the reader.
`net` scans only while its PHY is the ESP32 link; on the Ethernet
bitstream it does not, and `zerdesk` is the reader. A second reader
would clear the first one's bits. That is also why the app will not
start beside the ESP32 path.

**A frame is still one instant.** The copy is taken before input is
read. Input wakes `wm`, and a copy taken while `wm` is repainting a
drag band catches the band in two places. The pacing is the ESP32
path's: at most 20 passes a second on a quiet screen, 15 when more
than ten stripes changed. A new viewer is sent every stripe of the
copy. The first viewer, and the first one after the last one left,
also forgets the hashes, so that frame is read from VRAM. There is
no shadow on a second machine: the snapshot in the app is the copy,
38400 bytes of its own memory. Every five seconds the pass reads
every stripe again and counts one that changed with no DIRTY bit,
and prints the total when the last viewer leaves, the same sentence
`net` prints.

**The connection id only grows.** `net` finds a relay by listener and
id among every relay that is still around, including one that is
closing. Reusing an id while that relay lives delivers the new
connection's acknowledgements to the old one, and the new connection
stalls. `zerdesk` skips 0 and only counts up. Listeners share `net`'s
relays (6) and its TCP slots; the last listen on a port takes it
over. See [networking.md](networking.md), "Accepted connections".

**Memory.** The executable asks for the medium tier, 32 KB of stack
and heap (`APP_STACK = 32K`; the unnamed default is 16 KB).
Everything big is static. What the heap holds is `zport`'s copy
of bytes in flight to `net`, and that is capped at 6 KB in total and
4 KB to one viewer. The page is read into that buffer as it goes, so
it adds nothing of its own. A viewer that makes no progress for 10
seconds is dropped. While a viewer is caught up, one byte goes out
every five seconds; the page ignores it, and a send that fails is how
a browser that vanished gets its slot back.

With the cube running, six viewers, and a page load while five were
already connected, the heap high-water went from 9,332 bytes to
13,428. The tier gives 33,908 bytes of stack and heap; the stack was
2,496. That leaves 17,984 bytes free, which is the worst case the
cap was chosen against. The six share one 6 KB budget, so the stripes
each viewer gets fall as more connect: 88, 55, 49, 37, 29 and 24 a
second, from the board's own ten-second counts. Over a later ten
seconds with those six and the cube still up, `net` was 41% of the
CPU, the cube 30% and `zerdesk` 28%. A seventh connection, tried
while the six were still open, was refused by `net`; `zerdesk`
logged nothing.

**Where the two differ, measured.** ULX3S 85F. The Ethernet bitstream
is `ulx3s_85f_langkatze` with `PNR_SEED=7`: the Makefile's default
seed of 10 does not meet 48 MHz on that target (45.71 MHz), and seed
7 does (56.38 MHz). The ESP32 bitstream is `ulx3s_85f`, seed 10,
52.92 MHz. The desktop was the same scene on both: three `term`,
`draw`, and `gpu3d` in the upper right, unless a row says otherwise.
The Mac was not idle, so the key latency, which is timed on the Mac,
moves more than the board's own CPU.

The shared scan was measured on the ESP32 path against the previous
`net`, both built from this tree. A still desktop, a key, the six
scenes, `regress16` and a visible cube come out the same: the cube is
48 stripes a second and `net` is 59% either way. `net.bin` is 136,732
bytes, four bytes shorter than the previous build.

| | Zeitlos, Ethernet | the ESP32 |
|---|---|---|
| desktop still, one viewer | `net` 0.07%, `zerdesk` 0.44%. 30 stripes on connect, then none | `net` 3.9%. A short burst every 5 s, about 7 stripes/s |
| 30 keys in one `term` | 47 ms median | 130 ms median |
| `gpu3d`, three `term`, `draw`, one viewer, the cube at 140,120 | 42 stripes/s. `net` 25%, `zerdesk` 41%, `gpu3d` 31% | 48 stripes/s. `net` 59%, `gpu3d` 39% |
| the same cube, upper right | one viewer 47 stripes/s; two 33; three 24 each | — |
| a browser past the cap | with the cap at 3, a fourth browser is a plain-text 503 and the three are unchanged. At the default of 6 a seventh connection is refused by `net` | `screend` allows eight; a ninth drops the least recently served |
| viewer against VRAM, six scenes | 0 pixels | 0 pixels |
| two hours, one viewer, the cube | 46.5 stripes/s, the heap flat | |

With the cube animating the machine is full either way, and the
Ethernet path spends part of that on TCP in the app, so fewer stripes
reach each browser than the ESP32 relays. A still desktop is the other
way: with one viewer and nothing moving, the scan costs well under one
percent. A browser past the cap is the plain-text refusal above.
With the cap at 3 it does not slow the three that are already
connected. At the default of 6 the seventh never reaches the app.
