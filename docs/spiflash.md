# The flash controller

`rtl/spiflash.v` reads the configuration flash through a memory-mapped
window, as `rtl/spiflashro.v` did before it, and **erases and programs**
it through a small register block -- in hardware, and never below
`0x040000`, where the DFU bootloader lives.

It is step 2 of `docs/zboot.md` section 5: what the jumploader, user
gateware and self-upgrade are written with.

**Status: built, simulated against a W25Q16 model, awaiting a hardware
test** (`flashtest`, below).

---

## The map

| Address | |
|---|---|
| `0x1000_0000` - `0x1EFF_FFFF` | the flash, read-only, flash offset = the low 24 bits |
| `0x1F00_0000` - `0x1F00_00FF` | registers |
| `0x1F00_0100` - `0x1F00_01FF` | the page buffer: 256 bytes, write-only (reads 0) |

The window is exactly what it was: every 32-bit read is one `READ`
(`03h`) transaction, little-endian, SPI mode 0, one bit per two clocks.
The BIOS, the boot logo and the flash apps read it unchanged. Two
differences: a **write to the window is acknowledged and ignored** (the
old controller never acknowledged it, which hung the bus), and a read
that arrives **while an erase or program is running waits** for it to
finish -- up to 400 ms for a 4 KB erase -- rather than returning
garbage. Software should not read the flash while writing it (the
kernel keeps flash apps from launching, below), but if something does,
it gets the right answer late rather than a wrong one.

`0x1F00_0000` was flash offset 0, aliased, on the old controller:
check `Z_FEATURE2_FLASHW` (FEATURES2 bit 6, `docs/csrs.md`) before
touching the registers, and then `MAGIC`.

## Registers

| Offset | Register | |
|---|---|---|
| `0x00` | MAGIC | `0x5A46_4C53` (`"ZFLS"`) |
| `0x04` | STATUS | bit 0 busy -- an operation, or the ID read after reset; bit 1 done; bit 2 refused, locked region; bit 3 refused, not armed; bit 4 refused, crosses a page or bad LEN; bit 5 refused, busy; bits 15:8 the flash's own status register, as last polled. Bits 5:1 clear when a command is written. |
| `0x08` | ID | the JEDEC ID, `{8'h00, manufacturer, type, capacity}`: `EF 40 15` on a W25Q16. Read at reset; valid once busy clears. |
| `0x0C` | LOCK | `0x040000`: the end of the locked region |
| `0x10` | ADDR | flash offset for the next command |
| `0x14` | ARM | write `0x5A46_5752` (`"ZFWR"`), whole word, to allow **one** erase or program. Anything else disarms. |
| `0x18` | CMD | `1` erase the 4 KB sector containing ADDR; `2` program LEN bytes from the page buffer at ADDR; `3` read the ID again |
| `0x1C` | LEN | 1 to 256; ADDR + LEN must stay within one 256-byte page |

The page buffer's byte *i* is programmed at ADDR + *i*: word *i*/4,
byte lane *i*%4, little-endian, as the window returns it.

## Why it cannot erase the bootloader

- **There is no raw SPI.** The controller knows four command sequences --
  read, erase a 4 KB sector, program a page, read the ID -- and the
  `WREN` and status polling they need. It sends nothing else: no chip
  erase (`C7h`/`60h`), no block erase, no status-register write. The
  test bench's flash model counts any other command it receives; it
  must stay at zero.
- **The lock is checked before any pin moves.** An erase or program
  touching `0x000000`-`0x03FFFF` sets a refusal bit and nothing reaches
  the flash, whoever asked and with whatever key.
- **Arming is one-shot.** One whole-word store of the key allows one
  operation, so a stray store cannot start one.

`LOCK_END` is `0x040000` on every board: the gateware is the same with
or without a DFU bootloader, so it cannot know which it is. On a board
flashed without one, that region is the start of Zeitlos's own
gateware, which then cannot be rewritten from Zeitlos either -- which
is also why self-upgrade belongs to boards with DFU (`docs/zboot.md`
section 5).

What this does not cover is **another bitstream**: user gateware booted
through the jumploader has the flash pins to itself. `docs/zboot.md`
section 7 has why the chip's own block protection is not used for that.

## Writing from software

Apps use `Z_SYS_FLASH` through `sw/common/zflash.h`; the kernel owns the
registers.

```c
if (!z_flash_begin()) return;                 // one writer at a time
if (z_flash_erase(off) == 0)                  // started, or refusal bits
    while (z_flash_status() & Z_SPIFLASH_BUSY) yield_somehow();
z_flash_program(off, buf, 256);               // one page at a time
while (z_flash_status() & Z_SPIFLASH_BUSY) yield_somehow();
z_flash_end();
```

- **A session.** Erase and program are refused outside one, and only
  one process may hold it. A process that exits or is killed releases
  it (`k_flash_release_pid`, called where its file handles are
  released).
- **No flash-app launches while a session is open.** `zar.c` refuses:
  the archive the app would be copied from may be half-rewritten. Apps
  already running are in SDRAM and unaffected.
- **Nothing waits.** Erase and program start and return; poll the
  status from an event loop -- a 4 KB erase is 45 ms typically, a page
  under 3 ms.

## Testing it on a board

From the serial shell:

```
flash        JEDEC ID, size, the lock, the status
flashtest    erase and program sector 0x1FF000, and read it back
```

`flashtest` uses the last 4 KB of the first 2 MB: inside the jumploader
region, but past its end -- a 25F jumploader ends at `0x1E8530`, a 45F
one would end at `0x1F7958`. It
refuses to erase that sector unless it is blank or holds its own
pattern from an earlier run. It checks, in order:

1. the refusals, which send nothing to the flash: an unarmed command,
   an erase of sector 0, an erase of `0x03F000`, a program at
   `0x03FF00`;
2. erasing the sector, and that it reads blank;
3. a full page, and an odd, unaligned 13-byte run -- the kernel's byte
   packing -- read back through the window, neighbours untouched.

It ends `flashtest: passed`, or lists what failed.

## In simulation

```
cd rtl/tests && make spiflash
```

`tb_spiflash.v` runs the controller against `w25q16_model.v`, a
behavioural W25Q16 with three tripwires -- any command but the six it
may send, any erase or program aimed below `0x040000`, any command but a
status read while the chip is busy -- and 28 checks: the ID from reset,
window reads, an ignored window write, every refusal, erase and program
read back through the window, a window read held up by an erase, the
pins released when idle. It ends with the tripwires at zero.

The checks were themselves checked by breaking the controller three
ways -- no lock, no arming check, reads not waiting for an erase -- and
seeing each caught: ten failures without the lock, including the
tripwire for an erase reaching the bootloader region.

## Cost

Measured on the whole Lakritz SOC, `synth_ecp5`, before and after (this
includes `docs/zboot.md`'s reboot pin, which is a few cells):

| LUT4 | TRELLIS_FF | CCU2C | TRELLIS_DPR16X4 | DP16KD |
|---|---|---|---|---|
| +602 | +127 | +26 | +32 | **0** |

The page buffer is LUT RAM on purpose: Lakritz uses 54 of its 56 block
RAMs, its tightest resource (`docs/usb_host.md`). Reading the buffer
back over the bus would have cost about 600 LUTs more, for a debugging
convenience, so it reads 0.
