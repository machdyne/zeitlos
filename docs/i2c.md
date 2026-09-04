# I2C

A bit-banged I2C master over any two GPIO pins. There is no I2C
hardware on this SOC and this is why there does not need to be.

`sw/common/zi2c.h` / `.c`. Built on `docs/gpio.md`, which is worth
reading first — particularly the pull-up section, which is what makes
this usable with a bare PMOD.

## Quick start

From `repl`:

```
> i2c-init 0 0 1
0
> i2c-scan 0
(60)
> i2c-reg 0 60 1
92
> i2c-reg 0 60 1 255
#t
```

SCL on port 0 pin 0, SDA on pin 1, 100kHz. The `0` that comes back is
a bus handle; every other procedure takes it.

From C:

```c
#include "../../common/zi2c.h"

z_i2c_t bus = {
    .scl_port = 0, .scl_pin = 0,
    .sda_port = 0, .sda_pin = 1,
    .khz = 100, .timeout_us = 1000,
};

if (z_i2c_init(&bus) != Z_I2C_OK) { /* bus is stuck -- try recover */ }

uint8_t v;
if (z_i2c_reg_read8(&bus, 0x3c, 0x01, &v) == Z_I2C_OK)
    printf("reg 1 = %02x\n", v);
```

## Wiring

| | |
|---|---|
| SCL, SDA | any two GPIO pins, any ports (C API); one port from Scheme |
| Pull-ups | weak internal, always on. Real 2.2k–4.7k for anything beyond a short cable |
| Addresses | **7-bit throughout** — `0x3c`, not `0x78` |

The 7-bit convention is worth stating loudly because getting it wrong
is the single most common I2C mistake. The read/write bit is this
library's business; a caller never shifts anything.

## Open drain without open-drain hardware

Every line is driven the way I2C requires and no other way. The pin's
OUT bit is parked at 0 once by `z_i2c_init()`, and after that a 0 on
the bus is "make the pin an output" (`DIRSET`) and a 1 is "make it an
input" (`DIRCLR`) and let the pull-up take it high. One store per edge
— which is what `rtl/gpio.v`'s set/clear registers exist for.

The safety property is **structural rather than careful**: the register
that would have to hold a 1 to drive a line high is never written to
anything but 0, so this code physically cannot drive into a line
another device is pulling down. That's the failure that ends with a hot
part rather than a wrong reading, and it's worth removing by
construction instead of by review.

## Weak pull-ups cost speed, not correctness

The internal pull-ups are tens of kΩ — far too weak to meet the 1µs
rise time 100kHz I2C specifies. It doesn't matter here, because this
master picks its own clock and a microsecond or two of RC rise is
invisible against a 20µs bit period.

What makes that safe rather than lucky is that **this library never
assumes a released line is immediately high**:

- After releasing SCL it polls until SCL actually reads high. That is
  also exactly how clock stretching works, so it is code that has to
  exist anyway — the weak pull-up gets correct handling for free.
- It does the same on SDA before every rising SCL edge on which a 1 is
  being presented. Without it, a slow rise clocks out a 0 — silently,
  and only on bits following a 0, which reads as a flaky device.

So a slow bus runs slower and every byte is still right.
`z_i2c_measured_khz()` (`(i2c-khz bus)`) reports what the bus actually
managed, which is how you find out you need real resistors: far under
the requested rate on a bus that should be fast means the pull-ups.

## NACK is not TIMEOUT

The return value is an enum and not a bool for one reason:

| | |
|---|---|
| `Z_I2C_OK` | |
| `Z_I2C_NACK` | the bus works and nobody answered. What a scan is made of |
| `Z_I2C_TIMEOUT` | a line this code *released* never came back up |
| `Z_I2C_BUSY` | a line was already low at `init`, before anything was sent |

Nothing on an I2C bus can hold a line high, so a timeout is never a
device problem — it is a missing pull-up, a short, or a slave stuck
mid-byte. Folding it into NACK would make a wiring fault look like an
absent device, and you would spend the afternoon on the wrong half of
the board.

`Z_I2C_BUSY` is separate from timeout because it is detected *before*
anything is transmitted, so nothing was half-sent and a retry after
`z_i2c_recover()` is safe.

## Recovering a wedged bus

`z_i2c_recover()` / `(i2c-recover bus)` clocks SCL up to nine times
with SDA released, then sends a STOP.

That is the standard unwedge for the one failure a master can actually
fix. A slave interrupted mid-byte — by a reset, or by this process
being killed between two calls — is still holding SDA low waiting for
clocks that will never come. It is not listening for anything else, so
nine clocks are the only way to walk it to the end of the byte it
thinks it is sending; the STOP then resets it.

If SDA is still low afterwards, the problem is not a stuck slave and no
software can fix it.

## Repeated START, and why `write_read` exists

`z_i2c_write_read()` does a write, a **repeated START**, and a read,
with no STOP in the middle. `z_i2c_reg_read8()` is the one-byte case.

Doing it as a write-STOP-read pair instead is the second most common
I2C mistake. A device that has been given a register pointer and then
sees a STOP is entitled to forget it, and some do — intermittently,
which is a miserable thing to debug. Use these rather than assembling
it yourself.

## Under a preemptive scheduler

A KTIMER tick can land anywhere in a transfer, including between
setting SDA and raising SCL. **It cannot corrupt anything**: every
interval in I2C is a minimum, so being descheduled makes the clock-low
period longer, which is precisely what a slave does deliberately when
it stretches. There is no maximum this code can violate by being slow.

**The one exception:** SMBus devices, and a few I2C parts that borrow
its rules, reset their state machine if SCL is held low for 25–35ms. A
slice here is 1.365ms and a process can be off the CPU for several in a
row, so a badly-timed preemption mid-byte can make an SMBus part
abandon the transfer. The symptom is an occasional NACK or a garbage
read that goes away on retry.

If that matters, wrap the transfer in `maskirq()`
(`sw/common/zeitlos.h`). This library deliberately does not: a
multi-byte transfer at 50kHz is milliseconds long, and masking that is
worse for the system than an occasional retry is for the caller.

## Scheme API

| | |
|---|---|
| `(i2c-init port scl sda [khz])` | bus handle, or `#f` if the bus is unusable |
| `(i2c-scan bus)` | addresses that answered, as a list |
| `(i2c-write bus addr data)` | `data` is a list of bytes or a string |
| `(i2c-read bus addr [n])` | list of bytes, or `#f` |
| `(i2c-reg bus addr reg)` | read one register, or `#f` |
| `(i2c-reg bus addr reg val)` | write it |
| `(i2c-recover bus)` | unwedge |
| `(i2c-error)` | why the last call failed |
| `(i2c-khz bus)` | measured rate of the last transfer |

Failures return `#f` rather than raising, because "nothing answered" is
an ordinary result on a bus — it's what a scan is made of, and what a
busy device gives you. A procedure that blew up the whole expression on
a NACK would be unusable in a loop. Setup mistakes (a bad handle, a
byte out of range) *do* raise, because those are the caller's error
rather than the bus's.

`(i2c-error)` is how you tell which: `"nack"` means retry might help,
`"timeout"` means go look at the wiring.

A bus lives on one port in Scheme, and pins are numbers within it. The
C API lets each pin be on any port; a bus genuinely spanning two ports
is a C program.

Strings are accepted by `i2c-write` because
`(i2c-write bus 60 "hello")` is what anyone talking to a character
display wants to type.

At most 32 bytes per call, and at most 2 open buses.

## Testing

`sw/common/tests/test_bitbang.c` runs the real `zi2c.c` against a
simulated slave — bit-level, edge-driven, with its own register
pointer, deciding for itself when to acknowledge.

```
cc -std=gnu99 -I sw/common -o /tmp/t \
   sw/common/tests/test_bitbang.c \
   sw/common/zi2c.c sw/common/zspi.c sw/common/zgpio.c && /tmp/t
```

It covers START/STOP counts, ack and NACK, the repeated START, register
reads, scanning, the timeout-vs-NACK distinction, and recovery.

The model is **functional, not electrical**: it has pull-ups and
open-drain contention, so protocol logic is exercised honestly, but it
has no rise time, no capacitance and no clock skew. It will not catch a
timing bug. That needs a scope.

See that file's header for how the real sources run on a build machine
at all — the short version is that the GPIO page is mapped read-only
and every store is trapped and single-stepped, which is the only way to
emulate `rtl/gpio.v`'s write-1-to-modify registers faithfully.

## See also

- `docs/logic_app.md` — the `logic` app, which exists to debug this
- `docs/gpio.md` — the pins, the pull-ups, the open-drain idiom
- `docs/spi.md` — the same treatment for SPI
- `sw/common/zi2c.h` — the interface, and the reasoning for each choice
