# Zeitlos Toolchain

Everything needed to build Zeitlos, and where to get it.

Two independent halves:

- **FPGA tools** turn `rtl/` into a bitstream: a synthesiser (Yosys), a
  place-and-route tool (nextpnr), and a bitstream packer specific to
  your FPGA family.
- **A RISC-V cross-compiler** turns `sw/` into binaries that run on the
  PicoRV32 core inside that bitstream.

You need both, they come from different places, and the best source is
different for each.

**Short version:** FPGA tools from your distro or OSS CAD Suite, RISC-V
compiler from xPack.

---

## 1. FPGA tools

### From Debian / Ubuntu packages

```
sudo apt install yosys nextpnr-ecp5 fpga-trellis fpga-trellis-database \
                 openfpgaloader iverilog gtkwave
```

Verified on Ubuntu 24.04: this set builds a complete Lakritz bitstream
(yosys → nextpnr-ecp5 → ecppack).

Some package names are not what you would guess:

| You want | Package |
|---|---|
| `ecppack`, `ecpbram` (ECP5 bitstream packer) | `fpga-trellis` — there is no `prjtrellis` package |
| ECP5 chip database | `fpga-trellis-database` |
| `icepack` (iCE40) | `fpga-icestorm` |
| `openFPGALoader` | `openfpgaloader`, all lowercase |

`nextpnr-ecp5` does **not** need prjtrellis to place and route — its
chip database is compiled into the binary. prjtrellis (`fpga-trellis`)
is needed only for `ecppack`, which converts nextpnr's `.config` output
into a `.bit` file.

**The catch: distro versions lag upstream, sometimes badly.**

| Tool | Ubuntu 24.04 | Upstream (Aug 2026) |
|---|---|---|
| yosys | 0.33 | much newer |
| nextpnr | 0.6 | 0.11.1 |
| openFPGALoader | 0.12.0 | 1.1.1 |

That is not cosmetic. Newer versions give better area and timing on the
same RTL. With the packaged versions, nextpnr reports the *unmodified*
Zeitlos design as failing 48 MHz on Lakritz — which it plainly does not,
since it runs. **If your timing report looks alarming, check your tool
versions before you start changing RTL.**

The packages also cannot build the GateMate boards at all.

### From OSS CAD Suite (recommended)

A single tarball from the Yosys developers containing current builds of
everything: yosys, every nextpnr variant (including
`nextpnr-himbaechel`, needed for GateMate), prjtrellis, icestorm,
openFPGALoader, Icarus Verilog and GTKWave.

Releases: https://github.com/YosysHQ/oss-cad-suite-build/releases

Builds are dated rather than version-numbered. Take the newest
`oss-cad-suite-linux-x64-YYYYMMDD.tgz` (also available for
`linux-arm64`, `darwin-arm64`, `darwin-x64` and `windows-x64`):

```
cd ~/work/fpga
tar xzf ~/Downloads/oss-cad-suite-linux-x64-YYYYMMDD.tgz
source ~/work/fpga/oss-cad-suite/environment
```

`source environment` puts everything on `PATH` for that shell only. Add
it to `~/.bashrc` to make it permanent, but note it shadows any
system-installed yosys.

This is already what `Makefile` assumes for GateMate boards, which
reference `~/work/fpga/gatemate/oss-cad-suite/bin/nextpnr-himbaechel`.

**Yosys 0.69 and the DDR3 boards.** oss-cad-suite 2026-10-01 (Yosys
0.69+173, nextpnr-ecp5 0.11.1-40) is what Konfekt, Schoko, Noir and
Klinge were brought up with ([boards.md](boards.md)). With it, every
DDR3 board -- Mozart and Sergei ML2, Noir, Klinge -- failed synthesis
at synth_ecp5's last pass until two changes went in: a renamed
instance in `rtl/mem/ddr3_phy_ecp5.v`, and the Makefile's `YOSYS_PRE`
for `DDR3_BOARDS`, which round-trips the elaborated design through
RTLIL. [boards.md](boards.md#noir) explains both. A tree older than
those changes needs an older Yosys (0.63 built the ML2 bring-up) for
any DDR3 board.

### Artix-7 boards (Sergei MX1, Mozart MX1) — openXC7

The Artix-7 boards need **openXC7 1.0 or later**: the open toolchain for
Xilinx 7-series, built around openXC7's own nextpnr engine (the
"himbaechel" xilinx architecture). The older `nextpnr-xilinx` 0.9 line
is archived and no longer supported here — its command line and chip
databases are different.

Use the **prebuilt release package**. It is self-contained: place and
route (`nextpnr-xilinx`), the chip databases, prjxray-db, `fasm2frames`
and `xc7frames2bit`, with its own Python. Nothing to build and nothing
to source.

```
# 1. download a release from
#    https://github.com/cavearr/toolchain-openxc7-releases/releases
#    (openxc7-toolchain-linux-x86-64-<date>.tgz, plus SHA256SUMS)
sha256sum -c --ignore-missing SHA256SUMS

# 2. install it at /opt/openxc7, where the Makefile looks by default.
#    The archive has no top-level directory -- unpack INTO the target.
sudo mv /opt/openxc7 /opt/openxc7-old        # if an older one is there
sudo mkdir /opt/openxc7
sudo tar xzf openxc7-toolchain-linux-x86-64-<date>.tgz -C /opt/openxc7

# 3. check
/opt/openxc7/bin/nextpnr-xilinx --version
ls /opt/openxc7/chipdb/                      # chipdb-xc7a50t.bin, ...
```

Then build as for any board — `make BOARD=sergei_mx1`. Elsewhere than
`/opt/openxc7`, pass `OPENXC7=/path` (or set it in the environment).
`yosys` still comes from OSS CAD Suite, as for the other families.

**The chip database is per die, not per part.** A part name is die,
package and speed grade (`xc7a35tftg256-1`); the package ships one
database per die (`chipdb/chipdb-<die>.bin`), and the XC7A35T is the
XC7A50T die, so it uses `chipdb-xc7a50t.bin`. The Makefile works this
out from `PART` (`XC7_DIE`). There is no longer a database built inside
the tree: delete any `chipdb/` directory an older build left behind.

**The part name still carries the speed grade**, which selects the
timing model and names prjxray's part data
(`share/nextpnr/external/prjxray-db/artix7/<part>/`). The MX1 boards
default to `xc7a35tftg256-1`: modules are usually built with the -2 part
(`XC7A35T-2FTG256C`), but some carry a -1, and a design timed against -1
meets timing on both. Override with `PART=xc7a35tftg256-2`.

**The 1.x command line differs from 0.9's.** The part is given by name
(`--device`), and the constraints and FASM output are engine options:
`-o xdc=<file> -o fasm=<file>`, not `--xdc`/`--fasm`. The Makefile does
this; it matters only when running nextpnr by hand.

**Constraints are `.xdc`, and the 1.x reader is strict.** One `LOC` and
one `IOSTANDARD` line per pin, and both halves of every differential
pair. A comment after a command on the same line (`... ;# note`) is a
parse error in 1.x — 0.9 accepted it — so comments go on their own
lines; `create_clock -name` is ignored with a warning, so it is left
out. See `boards/sergei_mx1.xdc`.

**Timing estimates differ between the engines.** The same reduced
sergei_mx1 netlist (no GPU, Ethernet or audio) placed at 66.6 MHz (`clk48mhz`) under 0.9.4 and 87.0 MHz under
1.x. Neither has been validated against measurements on this silicon:
treat the reported margin as an estimate, not a guarantee.

**No LUT RAM: `XC7_LUTRAM ?= -nolutram` in the Makefile.** This is
the important one. openXC7's support for distributed RAM is incomplete
(nextpnr-xilinx issue #20, "Limited support for Distributed Memory /
LUTRAM"; its 0.9.0 release fixes RAM32M/RAM64M INIT interleaving and
stops some unsupported LUT RAM being built silently wrong). With LUT
RAM allowed, yosys put the CPU's register file and the console UART's
FIFOs there. The result on sergei_mx1 was a CPU that ran a few
instructions and a console printing wrong bytes -- differently on every
build, because placement decides which LUTs those memories land in --
while the BIOS block RAM checked out bit-exact against the FASM. The
same is the likeliest explanation of Kirsch's console troubles. `make
XC7_LUTRAM=` lets LUT RAM back in, to test a newer openXC7.

Three more openXC7 workarounds are in effect on these boards, each a
real issue found on Kirsch and each one line to undo once fixed
upstream:
`` `NO_TRNG `` (the TRNG's ring oscillators do not route),
`` `NO_CPU_MUL_FAST `` (a DSP48E1 behind the CPU's fast multiplier hung
the CPU), and `XC7_DSP ?= -nodsp` in the Makefile (multipliers in LUTs
rather than DSP48E1s, ~1,100 LUTs on sergei_mx1; `make XC7_DSP=` to
test DSPs again).

### GateMate boards (Kölsch, Lebkuchen)

These additionally need Cologne Chip's own place-and-route tool, `p_r`,
which is proprietary and not redistributed by anyone else:

https://www.colognechip.com/programmable-logic/gatemate/

Download it from there, get `nextpnr-himbaechel` from OSS CAD Suite, and
adjust the `PR` and `SYNTH` paths near the top of `Makefile`. Neither
apt nor OSS CAD Suite alone is sufficient for GateMate.

---

## 2. RISC-V toolchain

Zeitlos targets **`rv32im`** — 32-bit RISC-V with hardware multiply and
divide, see `docs/muldiv.md` — and is written against **newlib** as its
C library.

Neither of those is the default for a general-purpose RISC-V compiler,
so it is worth reading this section before installing the first thing
you find.

### xPack riscv-none-elf-gcc (recommended)

xPack publishes prebuilt, current, self-contained GNU toolchains for
embedded targets. `riscv-none-elf-gcc` is their bare-metal RISC-V one:
GCC, binutils and **newlib**, with rv32 multilibs included, as a tarball
that unpacks anywhere and needs no root.

Modern GCC, newlib, rv32 support, nothing to build — that combination is
exactly what Zeitlos wants, which is why it is the recommendation.

Releases:
https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases

Download the `linux-x64` (or matching) `.tar.gz`, unpack it somewhere,
and point `sw/common/arch.mk` at it:

```
RISCV_PREFIX ?= /opt/xpack/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-
```

The binaries are prefixed `riscv-none-elf-`, not `riscv32-unknown-elf-`
or `riscv64-unknown-elf-`, and the trailing `-` on `RISCV_PREFIX` is
required.

You do not need xPack's `xpm`/Node.js installer; the plain tarball from
the releases page is enough.

**One thing to know about GCC 14 and newer:** several long-standing
warnings became errors by default, `-Wint-conversion` among them. Code
that built fine on GCC 13 can now fail outright, typically where an
integer address is passed to a pointer parameter or vice versa. Zeitlos
is clean under these; if you are carrying local changes, that is where
new errors will come from.

### Building riscv-gnu-toolchain from source

The traditional route, and what the old README pointed at. Still works,
still takes an hour or more:

```
git clone https://github.com/riscv/riscv-gnu-toolchain
cd riscv-gnu-toolchain
./configure --prefix=/opt/riscv --with-arch=rv32im --with-abi=ilp32
sudo make -j$(nproc)
```

```
RISCV_PREFIX ?= /opt/riscv/bin/riscv32-unknown-elf-
```

That builds one multilib toolchain. picorv32's own instructions instead
build a separate toolchain per architecture, installed as
`/opt/riscv32i`, `/opt/riscv32im` and so on. If you already have
`/opt/riscv32i` from those instructions, note it is **rv32i only** — its
libgcc and newlib cannot produce an rv32im build. Add the matching one:

```
make -C /path/to/picorv32 build-riscv32im-tools     # installs /opt/riscv32im
```

`arch.mk` defaults `RISCV_PREFIX` to
`/opt/$(subst rv32,riscv32,$(ARCH))/bin/riscv32-unknown-elf-`, which
resolves to `/opt/riscv32im` for the default `ARCH = rv32im`, matching
that convention.

### Debian / Ubuntu packages — not currently supported

```
gcc-riscv64-unknown-elf + picolibc-riscv64-unknown-elf
```

Tempting, since it is one `apt install`, but it does not work today and
the reason is worth recording.

`gcc-riscv64-unknown-elf` ships **no C library at all** — just the
compiler and libgcc. Fine for `sw/bios` (built `-ffreestanding
-nostdlib`), not fine for `sw/os` or `sw/apps`, which use `printf`,
`sprintf` and `malloc`. The only packaged C library for it is picolibc,
and picolibc is a newlib *fork*, not a drop-in replacement:

1. **tinystdio does not define `stdin`/`stdout`/`stderr`.** The
   application must. *Handled* — see the `#ifdef __PICOLIBC__` blocks in
   `sw/os/kruntime.c` and `sw/common/zeitlos.c`.
2. **`picolibc.specs` injects its own linker script.** It contains
   `%{!T:-Tpicolibc.ld}`, and `-Wl,-T` is invisible to that test, so
   picolibc.ld gets added as well and wins — silently relinking the
   kernel away from `riscv-os.ld`'s `0x40000000`. The BIOS then copies
   that image to `0x40000000` and jumps to it with every absolute
   address wrong. It builds, it looks fine, it cannot boot. *Handled* —
   every Makefile now passes `-T` directly rather than through `-Wl`.
3. **picolibc supplies its own `sbrk()`**, wanting `__heap_start` /
   `__heap_end` from its linker script, colliding with this tree's own
   `_sbrk()`. **Not handled.** This needs a decision about which heap
   owns memory, not a flag.

`arch.mk` detects picolibc and sets `--specs=picolibc.specs` so the
flags are right if you deliberately choose it (`LIBC=picolibc`), but
finishing the port is outstanding work. picolibc is genuinely
attractive — considerably smaller than newlib — so it may be worth
completing. It is not done.

The FPGA half of the apt list above is unaffected and pairs fine with an
xPack compiler.

---

## 3. Programming the board

`openFPGALoader` handles every supported board:

```
sudo apt install openfpgaloader
```

or build the current release from
https://github.com/trabucayre/openFPGALoader.

Pass your cable with `CABLE=`:

```
make BOARD=lakritz CABLE=dirtyJtag flash
```

You will probably need udev rules to avoid running it as root; see
openFPGALoader's own documentation.

---

## 4. Simulation and debugging (optional)

```
sudo apt install iverilog gtkwave verilator
```

- **Icarus Verilog** runs the testbenches in `rtl/tb/` — the cache
  testbench (`tb_cache.v`) and the cycle-accurate CPU testbench
  (`tb_soc.v`). See `docs/icache.md`.
- **GTKWave** views the resulting VCD traces.
- **`sim/`** is a separate self-contained emulator that runs app
  binaries on a host machine with no FPGA at all. It needs only a C
  compiler and SDL2 (`libsdl2-dev`); see `sim/README.md`.

---

## 5. Getting the source

`sw/apps/repl` depends on the `ms` Lisp interpreter, which is a
submodule. A plain `git clone` leaves it empty and the build stops with
`No rule to make target '../../ext/ms/ms_stdlib.l'`:

```
git clone --recursive https://github.com/machdyne/zeitlos
```

In an existing clone:

```
git submodule update --init --recursive
```

---

## 6. Checking your setup

```
yosys --version
nextpnr-ecp5 --version
ecppack --version
openFPGALoader --version
riscv-none-elf-gcc --version        # or whatever RISCV_PREFIX points at
```

Confirm the compiler can actually produce rv32 code. The failure mode
here is a missing multilib, and it surfaces as a confusing link error
rather than a clear message:

```
riscv-none-elf-gcc --print-multi-lib | grep rv32im
```

You should see a line containing `rv32im`. If nothing matches, that
toolchain cannot build Zeitlos as configured.

Build the software half, which needs no FPGA attached:

```
cd sw/bios && make BOARD=LAKRITZ FAMILY=ECP5
cd ../os   && make
cd ../apps && make
```

That should produce `sw/bios/bios.bin`, `sw/os/kernel.bin`, and a `.bin`
in each `sw/apps/*/` directory.

Then the whole thing:

```
make BOARD=lakritz CABLE=dirtyJtag flash
```

Successful output ends with a `.bit` in `output/lakritz/`. Check
`output/lakritz/report.txt` for the timing summary while you are there.

---

## 7. Building gateware without a host

Everything above describes the host toolchain. `docs/zfpga.md` is the
other direction: a synthesiser, a placer, a router and a bitstream
packer that run **on Zeitlos**, targeting the FPGA Zeitlos is running on
-- the gateware equivalent of what `zcc` did for C. They are
subcommands of one tool, `zfpga`, in `sw/apps/zfpga/`, run from `posix`.
**Experimental, and working on hardware**: a Verilog blinky built on a
Lakritz by

```
zfpga build /fpga/examples/blink.v -b lakritz
```

blinks the LED (`docs/zfpga.md` section 23).

Each stage has a host counterpart it is checked against by
`sw/apps/zfpga/tests/run.sh`:

| zfpga | checked against |
|---|---|
| `synth` | yosys `synth_ecp5`, by simulation of both netlists |
| `place`, `pnr` | nextpnr, byte-identical where the placement is given; otherwise an independent legality check and simulation of the extracted circuit |
| `pack` | `ecppack`, byte-identical |
| `unpack` | `ecpunpack`, text-identical |
| `bram` | `ecpbram`: the same bitstream once packed; `-g -s` the same seed file |

`zfpga pack` can stand in for `ecppack` in the host flow of section 1:

```
ecppack --compress --freq 2.4 soc_final.config --bit soc.bit
zfpga pack soc_final.config -c -f 2.4 -o soc.bit        # same bytes
```

It builds for the host with `make -f Makefile.host` in `sw/apps/zfpga`,
and as a Zeitlos app reading its chip database and board profiles from
`/fpga` on the card (`docs/zfpga-formats.md`). The byte-identity tests
run against whatever `ecppack` is installed -- so this section's version
table matters there too: a Trellis database that differs from the
vendored one (`sw/apps/zfpga/ext/prjtrellis-db/`) will show up as a
mismatch that is not zfpga's.

`zfpga bram` does `ecpbram`'s job in the `soc` target the same way --
checked on a test ROM against ecpbram; not yet on `soc.config` itself,
whose nextpnr run needs more memory than the build machine had. The SOC itself is still built by `yosys` and `nextpnr`: zfpga's
synthesiser reads this tree's RTL -- 15 of its modules come out
equivalent to yosys's synthesis (`docs/zfpga.md` §24.6) -- but not yet
the block RAM, tristate IO and `generate` the rest need.

Loading what it builds is covered in `docs/zboot.md`: how
`--bootaddr` decides where the next configuration is read from, how the
DFU bootloader hands off to Zeitlos, and the two routes to booting
gateware the machine built itself.
