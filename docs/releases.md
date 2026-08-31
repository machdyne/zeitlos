# Releases

How a Zeitlos release is built and published.

A release is one flashable image per hardware configuration, plus a
shared sdcard image. Someone with a supported board runs one command and
has a working system; they build nothing.

Everything lives in `release/`. The rest of the tree is untouched except
for five small hooks, listed under [Changes outside
`release/`](#changes-outside-release) at the end.

---

## Quick start

```
$ release/zrelease check                 # before anything else
$ release/zrelease build v0.0.3          # into release/dist/0.0.3/
  ... flash one, boot it, look at it ...
$ release/zrelease ship v0.0.3           # create or update on GitHub
```

`make -C release build VERSION=v0.0.3` does the same if that fits the
hand better. `release/zrelease --help` lists everything.

Build and ship are separate commands and neither implies the other.
Building is slow, needs the FPGA toolchain, and produces something you
want to program onto a real board before anyone else sees it.
Publishing is a network operation that is awkward to take back. `ship`
never builds: if `dist/<version>/` is missing it says so rather than
helpfully rebuilding something nobody has looked at.

---

## What comes out

```
release/dist/0.0.3/
  zeitlos-lakritz_uart.img          gateware + logo + kernel + core apps
  zeitlos-lakritz_langkatze.img
  zeitlos-mozart_ml1.img
  zeitlos-sergei_ml1.img
  zeitlos-<target>-gateware.bit     the pieces, for partial reflashes
  zeitlos-<target>-apps.zar
  zeitlos-kernel.bin                identical for every target
  zeitlos-logo.bin                  identical for every target
  zeitlos.img.gz                    sdcard image, identical for every target
  README.txt                        what each file is, offline
  MANIFEST.json                     defines, Fmax, utilisation, commit
  SHA256SUMS
  NOTES.md                          the release body (not uploaded as an asset)
```

**Asset names carry the target but not the version.** That makes

```
.../releases/latest/download/zeitlos-lakritz_uart.img
```

a permanent URL that always resolves to the newest release, which is
what lets the project README print a command that stays correct. The
version is the release tag, and it is in `MANIFEST.json`, in
`README.txt`, and reported by the `info` app from the running system —
a filename is the one place it is least useful and most costly.

`zeitlos.img.gz` keeps its old name for the same reason: `v0.0.1` and
`v0.0.2` shipped it and the README already links to it.

`README.txt` is where the full list of boards and configurations lives.
The project README carries one example and a pointer, so adding a target
does not mean editing the front page.

---

## Where things are

```
release/
  zrelease              the CLI
  Makefile              thin wrapper, for `make -C release ...`
  hw/boards/*.spec      board definitions
  hw/pmods/*.spec       PMOD deltas + per-board pin fragments
  targets/*.spec        board + PMODs
  lib/layout.py         flash map, scraped from the tree and cross-checked
  lib/spec.py           spec parsing, composition, derived SW config
  lib/gen.py            generated files and their cleanup
  lib/mkflashimg.py     image assembly and region validation
  lib/mkfatimg.py       sdcard image, via mtools
  lib/build.py          drives the top-level Makefile
  lib/ship.py           gh release create/upload
  lib/notes.py          NOTES.md, README.txt, MANIFEST.json
  lib/selftest.py       end-to-end test with the toolchain stubbed
  dist/<version>/       artifacts (gitignored)

output/releases/<board>/    bitstreams and logs from a release build,
                            kept apart from output/<board>
```

`release/README.md` is a pointer to this file and nothing else. Two
documents describing one system is exactly the drift this system spends
effort preventing everywhere else; put anything worth writing down here.

---

## Targets

A target is a board plus whatever is plugged into it. `lakritz_uart` and
`lakritz_langkatze` are the same FPGA board with different PMODs, and
they need different gateware *and* different software.

Three kinds of file, composed in this order:

| | |
| --- | --- |
| `hw/boards/*.spec` | the board: FPGA, device, `.lpf`, flash command, base defines |
| `hw/pmods/*.spec` | a delta: what plugging this in adds, removes and constrains |
| `targets/*.spec` | a base board plus a PMOD set, plus any final say |

```
# targets/lakritz_langkatze.spec
description = Lakritz + Langkatze SPI Ethernet PMOD
base  = lakritz
pmods = langkatze
```

```
# hw/pmods/langkatze.spec
defines = +SPI_ETH
lpf_fragment.lakritz = langkatze-lakritz.lpf
```

Define operations are `NAME`, `NAME=VALUE` and `-NAME`.

### Why subtraction exists

Additive defines could have come in on the yosys command line. The
reason for a whole spec layer is `-NAME`: a variant is not always a
superset of its base. A PMOD that occupies the console pins means
building *without* `` `UART0 ``, and there is no command-line way to
remove a define. That case is live and is `lakritz_langkatze` — see
[A board without a UART](#a-board-without-a-uart).

### A spec states a hardware fact once

`langkatze.spec` says `+SPI_ETH`. **Nothing anywhere says
`NET_PHY=ENC28J60`** — `spec.py`'s `derive_sw()` works it out, and the
build passes it to `sw/apps/net`.

That is not tidiness. `sw/apps/net/Makefile`'s own header describes what
happens when the RTL define and the C define are set independently and
disagree: undefined `enc28j60_*` at link if you are lucky, and a silent
reversion of the DHCP, NTP and SSH configuration to defaults if you are
not. A value that cannot be set cannot be set wrong.

The same derivation drops `net` from the core app archive entirely on a
target with no MAC — it would build fine and then sit in flash doing
nothing but confusing whoever ran it.

`sw/common/arch.mk`'s `ARCH` is derived the same way, from
`` `CPU_MUL ``/`` `CPU_DIV `` in the universal section of
`rtl/boards.vh`. That file's header is blunt about the consequence of
getting it backwards: rv32im software on an rv32i bitstream makes every
`mul` an illegal instruction.

### Adding a board

Copy an existing `hw/boards/*.spec`, paste in that board's block from
`rtl/boards.vh`, add a `flash_cmd`, and run `zrelease check`. It diffs
the two and fails on any difference, so the duplication is verified
rather than hoped about.

Add a `pmod.<port>` ball map for each PMOD port it has, and a
`targets/*.spec` naming it as `base`. For a complete system with no
PMOD sockets — Mozart, Sergei — there are no ports to declare and the
target spec is three lines.

### PMODs and PMOD ports

A PMOD spec assigns functions to **PMOD pins**, not to FPGA balls:

```
# hw/pmods/langkatze.spec
pins =
	1=ETH_SS   2=ETH_MOSI  3=ETH_MISO  4=ETH_SCLK
	7=ETH_INT
io_type = LVCMOS33
defines = +SPI_ETH
```

A board spec declares its **ports** and which ball each pin lands on:

```
# hw/boards/lakritz.spec
pmod.a =
	1=B11  2=B12  3=B13  4=B14
	7=A11  8=A12  9=A13  10=A14
```

A target plugs one into the other:

```
pmods = langkatze@a
```

So a PMOD is described once and works on every board that declares a
port, and a board with several ports can take several PMODs. `@port` is
optional only when the board has exactly one. Pins 5, 6, 11 and 12 are
ground and power and are rejected if named.

The generator emits constraints for the named pins only, so leaving a
pin out is what guarantees nothing on the FPGA side is ever placed on
it. That is load-bearing rather than tidy: Langkatze's pin 10 is a
50MHz clock output *from* the PMOD, and its pin 8 is the ENC28J60's
active-low reset held up by an external pull-up. Constraining either
would be a fault — two drivers on one net in the first case, a NIC
permanently in reset in the second.

### Taking over a port

**Occupying a port takes every ball in it.** Any base-`.lpf` constraint
landing on one is released before the PMOD's own are added.

That is not a workaround. `boards/lakritz_v0.lpf` puts `UART0_TX` on
B12 and `UART0_RX` on B13, which are PMOD_A2 and PMOD_A3 — so it has
been encoding *"a USB-UART PMOD is in port A"* all along. Plugging a
Langkatze in instead makes that assumption false, and the constraints
carrying it have to go with it.

Which is why `lakritz_uart` regenerates the board `.lpf` exactly:
`usbuart.spec` puts `UART0_TX` back on pin 2 and `UART0_RX` back on pin
3 of the same port. `zrelease check` asserts it, and prints the delta
for every target:

```
lakritz_langkatze    ok -- +5, -2 vs the board .lpf
                          langkatze in port A
lakritz_uart         ok -- identical vs the board .lpf
                          usbuart in port A
```

`lpf_drop = PORT_NAME ...` in a target spec releases a constraint that
is *not* in an occupied port, for the rarer case of a collision outside
the PMOD system.

### Ports must be constrained

nextpnr-ecp5 runs here without `--lpf-allow-unconstrained`, so a
declared port with no `LOCATE` is a hard failure — and releasing a
constraint is precisely how to create one. So the checker parses
`rtl/sysctl.v`'s port list, tracking `` `ifdef ``, and compares it to
the final constraint set:

```
port 'UART0_TX' is declared (rtl/sysctl.v, inside `ifdef UART0) but
nothing constrains it.
    Either give it pins, or add '-UART0' to the target spec so the
    port is not built.
```

nextpnr names the port. It does not know which define would remove it,
or that a PMOD took its ball.

The reverse — a constraint for a port that no longer exists — is
tolerated and not reported. `lakritz_v0.lpf` already has two
(`SD_DAT1`, `SD_DAT2`), so it is clearly accepted, and this tool is in
no position to start rejecting the tree's existing files.

### A board without a UART

`lakritz_langkatze` is the case all of this was built for. Langkatze
needs B12 and B13, so that build cannot also have a serial console, and
its spec says so:

```
defines = -UART0
```

This did not work before. `rtl/sysctl.v` declared `UART0_TX` and
`UART0_RX` unconditionally — the only optional peripheral in that list
that did — and, worse, without `` `UART0 `` there was no `cs_uart0` in
the ack mux, so the mux fell through to `1'b0` and the read in
`sw/bios/bios.c`'s `putchar()`

```c
while ((reg_uart0_lsr & 0x20) == 0);
```

never completed. The CPU stalled on the first character of the boot
banner, before anything reached a screen.

`rtl/uart_null.v` fixes it. Every other optional block here degrades to
*acks, reads zero*; this makes the UART do the same. It reports a
transmitter always ready and a receiver never holding data (`LSR` reads
`0x60`), so every spin-until-ready loop exits on its first read and the
character written afterwards goes nowhere. Output is discarded, input is
empty, and **no software needed changing** — which was the point. A
`Z_FEATURE_UART0` check in the BIOS, the kernel console, the shell and
`uart.c`'s ISR would have been four branches that are dead on every
board anyone owns.

It is deliberately not a loopback: an echoing phantom would make the
kernel shell look like it was working while nothing was connected, which
is worse than silence. Software that wants to *say* there is no console,
rather than merely survive, should check `Z_FEATURE_UART0`
(`sw/common/zsoc.h`, bit 12) — it is clear on such a build.

`rtl/tb/tb_uart_null.v` asserts this against the actual loops from
`bios.c` and `uart.c`:

```
$ iverilog -g2005 -o /tmp/tb rtl/tb/tb_uart_null.v rtl/uart_null.v && vvp /tmp/tb
```

A Lakritz with a Langkatze therefore has HDMI, USB keyboard and mouse,
microSD and networking — a complete machine, just not one you can talk
to over a UART.

## How the gateware gets its defines

`rtl/boards.vh` gained one guard around its per-board `` `ifdef `` chain:

```verilog
`ifdef ZSPEC
`include "zspec.vh"
`else
   ... the existing chain, untouched ...
`endif
```

Release builds write `rtl/zspec.vh` and pass `EXTRA_DEFINES=-DZSPEC`.
Nothing else defines `ZSPEC`, so `make BOARD=lakritz flash` behaves
exactly as it always has.

The **universal** section of `boards.vh` — `` `RTC ``, `` `TRNG ``,
`` `GAME ``, `` `CPU_MUL ``, `` `DEBUG ``, `` `ARBITER `` — stays
outside the guard and specs cannot touch it. Each of those has a reason
in that file for being universal, and none of those reasons is something
a per-target choice would change.

`zrelease check` runs a real Verilog preprocessor over `boards.vh` both
ways and compares the resulting macro sets, so *the spec path builds the
same thing as the board path* is verified rather than asserted. It needs
`iverilog`; without it the check is skipped rather than silently
passing.

---

## The build, step by step

Everything is a `make` call against the **top-level Makefile**. Nothing
in `release/` knows how to run yosys. A release built by a second,
parallel set of build rules is a release that can differ from what
`make BOARD=x flash` produces on a developer's machine, and that
difference would only ever be found from a bug report.

Per target:

1. **Full software clean.** Not an optimisation to remove later. `sw/`
   has no per-board object directories, so the previous target's `.o`
   files are still sitting next to the sources. Switching `NET_PHY`
   changes no `.c` file, so nothing in a dependency graph notices and
   the link happily reuses an `eth.o` built for the other driver.
2. **Wipe `output/releases/<board>`.** `lakritz_uart` and
   `lakritz_langkatze` share a directory, because the Makefile keys it
   off `BOARD` and not off the target. If the second build fails at
   place-and-route, the first one's `soc.bit` is still there looking
   perfectly valid and would be shipped under the wrong name.

   Under `output/releases/` rather than `output/` so that wipe cannot
   take a development bitstream with it. The Makefile gained an
   `OUTDIR` variable for this; it defaults to `output/<board>` and
   nothing but the release build overrides it. `make clean_soc` still
   removes everything under `output/`, releases included.
3. **Gateware and BIOS** (`zeitlos_pico bios soc`). The slow step.
4. **Timing check** — see below.
5. **Kernel**, then **apps** with the derived `NET_PHY`.
6. **ZAR**, then image assembly and region validation.

Then once per release: the sdcard image, `README.txt`, `NOTES.md`,
`MANIFEST.json`, `SHA256SUMS`.

---

## What will stop a build, and why

| Guard | Why | Override |
| --- | --- | --- |
| Version mismatch | `sw/common/zversion.h` says 0.0.2 and you asked for 0.0.3. The version is compiled into the kernel and shown by `info`; shipping these mismatched means the release page and the running system disagree about what it is, discovered from a screenshot months later. | `--bump` (edits the header — commit it) |
| Dirty tree | `MANIFEST.json` would record a commit the artifacts did not come from. | `--allow-dirty` |
| Spec drift | A board spec no longer matches `rtl/boards.vh`, so a release would build something other than what `make BOARD=x flash` builds. | `--allow-drift` |
| Timing failure | A named clock domain missed its target in the **final** report. IO domains are advisory; see below. | `--allow-timing-fail` |
| Unconstrained port | `rtl/sysctl.v` declares a port nothing gives pins to. nextpnr rejects it; this says which define would remove it. | none — pins, or `-NAME` |
| Two ports on one ball | Two PMODs overlapping, or a PMOD pin hitting something constrained outside any port. | none — `lpf_drop`, or relocate |
| Region overrun | An oversized gateware silently eats the boot splash; an oversized kernel eats the start of the ZAR. Both present later as *that feature stopped working*. | none — shrink it or move the region |
| Layout disagreement | The flash offsets in `bios.c`, `logo.h`, `zar.h` and the Makefile no longer agree. | none — reconcile them |

### Timing

The top-level Makefile already greps `pnr.log` for `FAIL at` and prints
a warning, and its comment explains why: a bitstream that missed timing
still programs and still half-works, which is far harder to debug than
one that refused to build.

A warning is the right response during development. For a release it is
not — nobody reads scrollback from a build they did not run, and an
intermittently-misbehaving image is the worst thing to put behind a
download link. So this fails the build. The achieved Fmax and the
utilisation go into `MANIFEST.json` and the release notes either way.

**Only the final report counts.** nextpnr prints Fmax twice — once from
a placement estimate and again after routing with real delays. Reading
the whole log and keeping every match concatenates the two rounds,
showing each clock twice with different numbers and, worse, failing a
release on an estimate that routing then fixed. The parser splits on
repetition (a clock name appearing twice means a new round started) and
keeps the last, which needs no assumption about nextpnr's wording. The
summary says `[final of 2 reports]` when there was more than one.

**IO domains are advisory.** nextpnr names a timing domain after
whatever drives it, so paths that begin or end at a pin get bucketed
under the IO primitive — `TRELLIS_IO_IN` and friends — rather than
under a clock from the design. A `FAIL` there is usually nextpnr
applying a default target to a path that has no meaningful frequency:
there is no PLL to retune and nothing in the `.lpf` asked for it.

Those are recorded in `MANIFEST.json`, printed in the summary, and
called out by name in the build log, but they do not by themselves
block a release. `--strict-io-timing` gates on them too. They are not
silently dropped — a real problem hiding behind one stays visible.

---

## The flash map

Not defined in `release/`. `lib/layout.py` reads it out of
`sw/bios/bios.c`, `sw/os/logo.h`, `sw/os/zar.h` and the top-level
`Makefile`, cross-checks all four, and refuses to do anything if they
disagree. Two consequences worth having:

- The release tool cannot disagree with the running system, because it
  has no numbers of its own to disagree with.
- `zrelease layout` becomes a standing check on the four `KEEP IN SYNC`
  comments those files already carry. Run it after touching any of them.

```
$ release/zrelease layout

  offset    limit      region
  --------  ---------  ------------------
  0x000000     983040  gateware
  0x0f0000      65536  boot logo
  0x100000     262144  kernel
  0x140000     786432  core apps (ZAR)
  0x200000             end of flash
```

Two properties of the assembled image that matter:

**The fill byte is `0xFF`, not zero.** Erased NOR flash reads as `0xFF`
and two pieces of this system test for exactly that: `sw/os/logo.c`
skips drawing if it finds erased flash where the splash should be, and
`sw/os/zar.c` checks for the `ZAR1` magic. Filling gaps with `0x00`
would write real zeros over regions meant to read as erased, turning *no
logo programmed* into *a logo made of black pixels*.

**Images are trimmed, not padded to 2MB.** The last byte written is the
end of the ZAR, around 1.5MB in practice. The remaining half-megabyte is
erased flash either way, so padding it would cost erase and programming
time over JTAG for no change in the result. `--full-image` if some
flashing tool ever wants a literal 2MB.

---

## The sdcard image

Built by `lib/mkfatimg.py`, which is `tools/mkfatimg.sh` with one
change: `mount` + `cp` becomes `mmd` + `mcopy`. Same `mkfs.fat`
arguments, same `fsck.fat`, same files.

`mkfs.fat` has always been happy to format a plain file. It was only
`mount -o loop` that needed root, and that one line forced the whole
release under `sudo` — which in this tree is actively harmful, because a
build running as root leaves root-owned `.o` files scattered through
`sw/` that break every subsequent non-root build. The Makefile's
`tftp-dist` target carries a comment about exactly this hazard.

The shell script stays as the readable reference. `zrelease check`
diffs the two file lists and fails if they drift — which is not
hypothetical, since the committed script and the one actually in use had
already diverged.

**The core apps are deliberately absent from the card.** `sw/os/zar.h`
gives a card copy precedence over the flash copy, so shipping them here
would shadow the flash build — and for `net` that is not academic: the
flash copy is built per target with the right driver, and one shared
card image could only carry one of them.

One card image per release, not per target: everything on it is
board-independent and detects optional hardware at runtime. It is still
per *release*, because `sw/common/syscalls.def` is compiled into both the
kernel and every app.

The ARK scroll comes from `sw/data/ark` by default; `--ark` points
elsewhere. Its commit goes into `MANIFEST.json`.

---

## Generated files, and getting rid of them

A build puts two things into the source tree that do not belong there:

```
rtl/zspec.vh
boards/.zrelease-<target>.lpf     (only for a target with PMOD pins)
```

Both are removed when the build ends, however it ends. Three layers:

1. Normal exit *and* exceptions — the context manager removes them.
2. Hard kill — a manifest at `release/.generated` survives, and the
   **next** `zrelease` command of any kind sweeps before doing anything
   else.
3. `zrelease clean`, on demand.

A tracked file whose contents changed since it was generated is kept
rather than deleted: if you edited it you presumably meant to, and
losing that is worse than leaving a stray file behind. It says so when
it does this.

`--keep-generated` leaves them in place for inspection and prints a
loud reminder to run `zrelease clean` before using the normal build.

The `.zrelease-` prefix on the generated `.lpf` is deliberate: `boards/`
is full of hand-written checked-in constraint files, and a generated one
sitting among them looks exactly like another one of those — which
matters most at the moment it should not be there at all.

---

## Publishing and updating

```
$ release/zrelease ship v0.0.3
```

Creates the release if the tag is new, updates it if not. `--draft`,
`--prerelease`, `--title` and `--notes-only` do what they sound like.

Updating is the common case: a release goes out and one target needs a
fix.

```
$ release/zrelease build v0.0.2 --targets lakritz_uart
$ release/zrelease ship v0.0.2
```

`ship` on an existing tag uploads with `--clobber`, replacing assets by
name and leaving everything else alone. It prints its intent per asset
before doing anything:

```
  replace  zeitlos-lakritz_uart.img
  add      zeitlos-lakritz_uart-gateware.bit
  keep     zeitlos.img.gz  (not built by this run)
```

Because asset names are unversioned, a rebuilt target replaces itself
cleanly and `latest/download/` URLs keep working.

---

## Testing without a toolchain

```
$ python3 release/lib/selftest.py
```

Runs the whole pipeline with the `make` calls stubbed out and plausible
artifacts written in their place. Everything from the generated
`zspec.vh` through image assembly, the manifest, the notes and the
checksums is real code on real data. It checks, among other things, that
a timing failure stops a build, that `--allow-timing-fail` overrides it,
that the ZAR magic and the splash land at the right offsets, that gaps
are left erased, and that `net` is in the archive exactly when the
target has a MAC.

It cleans up after itself. `git status` is unchanged afterwards.

---

## Changes outside `release/`

Five, all small and all commented in place:

| File | Change |
| --- | --- |
| `rtl/boards.vh` | the `ZSPEC` guard around the per-board chain |
| `rtl/sysctl.v` | `UART0_TX`/`RX` ports guarded by `` `ifdef UART0 ``; `cs_uart0` and its mux entries made unconditional; `wbs_uart0_int` declared as a wire outside the guard (it was a `reg` inside it, but read unconditionally) |
| `rtl/uart_null.v` | new — the phantom UART, plus `rtl/tb/tb_uart_null.v` |
| `Makefile` | `EXTRA_DEFINES` on the three synthesis recipes; `OUTDIR` replacing the hardcoded `output/$(BOARD_LC)`, so release builds can be kept out of the way of development ones |
| `sw/apps/Makefile` | rewritten around one `APPS` list — it named `ping` and `pong`, which no longer exist, so `make apps` failed at the third line |
| `sw/common/zversion.h` | new; `Z_OS_VERSION` moved out of `zeitlos.h`, which now includes it |
| `tools/mkfatimg.sh` | header pointing at the no-root path |

`zversion.h` is still hand-maintained and still carries no build date or
git hash, for the reason its own header gives: both would change the
binary on every rebuild, which makes *is the flashed image the one I
just built?* harder to answer, not easier. `--bump` rewrites the line as
an edit you commit; the tool changes it, it does not generate it, so the
value in a checked-out tree is always the value that was built.

---

## Requirements

`yosys`, `nextpnr-ecp5`, `ecppack`, `ecpbram`, a RISC-V toolchain (see
`docs/toolchain.md`), `dosfstools` and `mtools` for the sdcard image,
`gh` for publishing, and optionally `iverilog` for the preprocessor
equivalence check.

```
$ sudo apt install dosfstools mtools iverilog
```

`zrelease check` reports which are missing. Nothing needs root.
