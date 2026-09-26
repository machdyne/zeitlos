# Console

The serial console (UART0) sees things nothing else does: boot messages,
crash reports, the kernel shell and its output. A machine with no host
computer attached used to have no way to see any of it. Now the kernel
keeps a copy of what it sends there, and `term` can show it, with a
prompt shared with the serial console.

In `term`, click **CONSOLE** on the start panel (or `port console0` in
the Open bar).

## What you get

- **Everything the kernel still holds**, replayed the moment you
  connect: the last 4 KB of console output, which on a normal boot
  includes the whole boot log. If more than that has been printed since
  boot, the replay starts with
  `[... earlier console output was overwritten ...]`.
- **New output as it happens**, including crash reports
  ([mpu.md](mpu.md)).
- **The kernel shell's prompt.** What you type goes to the kernel shell
  exactly as if typed on the serial console, and its echo appears in
  both places. Typing at the serial console at the same time works too:
  both feed the same input buffer.

Several `term` windows can be connected at once; each has its own
position in the log.

## What does not reach it

- **Anything the BIOS printed.** It runs before the kernel exists.
- **Output written straight to UART0's registers** by something other
  than the kernel's own `k_uart_putc()` and `kprint()`. Apps printing
  through the kernel (`z_uart_putc()`) do reach it.
- **A kernel panic** halts the machine, so no app is left to show it;
  instead the panic draws the end of this log straight onto the screen
  itself ([mpu.md](mpu.md#crash-reporting)).

## The password

The console lock ([security.md](security.md)) covers this port as it
covers UART0: they are one shell. `passwd reset`, which removes the
password without asking for it, is the exception -- it is refused here
and accepted only on UART0, because the kernel can tell the two apart
(bytes injected through this port are marked in the UART receive ring).

## A client that dies

A client killed without closing -- a `term` window closed from its
titlebar -- is noticed within about a second and the log stops going
to it. It used to go on forever, and once, when the pid came back as
posix, posix ran the log line by line as commands (ports.md, "A peer
that died").

## How it works

```
printf / kprint / z_uart_putc --> k_uart_putc() --> UART0
                                        |
                                        +--> console log ring (4 KB)
                                                   |
                    Z_SYS_KLOG_READ  <-------------+
                          |
                 sw/apps/console ("console0") --DATA--> term
                          |
                 Z_SYS_CONSOLE_INPUT <--DATA--- term keystrokes
                          |
                 UART RX buffer --> kernel shell (readline)
```

**The ring** (`sw/os/uart.c`) is 4 KB of `.bss` and a byte counter.
Recording happens where `k_uart_putc()` puts a byte into the UART's
transmit buffer, inside the section it already runs with interrupts
masked, so the cost is one store and one increment per byte. `kprint()`,
which writes UART0 directly, records through `k_klog_putc()`.

The counter counts bytes since boot, and a byte's slot is the counter
modulo the ring size, so there is no head or tail to keep consistent and
every reader keeps its own position.

**`Z_SYS_KLOG_READ`** copies up to 128 bytes from a given position,
returns the new position, and says how many bytes were skipped if the
position had already been overwritten. **`Z_SYS_CONSOLE_INPUT`** puts
bytes into the same receive buffer the UART interrupt fills and wakes the
kernel shell, so it cannot tell them from real typing. Both are in
`sw/common/zconsole.h`.

**`sw/apps/console`** is a port provider ([ports.md](ports.md)),
registered as `console0`. On connect it starts a client at position 0,
"everything still held". About 30 times a second it sends each client
what it has not seen yet. It only advances a client's position when a
send succeeds, and `z_port_send()` refuses once too many sends are
unacknowledged, so a slow client lags rather than making memory grow.
Up to four clients; one that reconnects without having closed (a
killed `term`) gets its old slot back.

It is a core app, in flash, started by `init()` before the shells: the
machines that most need it are exactly the ones with no card and no
serial cable.

It is small, about 4 KB of code, and staying that way takes two things,
both in `sw/apps/console/Makefile`:

- **Section GC** (`-ffunction-sections -fdata-sections`,
  `--gc-sections`), so only the runtime functions it calls are linked,
  not all of `zeitlos.c`, `zobj.c` and `zport.c`. Without it the first
  build was about 90 KB.
- **`-DZPORT_NO_PRINTF`.** `console.c` itself never calls `printf`, but
  `z_port_send()` and `z_port_send_ack()` in `sw/common/zport.c` print
  diagnostics for rare failures, and that alone linked the whole of
  `printf`, floating point included. The switch compiles those messages
  out, for this program only.

## Changing the size

`K_KLOG_SIZE` in `sw/os/uart.c` and `Z_KLOG_SIZE` in
`sw/common/zconsole.h` must agree, and must be a power of two.

The ring is `.bss`, but `kernel.bin` is padded through `.bss`
(`objcopy --pad-to=_end`, `sw/os/Makefile`), so it counts against the
kernel's 256 KB limit byte for byte: 4 KB now. 2 KB would still hold a
normal boot log; the build prints how much room is left.
