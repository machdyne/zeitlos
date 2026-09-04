# SPI

A bit-banged SPI master over GPIO pins. All four clock modes, either
bit order, optional chip select, optional MISO.

`sw/common/zspi.h` / `.c`. Built on `docs/gpio.md`.

## This is not the SD card's SPI

The SD card goes through `rtl/spisd.v` and the ethernet PMOD through
`rtl/spim.v`, both of which generate SCLK in gateware. That is
deliberate, and `rtl/spisd.v`'s own header explains why: the SD card
*was* bit-banged once, which made the SPI clock rate a function of
compiler codegen, and it broke when the toolchain changed.

**This is for the other case** — a display, a sensor, a shift register,
a flash chip you are poking at once. Devices where a few tens of
kilobits per second is plenty and having the pins be software is worth
more than having them be fast. If you find yourself wanting megabits,
the answer is a peripheral, not a tighter loop here.

## Quick start

From `repl`:

```
> spi-init 0 0 1 2 3
0
> spi-select 0 1
#t
> spi-xfer 0 '(159 0 0 0)
(255 239 64 24)
> spi-select 0 0
#f
```

Port 0, SCK on pin 0, MOSI pin 1, MISO pin 2, CS pin 3, mode 0, 1MHz.

From C:

```c
#include "../../common/zspi.h"

z_spi_t bus = {
    .sck_port = 0,  .sck_pin  = 0,
    .mosi_port = 0, .mosi_pin = 1,
    .miso_port = 0, .miso_pin = 2,
    .cs_port = 0,   .cs_pin   = 3,
    .mode = 0, .khz = 1000,
};

z_spi_init(&bus);

uint8_t cmd[4] = { 0x9f, 0, 0, 0 };
z_spi_select(&bus, true);
z_spi_xfer(&bus, cmd, cmd, 4);      /* in place -- see below */
z_spi_select(&bus, false);
```

## Clock modes

| mode | CPOL | CPHA | SCLK idles | data sampled on |
|---|---|---|---|---|
| 0 | 0 | 0 | low | rising edge |
| 1 | 0 | 1 | low | falling edge |
| 2 | 1 | 0 | high | falling edge |
| 3 | 1 | 1 | high | rising edge |

Mode 0 is the common one.

**CPHA is the one people get wrong**, and the symptom is
characteristic: every byte comes back shifted by one bit, so `0x01`
reads as `0x02` or `0x80`. If a device is answering with something that
looks like your data doubled, try the other CPHA before suspecting the
wiring.

`z_spi_init()` parks SCLK at its mode's idle level. Getting that wrong
is not subtle — the first clock edge of the first transfer would be
spurious and everything would be off by a bit forever.

## Push-pull, unlike I2C

SPI lines have exactly one driver each, so SCK, MOSI and CS are
ordinary outputs rather than the open-drain arrangement `docs/i2c.md`
describes. The pull-ups are irrelevant to a driven pin.

They matter to **MISO**, where they mean an absent or unselected device
reads as `0xff` rather than as noise. That's a useful default: `0xff`
from a device that should have answered is a recognisable "nothing
there" rather than a plausible-looking value.

## Missing pins

`Z_SPI_NO_PIN` for `miso_port` or `cs_port` (`-1` in Scheme) says the
device has no such pin — a write-only display needs no MISO, and a lone
device with CS tied low needs no chip select. Reads on a bus with no
MISO return `0xff`, matching what an unconnected input reads as.

The pin field itself says so rather than a pair of `has_*` flags: one
thing to set, and it cannot disagree with itself.

## CS is separate from transfers

`z_spi_select()` is its own call because almost every real SPI device
wants several transfers inside one selection — a command byte, then an
address, then a burst of data. A select-per-transfer API cannot express
that.

`z_spi_init()` deasserts CS *before* making it an output, so
configuring the bus never produces a momentary select. The order
matters: the pin is an input until `z_gpio_mode()` runs, so staging the
value first means it is already correct at the instant it starts
driving.

## In-place transfers

`z_spi_xfer()` reads `tx[i]` before writing `rx[i]`, so **tx and rx may
be the same buffer**. That is how a command-and-response is written and
the obvious thing for a caller to try, so it works.

A NULL `tx` sends `Z_SPI_TX_IDLE`, which is `0xff` and not `0x00`. That
is what the conventions of these devices expect: SD cards require it,
and on most flash and sensor parts `0x00` is a meaningful command byte
where `0xff` is not. Sending zeros while reading is a real way to erase
something.

## Under a preemptive scheduler

A KTIMER tick can land between any two edges, stretching the clock. SPI
has no minimum frequency and no timeout in the protocol, so for almost
every device this is invisible — the same property that makes bit-banged
I2C safe.

**The exceptions are devices that are not really SPI**: anything with a
self-clocking or timing-encoded protocol driven through a shift
register — addressable LEDs being the usual example — will glitch when
the clock stalls for a millisecond. Those need `maskirq()`
(`sw/common/zeitlos.h`) around the transfer, or gateware. This library
does not mask, because a long transfer would then block the whole
system.

## Scheme API

| | |
|---|---|
| `(spi-init port sck mosi miso cs [mode [khz]])` | bus handle |
| `(spi-select bus v)` | assert or release CS |
| `(spi-xfer bus data)` | `data` is a list of bytes or a string |
| `(spi-xfer bus n)` | send `n` idle bytes, return what came back |

`-1` for `miso` or `cs` means no such pin. Defaults are mode 0 and
1MHz.

`(spi-xfer bus n)` with a count rather than a list is the read case:
SPI is full duplex, so reading means sending something, and `0xff` is
what a device expects to see while it is talking.

**`spi-xfer` does not touch CS.** Wrap it in `spi-select` calls.

A bus lives on one port in Scheme. `(spi-init)` would otherwise need
ten arguments, and a four-pin SPI device is plugged into one PMOD
connector essentially always. A bus spanning two ports is a C program.

At most 32 bytes per call, and at most 2 open buses.

## Testing

`sw/common/tests/test_bitbang.c`. The SPI half checks idle levels for
each mode, CS polarity both ways, mode rejection, the no-MISO `0xff`
behaviour, that SCLK returns to idle after a transfer, and a **loopback
through all four modes plus LSB-first** with MISO tied to MOSI by the
wire model.

That loopback is the end-to-end check: it exercises both CPHA paths and
both bit orders with the sampled value coming back through a real read.

## See also

- `docs/mmod.md` — the `mmod` app: identifying an SPI memory module over this
- `docs/logic_app.md` — the `logic` app, for seeing what the bus actually did
- `docs/gpio.md` — the pins and the pull-ups
- `docs/i2c.md` — the same treatment for I2C
- `sw/common/zspi.h` — the interface, and the reasoning for each choice
