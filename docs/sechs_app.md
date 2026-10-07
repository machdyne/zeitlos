# sechs -- Sechs modules on a PMOD

A command for the posix shell that talks to Sechs modules (Machdyne's
six-pin modules, such as the Zwölf LS10 and LS11) on one of the PMOD
ports. The protocol is Sechs's master side, the library that comes with
Machdyne BASIC, a submodule (`sw/ext/basic/tools/sechs/sechsm.c`): the
same code as Linux's `sechsctl`, and the same output.

```
$ sechs scan
0x0c running networked
$ sechs info 0x0c
address 0x0c
version 0.5
caps    0x07
status  running networked
ok      0x1f
fault   0
fw=Machdyne BASIC
mod=LS10A
lang=basic
```

## Wiring

A module in a Wolfszahn plugs into any PMOD, top row: its pin 1 (A, the
bus clock) on PMOD pin 1, pin 2 (B, data) on PMOD pin 2, ground and 3.3V
on PMOD pins 5 and 6. Those are GPIO bits 0 and 1 of the port
([gpio.md](gpio.md)). Several modules can share the bus, each at its own
address.

`-p PORT` chooses the PMOD port (default 0): `sechs -p 1 scan`. `-b BUS`
names any bus zi2cx knows ([i2c.md](i2c.md)): `-b pmod1` is `-p 1`, and
`-b main` is the bench bus `main`, with virtual modules on it
([ls99.md](ls99.md)): `sechs -b main console 0x0c` types into a virtual
LS10 exactly as into a real one.

The bus runs off the GPIO pins' internal pull-ups. `zi2c` waits for each
released line to read high ([i2c.md](i2c.md)), so weak pull-ups cost
speed, never correctness. For long wires or many modules, add 2.2-4.7k
pull-ups on PMOD pins 1 and 2.

## Commands

| Command | Does |
|---|---|
| `sechs scan` | the modules on the bus, with their status |
| `sechs info ADDR` | a module's identity and state, and its `INFO` text |
| `sechs reg ADDR N` | program register `N` (0-15): a module's `REG N` |
| `sechs reg ADDR N VALUE` | sets it (0-255) |
| `sechs halt ADDR`, `run ADDR`, `reset ADDR` | stops its program, starts it, restarts the module |
| `sechs program ADDR` | restarts it in programming mode, if it has one (`CAPS` bit 7) |
| `sechs addr ADDR NEW` | moves the module to address `NEW` (0x08-0x77); it keeps it |
| `sechs console ADDR` | the module's I2C console, interactively |
| `sechs send ADDR FILE` | types `FILE` into the console, a line at a time |

Addresses are hex with `0x`, or decimal. The exit status is 0, 1 if the
module did not do it, 2 for a usage error.

### The console

`sechs console 0x0c` takes over the terminal (as `vi` does) and types
into the module's BASIC:

```
$ sechs console 0x0c
console on 0x0c; end with Ctrl-D
10 PRINT "HELLO"
RUN
HELLO
```

A line is edited here, with Backspace, then sent with Enter. What the
module prints appears as it arrives, also while nothing is typed: a
running program's output, for example. Ctrl-D or Ctrl-C ends the session
and gives the terminal back to the shell.

### Sending a program

```
$ sechs send 0x0c blink.bas
```

Each line goes to the module as if typed, and its answers are shown. The
exit status is 1 if the last line's command failed, which is what a
script checks.

## What it does not do

- **Write a module's firmware.** Programming through SWIO needs pulses
  of about 80 ns, which software cannot time on this machine; Werkzeug
  does it (upstream's `docs/ch32prog.md`).
- **The module's UART console** (its pins 3 and 4). The I2C console
  needs only the bus.

## How it works

| File | |
|---|---|
| `sechs_cmd.c` | the commands, independent of Zeitlos: argument parsing, the output, the console's line editing |
| `sechs_app.c` | the bus (`zi2c` on the chosen port) and the terminal: output through posix's `"stdout"` relay as `zcc` does, or for `console` the terminal handed over as for `vi` |

`make test` in `sw/apps/sechs`, on a host: every command against a
simulated module (its registers, `INFO`, an I2C console that echoes and
answers, `CONTROL`, `ADDR`, `REG`), checking the output and what reached
the module.
