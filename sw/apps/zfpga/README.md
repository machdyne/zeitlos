# zfpga

**Experimental.** An FPGA toolchain that runs on Zeitlos: Verilog to an
ECP5 bitstream on the machine itself, no host involved. A blinky built
this way on a Lakritz blinks the LED.

```
zfpga build /fpga/examples/blink.v -b lakritz
```

| | |
|---|---|
| `zfpga build IN -b BOARD` | every stage below, from a `.v`, `.zl`, `.zn` or `.cfg`, with a board profile |
| `zfpga synth IN.v [MORE.v ...] -l PINS.lpf` | Verilog-2001 -- modules and instances, the preprocessor, `for`, memories, signed, functions -- to a logical netlist `.zl` |
| `zfpga place IN.zl` | pack and place to a physical netlist `.zn` (`loc=` pins cells; `-l` writes the placement back) |
| `zfpga pnr IN.zn` | route, to a Trellis configuration `.cfg` |
| `zfpga pack IN.cfg` | to a bitstream, byte-identical to `ecppack`'s |
| `zfpga unpack IN.bit` | back to a `.cfg`, text-identical to `ecpunpack`'s |
| `zfpga bram IN.{cfg,bit} -f SEED.hex -t NEW.hex -o OUT` | new block RAM contents in a finished design, as `ecpbram` does |
| `zfpga jump TARGET -b BOARD` | a jumploader: reloads from TARGET, which the machine can change in place (`docs/zboot.md` sec. 5) |
| `zfpga flash FILE.bit [-a ADDR]` | install gateware in the machine's flash, after the core apps (on the machine) |
| `zfpga run FILE.bit [-a ADDR]` | ... and boot it through the jumploader |
| `zfpga info DEVICE [RrCc]` | the database, or what is at a location |

## Documentation

- `docs/zfpga.md` -- the design and its record: why each part is the
  way it is, what was found, what is still open (section 23.3).
- `docs/zfpga-formats.md` -- every file format, for editing by hand.
- `docs/zfpga-test.md` -- building and booting on a Lakritz, step by step.

## Building

```
make -f Makefile.host      # ./zfpga for the host, and db/ -- the card's /fpga
make                       # zfpga.bin for Zeitlos
```

`db/` is laid out exactly as `/fpga` on the card: the chip database
(`lfe5u25f.zdb`), board profiles, examples. Every name on the card is
8.3; Zeitlos's FatFs has no long names.

## Testing

```
tests/run.sh               # host: against ecppack, nextpnr's results, yosys's netlists
tests/run_dev.sh           # the device build under sim/, against the host build
tests/rtlcheck.sh          # optional, needs yosys: zfpga synth against yosys on rtl/
```

`run.sh` compares against the Project Trellis tools if they are
installed, and against recorded results if not
(`ZFPGA_NO_REF=1` forces the latter). The `tools/` scripts are the
independent checkers it uses -- `resolvecheck.py`, `routecheck.py`,
`simcheck.py` -- and the host-only converters that produced the test
fixtures from nextpnr and yosys (`np2zn.py`, `ys2zl.py`), plus
`mkzdb.py`, which builds the database from the vendored Trellis data.

## Layout

| | |
|---|---|
| `main.c`, `util.c`, `port_*.c` | commands, allocation, the host/device seam |
| `db.c`, `chip.c` | the packed database; the CRAM |
| `cfg.c`, `pack.c`, `unpack.c` | `.cfg` <-> bitstream |
| `pnr.c`, `route.c` | physical netlist to `.cfg`; the router |
| `place.c` | packing and placement |
| `synth_parse.c`, `synth_flat.c`, `synth_elab.c`, `synth_map.c` | the Verilog front end and preprocessor; flattening instances; the gate graph; LUT mapping |
| `bram.c` | `zfpga bram` |
| `build.c` | `zfpga build` and board profiles |
| `ext/` | vendored Trellis database (CC0) and nextpnr baseline data (ISC) |
| `examples/`, `boards/` | shipped to the card's `/fpga` |
| `tests/` | the suites and their fixtures |
