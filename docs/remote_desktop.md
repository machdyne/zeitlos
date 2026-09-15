# Remote desktop

The 640x480 1bpp framebuffer, in a browser, over WiFi. Point one at
the board's station address and you get the desktop as it is on HDMI,
with the keyboard and mouse going back the other way. There is nothing
to install and nothing to run: the page is served by the ESP32.

This is ULX3S-only, because the [ESP32 link](esp32link.md) is. `net`
produces the picture, the firmware serves it, and neither half exists
on a board with a wired NIC.

```
net (sw/apps/net/screen.c)          the ESP32 (esp32/zeitlos-nic)
  snapshot + hash 30 stripes
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
rssi=... ip=...` right after -- and `net` also writes it to `NET.IP` at
the root of the sdcard when the link comes up. Everything else is
`http://<that>/`.

## Stripes

The framebuffer is 30 stripes of 16 rows. A stripe is 1280 bytes
(640/8 x 16), which is one UDP datagram and one link frame, and 30 of
them is the whole screen.

Each pass hashes all thirty and sends the ones that differ from what
was last sent. Every five seconds the hashes are cleared so that a
stripe lost in flight heals itself -- this is UDP on purpose, and the
consumer, not the producer, keeps the shadow. That full pass waits if
the pointer is down or the screen is already busy: thirty stripes is
the longest the wire is ever held, and holding it through a drag is
what a viewer feels as seconds.

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
stripe is mostly one repeated value, so it collapses hard, and the
worst case grows the data by about 1/128, so there is no expansion
trap to guard against.

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
relay.

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
register that shares the layout of the USB one, so `wm` reads it the
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
