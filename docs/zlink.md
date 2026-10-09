# zlink

A link between two Zeitlos machines -- or a Zeitlos machine and a
Werkzeug -- over **a couple of wires**. One protocol that scales from
about 10 KB/s on two GPIO pins with no special hardware, to about
1 MB/s through a GPIO stream engine, to about 10 MB/s over the SATA-style
LVDS port on Mozart and Chopin.

What it is for: moving files between two computers that have no network
between them, a shell on the other machine (`port zlink0`), remote
debugging, cloning one machine onto another, cheap GPIO expansion, and
handing work to a second FPGA.

**Status.** This page is written ahead of most of the code, the way
[ports.md](ports.md) and [networking.md](networking.md) track their own
phases; each section says what exists.

| round | what | status |
|---|---|---|
| 1 | the physical layer in the GPIO stream engine: 8b/10b, clock recovery, any pin ([gpio.md](gpio.md#stream-engines)) | **done** |
| 2 | `sw/common/zlink` (negotiation, framing, CRC, retransmission, channels), soft zlink, `sw/apps/zlink` (files both ways, `port zlink0`, the upgrade from soft to a stream engine) ([zlink_app.md](zlink_app.md)) | **done** -- tested on the host, not yet on hardware |
| 3 | a bench I2C slave in `gpio.v` (`GPIO_I2CS`, for [bench.md](bench.md)) | planned |
| 4 | the fast block: the DS/SATA port on Mozart and Sergei, ULX3S pairs, the LVDS Pmod | planned |
| 5 | a hardware debug responder: peer memory access, load-and-run, cloning | planned |
| 6 | Werkzeug firmware | planned |
| 7 | Bach (two modules, one board) | planned |
| -- | `spiflash.v`'s page buffer to block RAM: -214 logic cells on every board ([spiflash.md](spiflash.md)) | **done** |
| last | more LUT recovery: the audio mixer's LUT RAM, the blitter's arithmetic ([boards.md](boards.md)) -- which may let Lakritz have an engine | planned |

## Tiers

Three ways to carry the same link, all speaking the same protocol, so
any two endpoints negotiate down to the best tier they share:

| tier | hardware | wires | direction | speed |
|---|---|---|---|---|
| **soft** | none: plain GPIO, in software | 2, open drain | half duplex | ~100-300 kbit/s, 10-35 KB/s |
| **GPIO stream** | a stream engine (`GPIO_STREAM_ENGINES`) | 1 | one way | up to 12 Mbit/s, ~1.2 MB/s |
| | | 2 | full duplex | up to 12 Mbit/s each way, ~1.1 MB/s |
| **fast** | the fast block (round 4) | 4: two LVDS pairs | full duplex | ~100 Mbit/s, ~10 MB/s |
| | | 6: three pairs (Bach) | full duplex | more, with a forwarded clock |

Every configuration also needs **ground**.

The GPIO stream figures are the line rate less 8b/10b's 20%; framing
and acknowledgements take a little more. For file transfer
the SD card is the limit anyway -- 600-850 KB/s reading
([sdcard.md](sdcard.md)) -- so the GPIO stream tier is already about as
fast as a file copy can go, and some 50 times the ~11 KB/s TCP manages
on the 10 Mbit Ethernet path ([networking.md](networking.md)). The fast
tier's extra speed is for things that bypass software: Bach, offload,
the debug responder.

### Soft zlink (round 2: done)

Two GPIO pins and no engine, which is every board with a GPIO port --
including `lakritz_gpio`, which has no LUTs to spare for an engine, and
any bitstream built before engines existed.

A link between two machines that each run a preemptive scheduler cannot
rely on timing: either end can be paused for a scheduler tick at any
moment. So soft zlink is **fully interlocked** -- every step waits for
the other side -- using the open-drain idiom GPIO already has (DIRSET
drives a pin low, DIRCLR releases it to the pull-up), which lets both
ends pull either wire:

1. Idle: both wires high.
2. The sender pulls **X** low for a 0 or **Y** low for a 1.
3. The receiver, whenever it runs, sees which wire is low, keeps the
   bit, and acknowledges by pulling **the other** wire low too.
4. The sender sees both low, knows the bit arrived, and releases its
   wire.
5. The receiver sees the sender's wire rise and releases its own.
6. Both high: the next bit.

A paused process only pauses the link; nothing is lost. Half duplex: the
link layer passes the turn back and forth. External 4.7k pull-ups make
it faster -- the internal ones are weak, and every bit waits for two
rising edges.

One wire cannot do this without timing, which is why 1-wire zlink needs
an engine (and is one way).

**Frames** go LSB first, delimited the way SLIP does it
(`sw/common/zlink_soft.h`): END END, the frame with END (`0xC0`), ESC
(`0xDB`) and `0x3F` escaped, then END. `0x3F` is END with every bit
inverted: on a **crossed** cable -- this end's X is the other's Y --
every bit arrives inverted, so the receiver finds `0x3F 0x3F` where it
hunts for END END and decodes the rest inverted. Either cable works,
and the receiver knows which it has: that is how the upgrade (below)
knows which wire to transmit on.

A step that waits more than 250 ms for the other side aborts the frame
(a collision while the link comes up, or a peer that went away); the
link layer resends it. A machine starts a frame only when nothing has
arrived for 3 ms, so it does not start one in the middle of the
other's.

**Speed** is set by how fast both machines notice the wires, not by a
clock: each bit is two round trips through both machines' GPIO
registers. While a frame is on the wires the `zlink` service polls in a
tight loop rather than sleeping a scheduler tick per bit. The estimate
in the table above (10-35 KB/s) is from that loop's length; it has not
been measured on hardware yet.

### GPIO stream (round 1: done)

A stream engine in zlink mode ([gpio.md](gpio.md#stream-engines)) on
any two pins of any port -- or one, for a one-way link. Built into every
GPIO target by default.

### Fast (round 4)

Two LVDS pairs, one each way, on the SATA-style connector that Mozart
and Chopin have (`DS1`, `DS2` -- see "The DS port" below), or on the
ULX3S's differential header pairs. The same 8b/10b and the same
protocol, at a higher sample rate.

## The line code

**8b/10b**, the standard IBM code (Fibre Channel, Gigabit Ethernet,
SATA). It gives a transition at least every five bits, so the receiver
can recover the clock from the data, and comma symbols, so it can find
where symbols start. `tools/gen_8b10b.py` generates the RTL's tables
from the published sub-block tables and checks the code's properties
-- DC balance, run length, that a comma never straddles two symbols --
before writing them; `rtl/tests/tb_8b10b.v` checks the RTL against all
536 symbols.

Control symbols zlink uses:

| symbol | byte | use |
|---|---|---|
| K28.5 | `0xBC` | **idle and alignment.** Sent whenever there is nothing else, and after every 255 data symbols in a row, so a receiver that slips re-aligns within 256 symbols. Never placed in the RX FIFO |
| K27.7 | `0xFB` | start of frame |
| K29.7 | `0xFD` | end of frame |
| K30.7 | `0xFE` | reserved |

K28.7 is never sent: it can form a false comma with the symbol after it
(the one documented exception in the code, and why the generator's
check excludes it).

## Clock recovery

Two machines have two oscillators, so the receiver cannot assume the
sender's bit rate is its own. The engine samples the line four times
per bit and **re-times on every edge**: an edge restarts the bit, and
the bit is taken two samples later, in its middle. 8b/10b guarantees an
edge every five bits at most, so drift between edges is tiny.

Measured in `rtl/tb/tb_gpio_stream.v`, with two complete GPIO blocks on
separate clocks and random delay on every wire edge:

| clock offset | wire jitter | 12 Mbit/s and 3 Mbit/s, both ways |
|---|---|---|
| 0 | 0 | pass |
| +-300 ppm | 3 ns | pass |
| +-1000 ppm | 3-5 ns | pass |
| +-2000 ppm | 3 ns | pass |
| +300 ppm | 8 ns | pass |

A 48 MHz crystal oscillator is good to +-50-100 ppm, so that is a wide
margin. Bursts of 1000 symbols each way, a fault on the wire mid-burst
(three bit-times of corruption) and the link afterwards are also
tested: the fault damages
that burst -- some of it as code errors counted in SERR, some as wrong
but valid symbols, which is why the link layer has a CRC -- and the receiver
re-aligns on the next comma and is clean again.

## The link layer (round 2: done)

`sw/common/zlink.c`: reliable, ordered messages of up to 448 bytes on
256 channels, over a **transport** that moves whole frames and may lose
or damage them. It is portable C with no OS calls -- time and
randomness come in as arguments -- so its tests run two links against
each other on the host. Two transports exist: soft
(`zlink_soft.c`, above) and the stream engine (`zlink_stream.c`: a
frame is K27.7, its bytes, K29.7; the engine fills the gaps with
K28.5). A fast-block transport (round 4) will be a third.

| | |
|---|---|
| frame | type, seq, ack, channel, length (2), payload, CRC-32 (IEEE, 4) |
| errors | a frame that fails its CRC, or is short, is dropped as if it never arrived |
| reliability | go-back-N: 2 frames in flight full duplex, 1 half duplex; cumulative acks; resend after the transport's timeout (30 ms stream, 400 ms soft) |
| flow control | a full receive queue does not take or ack a frame, so it is resent later |
| coming up | both send HELLO (a random number, capability bits, the other's number once heard, whether up); an end is up when the other's HELLO carries its own number back. The larger number is the **primary** |
| half duplex | the primary sends a frame (data, or an ACK as a poll) and the secondary answers each with exactly one; the primary polls every 20 ms when idle, at once when there is data |
| dead | nothing heard for 3 s: down. Sequence numbers restart when it comes up again |

Capability bits in HELLO say what each machine has: a stream engine,
files to read, files to write, a shell. `zlink status` shows both
machines'.

## From soft to stream (round 2: done)

`zlink start` with no `-m` begins on soft zlink, which every GPIO board
can do and which finds out which way round the cable is. Once up, if
both machines have a stream engine, the primary offers an **upgrade**:
the two agree a rate, let go of the wires, and each puts an engine on
the **same two pins** -- the primary transmits on its X, the secondary
on whichever of its wires the primary's X does not reach. If the
stream link is not up within 2.5 seconds, both go back to soft and try
again one step slower: 12, 6, 3, 1.5, 0.75 Mbit/s. `-r` caps the
ladder; when every step has failed, the link stays soft.

The hazard is electrical: soft only ever pulls a wire low, but an
engine **drives** its TX wire both ways, and an engine driving a wire
the other machine is still pulling low is a short through two pins. So
the changeover is timed to keep one rule -- a wire goes from pulled by
soft, to released by both, to driven by one engine; never two drivers,
and never soft pulling a wire an engine drives. Each machine times from
its own moments: P, the primary sends the offer; S, the secondary
handles it; T, the primary handles the answer.

| | secondary | primary |
|---|---|---|
| P | | sends `U`, the offer |
| S | handles `U` -- only if it arrived less than 100 ms ago -- and answers `u` with the rate: **committed** | |
| by S + 300 ms | lets go of both wires (at once if the `u` is acknowledged) | |
| T, by P + 400 ms | | handles `u`, lets go of both wires |
| S + 1000 ms | engine on | |
| T + 1000 ms | | engine on |
| | 2.5 s for the stream link to come up | the same |

If no `u` comes by P + 400 ms, the answer may have been lost with the
secondary committed, so the primary goes **quiet** -- both wires
released -- until P + 5.4 s.

Why that holds: the offer cannot arrive before it was sent (S >= P);
the primary sends nothing after P + 400 (it has the answer or is quiet)
and the secondary refuses an offer more than 100 ms old, so S is at
most about P + 500; and T is between S and P + 400. So the primary has
let go by P + 400, 600 ms before the secondary's engine (S + 1000 >=
P + 1000); the secondary by S + 300, 700 ms before the primary's (T +
1000 >= S + 1000); and a committed secondary whose link never came up
has its engine off by about P + 4 s, inside the primary's quiet time.
The margins are for a loop that stalls -- and while a change is under
way neither machine starts new work that could stall it (file I/O, a
password check): commands, requests and shells are refused with "busy
-- changing to a faster link; try again in a few seconds".

Every way back to soft (a failed rate, a stream link that later dies)
is quiet first for ZL_DEAD_MS + 1.5 s: long enough for the other
machine, if it is still on its engine, to notice the silence and let go
too. The host test models the wires electrically and fails at any
moment that breaks the rule (below); with the quiet time on the way
back removed, it fails. The **100 Ohm series resistors** recommended
below still make sense: they cover a bitstream or a program that gets
it wrong.

`-m stream` skips all of this: an engine from the start, on fixed TX
and RX pins, wired TX to RX. That is the mode for the LVDS Pmod, whose
pairs only go one way, so soft cannot run over it.

## Wiring

### Pmod to Pmod, direct

Two wires (TX, RX) plus ground between two boards' GPIO ports, 3.3V to
3.3V. Any pins: the engine's roles go wherever `SPINS` puts them, and
negotiation decides which wire each side transmits on, so a
straight or crossed cable both work.

- Keep it short: about 10 cm for the full 12 Mbit/s, slower for longer
  wires or loose jumpers -- negotiation steps down.
- **A series resistor of about 100 Ohm in each line** is cheap
  insurance: until negotiation has run, both ends listen, and only one
  drives each wire afterwards -- but a misconfigured build could put two
  outputs on one wire, and the resistor keeps that from being a short.
- Connect with both boards off, or at least touch grounds first.

### Loopback on one board

A jumper between two pins of one port tests the whole TX and RX path
with one board -- that is `rtl/tb/tb_gpio_stream.v`'s jumper from
port 0 pin 0 to pin 1. The engine also has an internal loopback
(SCTL's LOOP) that needs no wire at all.

### The LVDS Pmod

Machdyne's [LVDS Pmod](https://machdyne.com/product/lvds-pmod/) puts a
GPIO port's single-ended pins through an LVDS transceiver onto a SATA
connector. It is wired as the **host** side: it connects to a Mozart or
Chopin with an ordinary straight SATA cable. Two LVDS Pmods need a
**crossover** SATA cable -- the transceiver fixes each pair's direction,
so roles cannot fix that.

What it gives over direct wiring is **reach and robustness**, not a
higher ceiling: the hop from the FPGA to the transceiver is still
single-ended through the Pmod header. Over a SATA cable (100 Ohm pairs,
up to 1 m, shielded) it holds the GPIO tier's full rate where loose
wires would not, and it tolerates separate supplies and ground offset.
Its pinout and transceiver part are not yet published; round 4 adds
them here.

### The DS port (Mozart, Chopin; round 4)

A 7-pin SATA-style connector carrying two LVDS pairs straight from the
Sechzig module's `DS1`/`DS2` balls -- no coupling capacitors, no
termination resistors (the FPGA terminates on-chip), no ESD parts.

| SATA pin | signal | module direction (Sechzig spec) |
|---|---|---|
| 2, 3 | DS2_P, DS2_N | in |
| 5, 6 | DS1_P, DS1_N | out |
| 1, 4, 7 | GND | |

Mozart is wired like a SATA **device**, so with a standard (straight)
SATA cable two Mozarts meet transmitter-to-transmitter. One end has to
swap directions -- transmit on DS2, receive on DS1 -- which depends on
the module:

| module | FPGA | DS1 | DS2 | can it transmit on DS2? |
|---|---|---|---|---|
| ML0, ML1 | ECP5 25F / 45F | C1/C2 (A/B pair) | J1/J2 (A/B pair) | **yes** |
| ML2 | ECP5 45F | J1/J2 (A/B) | K1/K2 (**C/D** pair) | **no** -- ECP5 drives true LVDS only from A/B pairs |
| MX1, MX2 | Artix-7 35T | R1/R2 | T3/T4 (bank at 2.5V) | **yes** |

With auto-negotiation (round 4) choosing roles:

| straight cable | ML0/ML1 | MX | ML2 | LVDS Pmod |
|---|---|---|---|---|
| **ML0/ML1** | yes | yes | yes | yes |
| **MX** | yes | yes | yes | yes |
| **ML2** | yes | yes | **crossover** (or a slower fallback) | yes |
| **LVDS Pmod** | yes | yes | yes | **crossover** |

The ML2-to-ML2 fallback drives DS2 as two complementary 2.5V outputs
instead of true LVDS: more swing than the LVDS spec allows, so slower
and not guaranteed.

The DS port **only takes LVDS-level signals**. The modules' DS banks run
at 2.5V: 3.3V driven straight in is outside the ECP5's recommended
range and beyond the Artix-7's absolute maximum (VCCO + 0.55V). 3.3V
boards reach it through the LVDS Pmod, or through an adapter with a
resistor network that brings the swing to ~350 mV and the common mode
to ~1.2V (round 4 specifies one).

### ULX3S (round 4)

The ULX3S's header pairs GP/GN 10-21 sit on true-LVDS A/B sites on the
left and right banks, but those banks are 3.3V, so it transmits
pseudo-differential (`LVCMOS33D`, as its HDMI port does) and receives
on the ECP5's differential input, with external termination. To a
Mozart's DS port it goes through an adapter with the resistor network
above; to another ULX3S, twisted pairs are enough. 85F only by default.

### Werkzeug (round 6)

The RP2040's PIO can speak the GPIO-tier zlink at about 10-15 Mbit/s,
and the soft tier trivially, on two pins of Werkzeug's IO header -- a
few wires to a Zeitlos board's GPIO port, for a cheap GPIO expander.

## Boards

| target | soft | GPIO stream | fast |
|---|---|---|---|
| `minze_gpio`, `obst_langkatze_gpio`, `schoko_langkatze_gpio` | **yes** | **yes**, 1 engine | -- |
| `lakritz_gpio` | **yes** | no: out of LUTs | -- |
| a board with a 4-pin port (Sergei's 6-pin Pmod, `GPIO_PORT0_NARROW`) | **yes** | yes: two pins are enough | -- |
| Mozart, Chopin (ML0/ML1/ML2/MX) | no GPIO port | no GPIO port | round 4 |
| ULX3S 85F | -- | -- | round 4 |
| everything else | needs a GPIO port | needs a GPIO port | -- |

Mozart's extra-link pins (XA-XD) go to its RP2040, so Mozart's only
zlink is the DS port.

## Testing

```
make test_gpio_stream                       # physical layer, ~5 minutes
make test_gpio_stream PPM=-2000 JIT=5       # a corner
make -C rtl/tests 8b10b                     # the code tables, every symbol
python3 tools/gen_8b10b.py                  # the code's properties
make -C sw/common/tests -f Makefile.zlink   # link layer, soft, stream transports
make -C sw/apps/zlink/tests                 # the app: two machines, ~40 s
```

- `test_zlink`: two links over a simulated frame wire, full and half
  duplex, with frames dropped (up to 15%) and corrupted (5%), a stalled
  reader, a restart; the CRC's check value.
- `test_zlink_soft`: the soft transport over two modelled open-drain
  wires, straight and crossed, with either end stalled at random and
  one starting 2 s late.
- `test_zlink_stream`: the stream transport through the real
  `zgpio_stream.c` on two modelled engines, with symbols lost and
  changed, and a slow reader overflowing its RX FIFO.
- `test_zlink_app`: two copies of the real `zlinkapp.c` in two
  processes on shared simulated wires -- see
  [zlink_app.md](zlink_app.md#testing) for its five scenarios.

On hardware: `zlink loop` with a jumper (or `-i`, inside the engine)
checks one board's engine and wires; then two boards, X to X, Y to Y
and ground, `zlink start` on each, and `zlink status`
([zlink_app.md](zlink_app.md)).

## See also

- [gpio.md](gpio.md#stream-engines) -- the stream engine: registers,
  modes, rates, the C API
- `rtl/gpio_stream.v` -- the engine, and why it is built the way it is
- `tools/gen_8b10b.py` -- where the 8b/10b tables come from
- [zlink_app.md](zlink_app.md) -- the `zlink` command and service
- `sw/common/zlink.h`, `zlink_soft.h`, `zlink_stream.h` -- the link
  layer and transports, for anything else that wants a link
- [bench.md](bench.md) -- what the round 3 I2C slave is for
