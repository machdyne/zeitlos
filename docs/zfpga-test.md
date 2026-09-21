# zfpga on a board: the first test

How to build bitstreams **on a Lakritz, with zfpga**, and boot them.

**Result, first run: passed.** `blink.v`, built on the Lakritz by
`zfpga build`, peak memory 3,152 KB, blinks the LED when booted from an
MMOD (`docs/zfpga.md` section 23). The procedure is kept for re-testing
after changes, and for other boards.

You need: a Lakritz running Zeitlos, its sdcard, **a second MMOD** (the
2 MB NOR module the Lakritz ships with, or any SPI NOR MMOD), and a Pmod
socket the `mmod` app can use (`docs/mmod.md`).

---

## 1. Install

`zfpga` needs **a new kernel** -- `sw/os/kernel.h` gives it the 4 MB
memory tier; without it zfpga stops at once with `out of memory` -- so
Zeitlos has to be rebuilt and reflashed, not just the card.

**The simple way: a release build**, which makes both images with
everything in place:

```
release/zrelease check
release/zrelease build <version>
```

(`docs/releases.md`.) Flash `release/dist/<version>/zeitlos-lakritz_<variant>.img`
the way you normally do, and write `zeitlos.img.gz` to the sdcard.

**Or by hand, keeping your card.** Rebuild and reflash Zeitlos (the
kernel changed), then build the app and copy these onto the card:

```
make -C sw/apps/zfpga                    # zfpga.bin, and db/ laid out as /fpga
```

| On the card | From the tree |
|---|---|
| `/apps/zfpga` | `sw/apps/zfpga/zfpga.bin` (renamed: no `.bin`) |
| `/fpga/lfe5u25f.zdb` | `sw/apps/zfpga/db/lfe5u25f.zdb` |
| `/fpga/boards/lakritz.brd` | `sw/apps/zfpga/db/boards/lakritz.brd` |
| `/fpga/boards/lakritz.lpf` | `sw/apps/zfpga/db/boards/lakritz.lpf` |
| `/fpga/examples/*` | `sw/apps/zfpga/db/examples/*` |

`sw/apps/zfpga/db/` *is* `/fpga`: copying its contents across is the
whole of the second row onward.

**Every name on the card is 8.3** -- at most eight characters, a dot,
at most three -- because Zeitlos's FatFs has no long names. A longer
name copied from a PC looks fine there and simply cannot be found on
the Lakritz. The same goes for your own designs: `counter.v` is fine,
`my_counter.v` is not, and `zfpga build` refuses it before starting.

---

## 2. Build the bitstreams, on the Lakritz

Boot, open a terminal, start `posix` early (it and zfpga each want 4 MB,
and the allocator is first-fit), and run these **one at a time**:

```
$ zfpga build /fpga/examples/blink.v -b lakritz
$ zfpga build /fpga/examples/on.zn -b lakritz
$ zfpga build /fpga/examples/empty.zn -b lakritz
```

**Use absolute paths.** posix passes arguments to programs as typed,
without its working directory, so a relative path is looked up from the
root of the card.

**Wait for each to finish** -- the last line is `zfpga: wrote ...` --
before the next. posix's `&&` waits for a program to *start*, not to
finish, so chaining them races.

Each writes `.zl`, `.zn`, `.cfg` and `.bit` beside its input in
`/fpga/examples/`, and says what it is doing as it goes. For `blink.v`:

```
zfpga: board lakritz: LFE5U-25F, CABGA256, pins from /fpga/boards/lakritz.lpf
zfpga: loading the chip database...
zfpga:   database: 3.1 s
zfpga: synth...
zfpga: synthesised 0 LUTs, 24 flip-flops, 1 carry chains (12 cells)
zfpga:   synth: 0.1 s
zfpga: place...
zfpga: placed 0 LUTs, 12 carry cells in 1 chains and 24 flip-flops in 13 slices; ...
zfpga:   place: 6.2 s
zfpga: pnr...
zfpga: routed 49 nets: 99 arcs, 2 iterations, 51 reroutes, 7687 wires searched
zfpga:   pnr: 2.3 s
zfpga: pack...
zfpga:   pack: 1.4 s
zfpga: wrote /fpga/examples/blink.bit in 13.1 s (peak memory 3152 KB)
```

(Times illustrative.) The first line should appear **within a second**
of pressing return. What the simulator predicts, at ~12 MIPS, plus a
database read of 1.5 MB at the 240-600 KB/s `docs/sdcard.md` measured:

| | Instructions | Compute | + database | Total |
|---|---|---|---|---|
| `blink.v` | 126M | ~10 s | 3-6 s | **~15 s** |
| `on.zn` | 19M | ~1.5 s | 3-6 s | **~6 s** |
| `empty.zn` | 19M | ~1.5 s | 3-6 s | **~6 s** |

**Please note the `database:` time and the total** for each. They are
the two numbers the simulator cannot give, and they decide what gets
optimised next.

### If nothing appears

Every error `zfpga` stops on is written to **the serial console** as
well as to posix, beginning `zfpga:`. If the terminal shows nothing
within a few seconds, look there. The one most likely on a first run:

```
zfpga: error: this process has less than 3.3MB of memory; zfpga needs the 4MB tier. ...
```

which means the kernel in flash is not the new one (section 1).

`ls /fpga/examples` should show three `.bit` files of about 99 KB each.

---

## 3. Write one to the spare MMOD

With the spare module in the Pmod socket, open **mmod**:

1. **PORT**: the socket the module is in.
2. **DETECT**. It must report an ID *and* pass the SS check; WRITE and
   ERASE stay disabled until it does.
3. **START** `0`, **LEN** `20000` (hex: 128 KB, more than any of the
   three bitstreams) -> **ERASE**, and confirm.
4. **Open** (titlebar icon) -> `/fpga/examples/blink.bit` -> **WRITE**,
   and confirm. WRITE writes the file's length from START.
5. **VERIFY**. It should report the file's byte count matching.

Start with `blink.bit`: it is the whole chain, Verilog to bitstream on
the machine, and if it blinks every layer beneath it is right.

---

## 4. Boot it

1. Power off.
2. Take **Zeitlos's MMOD** out of the Lakritz's configuration socket and
   put it somewhere safe.
3. Put the **test MMOD** in the configuration socket.
4. Power on.

A bitstream at address 0 of the configuration flash is loaded straight
at power-on -- there is no DFU bootloader on this module, and nothing
else is needed.

| Bitstream | Pass | What it tests |
|---|---|---|
| `blink.bit` | the LED blinks about three times a second: 0.17 s on, 0.17 s off | everything: synthesis, carry chains, placement, routing, packing |
| `on.bit` | the LED is lit -- **or dark**, if the Lakritz LED is active-low; either way steady | IO configuration, one LUT, four hand-written arcs, the tristate tie, the bank voltage |
| `empty.bit` | nothing visible happens | the packer, IDCODE, CRCs: that the device configures at all |

If `blink.bit` blinks, the test passed. One more, for the synthesiser's
hierarchy (`docs/zfpga.md` §24): `zfpga build /fpga/examples/blinkh.v -b
lakritz` builds a blinky from two modules, a parameter and an
`` `include``d header; written to the MMOD and booted, its LED should
blink at **half** `blink.bit`'s rate, 0.35 s on and 0.35 s off. If not, try `on.bit`, then
`empty.bit` -- each strips a layer away, and which one is the first to
work is the most useful thing to know. `empty.bit` shows nothing by
design: it matters only if `on.bit` fails too, as a sign of whether
the device configures at all -- which, without instruments on the DONE
pin, it cannot show you directly.

**To return:** power off, put Zeitlos's MMOD back in the configuration
socket, power on.

---

## 5. Cross-checks worth doing

**The machine's bitstream is the host's.** Copy
`/fpga/examples/blink.bit` off the card, then on the host:

```
cd sw/apps/zfpga
cp db/examples/blink.v /tmp/
./zfpga build /tmp/blink.v -b lakritz -D db
cmp /tmp/blink.bit <card>/fpga/examples/blink.bit
```

They should be identical: the device build matches the host build
under the simulator, and this confirms it on the real machine, SD card
and all.

**A faster boot test, with a cable.** `openFPGALoader -c dirtyJtag
blink.bit` loads a bitstream into the FPGA's SRAM from a host --
volatile, nothing written to any flash, a power cycle brings Zeitlos
back. It skips the MMOD swap and is a good way to try the bitstreams
first, though it puts a host back in the loop.

---

## 6. What to report back

- For each bitstream you booted: blink / lit / dark / nothing.
- The `zfpga build` output for each, and how long each took.
- Whether `cmp` in section 5 matched.
- Anything that refused, with its message.

`docs/zfpga.md` §20 records what the tests established so far; this is
what closes it.
