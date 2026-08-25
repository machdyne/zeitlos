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
