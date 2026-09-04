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

## The block at a glance

| | |
|---|---|
| Base | `0xe000_0000` |
| Ports | `0xe000_1000 + N * 0x20` |
| Ports built | 0-4 today, 8 reserved in the map |
| Feature bit | `Z_FEATURE2_GPIO`, `FEATURES2` bit 0 |
| Pull-ups | weak internal pull-up, always on, not changeable at runtime |
| Reset state | every pin an input, so every pin pulled high |

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
order `rtl/sysctl.v` declares them, so on `obst_uart_gpio` the only
GPIO port is physically on PMOD *B* and it is still port 0.

There was briefly a letter notation -- `"B3"` for port 1 pin 3 -- and
it was removed for a reason worth recording, because it looks
convenient: **letters already mean something else in this project.** A
board spec (`release/hw/boards/*.spec`) says `pmod.a` and `pmod.b`, and
those *are* the physical connectors. So on `obst_uart_gpio` the port
the release system calls `b` was the port the API called `A` -- two
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
| `+0x0c` | CONFIG | r | `{16'h4750, 12'b0, nports[3:0]}` |
| `+0x10`..`+0x1c` | | | reserved, read 0 |

LED and LEDS are at the addresses `rtl/debug.v` had them at, and behave
identically, so `sw/bios/bios.c` and `sw/os/uart.c` needed no changes.

Both MAGIC and CONFIG carry a constant that software checks, and CONFIG
needs its signature for a specific reason: a bitstream from before this
block existed answers *every* address it doesn't recognise with
`{31'b0, led}`, so a CONFIG read there returns 0 or 1 -- and "1" is
exactly what a working block with one port would return. The `"GP"` in
the top half is what separates those.

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

## Building it

### Which targets have it

| target | GPIO | notes |
|---|---|---|
| `lakritz_gpio` | port 0 on PMOD A | no serial console -- `` -UART0 `` |
| `obst_uart_gpio` | port 0 on PMOD B | console on PMOD A, no ethernet -- `` -SPI_ETH `` |
| `sergei_gpio` | port 0, **4 pins**, on PMOD A | no optical audio -- `` -AUDIO_SPDIF `` |

```
./release/zrelease build lakritz_gpio
./release/zrelease build obst_uart_gpio
```

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
- **Obst has two**, with the console on A and the Langkatze ethernet
  PMOD on B. It's ethernet or GPIO, not both.

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
`sergei_gpio` the `logic` app shows four channels sitting flat at zero
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
So `sergei_gpio` gives **three usable pins plus one output-only pin**.
Enough for I2C with a spare if SCL and SDA go on bits 1–3; not enough
for SPI with a chip select. Open drain needs a pin that can read back,
which bit 0 cannot do.

**Pin 1 is also the optical S/PDIF output** on Sergei (ball A13), so
`sergei_gpio` removes `` `AUDIO_SPDIF ``. That board has no other audio
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

That's the deal the SD card and the ethernet MAC already have, and it's
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
at all. It's here for `obst_uart_gpio` and for bring-up: the first
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

`repl`, via `sw/apps/repl/zapi.c`. See `docs/scheme_api.md` for the
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

## Testing it

```
iverilog -g2005 -o /tmp/tb_gpio rtl/tb/tb_gpio.v rtl/gpio.v && /tmp/tb_gpio
```

`rtl/tb/tb_gpio.v` builds two ports (so the per-port address arithmetic
is actually exercised -- with one port, every wrong shift of the port
index still lands on port 0 and passes) and drives the pads through a
pull-up model. It covers the LED compatibility, the reset state, DIR
and OUT reaching the pads, IN reading the pad rather than OUT, the four
aliases including their read-back, port isolation, unbuilt ports, the
documented aliasing, and the full open-drain sequence end to end.

## See also

- `rtl/gpio.v` -- the block, and the reasoning for each decision above
- `release/hw/pmods/gpio.spec` -- pin map, pull-up choice
- `docs/logic_app.md` -- the `logic` app: analyser, pin driver, I2C decoder
- `docs/i2c.md` -- bit-banged I2C over these pins
- `docs/uart1.md` -- the second hardware UART, and why it is not a GPIO thing
- `docs/spi.md` -- bit-banged SPI over these pins
- `docs/csrs.md` -- `FEATURES2` and why it exists
- `docs/releases.md` -- targets and the PMOD spec layer
