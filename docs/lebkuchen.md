# Lebkuchen

Machdyne Lebkuchen: a Cologne Chip GateMate CCGM1A1 with an RP2040
supervisor. https://github.com/machdyne/lebkuchen

This document is the board's own facts -- what is wired to what, and
which of those facts change how Zeitlos is built. Toolchain setup is in
`docs/toolchain.md`.

**Port status: in progress, not yet booting.** Timing does not close at
48 MHz and the full build has not been through a successful route. See
"Timing" and "Port status" below.

## What is on the board

| | |
|---|---|
| FPGA | GateMate CCGM1A1 -- 20480 CPEs, 64 RAM halves (32 x 40 Kbit, about 160 KB), 4 PLLs |
| Main memory | 8 MB QSPI PSRAM, APS6404L-SQN-SN, **one chip** |
| Storage | 4 MB NOR flash, AT25FF321A -- **wired to the RP2040, not the FPGA** |
| Supervisor | RP2040, **2.5 V IO**, 12 MHz crystal, 4 MB flash shared as above |
| Video | VGA DB15 through an RGB332 resistor DAC |
| Audio/video jack | 3.5 mm TRRS: stereo audio (needs a sigma-delta DAC in gateware) + 4-bit composite video DAC |
| USB | 1 x USB-C (power, JTAG, console), 2 x USB-A host |
| Card | MicroSD, 1-bit SPI mode |
| LED | one white LED |

Level translation: a single LSF0108 sits between the FPGA's WB bank and
the MicroSD card and the two USB-A ports. Its `VREF_A` is 2.5 V, which
is also the RP2040's IO voltage -- **everything on the FPGA/RP2040
interface runs at 2.5 V**, so JTAG, CSPI, the UART and the clock need no
translation and none is fitted.

## Three facts that drive the port

**1. The FPGA's reference clock comes from the RP2040.** There is no
oscillator for the FPGA. `CLKRP` is RP2040 **GPIO23 = `clk_gpout1`**,
driven at 48 MHz from the RP2040's USB PLL, into `CLK1/IO_SB_A7`. The
only crystal on the board is the RP2040's 12 MHz part.

Three consequences. The FPGA has no clock at all until supervisor
firmware enables GPOUT1, so the RP2040 is load-bearing from the first
bring-up step rather than only for configuration. GPOUT jitter feeds all
three `CC_PLL` instances, including the 126 MHz video clock. And --
see "Timing" -- **sys_clk is a firmware constant on this board**, which
no other Zeitlos board can say.

**2. There is no flash on the FPGA's side of the board.** The 4 MB NOR
part is the RP2040's own boot flash, on its dedicated QSPI pins. Zeitlos
expects a memory-mapped read-only flash window at `0x1000_0000`
(`MEM_ROM`, `rtl/spiflashro.v`) holding the gateware, boot logo, kernel
and core-app archive. On this board that window has to be served by the
RP2040 emulating a NOR flash over CSPI.

The access pattern makes that tractable: `spiflashro_wb` is read-only,
issues command `0x03` (no dummy cycles, single-bit), and does **one
32-bit word per CS assertion** -- 8 bits command, 24 bits address,
32 bits data, then CS high again. CS toggles every word, so there is
nothing to resynchronise and no streaming requirement, only a
per-transaction latency budget. And every consumer is a boot-time bulk
copy: logo (37.5 KB at `0xF0000`), kernel (256 KB at `0x100000`), ZAR
core-app archive (about 100 KB at `0x140000`).

The emulator must present the first MISO bit at SCK cycle 33. At the
current hardcoded `sys_clk/2` that is 1.33 us from CS-fall and 42 ns
between bits -- not reachable in software. `spiflashro.v` needs a clock
divider parameter, defaulting to today's behaviour so no other board
changes:

| SCK | addr -> data window | RP2040 cycles @125/200 MHz | 400 KB boot copy |
|---|---|---|---|
| 24 MHz (today) | 1.33 us | 166 / 266 | 0.27 s |
| 6 MHz (/8) | 5.3 us | 666 / 1066 | 1.1 s |
| 3 MHz (/16) | 10.7 us | 1333 / 2133 | 2.1 s |

Start at /16, tune down once measured. A second optional parameter,
fetching N consecutive words per CS assertion using `0x03`'s
auto-increment, would halve sequential transfer time and let the
emulator decode one address and then stream -- useful to every board,
not just this one.

**3. CSPI is the FPGA's dedicated SPI configuration interface**, not
general-purpose IO -- `IO_WA_A8/~SPI_CS`, `IO_WA_B8/SPI_CLK`,
`IO_WA_B7/SPI_D0`, `IO_WA_A7/SPI_D1`. `SPI_D2`, `SPI_D3` and `SPI_FWD`
(balls R2, R1, R4) are **not connected**, so this bus is x1 only --
which is exactly what `spiflashro.v` uses, so nothing is lost.

Configuration mode straps: `CFG_MD0` = GND, `CFG_MD1` = GND,
`CFG_MD2` = VDD_WA, and **`CFG_MD3` is RP2040 GPIO29**, so the
supervisor selects the mode at runtime:

| GPIO29 | Mode | Who drives CSPI during configuration |
|---|---|---|
| high | JTAG | nobody -- config goes over TCK/TMS/TDI/TDO |
| low | SPI | the **FPGA**, as master, reading a bitstream from CSPI |

**JTAG is the default for this port.** The RP2040 is master, so every
timing parameter is under firmware control, and the PIO JTAG engine in
`firmware/pico-dirtyJtag` already works. CSPI is untouched by
configuration and becomes ordinary user IO the moment the FPGA enters
user mode, so the flash-emulation clock rate is set entirely by
`spiflashro.v`'s divider.

SPI mode is the more elegant endgame: the FPGA comes out of reset as an
SPI master looking for a bitstream, the RP2040 answers from emulated
flash, and the *same* emulator then serves `spiflashro.v` at runtime --
one mechanism for configuration and for the `0x1000_0000` window, with
no JTAG shifting in the boot path. The catch is that the FPGA picks the
config clock rate, not us, which is the one number in this design that
cannot be tuned. Revisit once the emulator's real latency is known.

RP2040 GPIO28 drives `~RESET`, so the supervisor can hold the FPGA down
until the emulator is armed. Worth keeping true whichever mode is used.

## Pinout

Extracted from `pcb/lebkuchen_v0/lebkuchen.kicad_pcb` (pad -> net)
joined against the `CCGM1A1` library symbol in `lebkuchen.kicad_sch`
(ball -> pin name). `boards/lebkuchen_v0.ccf` carries the same data in
the form the tools read.

### FPGA

| Zeitlos port | GateMate pin | Ball | Schematic net |
|---|---|---|---|
| `CLK_48` | `CLK1/IO_SB_A7` | P12 | `/CLKRP` |
| `LED_B` | `IO_WC_A0` | G1 | `/LED` |
| `UART0_RX` | `IO_SA_A8` | U10 | `/UART_TX` (RP2040 TX) |
| `UART0_TX` | `IO_SA_B8` | V10 | `/UART_RX` (RP2040 RX) |
| `QQSPI_SS` | `IO_NA_A1` | A4 | `/RAM_SS` |
| `QQSPI_SCK` | `IO_NA_B1` | B4 | `/RAM_SCK` |
| `QQSPI_MOSI` | `IO_NA_A3` | C6 | `/RAM_MOSI` |
| `QQSPI_MISO` | `IO_NA_B0` | B3 | `/RAM_MISO` |
| `QQSPI_SIO2` | `IO_NA_B2` | C5 | `/RAM_IO2` |
| `QQSPI_SIO3` | `IO_NA_A2` | B5 | `/RAM_IO3` |
| `CSPI_SS_FLASH` | `IO_WA_A8/~SPI_CS` | N3 | `/CSPI_SS` |
| `CSPI_SCK` | `IO_WA_B8/SPI_CLK` | N4 | `/CSPI_SCK` |
| `CSPI_MOSI` | `IO_WA_B7/SPI_D0` | P2 | `/CSPI_MOSI` |
| `CSPI_MISO` | `IO_WA_A7/SPI_D1` | P1 | `/CSPI_MISO` |
| `SD_SS` | `IO_WB_B2` | L4 | `/SD_SS` |
| `SD_MOSI` | `IO_WB_A2` | L3 | `/SD_MOSI` |
| `SD_MISO` | `IO_WB_B5` | K2 | `/SD_MISO` |
| `SD_SCK` | `IO_WB_A5` | K1 | `/SD_SCK` |
| `usb_host_dp[0]` | `IO_WB_B0` | N2 | `/USB2_DP` |
| `usb_host_dm[0]` | `IO_WB_A0` | N1 | `/USB2_DN` |
| `usb_host_dp[1]` | `IO_WB_B1` | M4 | `/USB1_DP` |
| `usb_host_dm[1]` | `IO_WB_A1` | M3 | `/USB1_DN` |
| `VGA_HS` | `IO_EB_A0` | F18 | `/VGA_HS` |
| `VGA_VS` | `IO_EB_B0` | F17 | `/VGA_VS` |

Wired and available, not yet Zeitlos ports:

| Function | GateMate pin | Ball |
|---|---|---|
| VGA red, MSB->LSB | `IO_EB_B4`, `IO_EB_A5`, `IO_EB_B5` | C17, B17, A17 |
| VGA green, MSB->LSB | `IO_EB_A2`, `IO_EB_B2`, `IO_EB_A4` | D18, D17, C18 |
| VGA blue, MSB->LSB | `IO_EB_B3`, `IO_EB_A3` | E15, E16 |
| Audio left / right | `IO_EB_A8` / `IO_EB_B8` | B15 / A15 |
| Composite DAC `[3:0]` | `IO_NB_A8`, `IO_NB_A6`, `IO_NB_B8`, `IO_NB_B6` | D14, D13, C14, C13 |
| FPGA reset in | `~RESET` | T15 |
| Config done / failed | `IO_WA_B2/CFG_DONE`, `IO_WA_A2/~CFG_FAILED` | V3, V2 |
| JTAG TCK/TMS/TDI/TDO | `IO_WA_A5`, `IO_WA_B4`, `IO_WA_A4`, `IO_WA_B3` | R3, T3, T2, U2 |

**Not available despite appearances:** the schematic carries nets named
`/PMOD_D1`, `/PMOD_D2`, `/PMOD_D9` and `/PMOD_D10` on `IO_SB_B0`,
`IO_SB_A0`, `IO_SA_A2` and `IO_SA_A1`. There is no PMOD socket on
Lebkuchen; those nets go nowhere. Do not plan a `` `GPIO_PORT0 ``
around them.

### RP2040

| GPIO | Signal | GPIO | Signal |
|---|---|---|---|
| 0-3 | JTAG TCK, TMS, TDI, TDO | 14 | CDONE (in) |
| 4-7 | CSPI MISO, SS, SCK, MOSI | 15 | CFAILED (in) |
| 8-11 | SD MISO, SS, SCK, MOSI | 23 | CLKRP -- `clk_gpout1`, 48 MHz |
| 12-13 | UART TX, RX | 28 | FPGA `~RESET` |
| | | 29 | `CFG_MD3` |

**Free: GPIO16-22 and GPIO24-27** (eleven pins).

Two groupings matter. GPIO4-7 are a contiguous nibble, which lets a
single PIO state machine watch the whole CSPI bus from one `in pins`
base. And the RP2040 is wired to the MicroSD card in parallel with the
FPGA (GPIO8-11 on the same nets, A side of the LSF0108), so bus
ownership needs arbitrating -- this is the path the board README's
"copy gateware to a MicroSD card" feature would use.

### VGA is RGB332

The DAC is a real resistor ladder, which is enough to render the
virtual phosphor modes (`rtl/gpu/gpu_video.v`) properly rather than
approximating them:

| Mode | `VGA_R[2:0]` | `VGA_G[2:0]` | `VGA_B[1:0]` | |
|---|---|---|---|---|
| white | 111 | 111 | 11 | |
| amber | 111 | 101 | 00 | about #FFB600 |
| green | 000 | 111 | 00 | |
| paper | 111 | 111 | 11 | `pset` inverted, as elsewhere |

On the existing 1-bit VGA boards amber can only be `R=1, G=1, B=0`,
which is yellow. Three bits of green is what makes it look like amber.
This wants a `` `GPU_VGA_RGB332 `` define and wide `VGA_R`/`VGA_G`/
`VGA_B` ports, with the existing 1-bit assignments left untouched for
every other board. The values above derive from the 8-bit
`ddmi_red`/`ddmi_green`/`ddmi_blue` weightings already in
`gpu_video.v`, scaled to full range: DDMI drives white at `0x80` for
historical reasons, but the 1-bit VGA boards drive white full-on, and
matching the VGA boards is the right call for a VGA board.

Until that lands, `boards/lebkuchen_v0.ccf` puts the single-bit
`VGA_R`/`VGA_G`/`VGA_B` on the **most** significant bit of each channel.
This corrects an earlier revision of that file which had `VGA_R` on
`IO_EB_A4` (actually `VGA_G0`) and `VGA_G` on `IO_EB_B5` (actually
`VGA_R0`) -- red and green crossed, both on their channel's least
significant bit, so white rendered as a dim off-colour grey.

## Resource usage

Measured with OSS CAD Suite `20260911`, through yosys and
nextpnr-himbaechel packing, at `fpga_mode=speed` / `time_mode=worst`.

| | Minimal (CPU + BRAM + UART + PSRAM) | Earlier stub block | Current config |
|---|---|---|---|
| `CPE_LT` | 8541 / 40960 -- 20% | 29567 / 40960 -- 72% | 26735 / 40960 -- **65%** |
| `CPE_FF` | 2305 -- 5% | 6373 -- 15% | 7229 -- 17% |
| `RAM_HALF` | 7 / 64 -- 10% | 43 / 64 -- 67% | 49 / 64 -- **76%** |
| `GPIO` | 10 / 162 | 17 / 162 | 27 / 162 |
| `PLL` | 3 / 4 | 3 / 4 | 3 / 4 |

Packed bitstream: **517451 bytes** for the minimal build. Budget around
600 KB for a full one -- comfortably under the 960 KB the flash map
leaves below the boot logo.

The current configuration is *smaller* than the earlier stub block
despite adding QSPI PSRAM, the flash window, the glyph ROM and a 2 KB
instruction cache, because `` `CPU_MUL_FAST `` is now off.

### `CPU_MUL_FAST` is off on all GateMate boards

`Makefile` passes `-nomult` to `synth_gatemate`, so picorv32's
DSP-backed `picorv32_pcpi_fast_mul` -- a full unpipelined 32x32
multiplier -- maps into LUTs. It costs roughly 7 points of `CPE_LT` on
this device, more than everything this port adds put together, and sits
squarely on the critical path.

`rtl/boards.vh` sets the existing `` `NO_CPU_MUL_FAST `` under
`` `ifdef FPGA_GATEMATE ``, rather than per board, because it follows
from the synthesis flags and not from any board's wiring. `` `CPU_MUL ``
stays, so the sequential shift-add multiplier is used: about 32 cycles,
still roughly an order of magnitude faster than the libgcc routine, and
no timing risk. **No software rebuild is needed** -- both are RV32IM as
far as the compiler is concerned. See `docs/muldiv.md`.

## Timing

**This is the open problem.** All figures below are post-*placement*
estimates at `fpga_mode=speed`, `time_mode=worst`; see
`docs/toolchain.md` on why the post-routing figures from this version of
the GateMate timing model cannot be used.

| Build | `CPE_LT` | post-placement Fmax on `clk48mhz` |
|---|---|---|
| Minimal (CPU + BRAM + UART + PSRAM) | 20% | 48.21 MHz |
| Stub block, `CPU_MUL_FAST` on | 72% | 29.83 MHz |
| Current config, `CPU_MUL_FAST` off | 65% | 31.71 MHz |

Two things follow. Dropping the LUT multiplier buys 7 points of area and
almost no frequency. And **48 MHz is already the `speed` number** --
`fpga_mode` defaults to `speed` whatever the board, so every figure here
was taken on the fast silicon model. Running the board at the speed core
voltage does not create headroom; it only makes these numbers honest
rather than optimistic.

### The critical path

In the *minimal* build -- 20% utilisation, nothing but CPU, BRAM, UART
and PSRAM -- the limiting path ran through `rtl/mtu.v:102`:

```verilog
assign addr_out = (ON_BUS != 0 && address_needs_translation) ?
    (translation_base + offset_address) : addr_in;
```

A combinational 32-bit adder between the CPU's address register and
everything downstream: the icache tag lookup, the arbiter, every
chip-select decode. On ECP5's carry chains that is free; on GateMate's
CPE chain it is a 32-stage ripple that was already the limiter at 20%
utilisation, with the rest of the design piled on top of it.

`rtl/mtu.v` documents the fix already. `ON_BUS = 0` moves translation
into the CPU wrapper's own address register transfer, so the bus sees a
physical address straight out of a flop and the adder leaves the bus
path entirely -- and `rtl/sysctl.v` already selects `MTU_ON_BUS = 0`
when `` `CPU_ZEITLOS32 `` is defined. On this board that one-line switch
in `rtl/boards.vh` is the most promising timing lever available, at the
cost of running the less-proven core. For picorv32 the same could be had
from a project-owned copy of the ~150-line `picorv32_wb` wrapper, or
from a carry-select split of the adder in `mtu.v` (two 16-bit halves
computed in parallel, one mux -- roughly half the chain depth, one extra
16-bit adder, no software cooperation needed).

### sys_clk is a firmware constant here

`rtl/sysctl.v` assigns `clk48mhz = CLK_48` and `sys_clk = clk48mhz`
with no PLL in between, and `CLK_48` is the RP2040's GPOUT1. So if the
fabric will not do 48 MHz, the supervisor can generate 36 or 32 instead.

The Zeitlos side is smaller than it looks. One constant,
`Z_SYSCLK_HZ` in `sw/common/zsoc.h`, feeds everything that derives from
it: UART divisors (`zuart.c`), I2C and SPI half-cycles, `Z_TICK_HZ`, the
MIPS report in `kernel.c`, SD delays in `sdmm.c`. Two things are not
parameterised and would need fixing: the BIOS hardcodes its UART divisor
as a literal `3`, and the three `CC_PLL`s hardcode `REF_CLK(48.0)` --
the video clock has to stay 25.2 MHz whatever the reference becomes.

This is the floor under the port: a slower machine rather than a failed
one, and it is unique to Lebkuchen among Zeitlos boards.

### Drop candidates

If area or congestion rather than path depth turns out to be the
problem, `` `MONTMUL `` is the largest block in the current
configuration that nothing in the boot path needs.

## Building

```
make BOARD=lebkuchen
```

Four GateMate knobs are overridable on the command line -- see the
block near `ABC9` in `Makefile`:

```
make BOARD=lebkuchen GM_PERF=economy       # match a 1.0V core rail
make BOARD=lebkuchen GM_CORNER=typical     # non-signoff corner
make BOARD=lebkuchen GM_FREQ=36            # lower target
make BOARD=lebkuchen GM_ROUTER=router2     # router1 is the default here
```

Programming over USB-C:

```
openFPGALoader -c dirtyJtag output/lebkuchen/soc.bit
```

**Any BIOS change needs a full resynthesis on this board.** There is no
`gmbram` to splice a new `sw/bios/bios.hex` into a finished bitstream
the way `ecpbram` and `icebram` do, so `rtl/mem/bram.v` does
`$readmemh` at synthesis time under `` `FPGA_GATEMATE `` and the `soc:`
target is a no-op. The top-level `zeitlos:` target therefore builds
`bios` *before* `zeitlos_pico`; with the old ordering every GateMate
bitstream embedded the previous build's BIOS, and the first build after
`make clean` embedded nothing at all, silently.

## Port status

Working:

- Toolchain end to end: yosys -> nextpnr-himbaechel -> gmpack -> `.bit`.
- Full pinout, in `boards/lebkuchen_v0.ccf`.
- Resource budget, measured; the design fits.

Not working:

- **Timing does not close at 48 MHz.** 31.71 MHz post-placement.
- The full configuration has not completed a route. `router2` fails
  outright (see `docs/toolchain.md`); `router1` was still converging
  when last observed and has not been confirmed to finish.
- Real Fmax on hardware is unknown, and the model's post-route numbers
  are not usable.

Not started:

- `spiflashro.v` clock divider, and the RP2040 flash emulator that the
  `MEM_ROM` window depends on. Until both exist there is nothing at
  `0x1000_0000`, which is why `sw/bios/bios.c` still guards
  `load_zeitlos()` with `` #ifndef FPGA_GATEMATE `` -- the board cannot
  auto-boot yet and the BIOS monitor's `xf` upload is the way in.
- `` `GPU_VGA_RGB332 `` and the wide VGA output path.
- Audio and composite video, wired on the board but not in the CCF.
