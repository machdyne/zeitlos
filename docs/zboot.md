# Multiboot, PROGRAMN and the flash layout

How an ECP5 decides which bitstream to load, how the DFU bootloader on
these boards uses that, and what it would take for Zeitlos to load
gateware it built itself.

**Status: reference plus proposal.** Sections 1-4 describe what exists
and is shipping today. Sections 5 onward propose changes and have not
been built.

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

On every board in this tree, it is. That is not incidental: it is how
the DFU bootloader hands off, and it means the hard prerequisite for
everything below is already satisfied on shipping hardware.

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

Lakritz and Obst, from `boardinfo.vh` and `docs/dfu_upgrade.md`, on the
2 MB MMOD both ship with:

| Range | Size | Contents |
|---|---|---|
| `0x000000` - `0x03FFFF` | 256 KB | DFU bootloader (uses ~123 KB) |
| `0x040000` - `0x1FFFFF` | 1792 KB | Zeitlos: gateware, kernel, splash, core apps |
| — | 0 KB | `DATAPART`, which is empty on a 2 MB part |

The Zeitlos image is 1303 KB of that 1792 KB. Rounded up to the next
64 KB boundary the free tail starts at `0x190000` and runs to the end of
the device: **448 KB, already 64 KB aligned, already addressable by
BOOTADDR.**

### What Zeitlos already has

This is the part that changes the cost of everything below.

`rtl/sysctl.v` already instantiates the primitive that the bootloader
needs:

```verilog
USRMCLK usrmclk0 (.USRMCLKI(CSPI_SCK), .USRMCLKTS(1'b0));
```

and already drives `CSPI_SS_FLASH` (site `N8`, the same site as the
bootloader's `flash_csel`), `CSPI_MOSI` and `CSPI_MISO` through
`rtl/spiflashro.v`. That block is what serves the memory-mapped ROM
window at `0x1000_0000` -- the one `sw/apps/mmod`'s **SRC ROM** setting
reads for backups (`docs/mmod.md`).

So **the pins, the primitive and an SPI master are all present and
working.** What is missing is narrow: `spiflashro.v` issues read
commands only, and PROGRAMN is not brought out.

---

## 5. Proposed: a third slot, and a safe cycle

The goal is a gateware slot Zeitlos can write and boot into, from which a
power cycle returns to Zeitlos.

### The layout

| Range | Contents | BOOTADDR it carries |
|---|---|---|
| `0x000000` | DFU bootloader | `USERPART_START` |
| `USERPART_START` | Zeitlos | `GWPART_START` |
| `GWPART_START` | user gateware | — (default 0) |

One change to how Zeitlos is packed:

```
ecppack --compress --freq 2.4 --bootaddr $(GWPART_START) ...
```

### The cycle

```
power on            -> 0x000000   bootloader
  PROGRAMN (auto)   -> Zeitlos    (bootloader's BOOTADDR)
  PROGRAMN (by zboot) -> gateware (Zeitlos's BOOTADDR)
  PROGRAMN or power -> 0x000000   bootloader, then Zeitlos
```

**The generated gateware needs nothing at all.** Its BOOTADDR defaults
to 0 and its multiboot flag is clear, so the next reconfiguration --
whether from a reset button, from logic inside the design, or from a
power cycle -- reads address 0, gets the bootloader, and is back in
Zeitlos five seconds later.

That is the property worth designing around: **the failure mode of a bad
generated bitstream is a power cycle.** A design that hangs, that fails
its CRC, that never raises DONE, or that was simply wrong -- all of them
recover the same way, with no cable and no host. Nothing a user builds
can strand the machine, because nothing a user builds is at address 0.

### Where the slot goes

On a stock 2 MB Lakritz MMOD, `GWPART_START = 0x190000` uses the 448 KB
tail of the user partition and needs no repartitioning. That is enough
for a small design -- an almost-empty 25F compresses to roughly 80 KB,
since a zero byte costs one bit -- and tight for a large one.

The better answer is a **larger MMOD**, which is the whole point of the
socket. `boardinfo.vh` already computes `DATAPART` as whatever is left
over, and on a 4 MB or larger module that becomes a real partition with a
name `dfu-util` can see. BOOTADDR reaches 16 MB, which is also the
largest capacity `docs/mmod.md`'s decoder recognises, so the ceiling is
the same from both directions.

### What has to be built

| | |
|---|---|
| **A writable flash controller** | Extend `rtl/spiflashro.v`, or a sibling block, with `WREN`, page program, sector erase and status polling. `usb_spiflash_bridge.v` in the bootloader does exactly this over the same pins -- but it is **Apache 2.0**, not this tree's licence (an earlier revision of this document said otherwise). Use it as a reference for the sequences, which come from the flash datasheet anyway, and write the extension in-house; see `docs/zfpga.md` §9.3. |
| **A PROGRAMN output** | One pin, site `M8` on a Lakritz, currently unconstrained. Open-drain, the same `BB` instantiation the bootloader uses. Held tri-state except when booting. |
| **A driver and an interlock** | `sw/common/zmmod.c` is the model and possibly the code: the command sequences are identical. The interlock is section 7. On the software side this is `zfpga boot` (write and verify the slot) and `zfpga reboot` (assert PROGRAMN), deliberately two commands -- see `docs/zfpga.md` §4.4. |
| **`--bootaddr` in the Makefile** | Per board, matching the layout. Same keep-in-sync hazard the bootloader documents. |

---

## 6. Reboot, and a launcher, are worth having on their own

Bringing PROGRAMN out, with nothing else, gives Zeitlos a reboot: assert
it, land at address 0, come back through the bootloader. No flash
writing, no new partition, no change to how anything is packed.

That is useful independently of any of this -- it is a `reboot` command,
and it is also the thing that makes the DFU bootloader reachable without
a power cycle, which matters on a board in a case.

Add the flash write and the gateware slot, still with no packer and no
synthesis, and the result is a **gateware launcher**: prebuilt
bitstreams on the card -- the LiteX image Lakritz ships for Kakao Linux,
a test image, anything self-contained -- written to the slot and booted
from Zeitlos, with a power cycle to come back. That is arguably the most
immediately useful outcome of everything in this document and
`docs/zfpga.md` §1.1, and it needs none of the toolchain.

Reboot alone is a sensible first step because **it exercises the risky
half in isolation.** If PROGRAMN is misrouted or the buffer is wrong, that is a
board that does not come back, and finding out while the flash is
untouched is considerably better than finding out during a write.

---

## 7. Hazards

### The system is running out of the flash it would be writing

Core applications (`wm`, `term`, `net`) live in flash and are read
through the memory-mapped window (`docs/flash_apps.md`). NOR flash
cannot be read while a program or erase is in progress on the device.

So a write to the gateware slot must guarantee that **nothing touches
the ROM window for its duration** -- no app launch from flash, no
`mmod` ROM source, nothing. The kernel and running apps are in SDRAM and
are fine; it is the flash-resident apps that are the hazard, and the
window between "erase started" and "status says ready" is hundreds of
milliseconds per sector.

`docs/filesystem.md`'s `k_no_preempt` deferral is capped at 64 ticks
(~87 ms), so a sector erase cannot simply be held inside one. The shape
that works is `sw/apps/mmod`'s: a chunked state machine, one sector per
pass of the event loop, with a flag the flash-app loader honours.

### Never write the boot partition

`docs/mmod.md`'s rule, and for the same reason it gives about chip
select: there is no read-only failure mode. Overwriting the bootloader
means the board does not enumerate, does not boot, and needs JTAG to
recover. The write path should refuse any address below
`USERPART_START` outright, as an interlock rather than a warning -- and
arguably refuse anything outside `GWPART` entirely.

### The alignment is load-bearing twice

BOOTADDR only carries `addr[23:16]`, so the slot start must be 64 KB
aligned; and the number appears in the Makefile (as `--bootaddr`) and in
the partition map (as where the writer puts bytes). The bootloader's
`boardinfo.vh` already documents what happens when those disagree.

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
