# bench -- virtual parts on I2C buses

The bench is where virtual parts live: I/O expanders, LEDs, buttons,
switches, lamps. A netlist (a short text file in `/bench`) says which
parts there are and how they are connected; bench shows them live, and
apps reach their I2C buses as they would a real one. Developing a program
against a virtual TCA9535 and running it unchanged against the real chip
is the point.

```
$ bench /bench/panel.net &
$ i2c -b main write 0x20 6 0x00        # port 0: outputs
$ i2c -b main write 0x20 2 0x55        # four LEDs light on the bench
```

Bench tests logic, never electrics: levels are high, low, floating or in
conflict, transactions are whole, and nothing is timed below a
millisecond. Real hardware stays the place for those.

## The screen

A resizable window: a status line, then a card for every part, laid out
on a grid that follows the window's width, with scrollbars when they do
not fit.

| On a card | Means |
|---|---|
| a filled square | the pin is high |
| an empty square | low |
| a dotted square with `?` | floating: nothing drives it |
| a square with an X | conflict: two outputs disagree |
| `o` / `i` under a pin | the part drives it (an output) / does not |
| an LED or lamp filled | lit |
| `down`, `on` (inverted) | a button held, a switch on |

The status line shows the netlist, the virtual time and its speed, the
parts, and how many conflicts there have been: a conflict anywhere is
never silent.

| Key or action | Does |
|---|---|
| click a button or switch | presses it while held / flips it |
| click a pin or a card | its net in the status line: level, pull, and every pin on it |
| F5 | reads the netlist again, after editing it |
| F6 | the cards, the bus log (every transaction, newest last), the modules' consoles |
| 1, 2, 3 | virtual time at x1, x60, x3600; space pauses it |
| Escape | the status line back |
| wheel, Up, Down | scroll |

Bench is started with a netlist (`bench /bench/panel.net`, or by opening
a `.net` file). `/bench/panel.net` is on the card as an example.

## Netlists

Plain text, one statement per line, `#` to the end of a line a comment.
Edit them with any text editor, then press F5 in bench.

```
tca9535  x1  addr=0x20             # a part: TYPE NAME, then its settings
bus      main  x1                  # an I2C bus, and the parts on it
led      l0  x1.P00                # joined to x1's pin P00
led      l1  x1.P01  active=low    # lit when the pin is low
button   b0  x1.P10  pullup        # a pull-up on its net
load     lamp  "Grow light"  x1.P04
```

| Statement | Does |
|---|---|
| `TYPE NAME ["label"] [PART.PIN ...] [key=value ...] [word ...]` | a part; its pins joined, in order, to the pins listed. The words `pullup` and `pulldown` put a resistor on its pins' nets |
| `net NAME PART.PIN ...` | those pins on one net, with a name |
| `pullup PART.PIN`, `pulldown PART.PIN` | a resistor on that pin's net |
| `bus NAME [master=MODULE] PART ...` | an I2C bus, and the I2C parts on it; `master=` makes it a module's own bus (its C and D) |
| `module NAME LS10\|LS11\|LS99 ...` | a virtual Zwölf module running BASIC ([ls99.md](ls99.md)) |
| `place NAME COL ROW` | the part's card at that cell (otherwise: the first free one) |

A part must be declared before another line names it. An error stops the
load and says where: `panel.net line 4: x2: no such part (declared
later?)`. A setting a part does not have is an error too, so a typo
cannot pass unnoticed. `gpio` (real PMOD ports) is recognised and
arrives in a later phase.

## Parts

| Type | Pins | Settings | |
|---|---|---|---|
| `tca9535` | P00-P07, P10-P17, INT | `addr=` 0x20-0x27 | 16-bit I2C I/O expander, no pull-ups (TI) |
| `tca9555` | the same | the same | the same, with pull-ups on its inputs |
| `led` | PIN | `active=high` or `low` | |
| `load` | PIN | `"label"`, `active=` | something switched: a lamp, a pump |
| `button` | PIN | `to=gnd` (default) or `vcc` | pulls its pin to ground while pressed |
| `switch` | PIN | `to=` | the same, staying put |

The TCA9535 and TCA9555 follow their datasheet: four register pairs
(input, output, polarity inversion, configuration) chosen by the command
byte, each further byte moving to the other register of its pair; outputs
0xFF, polarity 0 and every pin an input at first; the input port showing
the pins whatever their direction; polarity inverting inputs only; INT
pulled low while an input differs from when its port was last read. The
TCA9535 has no pull-ups, so an unconnected input floats, which is the
mistake the chip is known for and the reason bench shows floating pins
at all. (A floating input reads 1 here; on the chip it is unpredictable.)

### Adding a part

One C file in `sw/apps/bench/parts/`, filling in a `part_type_t`
(`core.h`): its pins, its I2C addresses, and what it does when written,
read, or when an input changes. The drawing comes free: a part with one
pin gets a small card, one with more a card showing every pin. Then a
line in `parts/parts.c`, its object in the Makefile, and tests against
its datasheet in `tests/`.

## Using bench buses from an app: zi2cx

`sw/common/zi2cx.h` opens a bus by name: `"main"` for a bench bus,
`"pmod1"` for PMOD port 1's real pins. The calls are the same for both
(write, read, write then read with a repeated start, probe), and so are
the results (zi2c's), so an app's I2C code does not care which it got.
See [i2c.md](i2c.md).

The `i2c` command ([i2c\_app.md](i2c_app.md)) is built on it.

## The protocol (zbench.h)

bench is the port provider `bench0`. A client connects with the tag
`"zi2cx"` and sends one request per message, getting one reply each:

| Request | Reply |
|---|---|
| `XFER`: bus name, address, bytes to write, how many to read | status (ACK, NACK at the address, NACK in the data, no such bus), bytes written, bytes read |
| `LIST` | the bus names |

Only zi2cx and bench use it.

## Inside

| File | |
|---|---|
| `core.c` | nets, resolved from the drives on them; buses, transactions and the log |
| `netlist.c` | the netlist parser |
| `draw.c` | the cards and the log, into a one-bit canvas |
| `parts/` | one file per part type |
| `bench_app.c` | the window, the mouse and keys, and the `bench0` port |

All but `bench_app.c` are portable. `make test` in `sw/apps/bench`, on a
host: the parser and its errors, net resolution (pulls, floating,
conflicts), bus semantics, each part against its datasheet, the drawing
(layout, clicks), and the example netlist.

## Phases

| Phase | Brings |
|---|---|
| 1 | the bench, netlists, the parts above, zi2cx, `i2c` |
| 2 (this) | LS99: virtual Zwölf modules running BASIC (LS10, LS11 profiles), on bench buses, with virtual time; `sechs` on zi2cx ([ls99.md](ls99.md)) |
| 3 | the BASIC computer's `I2C` on a bench bus |
| 4 | real hardware: a bench bus extended onto a PMOD; real pins as bench nets |
| 5 | a real module's own bus reaching virtual parts (gateware) |
