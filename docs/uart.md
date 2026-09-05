# UART

`rtl/uart.v` — a 16550-compatible serial port, written from scratch
for this project.

It answers `0xf000_0000` (UART0, the console) and `0xf000_0100`
(UART1). Same registers, same offsets, same bit meanings as the core
it replaces, so nothing in `sw/` changed when it did.

## Provenance

Written against the 16550 **register interface** and against what this
tree's software actually does with it — `sw/bios/bios.c`'s
`uart_init()`, `putchar()` and `getchar()`; `sw/os/uart.c`'s ISR and
`tx_pump()`; `sw/common/zuart.c`'s UART1 API.

No code, structure or naming is taken from `rtl/ext/uart16550`, which
it replaces. That core is LGPL; this one is under the same license as
the rest of the repo. The compatibility is with a published
programming model, not with an implementation.

## Why replace a working core

**Not for speed.** The 16550 was never on the critical path on any
board here. It does not appear in a single nextpnr critical path
report, and the timing differences between builds with and without it
are placement variance on a design whose real constraints are
`rtl/mtu.v`'s address adder and the GPU scanout cluster.

**For area, and for the license.** Measured standalone on ECP5 with
`-abc9`:

| | LUT4 | FF | LUTRAM | CCU2C | PFUMX |
|---|---|---|---|---|---|
| `rtl/ext/uart16550` `uart_top` | 583 | 308 | 4 | 46 | 89 |
| `rtl/uart.v` `uart_wb` | **256** | **152** | 4 | 28 | 30 |

**−56% LUT4, −51% FF**, per instance. Doubled on a board with UART1
(`obst_uart_uart1`, the ULX3S).

Most of what went was machinery this tree never used: a debug register
interface, a full modem control block whose four inputs `sysctl.v` tied
to `1'b1`, three error bits on every FIFO entry, and selectable receive
trigger levels nothing selects.

Whole-SoC effect on Obst (ECP5 12F), same tree, same seed:

| | COMB | FF | DP16KD | sys_clk |
|---|---|---|---|---|
| `uart_top` | 16894 (69%) | 7436 | 52/56 | 51.44 MHz |
| `uart_wb` | 16576 (68%) | 7290 | 52/56 | 53.83 MHz |

Treat the clock number as a single sample — this design has several
near-tied paths in the 48–55 MHz band and which one wins is placement
luck. The cell counts are the real result.

## Registers

| offset | read | write |
|---|---|---|
| `0x00` | RBR / DLL | THR / DLL |
| `0x04` | IER / DLM | IER / DLM |
| `0x08` | IIR | FCR |
| `0x0c` | LCR | LCR |
| `0x10` | *0* | *discarded* |
| `0x14` | LSR | — |
| `0x18` | *0* | — |
| `0x1c` | *0* | *discarded* |

DLL and DLM are reached through offsets `0x00`/`0x04` with LCR bit 7
(DLAB) set, as on the real part. This has to work: `bios.c`'s
`uart_init()` writes the divisor through that path before anything
else happens, and without DLAB those two writes would be transmitted
as characters.

16-byte transmit and receive FIFOs, 5–8 data bits, 1 or 2 stop bits,
none/even/odd parity — the whole of what `z_uart1_config()` can ask
for. Baud is `sys_clk / (16 * divisor)`, the 16550's own arithmetic
and what `zuart.c`'s `baud_div()` computes against `Z_SYSCLK_HZ`.

## What is not implemented

**MCR (`0x10`), MSR (`0x18`) and SCR (`0x1c`)** read back zero and
discard writes. There are no modem lines on any board here — `sysctl.v`
tied `cts`/`dsr`/`ri`/`dcd` to `1'b1` on the old core, so MSR was
already a constant — and nothing in `sw/` reads any of the three.

They are still **decoded and acked**, and that is the one thing here
that must not be "optimised" later. An address nothing acks gets no
response on this bus and the CPU waits for it forever; see
`rtl/uart_null.v`'s header for the same hazard found the hard way.
Every offset in the window answers.

**Receive trigger level** (FCR bits 7:6) is accepted and ignored; the
level is always 1 byte. `bios.c` and `sw/os/uart.c` both write
`0b111`, which selects 1 anyway, and a build asking for 14 gets more
interrupts than it wanted rather than fewer — the safe direction.
Because the level is 1, the **character timeout interrupt** (IIR
`0x0c`) can never be needed and is not generated. `sw/os/uart.c`
handles it identically to RDA in any case.

**Break generation** (LCR bit 6) is absent. Break *detection* is
present — `zuart.c` reads LSR's BI bit.

**Receiver-line-status and modem interrupts** (IER bits 2 and 3) are
stored and read back but never assert `int_o`. Nothing sets them.

## Two deliberate deviations

### Error bits are sticky, not per-byte

PE, FE and BI are global bits cleared when LSR is read, rather than
travelling with each byte through the receive FIFO.

The real 16550 tags every FIFO entry with its own three error bits, so
LSR describes the byte at the head. That is three extra bits of
storage per entry plus a mux, and it buys the ability to say *which*
byte in a burst was corrupt. Nothing here wants to know:
`sw/os/uart.c` reads LSR bit 7, discards one byte and returns, and
`zuart.c` accumulates the bits into its own `err_sticky` word
precisely because it is polling and expects to catch errors *between*
reads (see `lsr_poll()` there).

So the semantics are the earlier 16450's: an error sets the bit, the
bit stays set until LSR is read, and bit 7 is the OR of the three.
Polling software sees every error. Software wanting to attribute one
to a particular byte cannot — and none exists here.

### THRE means empty, not "has room"

LSR bit 5 is set when the transmit FIFO is **empty**, which is the
real part's contract and not the cheaper "there is room".

The difference matters to code this tree does not contain yet: the
standard 16550 idiom is to see THRE once and then write a full FIFO's
worth, and that is only safe if THRE means empty. Reporting "room"
would silently drop fifteen of those sixteen bytes.

Everything here writes one byte per LSR read, so today the FIFO buys
nothing on transmit. That is a software-side improvement available
later, and it is available *because* this bit is honest.

## The transmit interrupt is an edge

Raised when the transmit FIFO **becomes** empty, or when IER bit 1 is
enabled while it already is. Cleared by reading IIR or writing THR.

A level — "interrupt while empty" — passes every functional test and
then storms: reading IIR clears the flag and the next cycle re-raises
it, so an ISR finding nothing to send returns into itself forever.
`sw/os/uart.c` happens to survive that, because it only sets IER bit 1
while it has something queued and clears it when the ring drains. But
relying on a driver to avoid a hardware livelock is not a contract
worth writing down.

This was caught by `make test_uart`, not by reasoning.

## Testing

```
$ make test_uart
```

`rtl/tb/tb_uart.v` — 40 self-checking cases with TX looped back to RX:
the divisor latch, 8N1/8E1/8O1/5N1/7N1/8N2 at two divisors, FIFO depth
and ordering, overrun behaviour, the FCR flush, both interrupt sources
and IIR identity, and that the unimplemented registers still ack.

Worth running before any flash that touches this file. It answers the
window `bios.c` writes its *first* character into, so a fault here
does not degrade the console — it produces a board that prints nothing
at all.

Two things the bench does **not** cover. Loopback shares one bit clock
between the ends, which a real link does not, so it says nothing about
the receiver's tolerance to a transmitter running fast or slow — that
is what the 16x oversampling and mid-bit sampling are for, and
confirming it needs a second divisor and a longer run. And nothing
here has been tested against real hardware at the time of writing.

## Removing the old core

`rtl/ext/uart16550` is no longer referenced by the `Makefile` or by
`rtl/sysctl.v` and can be deleted:

```
$ git rm -r rtl/ext/uart16550
```

That also removes the LGPL entry from the README's license list, which
was the only copyleft component in the tree.

## Files

```
rtl/uart.v          the core
rtl/tb/tb_uart.v    the testbench (make test_uart)
rtl/sysctl.v        UART0 and UART1 instances
rtl/uart_null.v     the phantom, for builds with no console at all
rtl/usb_cdc_uart.v  the same register map over USB CDC-ACM
```

Note that three blocks now implement this register interface —
`uart.v`, `uart_null.v` and `usb_cdc_uart.v` — and each carries its
own copy of the decode. That is deliberate for now: they answer very
different questions and share only a table of offsets. If a fourth
appears, factor the register file out.
