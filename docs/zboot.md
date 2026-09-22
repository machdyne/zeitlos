# Multiboot, PROGRAMN and the flash layout

How an ECP5 decides which bitstream to load, how the DFU bootloader on
these boards uses that, and what it would take for Zeitlos to load
gateware it built itself.

**Status: reference, and the design largely built.** Sections 1-4
describe the machinery. Section 5 is the design -- a *jumploader* at
`0x1D0000` -- and ends with the jumploader as built (step 3). Step 1,
`reboot`, is section 6; step 2, the writable flash controller, is
`docs/spiflash.md`, tested on a Lakritz. `zfpga flash` and `zfpga run`
install user gateware and boot it. Still to come: self-upgrade.

---

## 1. Three ways to reconfigure, and only one is reachable

An ECP5 reloads its configuration on exactly three events:

| | |
|---|---|
| **Power-on reset** | VCC, VCCAUX and VCCIO8 rising past their POR thresholds |
| **PROGRAMN** | a falling edge on the pin |
| **REFRESH** | the instruction, issued over JTAG or the SPI *slave* port |

REFRESH is not reachable from the fabric -- there is no internal path
from user logic to the configuration ports. So for a design that wants
to reboot the FPGA into something else, **PROGRAMN is the only
mechanism**, and it requires the pin to be routed back to the FPGA's own
IO so that logic inside can pull it low.

On Lakritz, Obst and Mozart ML1 it is -- `M8`, confirmed -- and that is
not incidental: it is how the DFU bootloader hands off, so the hard
prerequisite for everything below is satisfied on shipping hardware.
The other boards in this tree (Mozart ML2, Sergei, ULX3S) are
unconfirmed, and build without it (section 6).

---

## 2. BOOTADDR: where the next configuration comes from

The ECP5 has no persistent boot-address register that software sets.
Instead, **the bitstream currently being loaded carries the address the
*next* configuration will be read from.**

`ecppack --bootaddr <addr>` does two things:

1. Writes an 8-bit `BOOTADDR` configuration word into the single
   `EFB1_PICB1` tile. The value is `addr[23:16]`, which is why the
   address must be 64 KB aligned and why the reachable range is 16 MB.
2. Sets bit 20 of `CTRL0` -- the multiboot enable flag -- in the
   `LSC_PROG_CNTRL0` command.

`zfpga pack -a <addr>` does exactly the same, byte for byte
(`docs/zfpga.md` §11.3).

That is the whole mechanism. Two consequences follow from it and both
matter:

- **Power-on always reads from address 0.** At power-up nothing has been
  configured, so there is nowhere for a BOOTADDR to come from. This is
  what makes the scheme in section 5 safe: whatever goes wrong, a power
  cycle lands at address 0.
- **BOOTADDR takes effect on the next reconfiguration, not this one.**
  A bitstream cannot redirect its own load.

`ecpmulti` is the tool that chains several images: it rewrites each
image's BOOTADDR to point at the next one and the last one back to 0, so
repeatedly pressing PROGRAMN cycles through them. It also pads with 256
bytes of `0xFF` before each image, which TN1216 attributes to garbage
bytes that SPI flash sometimes emits at the start of a read. Worth
copying rather than reasoning about.

---

## 3. How the DFU bootloader uses it

`machdyne/tinydfu-bootloader`, `boards/lakritz/`. Two lines carry the
whole handoff.

In the Makefile:

```
BOOTADDR = 0x040000    # User image starts at 256KB
...
ecppack --compress --bootaddr $(BOOTADDR) $< $@
```

And in `tinydfu_lakritz.v`, after a five-second timer expires or the
host issues a DFU detach:

```verilog
BB pin_resetn( .I( 1'b0 ), .T( ~user_boot_now ), .O( ), .B( resetn ) );
```

`resetn` is PROGRAMN, at site `M8` on a Lakritz. The bidirectional
buffer drives a hard 0 when `user_boot_now` goes high and tri-states
otherwise -- an open-drain assert. The device sees the falling edge,
reconfigures, and reads from the BOOTADDR that the bootloader's own
bitstream just installed: `0x040000`.

So the shipping boot sequence is:

```
power on
  -> read flash 0x000000        the DFU bootloader
  -> wait 5s (or DFU detach)
  -> assert PROGRAMN
  -> read flash 0x040000        Zeitlos
```

`boardinfo.vh` carries the same number as `USERPART_START`, and its
comment is emphatic about why: `dfu-util` uses one, `ecppack` bakes in
the other, and a mismatch produces a board that accepts a write and then
does not boot, with nothing to say why.

### What Zeitlos packs today

```
ecppack -v --compress --freq 2.4 soc_final.config --bit soc.bit
```

No `--bootaddr`, so Zeitlos's BOOTADDR is 0 and the multiboot flag is
clear. **If Zeitlos asserted PROGRAMN today it would reload the
bootloader**, which would wait five seconds and come back to Zeitlos.
That is a working reboot, and it is available for the cost of one output
pin -- see section 6.

---

## 4. The current flash layout

The whole map on the 2 MB MMOD Lakritz ships with, from
`release/lib/layout.py` (which reads it from the BIOS, `logo.h`,
`zar.h` and the Makefile and cross-checks them):

| Offset | Room | Contents |
|---|---|---|
| `0x000000` | 256 KB | DFU bootloader (uses ~123 KB), on boards that ship one |
| `0x040000` | 704 KB | Zeitlos gateware (960 KB from `0x000000` without DFU) |
| `0x0F0000` | 64 KB | boot logo |
| `0x100000` | 256 KB | kernel |
| `0x140000` | to `0x200000` | core apps (the ZAR) |

The logo, kernel and ZAR offsets are **absolute** and the same with or
without a bootloader: the BIOS and `zar.h` read them from fixed addresses
in the memory-mapped flash window, and one kernel binary runs on every
ECP5 board. Only the gateware's start moves.

The Zeitlos image is 1303 KB of the 1792 KB user partition, so today
`0x190000` onward -- 448 KB -- is unused.

### What Zeitlos already has

`rtl/sysctl.v` instantiates the primitive a flash writer needs,

```verilog
USRMCLK usrmclk0 (.USRMCLKI(CSPI_SCK), .USRMCLKTS(1'b0));
```

and drives `CSPI_SS_FLASH` (site `N8`, the bootloader's `flash_csel`),
`CSPI_MOSI` and `CSPI_MISO`. Until step 2 that was `rtl/spiflashro.v`,
which only read; it is now `rtl/spiflash.v`, which also erases and
programs, never below `0x040000` (`docs/spiflash.md`).

---

## 5. The jumploader: the agreed design

A **jumploader** is a tiny bitstream whose only job is to jump: it waits
a moment, pulls PROGRAMN, and the FPGA loads whatever address the
jumploader was packed with. Zeitlos is packed with the jumploader's
address, so when Zeitlos pulls PROGRAMN, the jumploader runs, and the
jumploader decides where the machine goes next. Changing the
destination means writing a different jumploader -- **Zeitlos's own
image is never rewritten to boot something else.**

### Why not just patch Zeitlos's boot address

It was measured (`zfpga pack -a`, three addresses, compared byte by
byte). In an **uncompressed** bitstream BOOTADDR's eight bits sit at a
fixed place -- one byte in each of four frames, each followed by its own
CRC, 12 bytes within 1 KB. In a **compressed** one, which is how Zeitlos
is packed, the frames are variable-length: changing the address changes
3,514 bytes and the file's length. Patching in place would mean shipping
Zeitlos uncompressed (~450 KB more), or packing it with a zfpga option
ecppack does not have -- and, either way, rewriting a sector of the
running system's own image.

### The layout

Everything that exists today stays where it is. The jumploader takes the
top of a 2 MB flash, **at `0x1D0000` on every board**:

| Offset | Contents | Room | Change |
|---|---|---|---|
| `0x000000` | DFU bootloader (optional) | 256 KB | write-protected, see section 7 |
| `0x040000` | Zeitlos gateware | 704 KB | unchanged; packed with boot address `0x1D0000` |
| `0x0F0000` | boot logo | 64 KB | unchanged |
| `0x100000` | kernel | 256 KB | unchanged |
| `0x140000` | core apps (ZAR) | 576 KB, to `0x1D0000` | limit only (was to `0x200000`) |
| *(run time)* | user gateware | see below | new |
| **`0x1D0000`** | **jumploader** | **192 KB** | **new** |

**Why one address for every board.** A jumploader is an almost-empty
bitstream, and its size is set by the die, not the design -- every frame
costs at least a CRC and a few bits. Compressed, as measured by this
tree's own tests:

| Die | Jumploader | In the 192 KB from `0x1D0000` |
|---|---|---|
| 12F / 25F (Lakritz, 2 MB) | ~99 KB | fits, 93 KB spare |
| 45F (Mozart ML1, 2 MB) | ~162 KB | fits, 30 KB spare |
| 85F (4 MB or more) | ~280 KB | runs past `0x200000` -- no 85F board has only 2 MB |

One address means no per-board constant, no register to report it, and
the code that writes jumploaders can be part of the one kernel and one
set of apps every ECP5 board runs. A smaller jumploader (per-frame CRCs
dropped; or a truncated bitstream, untested) would only leave the region
with more slack. Nothing would move.

**User gateware has no fixed slot.** It lives wherever there is room, and
`zfpga boot` chooses at run time:

- on a 2 MB module, between the ZAR's actual end (rounded up to 64 KB)
  and `0x1D0000` -- about 296 KB today, shrinking as the apps grow; the
  apps take room from user gateware, never the reverse;
- on 4 MB and larger, above the jumploader's end, where it never meets
  the apps at all.

A blinky is ~99 KB on a 25F and ~162 KB on a 45F, so every standard
board has room for at least one design.

### The cycle

```
power on               -> 0x000000   DFU bootloader (or Zeitlos, without one)
  PROGRAMN (bootloader) -> Zeitlos
  PROGRAMN (Zeitlos)    -> 0x1D0000   jumploader
  PROGRAMN (jumploader) -> its target:
                             0x000000  the default jumploader: a reboot
                             anywhere  a zfpga-made jumploader: user gateware
  PROGRAMN or power     -> 0x000000   and back to Zeitlos
```

**The default jumploader points at 0**, so `reboot` is: make sure the
default is in place, pull PROGRAMN. The release image carries it, built
on the host by `zfpga` -- `reboot` never needs the 1.5 MB chip database
on the card.

**Anything user gateware does ends at a power cycle.** A generated
bitstream carries boot address 0 unless it is packed otherwise, so a
reset, a hang, a failed CRC or a design that never raises DONE all come
back the same way, with no cable and no host.

### Chains: gateware that jumps on

Every bitstream carries its own next address, so gateware can jump to
more gateware: jumploader -> gateware #1 (packed with `-a B`, and pulling
PROGRAMN itself) -> gateware #2 at `B`. It is `ecpmulti`'s mechanism, and
the bootloader-to-Zeitlos hop is the same thing. Two consequences: a
design packed with an address sends its own resets there rather than to
0 (a power cycle still returns to 0), and addresses must be 64 KB
aligned and below 16 MB. `zfpga pack -a` already writes them.

Every image `zfpga boot` writes gets **256 bytes of `0xFF` in front**, as
`ecpmulti` does, for Lattice's documented reason: SPI flash can return
garbage at the start of a read.

### Self-upgrade and restore

The same writer can rewrite Zeitlos's own regions while it runs from
RAM: an upgrade, or a restore from a backup on the card. With the
bootloader write-protected, a power cut in the middle is recoverable
over DFU from a host. **Without a bootloader** there is no such net --
Zeitlos is at address 0, and a half-written image needs JTAG or an MMOD
swap -- so self-upgrade should be offered only on boards that ship DFU.
Any rewrite of the ZAR should also restore the default jumploader, so a
stale one never points into what is now app data (and if one did, the
result is a failed configuration and a power cycle, not a brick).

### What has to be built

| Step | | Status |
|---|---|---|
| 1 | **PROGRAMN and `reboot`.** With Zeitlos's boot address still 0, PROGRAMN alone reboots. It proves the pin while the flash is untouched. | **built**, section 6 |
| 2 | **A writable flash controller**: `WREN`, page program, 4 KB sector erase, status polling, and the write lock of section 7 -- `rtl/spiflash.v`, replacing `spiflashro.v`, and `Z_SYS_FLASH` in the kernel. Written in-house; the bootloader's `usb_spiflash_bridge.v` is Apache 2.0. | **built**, `docs/spiflash.md`; **`flashtest` passes on a Lakritz** |
| 3 | **The layout and the jumploader**: `layout.py` learns the jumploader region and caps the ZAR at `0x1D0000`; Zeitlos is packed `--bootaddr 0x1D0000`; release images carry the default jumploader; `reboot` and `jump` re-point it in place; `zfpga boot` writes user gateware. | **built** -- "The jumploader, as built", below, and `zfpga flash` / `run`; needs a hardware test |
| 4 | **Self-upgrade and restore**, on step 2's writer. | |

### The jumploader, as built

**The patchable format.** In a compressed bitstream a frame's length
depends on its bytes, so a different boot address would move everything
after it (section 5, "Why not just patch"). `zfpga pack -J` changes two
things: the frames holding BOOTADDR's eight bits are encoded with the
literal code only -- ten bits a byte, whatever the byte -- and are left
out of the dictionary's histogram. Each compressed frame is byte-aligned
and carries its own CRC, and the CRC restarts after each one, so
changing an address bit touches only its frame and that frame's CRC.
The header then gains a comment line the FPGA skips:

```
ZJUMP1 b=0169b9.80,01694b.80,... c=01669b.0166fa,016709.016768,...
```

-- for each address bit, the stream byte and mask where it sits (as
itself, in the literal code); for each patched frame, the first byte its
CRC covers and where the CRC is. Offsets count from the preamble, so the
header's own length does not matter. On a 25F the eight bits are in
eight frames, and a jumploader is 99,631 bytes -- 871 more than
ecppack's shortest encoding, well inside the 192 KB region.
`docs/zfpga-formats.md` section 10.

**Making one.** `zfpga jump TARGET -b BOARD`: the design is one line of
Verilog -- `assign PROGRAMN = 1'b0` -- built like any other against the
board's pin constraints, which name PROGRAMN (section 6), and packed
`-c -a TARGET -J`. On the machine it takes about 5 s.

**Re-pointing one.** `sw/common/zjump.c` reads the ZJUMP1 line, reads
the current target from the eight bits, and sets a new one: flips the
bits, recomputes the CRCs. The kernel (`sw/os/jumpapi.c`) gives it the
flash: the one or two 4 KB sectors the change touches are loaded into
RAM, patched there, erased and programmed back (blank pages skipped),
and the jumploader is read again to check. No chip database, no stored
copy.

**Reboot and jump.** On a board built with the Makefile's `JUMP`
(Lakritz and Obst; FEATURES2 bit 7), Zeitlos reloads from `0x1D0000`
when it pulls PROGRAMN. So `reboot` points the jumploader at 0 and then
pulls it; `jump ADDR` (serial shell, `posix`; `z_jump()` for apps)
points it at ADDR. Each refuses, having changed nothing and said why,
if the gateware cannot pull PROGRAMN, if it reloads through a
jumploader and none is there -- PROGRAMN would then reload into
nothing -- or if the re-pointing fails; `jump` also refuses on gateware
built without `JUMP`, which can only reboot. `jump` with no address
shows where the jumploader points.

**The build.** `make jumploader` makes `output/BOARD/jump.bit` with the
host zfpga; `make flash_jump` writes it at `0x1D0000`, and `make flash`
includes it. Releases carry it in the `.img` and the DFU image and ship
it on its own as `zeitlos-<target>-jump.bit`; the release build refuses
an image for a jumploader board without one (`docs/releases.md`,
"Flashing the parts separately"). And
`release/lib/layout.py` now has the region, checks that `zsoc.h` and the
Makefile agree on its address, and ends the ZAR at `0x1D0000`.

**Boards.** Lakritz, Obst (a 12F: the 25F's die, and its database),
Mozart ML1 and Sergei ML1 (45F; the 45F database is vendored for them).
A 45F jumploader is 162,793 bytes, and ends at `0x1F7BE9`, inside the
region. Sergei ML1's PROGRAMN pin, `M8`, is inferred rather than
confirmed: it carries the same ML1 module as Mozart ML1 -- 68 of Mozart
ML1's 72 pin assignments, clock, flash and SDRAM among them, are the
same on Sergei -- and Mozart ML1's `M8` is confirmed.

**Checked.**

- Patched in place, a jumploader is byte-identical to one packed for the
  new target: all 16 pairs of four targets.
- `ecpunpack` reads every jumploader -- it checks every CRC, and fails
  on a stale one -- and finds the same configuration ecppack writes,
  boot address included.
- The kernel's own code, on the host against a simulated W25Q16 that
  keeps NOR rules and the lock (`tests/kjump.c`, 22 checks): re-pointing
  gives zfpga's jumploader byte for byte, changes nothing else, and back
  again restores the whole flash -- once within one sector, once
  shifted across a sector boundary.
- The machine's own zfpga makes the host's jumploader, byte for byte.

**Testing it on a board.** Flash the new build (gateware, kernel, apps,
and `make flash_jump`). Then, from the serial shell:

1. `jump` -- expect "this gateware reloads through the jumploader" and
   "points at 0x000000 (a reboot)".
2. `reboot` -- the machine goes through the jumploader, the DFU
   bootloader, and back to Zeitlos.
3. To boot other gateware, from `posix`: `zfpga build
   /fpga/examples/blink.v -b mozart_ml1` (or `-b lakritz`), then `zfpga
   run blink.bit`. It is written after the core apps, the jumploader is
   pointed at it, and the LED blinks; power-cycle to return. `zfpga run`
   again rewrites nothing. `zfpga flash` writes without booting; `-a
   ADDR` chooses the address, anywhere after the jumploader on a flash
   larger than 2 MB.

**A hazard.** A power cut between erasing a jumploader sector and
programming it back leaves the jumploader broken. The machine still
comes back -- power-on is address 0 -- but `reboot` and `jump` then
refuse until `make flash_jump` (or a release image) rewrites it.

---

## 6. Reboot (built)

Bringing PROGRAMN out gives Zeitlos a reboot with no flash writing at
all, and it is the first step because it exercises the risky half in
isolation: a misrouted pin is a board that does not come back, and it is
better found while the flash is untouched.

**The pin.** `M8` on Lakritz, Obst and Mozart ML1, wired to PROGRAMN on
each, as the DFU bootloader drives it. `rtl/boards.vh` defines
`PROGRAMN_PIN` for exactly those three; `rtl/sysctl.v` then has an
`inout PROGRAMN` port, driven open-drain through a `BB` -- a hard 0 when
asked, tri-state otherwise, tri-state from power-on. Boards whose site
has not been confirmed (Mozart ML2, Sergei, ULX3S) do not define it and
build exactly as before.

**The register.** `rtl/socctl.v` word 6, `RECONFIG`, at `0x7000_0218`.
Writing the key `0x5A52_4254` ("ZRBT") as one whole-word store pulls
PROGRAMN; any other value, or a partial store, does nothing, so a stray
write cannot reset the machine. It reads back `{ 0x5A52, 15'b0, avail }`,
and on a board without the pin the request is forced off in hardware.
`rtl/csrs.v`'s FEATURES2 bit 5 (`Z_FEATURE2_RECONFIG`) says whether this
bitstream can do it (`docs/socctl.md`, `docs/csrs.md`).

**The syscall.** `Z_SYS_REBOOT`, appended to `syscalls.def`. The kernel
(`k_reboot`) checks the feature bit and the register's signature, flushes
every open write handle of every process (`k_fs_sync_all` -- safe here,
in a syscall, unlike the interrupt-path cleanup of a killed process),
writes the key, and -- if the FPGA is somehow still running half a second
later -- says so and fails, rather than pretending. Apps call
`z_reboot()`. **Kernel and apps must be rebuilt and flashed together**
after this change, as after any change to `syscalls.def`.

**The commands.** `reboot` in the kernel's serial shell and in `posix`.
On a board without the pin, both say so and do nothing. On a board
built with `JUMP` (section 5, "The jumploader, as built"), `reboot`
goes through the jumploader, pointing it at 0 first.

### Testing it on a board

1. Build and flash the new release (kernel, apps and gateware together).
2. From `posix`, or the serial shell: `reboot`.
3. Expect `reboot: N open files synced; reconfiguring` on the serial
   console, the machine going dark, the DFU bootloader's five seconds,
   and Zeitlos again. On a board flashed without a bootloader, Zeitlos
   comes straight back.

If instead it prints `PROGRAMN was asserted but the FPGA did not
reconfigure`, the pin is not wired as `boards.vh` says -- please report
which board.

---

## 7. Hazards

### The system is running out of the flash it would be writing

Core applications (`wm`, `term`, `net`) are read through the
memory-mapped window (`docs/flash_apps.md`), and NOR flash cannot be read
while a program or erase is in progress. So a write must guarantee that
**nothing touches the ROM window for its duration** -- no app launch from
flash, no `mmod` ROM source. The kernel and running apps are in SDRAM and
are fine; the window between "erase started" and "status says ready" is
hundreds of milliseconds per sector.

`docs/filesystem.md`'s `k_no_preempt` deferral is capped at 64 ticks
(~87 ms), so a sector erase cannot simply be held inside one. The shape
that works is `sw/apps/mmod`'s: a chunked state machine, one sector per
pass of the event loop, with a flag the flash-app loader honours.

### Never write the bootloader

`docs/mmod.md`'s rule: there is no read-only failure mode. Overwriting
the bootloader means the board does not enumerate, does not boot, and
needs JTAG.

**Zeitlos's flash controller makes it impossible** (`docs/spiflash.md`):
it knows only four command sequences, has no raw SPI path, and refuses
any erase or program below `0x040000` before a pin moves. No software on
Zeitlos -- no bug, no key, no stray pointer -- can get past that. It is
the protection, and the flash chip's settings are not part of it.

What it cannot cover is a *different bitstream*: user gateware booted
through the jumploader has the flash pins to itself. The 2 MB MMODs
carry W25Q16s, whose block-protect bits could guard the bottom 256 KB
(TB = 1, BP = `011`, the bottom eighth), but they are **deliberately not
used**:

- they can be cleared again by software unless the chip's `/WP` pin is
  held low, which a module socket does not do -- so they stop accidents,
  not a design that means to write;
- they would stop the DFU bootloader updating *itself*, which
  `docs/dfu_upgrade.md` supports, unless the bootloader learned to clear
  them first;
- and user gateware only writes flash if someone wrote a flash writer
  into it.

### The alignment is load-bearing twice

BOOTADDR carries only `addr[23:16]`, so every target is 64 KB aligned;
and the jumploader's address appears both where Zeitlos is packed
(`--bootaddr`) and where the writer puts bytes. `layout.py` should check
the two agree, as it checks the other four copies of the map.

---

## 8. The MMOD path, which needs nothing

There is a second way to close the loop, and it works today with no new
RTL, no new partition and no PROGRAMN:

The configuration flash on these boards **is** an MMOD -- Lakritz ships
2 MB of NOR on a user-replaceable module -- and `sw/apps/mmod` can
already write an MMOD plugged into the Pmod socket, with DETECT, the
chip-select interlock, ERASE, WRITE and VERIFY, all chunked.

So:

```
build a bitstream on the machine
write it to a second MMOD in the Pmod socket   (mmod, today)
power down, swap the modules
power up
```

And to get back, swap the Zeitlos module in again. With two modules the
loop is closed with no host PC at all, and the "return after a power
cycle" property is physical rather than configured.

This is worth taking seriously rather than treating as a fallback:

- It is available **now**, which means the zfpga work can be validated
  on real hardware long before section 5 is built.
- It cannot brick anything. The Zeitlos module is never written.
- A module written this way is a complete, standalone boot image, which
  is a more useful artifact than a slot inside somebody else's layout.

Its cost is a power cycle and a physical swap, which is exactly the cost
section 5 removes. **They are complementary, not alternatives**, and the
MMOD path should be the one the first working bitstream is tested
through.

One thing to note: a bitstream written to a bare MMOD for use in the
configuration socket goes at **address 0**, not at `USERPART_START`, and
carries no DFU bootloader at all unless one is written too. A module
prepared that way boots straight into the user gateware and has no
recovery path except reprogramming the module -- which is fine, because
the recovery path is the other module in your hand.

---

## See also

- `docs/zfpga.md` — the tools that would produce the bitstream this
  loads
- `docs/mmod.md` — the MMOD app, the chip-select interlock, and the
  write path's refusals
- `docs/dfu_upgrade.md` — the current partition layout and how to change
  it
- `docs/flash_apps.md` — what is in the boot ROM, and what reads it
- `machdyne/tinydfu-bootloader` — `boards/lakritz/`, the reference for
  every mechanism above
- Lattice FPGA-TN-02203 — Dual Boot and Multiple Boot Feature
- Lattice FPGA-TN-02039 — ECP5 sysCONFIG Usage Guide
