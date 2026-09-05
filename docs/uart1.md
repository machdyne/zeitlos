# UART1

A second `rtl/uart.v` instance at `0xf000_0100`, available to software
as a general-purpose serial port. `sw/common/zuart.h`,
`sw/apps/serial`.

Since the move off `rtl/ext/uart16550` this is **our** core rather than
a third-party one — see `docs/uart.md`. The register interface is
unchanged and no API here changed with it, but three things in this
document did, and they are marked below.

## UART0 is not this, and never will be

UART0 is the console. `sw/bios/bios.c` writes to it before anything
else in the system exists, the kernel prints to it, and `sh` reads from
it.

There is deliberately **no API for UART0** — not in `zuart.h`, not in
Scheme. An app that reconfigured the console's baud rate would take the
machine's only diagnostic channel with it, and on a board where the
console is the only I/O at all that is unrecoverable without a reflash.

## The gateware was already there

`rtl/sysctl.v` has had the block, the decode at `0xf000_0100`, and the
`UART1_TX`/`UART1_RX` pins behind `` `ifdef UART1 `` all along. The
register map has been in `sw/common/zeitlos.h` as `reg_uart1_*` just as
long, and `sw/apps/net/esp32link.c` has been using it.

What was missing was a board where the pins go somewhere you can plug
something into, and software that treats it as a port rather than as
one app's private link.

### It is off on the ULX3S, on purpose

`rtl/csrs.vh` sets `FEATURES2` bit 1 for `` `UART1 `` **only when
`` `ESP32_LINK `` is absent**, so `z_uart1_present()` is false on a
ULX3S even though that board has a UART1 and always has.

That UART is soldered to the on-board ESP32. No header, no connector,
no second owner — it is `sw/apps/net`'s data plane. A bit saying "this
board has a serial port you can open" would be true of the gateware and
false of the board, and an app that believed it would take the network
down to say hello to nothing. Software finds that one through
`Z_FEATURE_ESP32_LINK`, as it always did.

## Building it

| target | |
|---|---|
| `obst_uart_uart1` | console on PMOD A, UART1 on PMOD B pins 2 and 3 |

```
./release/zrelease build obst_uart_uart1
```

`` -SPI_ETH ``, for the same reason `obst_uart_gpio` drops it: Obst has
two PMOD connectors, the console is on one and the Langkatze ethernet
PMOD is on the other. There is no third socket.

**`obst_uart_uart1` and `obst_uart_gpio` are alternatives, not a
progression.** Both want PMOD B. Getting GPIO *and* a second UART on
Obst would need a combined PMOD spec splitting the connector — UART1 on
pins 2 and 3, six GPIO pins on the rest. That is a real option worth
building if anyone wants it; it isn't built, because a GPIO port with
two of its eight pins missing needs explaining everywhere a port count
is reported, and nobody has asked.

**Lakritz is not offered at all**, and that is a board fact rather than
an omission: it has one PMOD connector and the console is on it. Two
serial ports and no console is just one serial port with extra steps —
unlike `lakritz_gpio`, where giving up the console buys eight pins that
HDMI and a keyboard cannot replace.

Pins 2 and 3 match `usbuart.spec`, so the same USB-UART PMOD that
serves as a console in port A works here without rewiring.

## No flow control — but it is available now

**Changed.** `rtl/uart.v` implements auto-RTS/auto-CTS, and UART1 can
have it:

```
`define UART1_FLOW      // rtl/boards.vh, per board
```

**No board defines it**, so as shipped the statement above still
holds: `sysctl.v` ties `cts_pad_i` to 1 and `usbuart1.spec` wires only
TX and RX. `sw/apps/serial` does not implement software flow control
either, and its banner saying "no flow control" is accurate for every
current target.

What changed is that it is now a target's decision rather than a
missing feature. Turning it on costs +5 LUT4 and needs LPF entries for
`UART1_RTS`/`UART1_CTS`; connector pins 1 and 4 are the ones, matching
the PMOD USB-UART pinout. See `docs/uart.md` for the thresholds and
for why RTS is the half that protects you.

`z_uart1_config()` does not touch MCR, and MCR resets to zero with AFE
clear, so even on a board that builds it nothing happens until
software asks. There is deliberately **no `zuart.h` API for it yet** —
an API that no board can exercise is untested surface. When a target
wires the pins, the call is one write of MCR bit 5.

## Polled, not interrupt-driven

**`cpu_irq[9]` is not wired.** That is a considered omission, and I
should correct an earlier claim of mine that it would be wired
regardless: it should not be, yet.

A level-sensitive interrupt source with no handler is a livelock — the
ISR returns, the level is still asserted, and the machine stops making
forward progress. UART0 escapes this by having a handler that drains
the FIFO and lowers the level; UART1 has no handler. Wiring it as a
*latched* IRQ instead avoids the storm but gives one spurious ISR entry
per edge for no benefit.

`IER` resets to 0 and `z_uart1_config()` writes 0 to it explicitly, so
the source is quiet either way.

**Changed.** One thing to know before wiring it: `rtl/uart.v` has a
fixed receive trigger level of one byte and therefore never generates
the character timeout interrupt (IIR `0x0c`). A driver written against
it sees RDA (`0x04`) per byte and nothing else. `sw/os/uart.c` treats
the two identically in any case, so a UART1 handler modelled on it
needs no change — but a handler that waited for a timeout to flush a
partial buffer would wait forever.

When a driver wants it, the change is two lines and no more work later
than now:

```verilog
cpu_irq[9] = wbs_uart1_int;          // in the always @* block
.LATCHED_IRQ(32'b..._1101_0110_1111) // clear bit 9, both CPU instances
```

Bit 9 must be **non-latched** (0), for exactly the reason bits 4 and 7
are: a latched level source re-fires the instant the handler returns.

### What polling costs

The receive FIFO is 16 bytes. A reader polling once per scheduler slice
sees 16 bytes per 1.365ms — about 11.7 kB/s.

| baud | bytes/s | per 1.365ms slice | |
|---|---|---|---|
| 9600 | 960 | 1.3 | comfortable |
| 115200 | 11520 | 15.7 | **at the edge** — a busy system will drop |
| 1000000 | 100000 | 136 | hopeless |

`sw/apps/serial` polls at `Z_TICK_HZ/60` rather than once per slice for
this reason, and even that is a floor rather than a ceiling when
something else is busy.

`z_uart1_status()` reports the overrun bit, so a reader that is losing
bytes finds out rather than quietly receiving corruption — and
`sw/apps/serial` prints it on the connection instead of hiding it. A
terminal session that silently drops every 17th byte is worse than one
that says it did.

The ULX3S needed a 2KB block-RAM FIFO in gateware
(`rtl/esp32_rxfifo.v`) for exactly this reason at 1 Mbaud. If UART1
ever needs to go that fast here, that is the shape of the answer.

## Baud rates that don't exist

`rtl/uart.v` divides `sys_clk` by `16 × n` with `n` an integer, and at
48MHz that makes some common rates unreachable:

| requested | n | actual | error | |
|---|---|---|---|---|
| 9600 | 313 | 9584 | 0.16% | ok |
| 115200 | 26 | 115384 | 0.15% | ok |
| 230400 | 13 | 230769 | 0.16% | ok |
| **460800** | 7 | 428571 | **6.99%** | refused |
| **921600** | 3 | 1000000 | **8.50%** | refused |
| 1000000 | 3 | 1000000 | 0.00% | exact |
| 1500000 | 2 | 1500000 | 0.00% | exact |
| **2000000** | 2 | 1500000 | **3.52%** | refused |

`z_uart1_open()` refuses anything worse than 3%, because a UART samples
mid-bit and accumulates the error over ten bit times — a few percent
walks the sample point off the end of the byte.

That refusal matters more than it looks. Without it, asking for 921600
would silently give you 1 Mbaud, and the symptom is a port that works
at 115200 and produces garbage at 921600 — which reads like a cable
problem and is not. `z_uart1_baud_error()` /
`(uart1-baud-error baud)` is the thing to check before suspecting the
wiring.

(This is also why the console runs at 1000000: it is exact.)

## Using it from `term`

`sw/apps/serial` owns UART1 and exposes it as a port
(`docs/ports.md`), the same way `net` owns the MAC. From a `term`
window:

```
> serial            (in repl -- 115200, or whatever the port is at)
> serial 9600
```

`repl` sends `term` a `Z_TERM_SET_PORT` naming `serial0` with the baud
rate as the CONNECT argument — the same mechanism `telnet` uses to hand
a window to `net`, with a baud rate where telnet puts a target IP.
**F12 comes back.**

`port serial0` connects with no argument and gets whatever rate the
port is already at, which is what you want after F12.

### One connection at a time

There is one wire. Two `term` windows on one serial port would
interleave their keystrokes into one byte stream and split the replies
between them at random, so a second CONNECT is refused with a message
saying so.

### If there is no UART1

`sw/apps/serial` exits at startup rather than staying resident to
refuse every connection — a process taking a scheduler share to say no
is worse than not being there. `term`'s name lookup then fails and the
window stays in local echo, which is the same clean failure any absent
port provider gives.

## Why a whole process

Because UART1 is one piece of hardware and nothing arbitrates MMIO
(`docs/app_runtime.md`). Two apps calling `z_uart1_open()` with
different baud rates would each believe they had configured the port,
and bytes would go out at whichever rate was written last. A single
owner is the only thing that makes "what baud rate is the port at" a
question with an answer.

It also gets `term` for free: `term` already knows how to be a port
client, so a serial terminal is a port provider that happens to put
bytes on a wire, and none of `term`'s code cares which.

## C API

```c
#include "../../common/zuart.h"

if (!z_uart1_present()) return;
if (!z_uart1_open(115200)) { /* rate unreachable -- see baud_error */ }

z_uart1_write("AT\r\n", 4);

uint8_t buf[32];
uint32_t n = z_uart1_read(buf, sizeof(buf));   /* never blocks */

if (z_uart1_status() & Z_UART1_OVERRUN) { /* bytes were lost */ }
```

| | |
|---|---|
| `z_uart1_present()` | FEATURES2 bit 1 |
| `z_uart1_open(baud)` | 8N1 |
| `z_uart1_config(baud, bits, parity, stop)` | `'n'`, `'e'`, `'o'`; 5-8 bits; 1-2 stop |
| `z_uart1_baud_error(baud)` | percent × 100 |
| `z_uart1_close()` | leaves the divisor alone |
| `z_uart1_tx_ready()`, `z_uart1_putc()`, `z_uart1_write()` | blocking |
| `z_uart1_write_nb()` | never blocks; returns how many went |
| `z_uart1_rx_ready()`, `z_uart1_getc()`, `z_uart1_read()` | never block |
| `z_uart1_flush_rx()` | bounded at 64 bytes — see below |
| `z_uart1_status()` | sticky error bits, cleared by reading |

**`z_uart1_write_nb()` is the one a port server wants.**
`z_uart1_write()` blocking looks harmless until the far end is not
reading and CTS is not wired, at which point the process stops
answering messages and `term` appears to hang.

**`z_uart1_flush_rx()` is bounded rather than "until empty"** — at
speed a live sender can refill the FIFO as fast as this drains it, and
an unbounded loop would never return.

**`z_uart1_status()` is sticky** because the hardware's own LSR bits
are cleared by a read of that register, and every read of the data
register goes past it. An overrun between two `z_uart1_read()` calls
would be gone before anyone asked.

**Changed, and the change makes this more true rather than less.**
`rtl/uart.v` keeps PE, FE and BI as global bits cleared on an LSR read
rather than tagging each FIFO entry with its own, which is what the
16550 did (see `docs/uart.md`). So an error is now attached to "since
you last looked" rather than to a particular byte — which is exactly
the model `lsr_poll()` was already written for. The practical
difference: an error on a byte the caller never reads is no longer
silently discarded with it.

There is no magic register to cross-check presence against, unlike
`rtl/gpio.v`. On a board without a UART1 this address falls through to
whatever the bus resolves to — so the feature bit is the only answer,
and calling anything above without checking it first is reading a
register that may not exist.

**Changed.** The old reason was that the 16550 was somebody else's
core. It is ours now, so an identity register is technically
available — but there is nowhere in the 16550 map to put one. Every
offset means something, and the only unused one, SCR at `0x1c`, is a
scratch register that software is entitled to expect behaves as
scratch. Spending it on a magic value would trade drop-in
compatibility for a check the feature bit already answers. Left
alone deliberately.

## Scheme API

| | |
|---|---|
| `(uart1?)` | is there a general-purpose UART1 |
| `(uart1-open [baud])` | default 115200; `#f` if unreachable |
| `(uart1-close)` | |
| `(uart1-baud-error baud)` | percent, as a real |
| `(uart1-write data)` | string or list of bytes; blocks |
| `(uart1-read [n])` | list of bytes; never blocks |
| `(uart1-ready?)` | |
| `(uart1-status)` | `(overrun)`, `(framing)`, `()`, ... |

```
> uart1-open 9600
#t
> uart1-write "AT\r\n"
4
> uart1-read
(79 75 13 10)
```

These talk to UART1 **directly**, so this process competes with
`sw/apps/serial` for it if that is running. The honest reading: these
are for poking at a serial port from a prompt — send an AT command, see
what a device says on power-up, check a baud rate — and `serial` in
`repl` is for actually using one. Doing both at once produces
interleaved bytes and two processes that each think they set the baud
rate.

`(uart1-read)` never blocks and returns what is there rather than
waiting for `n`. A blocking read at a prompt with nothing on the other
end would hang the REPL with no way out.

At most 32 bytes per call.

## See also

- `docs/connections.md` — the four connection kinds, and term's F11 Open bar
- `docs/ports.md` — the port mechanism `serial` uses
- `docs/gpio.md` — for bit-banging a second serial port on a GPIO pin, if you need more than one
- `sw/common/zuart.h` — the interface, and the reasoning for each choice
