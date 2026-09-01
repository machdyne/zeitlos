# Zeitlos SOC Capability CSRs

## Overview

Every board this project targets is different: how much main RAM it
has, whether it has ethernet at all (and if so, which of two very
different backends), whether it has a second USB HID port, an LED,
a VGA output vs. an HDMI-ish one -- all of this varies per board
(`rtl/boards.vh`), and a given bitstream is only built with the
subset of peripherals that board's `` `ifdef ``s turn on
(`rtl/sysctl.v`).

Software running on top of that has historically had to just assume:
`sw/bios/bios.c` hardcoded 1MB of main RAM (correct for Obst, wrong
for Lakritz/mozart_ml1's 32MB, and wrong for anything bigger a future
board might have); `sw/apps/net` picked an ethernet backend at build
time and simply trusted that the board it eventually ran on actually
had it.

CSRs (`rtl/csrs.v`) are a small, always-present, read-only register
block that lets software ask instead of assume: how much main RAM
this specific bitstream was built for, and which optional peripherals
were actually synthesized into it.

## Why this needed a real, positive "present" signal

The wishbone bus this SOC is built on doesn't fault on an unmapped
read. `rtl/sysctl.v`'s top-level `wbm_dat_i` mux has a single
`32'hzzzz_zzzz` default case covering every address nothing else
claims -- reading a register that doesn't physically exist in this
bitstream doesn't trap, doesn't return a predictable sentinel, it
just resolves to whatever that default case happens to synthesize
down to. That means there was no way for software to distinguish
"this hardware genuinely isn't here" from "it's here, and this is
just what it currently reads as" by reading a peripheral's own
registers directly.

CSRs solve this the same way any capability-discovery mechanism has
to: with a fixed, independently-verifiable signal that only reads back
correctly if the discovery mechanism itself is actually present.
`rtl/csrs.v`'s `MAGIC` register (`0x5A45_4954`, "ZEIT") is that
signal -- check it first, always, before trusting `MEM_MB` or
`FEATURES`. If it doesn't match, this bitstream predates `rtl/csrs.v`
entirely, and the honest answer to "does this board have X" is
"unknown", not "no".

## Register map

Word-addressed, base `0x7000_0000` (the first unused top-nibble slot
in `rtl/sysctl.v`'s address decode at the time this was added --
`0x8` is the next one still free):

| Offset | Name       | Meaning |
|---|---|---|
| `0x00` | `MAGIC`    | Fixed `0x5A45_4954` ("ZEIT"). Check this first. |
| `0x04` | `MEM_MB`   | Total main RAM, in megabytes, from `rtl/boards.vh`'s `` `MEM ``. |
| `0x08` | `FEATURES` | Bitmask of which optional peripherals this build actually has -- see below. |

Read-only, no side effects, no clocked state -- `rtl/csrs.v`'s whole
implementation is a handful of combinational `assign`s, same "keep it
simple" spirit as `rtl/debug.v`. Writes are silently ignored, not
errors.

Unlike every other peripheral in `rtl/sysctl.v`, this one has **no**
`` `ifdef `` guarding whether it exists at all -- it's unconditionally
instantiated on every board. Its whole job is to be a reliable way to
ask what else exists, so it can't itself be one of the things that
might be missing.

## Feature bits

`FEATURES`' bit assignments mirror `rtl/boards.vh`'s own `` `ifdef ``
flags directly -- each bit is just "was this `` `ifdef `` active in
this specific build", nothing computed or inferred beyond that.
Assigned in `rtl/sysctl.v`'s `CSR_FEATURES` localparam; mirrored on
the software side in `sw/common/zsoc.h`'s `Z_FEATURE_*` constants.

| Bit | Flag | Bit | Flag |
|---|---|---|---|
| 0 | `MEM_SRAM` | 10 | `GPU_VGA` |
| 1 | `MEM_SDRAM` | 11 | `GPU_DDMI` |
| 2 | `MEM_VRAM` | 12 | `UART0` |
| 3 | `MEM_QQSPI` | 13 | `USB_HID` |
| 4 | `MEM_ROM` | 14 | `SPI_SDCARD` |
| 5 | `MEM_GLYPH` | 15 | `SPI_ETH` |
| 6 | `GPU` | 16 | `SPI_FLASH` |
| 7 | `GPU_RASTER` | 17 | `ETH_RMII` |
| 8 | `GPU_BLIT` | 18 | `LED_RGB` |
| 9 | `GPU_CURSOR` | 19 | `LED_DEBUG` |
| 25 | `ESP32_LINK` | | ULX3S: UART1 + ESP32 control + rx fifo (moved from 20) |

There's no single source shared between the Verilog and C sides here
-- both have to be hand-edited together and kept in sync deliberately
whenever a bit is added, same split `rtl/usb_hid.v`/`sw/common/zkbd.h`
already have for HID-usage translation. Only bit *position* has to
match between the two; the C-side name is just documentation.

Room for future bits: 20-31 are unused so far.

## Software access

### `sw/common/zsoc.h` -- everything except BIOS

Header-only, no separate `.c` -- these are plain MMIO reads at a fixed
physical address, same as any other `reg_*` register already used
directly throughout this codebase (`reg_sdcard`, `reg_eth`, ...). No
kernel/syscall indirection needed, unlike `z_msg_send()`/
`z_win_create()` and friends, which genuinely need the kernel's
involvement for cross-process coordination -- a CSR read doesn't. Safe
to `#include` from kernel-compiled code too (`sw/os/kernel.c` does) --
unlike `zeitlos.c`, this header has no symbols that could collide with
`kruntime.c`'s own.

```c
#include "zsoc.h"   // or "../../common/zsoc.h" from an app

bool z_soc_csrs_present(void);
uint32_t z_soc_mem_mb(void);
bool z_soc_has_feature(uint32_t feature);              // Z_FEATURE_*
bool z_soc_feature_confirmed_absent(uint32_t feature);  // Z_FEATURE_*
```

`z_soc_has_feature()` and `z_soc_feature_confirmed_absent()` are
**not** simple negations of each other. Both can be false at once,
when CSRs aren't present at all and the honest answer is "unknown"
rather than either yes or no:

| | CSRs absent | CSRs present, bit clear | CSRs present, bit set |
|---|---|---|---|
| `z_soc_has_feature()` | `false` | `false` | `true` |
| `z_soc_feature_confirmed_absent()` | `false` | `true` | `false` |

Use `z_soc_feature_confirmed_absent()` when deciding whether it's
*safe to skip/refuse* touching some piece of hardware -- refusing
should require positive evidence the hardware isn't there, not just
the absence of evidence it is. Simply negating `z_soc_has_feature()`
would also (wrongly) refuse on an older bitstream that predates
`rtl/csrs.v` entirely, silently breaking every board that hasn't been
rebuilt with it yet. `sw/apps/net`'s own startup check (below) is the
motivating example.

### `sw/bios/bios.c` -- its own private copies

`sw/bios` is a fully freestanding, self-contained build with no shared
include path (same reasoning every other `reg_*` macro in that file
is its own private copy, not a `#include`). `bios.c` defines its own
`reg_csr_magic`/`reg_csr_mem_mb` and a small `get_mem_main_size()`
helper, used everywhere `MEM_MAIN_SIZE` used to be a hardcoded
constant -- falls back to the old 1MB default (renamed
`MEM_MAIN_SIZE_DEFAULT`) if CSRs aren't present.

### `sw/os/kernel.c` -- the kernel's own memory pool

`k_mem_init()` (`sw/os/mem.c`/`mem.h`) now takes a `total_size`
parameter instead of assuming a fixed `Z_MEM_SIZE` -- it's only ever
assigned into a plain `uint32_t` struct field, never used for
compile-time array sizing, so this was a safe, mechanical change.
`kernel.c` computes that size once at boot: `z_soc_mem_mb() * 1MB` if
CSRs are present, `Z_MEM_SIZE_DEFAULT` (still 1MB, same fallback
reasoning as everywhere else in this doc) otherwise. This means every
board's actual RAM is now available to `k_mem_alloc()` -- previously,
even a 32MB board only ever exposed 1MB to the kernel's own allocator
(process memory, the `xf` upload buffer, everything `k_mem_alloc()`
backs), regardless of how much RAM the hardware actually had.

## `sw/apps/net`: graceful startup instead of hanging

The original motivating case. `sw/apps/net` is built for one specific
ethernet backend at a time (`NET_PHY` build variable -- `ENC28J60` or
`RMII`, see `net_phy.h`'s own header comment for why that part stays
build-time regardless: they're different drivers with different APIs,
not two configurations of one driver). What CSRs add is a runtime
check, right at the top of `main()`, for whether *this specific
board* actually has the backend this binary was compiled for:

```c
#ifdef NET_PHY_RMII
	if (z_soc_feature_confirmed_absent(Z_FEATURE_ETH_RMII)) {
		printf("net: this SOC build has no RMII ethernet ... exiting cleanly.\n");
		return 1;
	}
#else
	if (z_soc_feature_confirmed_absent(Z_FEATURE_SPI_ETH)) {
		printf("net: this SOC build has no SPI ethernet ... exiting cleanly.\n");
		return 1;
	}
#endif
```

Before this, `net.c`'s startup would hang forever on a board with
neither backend (Lakritz, which has only one PMOD slot and it's
already occupied by the USB-UART PMOD the console runs over) --
`sw/os/sh.c`'s `init` used to work around this by only *reserving*
net's pid slot rather than actually starting it, specifically to avoid
triggering that hang automatically at boot. With the check above in
place, `init` now starts `net` normally, the same way it starts every
other app -- see `sh.c`'s own comment at that call site for the
current reasoning, and `docs/networking.md`'s staged plan for where
this sits relative to the rest of net's own history.

## Testing this

`rtl/csrs.v` was verified standalone with `iverilog` (its own
combinational logic has no board-specific dependencies to pull in):

```
iverilog -g2005 -o /tmp/tb rtl/csrs.v testbench.v && vvp /tmp/tb
```
(strip the trailing comma from `csrs.v`'s own port list first for
plain `iverilog` -- same Yosys-tolerated-but-not-`iverilog`-tolerated
convention already used throughout `rtl/`, e.g. `rtl/debug.v`'s port
list ends the same way.)

`rtl/sysctl.v`'s integration (the `CSR_FEATURES` localparam's
preprocessor conditional chain, and that `` `MEM ``/`CSR_FEATURES``
actually resolve to the right values per board) was checked with
`iverilog -E` (preprocess-only, no full elaboration needed for this)
against `BOARD_OBST`, `BOARD_LAKRITZ`, `BOARD_MOZART_ML1`, and
`BOARD_KOLSCH` (the last one specifically to confirm the `` `ifndef
MEM `` fallback default triggers correctly on a board that doesn't
set `` `MEM `` yet).

**None of this has been run on real hardware yet.** Simulation and
preprocessor-output inspection confirm the logic is internally
correct; they say nothing about synthesis, timing, or how the real
bus behaves with a new, always-present slave added to its top-level
mux. Treat the first real build+flash as the actual first evidence,
same convention every other hardware feature in this project follows
(see `docs/networking.md`'s own "Confirmed working" sections for the
pattern) -- worth confirming, in order:

1. `reg_csr_magic` (or `z_soc_csrs_present()`) reads back `0x5A45_4954`
   at all, on a board actually flashed with the new bitstream.
2. `z_soc_mem_mb()` matches that board's real RAM.
3. `z_soc_has_feature()`/`z_soc_feature_confirmed_absent()` agree with
   that board's actual `rtl/boards.vh` block for a feature it does and
   doesn't have.
4. `init` actually starts `net` cleanly on Lakritz (no hang) and
   confirms `Z_FEATURE_SPI_ETH` absent, vs. actually working on Obst
   (confirmed absent check passes as "not absent", proceeds to
   `phy_init()` as before).

## Two boards still need real numbers

`rtl/boards.vh`'s Lebkuchen and Kolsch blocks don't set `` `MEM `` yet
-- left as an explicit `TODO` rather than a guessed number, so they
fall back to `rtl/sysctl.v`'s 1MB default (matching what every board
effectively assumed before this feature existed, not a regression).
Lebkuchen's main-memory backend (`MEM_QQSPI`) is itself still
commented out in that board's block, so it has no working main memory
configured at all yet independent of this. Update both once real
numbers are known.
