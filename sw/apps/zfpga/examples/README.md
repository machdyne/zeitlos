# zfpga examples and where the test fixtures came from

## The three rungs (docs/zfpga.md sec. 5.3)

| | |
|---|---|
| `empty.zn` | a bitstream that loads and does nothing |
| `on.zn` | the Lakritz LED on -- written by hand, each arc explained |
| `blink.zn` | `blink.v` as nextpnr placed it, via `tools/np2zn.py` |

```
zfpga pnr on.zn            # -> on.config
zfpga pack on.config -c    # -> on.bit
```

All three are byte-identical to what nextpnr would produce, and
`tests/run.sh` checks it. None has been on a board yet.

## Designs

`blink.v` is the design every rung is measured against. `on.v` is rung
2's source; `sides.v` with `sides.lpf` is the Phase 3 stress test --
IO on all four sides, two banks, a tristate and every per-pin option;
`dense.v` is the router's congestion test (synthesise it with
`synth_ecp5 -nodsp`, and give nextpnr `--lpf-allow-unconstrained`).

The placer's inputs, `tests/fixtures/{blinkf,sidesp,dense}.zl`, come
from yosys with the Phase 5 v1 flags, through the converter:

```
yosys -p "synth_ecp5 -noccu2 -nowidelut -nodsp -nobram -nolutram -top top -json d.json" d.v
../tools/ys2zl.py d.json d.lpf --device LFE5U-25F --package CABGA256 -o d.zl
zfpga place d.zl && zfpga pnr d.zn && zfpga pack d.config -c
```

With carry chains (Phase 5 v2), drop `-noccu2`; those fixtures are the
`*_c.zl` files. `adder.v` feeds a chain from a pin and `cmp.v` feeds a
chain's carry out to logic -- the feed-in and feed-out paths.

`blinkf.v` is the blinky with an 8-bit counter, so the LED changes
within a simulated run; `sidesp.v` is `sides.v` without the tristate.

The `tests/fixtures/*_route.zn` files are the same designs with nets
declared by terminal instead of arcs, for the router:
`../tools/np2zn.py --route ../ext/prjtrellis-db routed.json design.config -o x.zn`.
Until `zfpga synth` exists they are built on the host.

## Regenerating `tests/fixtures/`

Tools as in `docs/toolchain.md` (the fixtures were made with the Ubuntu
24.04 packages: yosys 0.33, nextpnr-ecp5 0.6, ecppack 1.4).

```
yosys -q -p "synth_ecp5 -top top -json blink.json" blink.v
nextpnr-ecp5 --25k --package CABGA256 --lpf blink_lakritz.lpf \
    --json blink.json --textcfg blink.config
nextpnr-ecp5 --45k --package CABGA256 --lpf blink_lakritz.lpf \
    --json blink.json --textcfg blink45.config
nextpnr-ecp5 --85k --package CABGA381 --lpf blink_ulx3s85.lpf \
    --json blink.json --textcfg blink85.config
```

`dfu.config` is the real Lakritz DFU bootloader,
`machdyne/tinydfu-bootloader`, `boards/lakritz/`: its Makefile's
yosys and nextpnr steps, unchanged, with `--textcfg`. It is the
fixture that exercises `.bram_init` and `.tile_group`.

The `.zn` files come from nextpnr's placed design through the
converter, which takes only physical facts from the `.config`
(docs/zfpga.md sec. 12.5):

```
yosys -q -p "synth_ecp5 -top top -json sides.json" sides.v
nextpnr-ecp5 --25k --package CABGA256 --lpf sides.lpf --json sides.json \
    --textcfg sides.config --write sides_routed.json
../tools/np2zn.py sides_routed.json sides.config -o sides.zn
```

`tests/fixtures/sides.config.gz` and `on.config.gz` are the nextpnr
configs the `pnr` cases compare against.

`edge.config` was assembled by a script from the other two to cover
what nextpnr rarely writes; see the header of `tests/cases.txt`.

After regenerating, `./tests/run.sh --record` rewrites
`tests/expect.md5` from the installed ecppack. Fixtures are stored
gzipped (`gzip -9 -n`).
