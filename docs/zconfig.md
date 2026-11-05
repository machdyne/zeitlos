# The system configurator (`make config`)

Menus, in a terminal, to put a system together: the board and what is
plugged into it, the profile, the components, and what goes on the SD
card -- with an estimate of the LUTs and block RAMs it takes, against
the board's FPGA. What it makes is a target spec, like the release's own
(`release/targets/*.spec`, [releases.md](releases.md)), so it is built,
checked and released the same way.

```
make config                        the menus (also `make menu-config`, `make menuconfig`)
make config TARGET=sergei_ml2      the menus, starting from that target
make config-report TARGET=lakritz_console   the estimate, block by block, no menus
```

## The menus

- **Target name**, **Description**: the spec's file name and its line in
  the release notes.
- **Start from a target**: a release target's board, PMODs and defines
  as the starting point.
- **Board**, **PMODs**: from `release/hw/boards` and `release/hw/pmods`.
  A PMOD goes in a named port (`langkatze@a`).
- **Profile**: desktop, console or headless ([profiles.md](profiles.md)).
- **CPU and caches, Graphics, 3D, Memory, Audio, USB and input,
  Network, Storage and I/O, Security and system**: the components.
  `[*]` on, `( )`/`(*)` one of a group (the CPU, the USB, the network),
  `<8>` a value (Enter steps through them). A dim entry needs pins the
  board does not have: it can be left out, not added (a PMOD can bring
  the pins).
- **Card: app sets**, **Card: packages**: what the SD card made with it
  holds -- the sets of [update.md](update.md) ("Sets") and the ask
  packs (documents, the Ark).
- **Other defines**: what the board brings that the catalog does not
  know; they can be changed or removed, and new ones added (`a`).

The status line shows the estimate, `LUT ~23400 / 24288 (96%)`, block
RAMs and multipliers, in yellow from 95% (where place and route gets
slow; release/lib/build.py warns there too) and red past the device.
Under it, the first problem: a component without what it needs (zeitlos32p
without the data cache), one that conflicts with another, chunky without
8 planes or `VRAM_SDRAM`, more than the FPGA has.

Keys: arrows, Enter or Space to choose, Esc back, `s` save, `b` build,
`m` measure, `r` the estimate block by block, `?` help, `q` quit.

## Save, build, measure

- **Save** writes `release/targets/<name>.spec`: the board, the PMODs,
  and the defines as changes from them (`-USB_HOST`, `USB_HID`,
  `NO_TRNG`, `T2_TEX_LOG2=13`), with `card_sets` and `card_packs`
  for the card. It is loaded back at once to check it composes;
  `./release/zrelease check` checks it against `rtl/boards.vh` like any
  target. A release target is not overwritten without asking.
- **Build** saves, then runs `zrelease build <version> --targets <name>
  --no-sdcard --allow-dirty` (the bitstream and the flash image, in
  `release/dist/<version>/`), and/or `zrelease sdcard --sets ... --out
  images/<name>.img` with `ZEITLOS_ASK_PACKS` set to the packs.
- **Measure** saves, then synthesises the target as a release build
  does and packs it (`tools/zconfig/measure.py`, 10-25 minutes): the
  real TRELLIS_COMB and DP16KD, on the status line after.

## The estimates

`tools/zconfig/catalog.cfg`: each component's define, menu, what it
needs and conflicts with, and its LUTs, block RAMs and multipliers. ECP5
only for now; other families build and save, without numbers.

They come from per-block synthesis -- `tools/zconfig/measure.py
<target> --modules` lists each of sysctl's blocks, two or three minutes
-- scaled to what a release build packs (a block synthesised on its own
is not optimised across its edges, so it comes out larger), and are
checked against whole builds:

| target | estimate | packed (yosys 0.70 `-abc9`, nextpnr's packer) |
|---|---|---|
| sergei_ml2 | 32,880 LUT, 69 BRAM, 15 DSP | 33,043, 69, 15 |
| sergei_ml2_console (zeitlos32p, T2 with textures) | 31,030, 79, 18 | 30,444, 80, 19 |
| lakritz_console | 21,300, 56, 16 | 22,685, 56 |

The scale is 0.78: sergei_ml2's blocks alone come to 41,800 and
sergei_ml2_console's to 39,900, against 33,043 and 30,444 packed. A
sum of parts is within about 10% of a real build (6% low on
lakritz_console, whose SDRAM-side blocks were measured on DDR3's). When a build is
measured, the catalog is where its numbers go.

```
python3 tools/zconfig/measure.py <target>              the real totals (synthesis and packing)
python3 tools/zconfig/measure.py <target> --modules    block by block
python3 tools/zconfig/measure.py --from output/<dir>   a --keep'd --modules run's table again
YOSYS=yowasp-yosys NEXTPNR=yowasp-nextpnr-ecp5 ...     with the YoWASP tools
```
