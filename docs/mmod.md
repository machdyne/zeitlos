# MMOD

`sw/apps/mmod` — identify the SPI memory module in an MMOD socket.

```
> run mmod
```

Prints the JEDEC ID to the serial console and exits. That is all it
does today.

## What MMOD is

[github.com/machdyne/mmod](https://github.com/machdyne/mmod). A 3.3V
SPI memory module, Pmod Interface Type 2 compatible, usable in a 6-pin
or a 12-pin socket. In both cases it occupies pins 1–4, so the pin map
is the same either way and there is nothing to configure.

| MMOD pin | signal | GPIO bit |
|---|---|---|
| 1 | SS | 0 |
| 2 | MOSI | 1 |
| 3 | MISO | 2 |
| 4 | SCK | 3 |
| 5 | GND | — |
| 6 | 3V3 | — |

The spec requires a **10k pull-up on SS** and **none** on the other
three. That pull-up is what holds the device deselected while the FPGA
is being configured and its pins are tri-stated.

## Why identify first, and why it is read-only

Reading the ID exercises every wire at once: SS has to assert and
deassert, SCK has to clock, MOSI has to carry a command byte and MISO
has to bring three bytes back. A plausible ID means all four pins work.

**Writing before the bus is proven is how you lose a module.** If SS
cannot deassert, the device is permanently selected, every clock edge
on the bus is a command byte to it, and a write or erase lands
somewhere nobody chose. There is no read-only failure mode of that. So
this identifies, and load/save/verify comes after the bus is proven.

## The SS check

The app reads the ID a second time **without asserting SS**. A working
device is not listening, so MISO stays tri-stated and reads back as
`ff` — anything but the real ID.

If the ID comes back regardless, the device is permanently selected:

```
mmod: SS IS NOT WORKING -- the device answered with SS deasserted.
```

That is not a hypothetical on Sergei. Its 6-pin MMOD socket puts SS on
ball A13, which also reaches the optical S/PDIF transmitter through a
series resistor. That load holds the pin low against the internal
pull-up — measured: with all four pins as inputs the port reads
`0b1110`, and jumpering pin 1 to ground changes nothing because it was
already there. Whether the FPGA can drive it above the flash's
V_IH (0.7 × Vcc = 2.31V) is the open question, and this check answers
it directly rather than inferring it from a voltage.

**What the check does not prove:** a pass means SS reaches a valid high
while the FPGA is *driving* it. It says nothing about the configuration
window, when the pin is tri-stated and only the module's 10k pull-up
holds SS up against that same load.

## Reading nothing

`00` and `ff` are the two ways of reading nothing and they point at
different halves of the board, so the app separates them:

| ID | means |
|---|---|
| `ff ff ff` | MISO stayed high — nothing driving it. Empty socket, no power, or SS/SCK/MOSI not reaching the module |
| `00 00 00` | MISO stayed low — something is holding it down. Check pin 3 is on GPIO bit 2 and not shorted |

Either way it also prints the raw port state with the bus idle, which
is the same reading the jumper test in `docs/gpio.md` uses.

## Capacity decoding

The third ID byte is `log2(bytes)` on essentially every SPI NOR part,
so `18` is 16MB. The app only decodes `0x10`–`0x1b` and says so
otherwise — a byte outside that range is either a device that doesn't
follow the convention or a misread, and printing "size 2^195" would be
worse than saying nothing.

**FRAM and EEPROM report differently and are not decoded yet.** Many
25-series EEPROMs have no `9F` command at all and will read as `ff ff
ff`, which this app currently reports as an empty socket. That is a
real limitation, not a bug to be surprised by later.

## Configuration

`MMOD_PORT` at the top of `sw/apps/mmod/mmod.c`, currently 0 — the only
port any board builds. `MMOD_KHZ` is 400, modest on purpose: this is
bring-up, the ID is four bytes, and the bit-bang loop won't reach it
anyway (`docs/spi.md`).

The app releases all four pins to inputs on exit. SS in particular:
leaving it driven low would hold the module selected after the app
exits, and leaving it driven high on a board where that is a marginal
level is worse than letting the module's own pull-up do the job it is
specified for.

## The windowed app

`run wm` then `run mmod`. The panel is `sw/apps/mmod/panel.c` (layout
and drawing) and `sw/apps/mmod/mmod.c` (devices and events); the split
exists so `tests/render.c` can draw the panel on the build machine
without the event loop.

DETECT and PROBE work, and the device fields describe what was found —
both go through `z_mmod_*` below, so the panel is only moving values.
Editing CLASS or ADDR pushes back into the profile.

**READ and VERIFY work.** Both run as a chunked state machine advanced
one 512-byte slice per pass of the event loop — so the window keeps
repainting, CANCEL stays live, and wm's redraw acks keep flowing. A
32MB read over bit-banged SPI is minutes; it cannot be a function
call.

The rate and ETA are **measured**, not derived from the requested
clock — the same rule `i2c-khz` follows, and it matters more here
because the two backends differ by more than an order of magnitude.

**CANCEL says which kind of cancel it is.** During READ or VERIFY it
reports that nothing is being written; during WRITE or ERASE it says
that what has already gone stays gone. Both stop between units, so
nothing is left half-programmed — but telling somebody a cancelled
erase left the device untouched would be a comfortable lie.

**WRITE and ERASE work too.** Both confirm through a dialog before
touching anything, and both run as the same chunked state machine —
one 512-byte block per slice for WRITE, one sector per slice for
ERASE, since a sector erase is specified in the hundreds of
milliseconds.

### Files and ranges

The titlebar **open** and **save** icons run the standard file
dialogs (`sw/common/zdialog.h`). There is deliberately **no default
filename** — an earlier version used `mmod.bin`, which is one
character from the app's own executable and reads like something the
user chose. Pressing READ with no destination set opens the save
dialog instead of reporting an error nobody caused; cancelling it
abandons quietly.

The panel shows the base name; operations use the full path.

**START and LEN take typed hex.** Not a cycle of presets: on a 32MB
part the useful ranges are not a short list — testing means reading a
window, moving it, reading again — and a preset cycle would be either
tedious or useless. `0x` is optional.

A length that runs past the end of the device is **clamped rather
than refused**, because the obvious intent is "to the end". The status
line says when that happened, so it is not a silent adjustment.

`ALL` sets the range to the whole device.

`WRITE` and `ERASE` are disabled until DETECT has run *and* the SS
check passed. That is an interlock, not a hint.

### Rendering the panel

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
   sw/apps/mmod/tests/render.c \
   sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
   sw/common/zobj.c sw/common/zeitlos.c
/tmp/render /tmp/panel.pbm
```

Do this before changing the layout. `docs/window_manager.md`,
"Rendering a panel on the build machine", explains what it can and
cannot catch.

## The device layer

`sw/common/zmmod.h` / `.c`, over `sw/common/zspi.h`.

| | |
|---|---|
| `z_mmod_init(m, port, khz)` | pins and profile defaults |
| `z_mmod_detect(m)` | RDID, fills in what it can |
| `z_mmod_ss_ok(m)` | the chip-select check |
| `z_mmod_read/write(m, addr, buf, n)` | |
| `z_mmod_erase_sector(m, addr)` | snaps to the sector |
| `z_mmod_blank_check(m, addr, n, &bad)` | |
| `z_mmod_verify(m, addr, buf, n, &bad)` | |
| `z_mmod_probe_widths(m, addr, o2, o3, o4)` | 16 bytes at each width |

**The profile is the API.** A `z_mmod_t` carries class, size,
`addr_bytes`, page, sector, `needs_erase` and `has_wip`, every field
public and writable, because detection often cannot fill them in and
correcting them by hand is the intended workflow rather than a
fallback.

### Four-byte addressing uses the 4-byte opcodes, not EN4B

`0x13`, `0x12`, `0x21` rather than `EN4B` (`0xB7`). EN4B puts the
device into a mode that **persists after this process exits**, and
after a warm reset on many parts — so a bootloader or another app that
assumes 3-byte addressing would then read from the wrong place, a
failure caused by a program that is no longer running. The 4-byte
opcodes are stateless.

### What the write path refuses

- **Unproven chip select.** `z_mmod_ss_ok()` must have passed.
- **Unerased target**, on a device with `needs_erase`. Writing over
  anything not already erased produces the AND of old and new data —
  a write that reports success and verifies wrong.
- **Anything outside the device**, or crossing its end.
- **A profile that cannot describe the operation** — an address width
  outside 2–4, or an erase on FRAM.

Page splitting is automatic: page program wraps *within* the page
rather than advancing, so an over-long write overwrites its own start.

### VERIFY compares the file, not the range

The question VERIFY answers is "does the device match this file", so
**the file's length is the answer's length**. A range wider than the
file compares only the file and passes; the status line says so when
that happens, and the completion message names the byte count, since
it may be less than the range.

A range *narrower* than the file still compares only the range — that
was set deliberately.

An earlier version read the whole range and reported "file ended after
N bytes; device range is M" as a failure, which invented one out of an
ordinary case. WRITE had always clamped to the file size; VERIFY not
doing the same was simply an inconsistency.

Only an empty or unreadable file is an error, because then nothing was
compared at all.

### Erase snaps to sectors, visibly

NOR flash can only erase whole sectors, so a byte range is a lie
there. The confirmation dialog names the **snapped** range, the byte
count, the sector count, and says explicitly when that is wider than
what you asked for:

```
Erase 00000000 - 00001fff?
8192 bytes, 2 sectors of 4096.
Widened from the range you set, to whole sectors.
This cannot be undone.
```

Widening a range silently is how a neighbouring sector gets lost to a
range that was not aligned.

`ERASE` refuses on FRAM and EEPROM and says why — those are
overwritten in place, so the answer is just to WRITE.

### WRITE does not erase for you

On a device with `needs_erase`, `z_mmod_write()` blank-checks each
block and refuses if it is not erased. The app names that specific
failure — "That range is not erased. ERASE it first." — rather than
surfacing the library's generic mismatch error on a write.

Automatic erase would be the convenient choice and the wrong one: a
write whose range is not sector-aligned would take out the sectors on
either side of it.

The write length is the smaller of the range and the file. Running out
of file ends the write normally rather than as an error, since a range
longer than its source is an ordinary thing to ask for.

## Testing the device layer

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/t \
   sw/common/tests/test_mmod.c sw/common/zmmod.c && /tmp/t
```

46 assertions against a **byte-level SPI flash model** that decodes
opcodes, consumes an address of a configurable width, honours chip
select and write-enable, and reports busy for a few status reads.

The model is deliberately strict: it faults on a program crossing a
page boundary, a program without a preceding WREN, and writing a 1
over a 0 without an erase. Real flash does none of those things — it
silently does something else, which is exactly why they are hard to
find on hardware.

This is a different level from `test_bitbang.c`, which models actual
pins because pin wiggling is what it tests. What `zmmod.c` does is
emit correct command sequences, so the SPI layer is the right seam.
It therefore **cannot catch** anything below `z_spi_xfer()` — clock
modes, bit order, chip-select timing. `test_bitbang.c` covers those.

## Next

Load, save and verify a file against the module — `z_mmod_read/write/erase` on top of `sw/common/zspi.h`, with
status polling and a device table covering NOR, FRAM and EEPROM. Not
before an SS check passes on the board it will run on.

## See also

- `docs/spi.md` — the bit-bang SPI this runs on, including the clock modes
- `docs/gpio.md` — the pins, and Sergei's pin 1 in particular
- `docs/logic_app.md` — for watching the bus while this runs
