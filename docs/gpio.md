# GPIO

General purpose I/O: up to eight ports of eight bidirectional pins,
mapped onto PMOD connectors, driven entirely from software.

This exists because the alternative was a hardware I2C master, a
hardware SPI master, a hardware 1-Wire master and a hardware
whatever-comes-next, each with its own register block, its own pins and
its own `ifdef, on boards that have one or two PMOD connectors between
them. Eight software-controlled pins plus a bit-bang library is slower
and covers all of it, and the mapping from function to pin is decided
at a `repl` prompt rather than at synthesis.

Hardware: `rtl/gpio.v`. Software: `sw/common/zgpio.h` (phase 2),
`sw/common/zi2c.h` and `sw/common/zspi.h` (phase 4).

Where software is too slow, a **stream engine** can take over any pins
of any port: zlink, an SPI master, a UART, or raw bits in and out at a
fixed rate, chosen at runtime. See [Stream engines](#stream-engines)
below, and [zlink.md](zlink.md) for the link the engines were built for.

For one job -- a real I2C master answered by chips that exist only in
software, for [bench](bench.md) -- an optional **I2C slave** can take
two pins and answer from a register image in block RAM. See
[I2C slave](#i2c-slave).

## The block at a glance

| | |
|---|---|
| Base | `0xe000_0000` |
| Ports | `0xe000_1000 + N * 0x20` |
| Ports built | 0-4 today, 8 reserved in the map |
| Feature bit | `Z_FEATURE2_GPIO`, `FEATURES2` bit 0 |
| Pull-ups | weak internal pull-up, always on, not changeable at runtime |
| Reset state | every pin an input, so every pin pulled high |
| Stream engines | `0xe000_2000 + E * 0x40`; `GPIO_STREAM_ENGINES` per board, 1 by default where a port is built |
| I2C slave | `0xe000_2100`; `GPIO_I2CS_SLOTS`, 0 (off) unless a build asks |
| Fast zlink | `0xe000_4000`; Mozart only, with `ZLINK_FAST`, off by default ([below](#fast-zlink)) |

`rtl/gpio.v` is ALWAYS instantiated, on every board, whether or not any
port has pins. It also owns the board LEDs, which is why: it grew out
of `rtl/debug.v`, and `sw/bios/bios.c` writes the LED register before
anything else in the system is alive.

That block used to sit behind `` `DEBUG ``. The define was universal so
the missing case never happened, but if it ever had, nothing would have
decoded the `0xE` nibble -- and an address nothing decodes gets no ack
on this bus, so the CPU would have stalled on the first LED write in
the BIOS, before a character of the boot banner. Survivable while it
held two LED bits nobody probes; not once software reads a MAGIC there
to ask whether GPIO exists. `` `DEBUG `` is gone; the block is
unconditional, like `rtl/csrs.v` and `rtl/socctl.v`.

## Pins and numbering

A GPIO port is eight pins and maps onto one PMOD connector:

| PMOD pin | 1 | 2 | 3 | 4 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|
| **GPIO bit** | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |

Bits 0-3 are the top row and bits 4-7 the bottom row, so bit order is
pin order as printed on the connector. Pins 5, 6, 11 and 12 are ground
and power and never carry a signal.

The thing to remember when wiring: **bit 4 is diagonally below bit 0**,
not next to bit 3.

This isn't arbitrary. `rtl/debug.v`'s `DBG[7:0]` LED bar already landed
on Obst's PMOD B in exactly this order, so this is the convention the
tree already had, written down.

### Ports are numbers, and only numbers

Ports are 0..7 and pins are 0..7, everywhere -- the C API, the shell,
Scheme, this document. There is no letter form.

**The port number says which port the gateware built, not which
connector it landed on.** Ports are numbered densely from 0 in the
order `rtl/sysctl.v` declares them, so a board whose only GPIO port is
physically on PMOD *B* still calls it port 0.

There was briefly a letter notation -- `"B3"` for port 1 pin 3 -- and
it was removed for a reason worth recording, because it looks
convenient: **letters already mean something else in this project.** A
board spec (`release/hw/boards/*.spec`) says `pmod.a` and `pmod.b`, and
those *are* the physical connectors. So a port the release system
calls `b` would be the port the API called `A` -- two
lettering systems in one project disagreeing about the same piece of
plastic. It needed a warning paragraph in the header, another in the
shell, and another here; three warnings about one notation is usually
evidence the notation is wrong rather than that the reader is careless.

Numbers have none of that problem and compose with everything else: the
API is `(port, pin)` throughout, Scheme handles in this tree are plain
numbers so bare-word syntax works without quoting, and `0.3` covers any
place something shorter than "port 0 pin 3" reads better. Nothing
parses `0.3` -- it is just how to write it.

An ASCII label register in the hardware was considered earlier and
rejected for a related reason: with one GPIO port, which is every
target that exists, "port 0" is unambiguous because there is nothing
else it could be, and a labelling mechanism would be machinery on every
board to solve a problem only a hypothetical multi-port board has. The
release notes for each target say which connector each port is on, and
that is where to look.

## Register map

Word addresses; `rtl/gpio.v`'s `wb_adr_i` is `sysctl.v`'s
`wbm_adr_sel_word`, like every other simple slave here.

### Board registers, `0xe000_0000`

| addr | name | | |
|---|---|---|---|
| `+0x00` | LED | r/w | bit 0: `LED_B`. Resets to 1. |
| `+0x04` | LEDS | r/w | bits 7:0: `DBG[7:0]`, the `` `LED_DEBUG `` bar |
| `+0x08` | MAGIC | r | `0x5A47_5049` ("ZGPI") |
| `+0x0c` | CONFIG | r | `{16'h4750, fast, i2cs_slots[2:0], modes[3:0], engines[3:0], nports[3:0]}` |
| `+0x10`..`+0x1c` | | | reserved, read 0 |

LED and LEDS are at the addresses `rtl/debug.v` had them at, and behave
identically, so `sw/bios/bios.c` and `sw/os/uart.c` needed no changes.

Both MAGIC and CONFIG carry a constant that software checks, and CONFIG
needs its signature for a specific reason: a bitstream from before this
block existed answers *every* address it doesn't recognise with
`{31'b0, led}`, so a CONFIG read there returns 0 or 1 -- and "1" is
exactly what a working block with one port would return. The `"GP"` in
the top half is what separates those.

CONFIG's `engines` field is how many stream engines this bitstream has,
and `modes` which optional modes they have: bit 0 zlink (always set
when there is an engine), bit 1 SPI, bit 2 UART, bit 3 raw. Both are 0
on a bitstream without engines and on every bitstream that predates
them, so software that reads them needs no other check. `i2cs_slots`
is how many chips the I2C slave can emulate (below): 0 when it is not
built, which is the default. `fast` (bit 15) is 1 when fast zlink's
block is built (below); 0 everywhere else, as it always read before.

### Port registers, `0xe000_1000 + N * 0x20`

| off | name | | |
|---|---|---|---|
| `+0x00` | DIR | r/w | 1 = drive the pin, 0 = input / high-Z |
| `+0x04` | OUT | r/w | value driven where DIR is 1 |
| `+0x08` | IN | r | pin state, two-flop synchronised |
| `+0x0c` | OUTSET | w | write 1 -> set that OUT bit |
| `+0x10` | OUTCLR | w | write 1 -> clear that OUT bit |
| `+0x14` | DIRSET | w | write 1 -> drive that pin |
| `+0x18` | DIRCLR | w | write 1 -> float that pin |
| `+0x1c` | | | reserved |

Only bits 7:0 mean anything; the rest read 0. The four write aliases
**read back the register they modify** (OUTSET and OUTCLR read OUT,
DIRSET and DIRCLR read DIR) rather than 0 or something arbitrary -- a
write-only address that reads as garbage is a trap for anyone dumping
registers, and there was a correct answer available for free.

A write to a port this bitstream doesn't have is dropped; a read of one
returns 0. There is no way to signal an error on this bus, so CONFIG is
how software finds out, before it writes rather than after.

### Why 0x1000

Room and memory. The board registers are a handful of words that will
grow slowly and unpredictably; the port array is a regular structure
whose address arithmetic somebody will eventually do in their head at a
prompt. At a round offset, "port 2 DIR" is `0xe000_1040` rather than
something to look up, and 4KB is left for whatever else wants to live
at the bottom of the nibble.

### Aliasing

`rtl/gpio.v` decodes `adr[10]` for the port region, `[5:3]` for the
port and `[2:0]` for the register, **and nothing above bit 10**. The
map therefore repeats every 8KB across the 256MB nibble: LED is also at
`0xe000_0020`, port 0 DIR is also at `0xe000_3000`.

**With stream engines built, bit 11 is decoded too**, and selects the
engine window: `0xe000_2000`-`0xe000_3fff` (engine in bits 5:4,
register in 3:0). The map then repeats every 16KB instead: port 0 DIR
is also at `0xe000_5000`, and `0xe000_3000` is engine 0 again. A build
without engines decodes exactly as before, and `tb_gpio.v` (no engines)
still asserts the 8KB aliasing.

**With the I2C slave built, bit 6 of that window is decoded as well**:
`0xe000_2100` and every address in the window with word-address bit 6
set is the slave (register in bits 3:0, so it repeats every 64 bytes
there); the engines keep the half with bit 6 clear. Without the slave,
bit 6 is ignored and engine E is also at `0xe000_2100 + E * 0x40`.

**With fast zlink built, bit 12 is decoded too** (byte address bit 14)
and selects its window, `0xe000_4000`-`0xe000_7fff`, register in bits
3:0 of the word address (so it repeats every 64 bytes there). The map
then repeats every 32KB: port 0 DIR is also at `0xe000_9000`. Only a
`ZLINK_FAST` build decodes the bit; every other build is as above.

That is a deliberate trade. Timing on this design is tight, and the
alternative is a comparator against the top fifteen bits of the word
address landing in the same cycle that already carries the ack mux. The
nibble is 256MB allocated to one small peripheral; there is nothing in
it to collide with. `rtl/tb/tb_gpio.v` asserts the aliasing, so if
somebody later tightens the decode they find out it was a documented
choice rather than an oversight.

## Pull-ups

**Every GPIO pin has a weak internal pull-up, and it cannot be changed
after the bitstream is built.** `PULLMODE` is an IOBUF attribute fixed
at place-and-route; `release/hw/pmods/gpio.spec` sets `PULLMODE=UP`.

This is not the toolchain default. nextpnr writes `PULLMODE=NONE` for a
constrained pin that doesn't say otherwise, so leaving it off would
mean genuinely floating pins. UP is a positive choice, for three
reasons:

**A floating LVCMOS input is not merely undefined.** It sits near the
switching threshold, it can oscillate, and the input buffer draws
crowbar current while it does. `rtl/gpio.v` resets every pin to input,
so from bitstream load until software configures something, all eight
pins are in exactly that state.

**High is the right idle level for most of what gets plugged in here.**
Idle for I2C, deasserted for an active-low SPI chip select, mark for a
UART TX. A module plugged into a machine that hasn't configured its
GPIO doesn't have its bus held down.

**It makes a bare I2C PMOD work.** The pull is specified as a current
(`I_PU` in FPGA-DS-02012) rather than a resistance, and works out to
tens of kΩ at 3V3 -- too weak for the 1µs rise time 100kHz I2C
requires. It doesn't matter, because the master here is bit-banged and
picks its own clock: at the 20-50kHz it will realistically reach, a
microsecond or two of RC rise is invisible against a 20-50µs bit
period.

What makes that safe rather than lucky is structural. `sw/common/zi2c.c`
has to poll SCL after releasing it anyway, because that is how clock
stretching works, and the same wait absorbs the RC rise; it does the
equivalent read-back on SDA before each SCL rising edge. **A weak
pull-up costs speed, not correctness** -- the bus gets slower, it
doesn't get wrong.

### What this costs you

Nothing you can't get back. Driving a pin low beats the pull-up
outright, and an external 10k pull-down against a pull this weak puts
the pin around 0.5V, inside `V_IL`. The only thing genuinely lost is a
wired-OR bus with external pull-downs and open-source drivers, which is
rare enough not to weigh against the rest. (Telling "nothing connected"
from "connected and high" was never possible with NONE either -- you'd
be reading noise.)

External resistors are still the right answer for anything past a short
cable or a couple of devices: 2.2k-4.7k to 3V3. This makes the bare
case work, not the general case fast.

### The one thing to check before plugging something in

**An active-high enable, reset or mode strap now sees a high** from the
moment configuration finishes -- and because ECP5 pins are tri-stated
*during* configuration, it sees a low-to-high edge at the end of it,
which an already-powered device may sample. A 10k pull-down on the
module wins comfortably. Knowing to look is the hard part.

### Do not switch to ECP5's OPENDRAIN IO_TYPE

FPGA-TN-02032 is explicit that configuring an output as OPENDRAIN
forces `PULLMODE` to NONE. Open drain here is done in the fabric
instead (see below), which keeps the pull-up. Switching to the hardware
mode would silently turn off the pull-ups for exactly the case they
were enabled for.

## Open drain, and why DIRSET/DIRCLR exist

The set/clear aliases are not convenience. With only DIR and OUT,
moving one pin while another holds its state is a bus read, an ALU op
and a bus write -- three times the traffic on the hot path of every bit
of every byte, and not atomic against the KTIMER interrupt.

More to the point, I2C is open-drain, which this block has no hardware
for and doesn't need:

1. Write 0 to OUT once, for the SDA and SCL bits. Leave it there.
2. To release the line, write its bit to **DIRCLR**. The pin floats and
   the pull-up makes it a 1.
3. To pull the line low, write its bit to **DIRSET**. The pin drives 0.

One store per edge. And it is structurally impossible to drive a 1 into
a line another device is pulling down, because the register that would
have to hold that 1 is never written to anything but 0 -- which is the
failure that ends with a hot part rather than a wrong reading.

`rtl/gpio.v` resets OUT to 0 as well as DIR, so a caller that only ever
touches DIRSET/DIRCLR never has to remember to clear OUT first. It was
already clear.

## Reset behaviour

Every pin is an input at reset, on every port. This is the only
defensible power-on state for a connector going to somebody else's
hardware: the FPGA has no idea what is on the other end of a PMOD, and
driving a pin that turns out to be an output on the module means two
drivers fighting from the moment the bitstream loads until software
gets around to saying otherwise.

With the pull-ups above, "input" means "pulled high" rather than
"floating", which is a better-defined state than the design originally
had and is the correct idle level for the buses this port will mostly
carry.

## Stream engines

A stream engine is one serial peripheral that can be put on **any pin
of any port**, in one of four modes chosen at runtime:

| mode | | speed | pins |
|---|---|---|---|
| 1 zlink | 8b/10b, self-clocked, 4x oversampled ([zlink.md](zlink.md)) | up to 12 Mbit/s | TX and/or RX |
| 2 SPI | master, full duplex, modes 0-3, MSB or LSB first | SCK up to 12 MHz | SCK, MOSI and/or MISO, CS |
| 3 UART | 8N1 or 8N2 | 300 baud to 3 Mbaud | TX and/or RX |
| 4 raw | bits out and/or in, one per tick; TX can loop | up to 48 Mbit/s | TX and/or RX |

It is the answer to the paragraph at the top of this page for the cases
where software is not enough: a link to another machine
([zlink.md](zlink.md)), an SPI display or flash at megabits rather than
the ~60 KB/s [spi.md](spi.md) manages, a second serial port on any
board, MIDI at 31250 baud, or a fixed bit pattern (WS2812 LEDs, a
test signal) at fabric speed. It is a fixed-function cousin of the
RP2040's PIO: the same FIFOs, clock divider and any-pin placement, a
handful of fixed modes instead of a program.

Hardware: `rtl/gpio_stream.v`, instantiated by `rtl/gpio.v`. Software:
`sw/common/zgpio_stream.h`.

### How many, and which modes

`rtl/boards.vh` gives every board that builds a GPIO port **one engine
with every mode**, unless its block says otherwise:

| define | effect |
|---|---|
| `` `define GPIO_STREAM_ENGINES 0 `` | no engine (Lakritz, which is out of LUTs) |
| `` `define GPIO_STREAM_ENGINES 2 `` | two, up to 4 |
| `` `define GPIO_STREAM_NO_SPI `` | drop SPI mode (also `_NO_UART`, `_NO_RAW`) |

A board with no GPIO port gets no engine whatever it says: there is
nowhere for its pins to go. zlink is in every engine; it is what they
are for.

### Pins

Four **roles**, each placed on any pin of any port by `SPINS`:

| role | | byte of SPINS |
|---|---|---|
| 0 | TX, or SPI MOSI | 7:0 |
| 1 | RX, or SPI MISO | 15:8 |
| 2 | SPI SCK (driven low in other modes) | 23:16 |
| 3 | CS: drives SCTL's CS level, in any mode | 31:24 |

Each byte is `{enable, 1'b0, port[2:0], pin[2:0]}`.

**While MODE is not 0, an enabled role owns its pin.** The port's DIR
and OUT stop reaching it -- they still read and write, they just do not
drive -- and the engine sets the pin's direction and level. IN still
reads the pin. Every other pin of the port is ordinary GPIO. Setting
MODE back to 0 hands the pins back.

Any pin, because nothing in the engine uses a pin-specific FPGA
primitive: it samples `gpio.v`'s already-synchronised inputs at 48 MHz
and does everything else in fabric. That is also its speed limit.

If two roles name one pin, TX wins, then SCK, then CS, then RX. Two
*engines* on one pin are ORed. Both are configuration errors, which
`z_gs_config()` refuses rather than the hardware spending logic on.

### Registers, `0xe000_2000 + E * 0x40`

| off | name | | |
|---|---|---|---|
| `+0x00` | SCTL | r/w | mode and flags, below |
| `+0x04` | SPINS | r/w | the four roles |
| `+0x08` | SRATE | r/w | bits 15:0: the divider. tick = 48 MHz / (SRATE + 1) |
| `+0x0c` | SSTAT | r | below |
| `+0x10` | STX | w | push one entry: bit 8 is the zlink K flag, bits 7:0 the byte |
| `+0x14` | STX4 | w | push four data bytes, bits 7:0 first |
| `+0x18` | SRX | r | pop one entry: bit 9 valid, bit 8 K, bits 7:0 the byte. 0 if empty |
| `+0x1c` | SRX4 | r | pop four data bytes, the first in bits 7:0 -- **only if four are waiting**; otherwise 0, and nothing is popped |
| `+0x20` | SERR | r | bits 15:0: errors (zlink code errors and misplaced commas, UART framing), saturating |
| `+0x24` | SFLUSH | w | bit 0 empty TX, bit 1 empty RX, bit 2 clear SERR and the sticky flags |
| `+0x28` | SLOCK | r/w | read: the lock, and take it (test-and-set). Write 0: release |

**SCTL**

| bits | name | |
|---|---|---|
| 2:0 | MODE | 0 off, 1 zlink, 2 SPI, 3 UART, 4 raw. A mode this build lacks reads back as 0 |
| 3 | LSB | SPI, raw: least significant bit first (zlink and UART always are) |
| 4 | CPHA | SPI |
| 5 | CPOL | SPI |
| 6 | TXINV | invert the TX / MOSI pin |
| 7 | RXINV | invert the RX / MISO pin |
| 8 | TXOD | TX open drain: drives low, releases for a 1 (1-Wire, wired-AND) |
| 9 | RXEN | keep what is received. Without it: SPI is write-only, the others ignore their input |
| 10 | LOOP | TX straight to RX inside the engine, for testing |
| 11 | REPEAT | raw: loop the TX FIFO, below |
| 12 | TRIG | raw: start capturing at the first edge on RX |
| 13 | STOP2 | UART: two stop bits |
| 14 | CS | the level driven on the CS pin |
| 15 | IDLE | raw: the TX level when there is nothing to send |
| 18:16 | SPIDLY | SPI: MISO sample delay, in clocks, below |

**Writing a different MODE resets the engine** -- the shift registers,
the receiver's alignment, the divider -- but not the FIFOs. Writing
SCTL with the same MODE changes the flags without a reset, which is
how CS and the inversions move mid-stream.

**SSTAT**

| bits | |
|---|---|
| 10:0 | TX level, 0-1024 |
| 11 | busy: something still to send, or being sent |
| 12 | RX overrun, sticky: a byte arrived with the RX FIFO full and was dropped |
| 13 | TX overflow, sticky: a push into a full TX FIFO was dropped |
| 14 | zlink: the receiver is aligned to the comma |
| 15 | sticky: an SRX4 popped a K symbol (zlink data and K should be read with SRX) |
| 26:16 | RX level, 0-1024 |

### The FIFOs

1024 entries each way, in **one** DP16KD (2048x9). The CPU and the
engine share the RAM's one write port and one read port, the engine
first; it needs a port at most once every 8 clocks, so the CPU waits a
clock at most, and rarely.

A true dual-port RAM -- each side with its own port -- would look more
natural, and yosys cannot build one from it: given two processes
writing one array it keeps a write priority between them that a
DP16KD has no way to provide, and quietly falls back to 18,432
flip-flops. The first full build of this engine came out at 333% of a
25F. The single-port arrangement is why it is one block RAM.

### Rates

Every mode runs off the 48 MHz bus clock divided by `SRATE + 1`:

| mode | clocks per bit | rate | examples |
|---|---|---|---|
| zlink | 4 (x4 oversampling) | 12 MHz / (SRATE + 1) | 0: 12 Mbit/s, 3: 3 Mbit/s |
| UART | 4 | 12 MHz / (SRATE + 1) baud | 103: 115384 (+0.16%), 383: 31250 exact, 3: 3 Mbaud |
| SPI | 2 (one tick per SCK edge) | SCK = 24 MHz / (SRATE + 1) | 1: 12 MHz, 2: 8 MHz, 5: 4 MHz |
| raw | 1 | 48 MHz / (SRATE + 1) | 0: 48 Mbit/s, 19: 2.4 Mbit/s (WS2812, 3 bits per LED bit) |

`z_gs_div_for()` works these out.

### SPI timing

MOSI and SCK leave registered on the same clock. MISO comes back
through `gpio.v`'s two-flop synchroniser, so the engine reads it
SPIDLY clocks after a strobe that marks the sampling edge; with
SPIDLY = 0 that is the pin **as it was just before the edge**, which is
what a device expects. A larger SPIDLY samples later, which helps a
slow device or a long wire, up to half an SCK period.

12 MHz works in simulation against a device model with a 6 ns
clock-to-out. On real wires treat it as "if it works"; 8 MHz (SRATE 2)
is the comfortable setting.

The engine will not start a byte unless the RX FIFO has room for its
answer, so a full-duplex transfer cannot overrun: if software falls
behind, SCK simply stops between bytes.

### UART

LSB first, a start bit, eight data bits, one or two stop bits. The
receiver finds the start bit on a low sample, checks it again at its
middle (a glitch shorter than half a bit is ignored), then samples
every 4 ticks. A low stop bit counts a framing error in SERR and drops
the byte.

### Raw

TX shifts one bit out per tick; RX shifts one bit in per tick and
pushes a byte every 8. With TRIG set, RX waits for the first edge on
its pin and counts that sample as bit 0 -- so a TX that idles high and
starts with a 0 bit lines up byte for byte on the far side.

**REPEAT** loops the TX FIFO: when it runs dry, the engine starts again
from where the loop began. The loop begins where the FIFO's read pointer
was **when MODE was written**, so load the pattern with the engine off
(MODE 0) and then turn it on with REPEAT set. Turning it on first and
loading afterwards races the engine, which will loop on whatever part
of the pattern has arrived.

### Sharing engines between processes

SLOCK is a test-and-set: a read returns it and sets it, in one bus
cycle, so two processes cannot both claim one engine. Nothing in the
kernel knows about it -- a process that exits without releasing leaves
the engine claimed and, possibly, driving its pins. `z_gs_force_release()`
is there for a shell command to clean up after it.

### From C

```c
#include "../../common/zgpio_stream.h"

int e = z_gs_claim(Z_GS_ANY);          // -1: none built, or none free
z_gs_cfg_t c = Z_GS_CFG_INIT;
c.mode = Z_GS_UART;
c.tx = Z_GS_PIN(0, 0);                  // port 0, pin 0
c.rx = Z_GS_PIN(0, 1);
c.flags = Z_GS_RXEN;
c.div = z_gs_div_for(Z_GS_UART, 31250, NULL);   // MIDI
if (z_gs_config(e, &c) == Z_GS_OK) {
	z_gs_write(e, buf, n);              // queues what fits
	n = z_gs_read(e, in, sizeof in);    // takes what has arrived
}
z_gs_release(e);
```

Pin numbers are `port * 8 + pin` throughout (`Z_GS_PIN()`), -1 for a
role not used. `z_gs_config()` refuses pins the board does not have,
two roles on one pin, a pin another engine is using, and a mode the
bitstream lacks, each with its own error code. `z_gs_write()` and
`z_gs_read()` never wait; `z_gs_spi_xfer()` does, for any length, and
keeps both FIFOs moving. In zlink mode `z_gs_read()` stops in front of
a control symbol and `z_gs_read_sym()` returns it.

The library is standalone -- it does not link `zgpio.c` -- and adds
about 4 KB to an app that uses all of it (clang -Os, RV32IM; section GC
drops what is not called).

### Cost

One engine, all four modes. Place-and-route with and without the
engine (`GPIO_STREAM_ENGINES` 1 and 0) on the same target, Yosys 0.69
and nextpnr-ecp5 0.11.1 (YoWASP), default seed, one run each:

| target | device | without | with one engine | engine |
|---|---|---|---|---|
| `obst_langkatze_gpio` | 12F (25F die) | 20,821 COMB (86%), 8,990 FF, 32 DP16KD | 21,781 (90%), 9,445 FF, 33 DP16KD | **+960 COMB**, +455 FF, +1 DP16KD |
| `minze_gpio` | 25F | 20,945 (86%), 9,665 FF, 38 DP16KD | 22,335 (92%), 10,121 FF, 39 DP16KD | +1,390 COMB, +456 FF, +1 DP16KD |
| `schoko_langkatze_gpio` | 45F | not measured | 25,608 / 43,848 (58%), 11,664 FF, 41 DP16KD | |
| `lakritz_gpio` | 25F | 23,361 (96%), 10,651 FF, 38 DP16KD | -- (no engine: `GPIO_STREAM_ENGINES 0`) | |

The same engine costs 960 cells on one board and 1,390 on another:
that spread is nextpnr's packing, not the engine. Synthesised on its
own with eight pins (yosys `synth_ecp5`) it is 924 LUT4, 67 CCU2C,
434 FF and one DP16KD. The optional modes are cheap -- SPI about 40
LUT4, UART and raw close to nothing -- so `GPIO_STREAM_NO_*` saves
little; the cost is the core (FIFO pointers, the bus side, the
divider, zlink's receiver). It is about twice what was estimated
before it was built, and the place to look first if a board needs it
smaller.

## I2C slave

`rtl/gpio_i2cs.v`. A real I2C master -- a Zwölf module's LS10,
bit-banging its C/D bus -- talks to chips that exist only in software,
`bench`'s virtual parts ([bench.md](bench.md)), with **no software in
the transaction**. Each of up to four **slots** is one emulated chip:
an I2C address, a 256-register image (the **mirror**), a register
pointer and an auto-increment rule. The hardware answers the master
from the mirror; software keeps the mirror current and applies the
side effects of what the master wrote, from a log. This is "mirror
mode". There is no mode where software answers byte by byte: that
needs clock stretching, which the LS10 does not tolerate (below).

**Off by default, everywhere.** It is for one job, so a build asks for
it: `` `define GPIO_I2CS_SLOTS 4 `` in a board block, or

```
make BOARD=obst_langkatze_gpio EXTRA_DEFINES=-DGPIO_I2CS_SLOTS=4
```

1, 2 or 4 slots -- one block RAM whatever the number. Not on Lakritz,
which has no logic cells to spare ([boards.md](boards.md)).

### What the master sees

| the master does | the slave |
|---|---|
| a slot's address (write or read) | ACKs |
| any other address | does not ACK |
| a bare probe: the address, then stop | ACKs if it is a slot's |
| write: the first byte after the address | sets the slot's pointer |
| write: each byte after that | stores it at the pointer -- if that register is writable -- logs it, advances the pointer, ACKs |
| a byte for a register that is not writable | ACKs it, leaves the mirror alone, logs it (a TCA9535's input port does the same) |
| read | sends from the pointer, advancing after each byte, until the master NACKs |
| write the pointer, repeated start, read | reads from the pointer just set |
| a read with no pointer written first | starts where the last transaction left the pointer: it **persists**, per slot |

Auto-increment, per slot: **none** (the pointer stays), **linear** (+1,
wrapping at 255), or **pair toggle** (XOR 1 -- a TCA9535/9555 inside
its register pairs).

**It never stretches the clock.** SCL is an input only; SDA is pulled
low or released, never driven high. The LS10 waits about 1 ms for a
stretched clock and then carries on as though it were high, so
stretching would corrupt a transaction rather than slow it. No 10-bit
addresses, no general call.

**Timing.** Both lines pass gpio.v's synchroniser and a glitch filter
(12 clocks, 250 ns), so the LS10's ~40 kHz and 400 kHz fast mode are
both easy. A master may move SDA the instant after SCL falls -- I2C
allows a hold time of 0 ns -- and the slave bridges that, as the I2C
specification requires: a start or stop counts only while SCL is still
high at the pin. SDA changes only after the slave has seen SCL fall,
which gives the master a filter time of hold.

### What software has to allow for

- **Derived registers are a loop behind.** A register that depends on
  others -- a TCA's input port showing what its outputs drive -- is only
  as current as the last time software rewrote it. A master that writes
  the outputs and immediately reads the input port gets the old value
  until bench has recomputed it (one bench loop, 10-20 ms). Rare in
  practice: `PANEL.BAS` only reads port 1's inputs.
- **Multi-byte reads are consistent byte by byte only.** If software
  updates two registers while the master is reading them, the master can
  get one old byte and one new. Enough for buttons and LEDs.
- **The log holds 512 writes.** Past that, writes still reach the
  mirror but are not logged, and ISTAT's overflow flag says so.
- **Parts with more than 256 registers do not fit a slot** (a 24C64
  EEPROM's 16-bit address). A two-byte pointer would be the way to add
  them, if one is ever wanted.

### Pins

SCL and SDA are any two pins of any port (ICTL). While the slave is
enabled both are **owned**, as an engine owns its pins: DIR and OUT
stop reaching them, so `zgpio`'s read-modify-write on the other pins of
the port -- zi2c bit-banging pins 1 and 2, say -- cannot disturb the
bus. IN still reads them. `zi2cs.c` and `zgpio_stream.c` each refuse a
pin the other has.

The pins need pull-ups, as any I2C bus does; on a Zwölf module's bus
the module's own provide them.

### Registers, `0xe000_2100`

| offset | name | access | |
|---|---|---|---|
| `+0x00` | ICTL | r/w | `[0]` enable, `[13:8]` SCL pin, `[21:16]` SDA pin (port * 8 + pin) |
| `+0x04` | ISTAT | r, w1c | `[9:0]` log level, `[12]` log overflow, `[13]` busy (a transaction to one of our slots), `[19:16]` read flags per slot, `[20]` live (enabled on valid pins). Write 1 to bit 12 or 16-19 to clear it |
| `+0x08` | IMADDR | r/w | `[9:0]` `{slot, reg}`: the mirror register IMDATA reaches |
| `+0x0c` | IMDATA | r/w | `[7:0]` the register, `[8]` writable by the master. IMADDR advances after each access |
| `+0x10`..`+0x1c` | ISLOT0-3 | r/w | `[6:0]` address, `[7]` valid, `[9:8]` auto-increment (0 none, 1 linear, 2 pair toggle), `[15]` on a write: set the pointer to `[31:24]`. Reads: `[23:16]` the register the latest read started at, `[31:24]` the pointer |
| `+0x20` | ILOG | r | pop the oldest write: `[31]` valid, `[25:24]` slot, `[15:8]` register, `[7:0]` data. 0 when empty |
| `+0x24` | IFLUSH | w | `[0]` empty the log and clear the overflow flag |

A slot past the number built reads 0 and ignores writes. Two valid
slots with one address: the lower slot answers.

**Reads are not logged** -- only flagged. A slot's read flag goes up
when a read on it begins, and ISLOT keeps the register it began at:
enough to release a TCA's INT after its input port is read, without
spending the log on every byte read.

### The block RAM

One DP16KD in 2048x9 mode: the mirror in the lower half (`{slot, reg}`,
each a byte and its writable bit), the log in the upper half (512
entries of two words). One write port and one read port, shared by the
I2C side and the CPU, I2C first; a job takes at most four clocks and
I2C moves in microseconds, so neither side waits long enough to notice.

### From C

```c
#include "../../common/zi2cs.h"

if (z_i2cs_open(Z_I2CS_PIN(0, 2), Z_I2CS_PIN(0, 3)) != Z_I2CS_OK) return;
z_i2cs_map(0, 0x20, Z_I2CS_INC_PAIR);              // a TCA9535
for (int r = 0; r < 8; r++)
    z_i2cs_mirror_write(0, r, regs[r], r >= 2);    // 0-1: input port, read only

for (;;) {                                         // every bench loop
    z_i2cs_event_t ev;
    while (z_i2cs_log_pop(&ev))
        part_i2c_write(ev.slot, ev.reg, ev.data);  // the part's own code
    if (z_i2cs_reads(true) & 1)                    // slot 0 was read from
        release_int_if(z_i2cs_read_start(0) <= 1);
    ...rewrite the mirror registers whose values changed...
}
```

`z_i2cs_slots()` is 0 on a bitstream without the slave. The slave is
one block for the machine: `z_i2cs_open()` refuses with
`Z_I2CS_EBUSY` when it is already enabled, and like an engine it stays
on if its process exits without `z_i2cs_close()`.

### Interrupt

None, deliberately: bench loops every 10-20 ms anyway and polls ISTAT
and the log, which needs no kernel work to route an interrupt to a
user process. One could be added behind a parameter if something ever
needs to react faster than a loop.

### Cost

Place-and-route with and without it on the same target (Yosys 0.69,
nextpnr-ecp5 0.11.1 via YoWASP, default seed, one run each, today's
tree with one stream engine):

| target | without | with four slots | the slave |
|---|---|---|---|
| `obst_langkatze_gpio` (12F, 25F die) | 21,114 COMB (87%), 9,539 FF, 34 DP16KD | 22,114 (91%), 9,883 FF, 35 DP16KD | **+1,000 COMB**, +344 FF, +1 DP16KD |

48 MHz still closes (58.3 MHz). Synthesised on its own with eight pins
it is 774 LUT4, 24 CCU2C, 340 FF and one DP16KD; one slot instead of
four saves only about 300 of those, since most of the cost is the
protocol and the CPU's side, not the slots.

That is **five to eight times the 120-180 estimated** before it was
built. Taking pieces out one at a time showed no single culprit: the
protocol, the CPU's register reads and the per-slot state each cost a
few hundred cells. A pass to trim it -- the slots' addresses and
pointers into the block RAM, a narrower read path -- is possible if a
board that wants it is short of room; on a 25F board with an engine it
fits (91% on Obst; Minze, at 92% with its engine, was not measured).


## Fast zlink

On a Mozart (ML0/ML1/ML2) built with `ZLINK_FAST` -- off by default,
experimental -- the DS port's two LVDS pairs carry zlink at 100, 50 or
25 Mbit/s ([zlink.md](zlink.md#fast-round-4-done-experimental)). Mozart
has no GPIO port, but its registers live in this block's address
window: adding another slave to the system bus's read mux costs about
800 LUT4 on these boards, decoding one more address bit here costs
almost nothing. `gpio.v` decodes `0xe000_4000` and hands the access to
`rtl/zfast.v`, which sits in `sysctl.v` next to the pads and the
200 MHz clock. CONFIG bit 15 says it is there.

### Registers, `0xe000_4000`

Laid out like a stream engine's, so the C is the same shape
(`sw/common/zfast.h`):

| off | name | | |
|---|---|---|---|
| `+0x00` | FCTL | r/w | `[0]` EN, `[2:1]` TXSEL (0 none, 1 DS1, 2 DS2), `[3]` RXSEL (0 DS1, 1 DS2), `[4]` RXEN, `[5]` LOOP (TX to RX inside the block), `[7:6]` RATE (0 100, 1 50, 2 25 Mbit/s), `[8]` TXINV, `[9]` RXINV. Reads `[16]`: DS2 can transmit (0 on an ML2, where TXSEL 2 is refused) |
| `+0x0c` | FSTAT | r | `[10:0]` TX level, `[11]` busy, `[12]` RX overrun, `[13]` TX overflow, `[14]` aligned, `[15]` a K symbol was popped by FRX4, `[26:16]` RX level |
| `+0x10` | FTX | w | push one entry: `[8]` K, `[7:0]` byte |
| `+0x14` | FTX4 | w | push four data bytes, byte 0 in `[7:0]` first |
| `+0x18` | FRX | r | pop one: `[9]` valid, `[8]` K, `[7:0]` byte; 0 when empty |
| `+0x1c` | FRX4 | r | pop four data bytes, only when four are waiting, else 0 |
| `+0x20` | FERR | r | `[15:0]` code errors and misplaced commas, saturating |
| `+0x24` | FFLUSH | w | `[0]` TX FIFO, `[1]` RX FIFO, `[2]` sticky flags and FERR |
| `+0x28` | FLOCK | r/w | test-and-set: reading takes it, 0 back means it was free; write 0 to give it back |

FIFOs of 1024 entries each way, one block RAM each. A change to FCTL
other than RXEN restarts the line side (not the FIFOs). While pll0 is
not locked the line side is held in reset.

### Cost

PnR with and without it is in [zlink.md](zlink.md#fast-round-4-done-experimental).

## Building it

### Which targets have it

| target | GPIO | notes |
|---|---|---|
| `lakritz_gpio` | port 0 on PMOD A | no ethernet -- `` -SPI_ETH `` |
| `obst_langkatze_gpio` | port 0 on PMOD B | none; ethernet on PMOD A |
| `schoko_langkatze_gpio` | port 0 on PMOD B | none; ethernet on PMOD A |
| `minze_gpio` | port 0 on PMOD A | none; Minze has no ethernet |
| `sergei_ml1_gpio` | port 0, **4 pins**, on PMOD A | no audio at all -- `` -AUDIO_SPDIF `` |

```
./release/zrelease build lakritz_gpio
./release/zrelease build obst_langkatze_gpio
./release/zrelease build schoko_langkatze_gpio
./release/zrelease build minze_gpio
```

Target names list PMODs in port order: `obst_langkatze_gpio` is a
Langkatze in PMOD A and GPIO in PMOD B. (Before the Schoko port it was
the reverse, which made the name read backwards; a Langkatze moving
from B to A is a re-plug, nothing more.)

Neither is a plain `make BOARD=x` build, and that is not an oversight.
Both boards' PMOD connectors are already spoken for, so GPIO is a
**swap**, not an addition -- and removing a define is precisely what an
additive `-D` on the yosys command line cannot express. That is what
the release spec layer is for; see `rtl/boards.vh`'s ZSPEC note.

- **Lakritz has one PMOD connector**, and `boards/lakritz_v0.lpf` puts
  `UART0_TX`/`RX` on B12/B13, which are PMOD_A2 and PMOD_A3. GPIO takes
  the whole connector, so the console goes. `rtl/uart_null.v` answers
  the console window (an unanswered read there would hang the BIOS on
  the first character), and the machine comes up on HDMI with a USB
  keyboard, with `repl` in a `term` window as the shell.
- **Obst has two**, and its console is on the USB-C socket, so both
  are free: the Langkatze ethernet PMOD on A and GPIO on B. (A plain
  `make BOARD=obst` build still constrains the Langkatze on B, as
  `boards/obst_v0.lpf` always has; the release target moves it.)
- **Schoko has two** and the same USB-C console, and is laid out the
  same way: Langkatze on A, GPIO on B. Here the board's own `.lpf` and
  `rtl/boards.vh` block already describe that, so a plain
  `make BOARD=schoko` build has GPIO too.
- **Minze has one**, with the same USB-C console, so it is free for
  GPIO. As on Schoko, the board's own `.lpf` and block already put
  port 0 there.

For a hand build, `rtl/boards.vh` carries a commented `` `GPIO_PORT0 ``
in each board block explaining what else has to change, and the base
`.lpf` files carry matching commented constraint blocks. The release
build generates its own constraints and ignores those.

### Ports with fewer than eight pins

Sergei's PMOD connector has six pins, four of them signals. A port
there is declared **four bits wide** (`` `GPIO_PORT0_NARROW `` alongside
`` `GPIO_PORT0 ``), and `release/hw/pmods/gpio4.spec` maps them.

The pins that don't exist are not in the port at all, and that is the
only thing that works: **an unconstrained top-level IO is a hard
`nextpnr-ecp5` error.** The flag that suppresses it (`--lpf-allow-
unconstrained`, which the ice40 path passes and the ECP5 path
deliberately does not) would place the missing pins on whatever balls
are free — and "free in this bitstream" includes balls wired to the
SDRAM, the PHY and the SD card on a populated board.

**Software still sees an eight-bit port.** DIR and OUT bits 4–7 exist
and drive nothing. IN bits 4–7 read **0**, where a real floating pin
reads 1 because of the pull-up. Nothing reports the difference, so on
`sergei_ml1_gpio` the `logic` app shows four channels sitting flat at zero
and the shell will happily accept `gpio 0 6 out`.

That asymmetry is a deliberate scope choice rather than an oversight. A
per-port pin-count register — two lines in `gpio.v`'s CONFIG word plus
a `z_gpio_port_pins()` and a change to every consumer — would be
honest, and it is machinery carried by every board to describe one
connector on one board. If a second narrow board ever appears, that is
the point to add it.

**On Sergei, pin 1 is output-only.** A13 reaches the connector through
the optical transmitter's series resistor, and the transmitter stays on
the net whether or not `` `AUDIO_SPDIF `` is built — removing the define
frees the FPGA pin, not the board wiring. Bit 0 therefore **always
reads 0**: the load beats the ~50kΩ internal pull-up, so the pin cannot
go high and the pull-up cannot help whatever you connect there. As an
output it drives fine into a high-impedance input, and lights the
optical LED while it does.

Measured, not assumed: with all four pins as inputs and nothing
attached the port reads `14` (`0b1110`), and jumpering each pin to
ground clears exactly its own bit — except P1, which was already there.
So `sergei_ml1_gpio` gives **three usable pins plus one output-only pin**.
Enough for I2C with a spare if SCL and SDA go on bits 1–3; not enough
for SPI with a chip select. Open drain needs a pin that can read back,
which bit 0 cannot do.

**Pin 1 is also the optical S/PDIF output** on Sergei (ball A13), so
`sergei_ml1_gpio` removes `` `AUDIO_SPDIF ``. That board has no other audio
output, so it is genuinely optical audio *or* four GPIO pins — and with
`` `AUDIO `` and `` `AUDIO_MIXER `` still defined, `sw/apps/play` will
go on thinking it is playing while the samples go nowhere. Worth
knowing before flashing it over a working machine.

Keeping S/PDIF and taking only pins 2–4 isn't offered: those would be
bits 1–3 with a hole at the bottom, which is a different shape from
"short" and is exactly where the pin mask above stops being avoidable.

### Adding a port

`` `GPIO_PORT0 `` through `` `GPIO_PORT3 ``, one define per port.

Not a single `` `GPIO_PORTS 3 ``, because the preprocessor cannot
compare numbers -- one count could not gate three port declarations and
not a fourth. So the defines *are* the count: `sysctl.v`'s
`GPIO_NPORTS` is derived by summing them, which means there is one
place to edit and no way for a count and a pin declaration to disagree.

Define them **in order**. The sum makes a gap (`GPIO_PORT0` and
`GPIO_PORT2` but not `GPIO_PORT1`) come out as 2, which is wrong and
deliberately so: `gpio.v` numbers ports densely from 0, so a gap is a
board description error and a count that doesn't match the pins is how
it gets noticed.

Four ports are declared, eight are reserved in the register map (an
ECPIX-5 has that many PMOD connectors). Adding ports 4-7 is the port
declaration block and the tri-state block in `sysctl.v` and nothing
else -- `gpio.v` already handles them.

A second port in a release target needs a second PMOD spec naming
`GPIO1[0..7]` with `+GPIO_PORT1`: copy `release/hw/pmods/gpio.spec`,
change those two things, plug it into the other port.

### Cost

Measured with yosys 0.33 for ECP5, in context (unused lanes tied off):

| | LUT4 | FF |
|---|---|---|
| no ports | 19 | 22 |
| one port | 151 | 53 |

Plus 8 tri-state buffers per port at the top level, the same way
`SRAM_D` already works.

Zero ports is essentially the old `debug.v`, so a board that doesn't
build GPIO pays nothing for the block existing. The register file in
`gpio.v` is written for eight ports, but every write is gated on
`pidx < NPORTS`, which is a comparison against a parameter and
therefore constant-folded per index -- so unbuilt ports have no driver
but reset, synthesis proves them constant, and they disappear along
with the read mux entries and synchroniser flops that feed them.
`NPORTS` is the only thing that decides the cost. That matters on Obst,
which is an ECP5 12F.

## Detecting it from software

Two questions, two answers, in this order:

1. **Is there GPIO at all?** `Z_FEATURE2_GPIO` in `FEATURES2`
   (`rtl/csrs.v` word 3), via `z_soc_has_feature2()`. Go through the
   helper: a bitstream that predates the register answers with a clean
   zero that looks exactly like "no GPIO", and the signature check in
   `z_soc_features2_present()` is the only thing that separates them.
   See `docs/csrs.md`.
2. **How many ports?** `gpio.v`'s own CONFIG register. The feature bit
   says "at least one"; this says how many.

The feature bit is set when at least one port has **pins**, not when
the block exists -- it always exists, so a bit meaning "instantiated"
would be a constant 1 and would tell nobody anything.

Reading MAGIC first is safe on any bitstream ever built, because the
`0xE` nibble has always been decoded and acked. That is not true of
`rtl/rtc.v` or `rtl/trng.v`, whose windows are undecoded on older
builds and whose magic reads can hang -- so the "check the feature bit
before touching the block" rule those have does not apply here for
safety reasons. Check it anyway for the port question.

## The C API

`sw/common/zgpio.h` / `.c`. Link `zgpio.c`; section GC drops whatever
you don't call, so the parts you don't use cost nothing.

```c
#include "../../common/zgpio.h"

if (!z_gpio_present()) return;          // no GPIO in this bitstream
uint32_t n = z_gpio_port_count();       // how many ports have pins

z_gpio_mode(0, 3, Z_GPIO_OUT);          // port 0 pin 3 -> output
z_gpio_write(0, 3, true);               // drive it high
bool v = z_gpio_read(0, 2);             // read the PIN, not OUT

z_gpio_dir_set(0, 0x0f);                // low nibble out, high nibble in
uint8_t pins = z_gpio_in_get(0);        // all eight at once
```

| | |
|---|---|
| `z_gpio_present()` | GPIO with at least one port that has pins |
| `z_gpio_port_count()` | how many, 0 if none |
| `z_gpio_dir_get/set(port[, mask])` | whole port, 1 = output |
| `z_gpio_out_get/put(port[, val])` | whole port |
| `z_gpio_in_get(port)` | whole port, the pins |
| `z_gpio_mode(port, pin, mode)` | `Z_GPIO_IN` / `_OUT` / `_OD` |
| `z_gpio_mode_get(port, pin)` | `_IN` or `_OUT` -- never `_OD` |
| `z_gpio_read(port, pin)` | the pin |
| `z_gpio_write(port, pin, v)` | drive it, one store |
| `z_gpio_toggle(port, pin)` | flip OUT |
| `z_gpio_od_write(port, pin, v)` | open drain: **false drives low** |
| `z_led_set(on)` / `z_led_bar_set(bits)` | the board LEDs |

### Four things worth knowing before using it

**`z_gpio_write()` is one store.** It goes to OUTSET or OUTCLR, never
read-modify-write, so there is nothing a timer interrupt can land in
the middle of. `z_gpio_od_write()` likewise, to DIRSET or DIRCLR.
`z_gpio_toggle()` is the one exception -- it has to read OUT first --
and it reads OUT rather than the pin, because an output that flipped
to agree with something fighting it is not what anyone means by
"toggle".

**`Z_GPIO_OD` is a software mode with nowhere to live.** Setting it
clears the pin's OUT bit and floats the pin; after that
`z_gpio_od_write()` moves DIR. The hardware has no bit recording that a
pin is "open drain", so `z_gpio_mode_get()` reports `_IN` or `_OUT`
depending on whether the pin happens to be driving right now. Callers
that need to remember keep their own record -- `zi2c` holds its pins in
a struct and never asks.

**`z_gpio_od_write()` is inverted relative to `z_gpio_write()`.**
`false` drives the line low; `true` releases it. That is what open
drain means, and the alternative -- making them agree -- would mean a
function whose name says "open drain" and whose argument doesn't.

**Nothing validates against the hardware.** `z_gpio_write(3, 0, true)`
on a one-port board writes a register that doesn't exist and
`rtl/gpio.v` drops it: no fault, no error, no way to find out
afterwards. That's a property of this bus, not an oversight, and it's
why `z_gpio_port_count()` exists -- ask before you write. The bounds
check that *is* in `zgpio.c` is against `Z_GPIO_MAX_PORTS` (8), and
only stops a wild index landing on another peripheral.

### There is no arbitration

These are plain MMIO accesses at a fixed physical address, same as
`reg_sdcard` and the `gpu_*` registers (`docs/app_runtime.md`). Two
processes writing the same port will interleave and nothing detects
it.

That's the deal the SD card and the ethernet controller already have,
and it's
fine for the same reason: one process owns the hardware by convention.
If that stops being enough, the answer is a server process that owns
the pins and takes messages, not a lock in `zgpio.c`.

## From the console shell

`sh`'s `gpio` command, for bring-up:

```
> gpio
1 gpio port
 0: dir 00 out 00 in ff
usage: gpio <port> <pin> [in|out|od|0|1]  e.g. gpio 0 3 out

> gpio 0 3 out
> gpio 0 3 1
> gpio 0 3
0.3 = 1 (output)
```

`in ff` with nothing connected is the pull-ups, and is what a working
port looks like at rest.

This is a slightly odd home for it, since `lakritz_gpio` has no console
at all. It's here for `obst_langkatze_gpio` and for bring-up: the first
thing anyone does with a new board and a new PMOD is wiggle a pin and
look at it with a meter, and that shouldn't need the window manager, an
app loader and a working SD card to have come up first. From a running
desktop, use the Scheme API instead.

`gpio <port> <pin> 0|1` always drives OUT, even on a pin set to `od` --
the command has no way to know, since the mode isn't stored anywhere.
Open-drain bit-banging belongs in a library, not at a prompt.

Arguments are parsed strictly rather than with `atoi()`, because
`atoi("out")` is 0 -- so `gpio 0 out` would otherwise be read as port
0, pin 0 and drive a pin nobody named.

## From Scheme

`repl`, via `sw/common/zapi/zapi.c`. See `docs/scheme_api.md` for the
language mechanics.

```
> gpio-ports
1
> gpio-mode 0 3 out
"out"
> gpio-set 0 3 1
#t
> gpio-in 0
251
```

| | |
|---|---|
| `(gpio-ports)` | how many ports have pins; 0 means no GPIO |
| `(gpio-dir p)` / `(gpio-dir p mask)` | the DIR register |
| `(gpio-out p)` / `(gpio-out p v)` | the OUT register |
| `(gpio-in p)` | the pins, read-only |
| `(gpio-mode p n)` / `(gpio-mode p n "out")` | `"in"`, `"out"`, `"od"` |
| `(gpio-get p n)` | the pin, `#t` / `#f` |
| `(gpio-set p n v)` | drive it |
| `(gpio-toggle p n)` | flip OUT |
| `(gpio-od p n v)` | open drain: `v` is the level the line ends up at |
| `(led)` / `(led v)`, `(leds)` / `(leds v)` | the board LEDs |

Bare-word syntax works throughout, which is the payoff for ports and
pins being numbers: `gpio-set 0 3 1` reaches `(gpio-set 0 3 1)` with no
quoting, because a token that parses wholly as a number passes through
unquoted.

**Setters take `#t`/`#f` or `1`/`0`.** Both exist in the wild here --
`#f` is what `(gpio-get)` hands back, so a pin can be mirrored with
`(gpio-set 0 3 (gpio-get 0 2))`, and `1`/`0` is what somebody types who
has just been using the shell. Note that **`0` is false**, which
deviates from Scheme, where every number is true: `(gpio-set 0 3 0)`
meaning "drive it high" would be an afternoon lost, especially since
the shell's `gpio 0 3 0` means low.

**Readers return the pin, not what you asked for.** `(gpio-set 0 3 1)`
on a pin still configured as an input returns `#f`, because nothing is
driving it -- the value was staged for whenever the pin becomes an
output. An echo of the argument would have been a lie.

That matters most for `(gpio-od)`: on a working bus with a pull-up,
`(gpio-od 0 3 #t)` reads back `#t`, and on a bus another device is
holding down it reads back `#f`. That is how you see a stuck I2C slave
from a prompt.

**`(gpio-mode p n "od")` then reads back as `"in"`.** Open drain is not
a hardware mode and there is nowhere to record it; a pin set that way
is an input that `(gpio-od)` drives low on demand, which is exactly
what DIR says about it.

`(led)` and `(leds)` are not gated on `(gpio-ports)` -- they are words
0 and 1 of the same block and exist on every board.

## Simulating it

`sim/machine.c` models the block, including the pull-up, so
`sw/common/zgpio.c` and the bit-bang libraries can be developed without
a board. A pin reads as what we drive if we're driving, what the far
end drives if it is, and **1** if nobody is.

That last line is the important one. A model that returned 0 for a
floating pin would let an I2C library pass in simulation and then read
nothing but zeros from a real bus.

`ZS_GPIO_NPORTS` is 2, not the 1 a real target builds, so code that
gets its port indexing wrong fails in the simulator rather than on
hardware -- with one port, every wrong index still lands on port 0 and
looks fine.

Not modelled: the input synchroniser (reads are immediate, which no
software can tell apart) and the address aliasing (the window is
bounded at 8KB, so accesses above it read as open bus rather than
wrapping). And if both ends drive a pin at once the model reports our
value, where hardware would give contention and an indeterminate level
-- it cannot represent that usefully, so it picks the answer that makes
the bug quiet, which is the one thing in there worth being suspicious
of.

The simulator has **no stream engines and no I2C slave**: its CONFIG
reports 0 for both, so `z_gs_count()` and `z_i2cs_slots()` are 0 and
code falls back exactly as it would on a board without them. The engines are tested in RTL simulation instead (below),
and `zgpio_stream.c` against a model of its registers.

## Testing it

```
make test_gpio                    # the block: ports, LEDs, aliasing
make test_gpio_stream             # the stream engines, a few minutes
make test_gpio_i2cs               # the I2C slave, under a minute
make -C rtl/tests 8b10b           # zlink's 8b/10b, every symbol
make -C sw/common/tests -f Makefile.zgpio_stream   # the C library
make -C sw/common/tests -f Makefile.zi2cs          # the I2C slave's
```

`rtl/tb/tb_gpio.v` builds two ports (so the per-port address arithmetic
is actually exercised -- with one port, every wrong shift of the port
index still lands on port 0 and passes) and drives the pads through a
pull-up model. It covers the LED compatibility, the reset state, DIR
and OUT reaching the pads, IN reading the pad rather than OUT, the four
aliases including their read-back, port isolation, unbuilt ports, the
documented aliasing, and the full open-drain sequence end to end.

`rtl/tb/tb_gpio_stream.v` builds two whole GPIO blocks, each with two
ports and two engines, **on separate clocks** -- the second runs `PPM`
parts per million fast or slow, as a second board would -- and wires
pins between them with `JIT` ns of random delay on every edge. It
covers:

- CONFIG, an unbuilt engine, and SLOCK;
- pin ownership: DIR/OUT ignored on a claimed pin, IN still reading
  it, and the pin returned to GPIO when MODE goes back to 0;
- the FIFO plumbing: STX/STX4/SRX/SRX4, levels, TX overflow, flush;
- raw at 48 Mbit/s through a jumper, both bit orders, the trigger, and
  REPEAT;
- UART both ways at 115200, 31250 and 3 Mbaud, and a framing error;
- SPI against a device model in all four modes, MSB and LSB first, at
  12 and 6 MHz, full duplex and write-only;
- zlink both ways at 12 and 3 Mbit/s with data and K symbols, a fault
  on the wire mid-burst, and the link clean again afterwards;
- a second engine working independently of the first.

It passes at 0, +-300, +-1000 and +-2000 ppm and with up to 8 ns of
jitter (`make test_gpio_stream PPM=-2000 JIT=5`); a 48 MHz crystal
oscillator is good to +-50-100 ppm.

Two things it found that are worth knowing about:

- **A glitch on enable.** For one clock after MODE was written, a pin
  was already the engine's but its shift register still held the
  previous mode's idle level, so a raw or UART line that should idle
  high dipped low -- a false start bit or trigger at the far end. Pins
  are now taken one clock later, once the reset has run.
- **A false comma after reconfiguring.** The zlink receiver's shift
  register kept the previous run's bits across a mode change, and the
  first new sample could complete a K28.5 with them, aligning the
  receiver off the symbol boundary -- a stray byte at the start of the
  next stream. It now clears to all ones, which cannot form a comma
  with anything but a real one.

`rtl/tb/tb_gpio_i2cs.v` puts the I2C slave in a whole `gpio_wb` (one
port, one engine, four slots) against a master modelled on the LS10's:
open drain, 4 us per line change (about 40 kHz), no patience for clock
stretching -- and **zero hold time**, SDA moving the instant SCL falls,
which I2C allows and which is the case the slave must bridge. It
covers CONFIG and the engine next door; loading and reading the mirror;
probes of mapped and unmapped addresses; writes with pair toggle; a
write to a read-only register (ACKed, unchanged, logged); write then
read with a repeated start, the read flag and where the read started;
the persistent pointer; linear auto-increment across the wrap at 255,
and none; the log in order and empty; a 50 ns glitch on SCL in the
middle of a byte; pin ownership with DIR set on the whole port; 513
writes at 400 kHz, overflowing the log while every byte still reaches
the mirror; IFLUSH; and disabling. At every clock it checks that the
slave never drives SCL and never drives SDA high.

The first run found the zero-hold-time case: the test's master moves
SDA in the same instant it pulls SCL low, the two filters flipped a
clock apart, and the slave saw a stop. A start or stop now counts only
while SCL is still high at the pin.

## See also

- `rtl/gpio.v` -- the block, and the reasoning for each decision above
- `rtl/gpio_stream.v` -- the stream engines
- `rtl/gpio_i2cs.v`, `sw/common/zi2cs.h` -- the I2C slave
- `docs/bench.md` -- what the I2C slave is for
- `docs/zlink.md` -- zlink, which the stream engines carry
- `release/hw/pmods/gpio.spec` -- pin map, pull-up choice
- `docs/logic_app.md` -- the `logic` app: analyser, pin driver, I2C decoder
- `docs/i2c.md` -- bit-banged I2C over these pins
- `docs/uart1.md` -- the second hardware UART, and why it is not a GPIO thing
- `docs/spi.md` -- bit-banged SPI over these pins
- `docs/csrs.md` -- `FEATURES2` and why it exists
- `docs/releases.md` -- targets and the PMOD spec layer
