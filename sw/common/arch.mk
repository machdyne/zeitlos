# Zeitlos -- RISC-V architecture flags, single source of truth.
#
# Every Makefile under sw/ includes this instead of defining ARCH
# itself. That is not tidiness for its own sake: a mixed build, where
# some binaries are rv32im and others rv32i, is one of the nastier
# failure modes available here, and 18 separate copies of the string
# is exactly how that happens.
#
# The rule that matters:
#
#   rv32i  software on an rv32im bitstream  -- fine, M just goes unused
#   rv32im software on an rv32i  bitstream  -- FATAL. Every mul/div is
#                                              an illegal instruction.
#
# So ARCH here must match rtl/boards.vh's `CPU_MUL/`CPU_DIV. If they
# disagree, z_soc_check_cpu_arch() (sw/common/zsoc.h) catches it at
# boot and says so plainly rather than letting it become a mystery
# hang. Flash gateware and software together.
#
# Override for a one-off build with e.g.:  make ARCH=rv32i
#
# rv32i_zmmul -- multiply without divide -- would be the natural
# fallback if the sequential divider ever proves troublesome, since
# multiplies vastly outnumber divides in practice. It needs gcc and
# binutils 12 or newer, so it is NOT available on the 2018-era
# toolchain picorv32's instructions pin. Noted here so nobody reaches
# for it and gets a confusing "unknown ISA extension" error.

ARCH ?= rv32im
ABI  ?= ilp32

# Whether ARCH includes multiply/divide, decided HERE rather than left
# to the compiler's predefined macros.
#
# z_soc_check_cpu_arch() (sw/common/zsoc.h) needs to know what this
# binary was built for. The obvious source is GCC's __riscv_mul /
# __riscv_div, and those are used as a fallback -- but relying on them
# alone has a bad failure mode: if a given compiler doesn't define them
# the way expected, the check does not fail loudly, it silently becomes
# "no M, nothing to verify" and the safety net disappears without a
# word. This tree's toolchain is deliberately an old one (picorv32's
# instructions pin riscv-gnu-toolchain rev 411d134, 2018), so "the
# macros are surely fine" is not an assumption worth making silently.
#
# Note zmmul is handled separately and is NOT just "m without div":
# -march=rv32i_zmmul defines __riscv_zmmul and does NOT define
# __riscv_mul, so a naive check would conclude such a build has no
# multiply at all -- while its binary is full of mul instructions.
ifneq ($(findstring zmmul,$(ARCH)),)
  ARCH_HAS_MUL = 1
  ARCH_HAS_DIV = 0
else ifneq ($(findstring m,$(ARCH)),)
  ARCH_HAS_MUL = 1
  ARCH_HAS_DIV = 1
else
  ARCH_HAS_MUL = 0
  ARCH_HAS_DIV = 0
endif

ARCH_DEFS = -DZ_ARCH_HAS_MUL=$(ARCH_HAS_MUL) -DZ_ARCH_HAS_DIV=$(ARCH_HAS_DIV)

# The toolchain has to match ARCH, not just the -march flag.
#
# This tree was built against /opt/riscv32i -- the "pure RV32I
# toolchain" from picorv32's own build instructions, which the README
# links to. Its libgcc and newlib are compiled for rv32i ONLY. Passing
# -march=rv32im to it does not give you an rv32im build: at best gcc
# links the rv32i libraries anyway (so libc keeps calling __mulsi3 and
# you get none of the benefit where most of it would be), at worst it
# fails outright with no suitable multilib.
#
# picorv32's Makefile builds each variant into its own prefix, so the
# rv32im toolchain lives at /opt/riscv32im. Build it with:
#
#     make -C /path/to/picorv32 build-riscv32im-tools
#
# Deriving PREFIX from ARCH here means the two cannot drift apart.
#
# Override it for any other toolchain -- see docs/toolchain.md. The
# common ones:
#
#   xPack prebuilt        .../xpack-riscv-none-elf-gcc-VERSION/bin/riscv-none-elf-
#   riscv-gnu-toolchain   /opt/riscv/bin/riscv32-unknown-elf-
#   picorv32 per-arch     /opt/riscv32im/bin/riscv32-unknown-elf-
#
# The default below is an xPack install: prebuilt, current, and newlib
# based, which is what this tree is written against. Adjust the version
# in the path to match what you unpacked.
#
# Set it here, or pass it on the command line:
#   make BOARD=lakritz RISCV_PREFIX=riscv64-unknown-elf- flash
RISCV_PREFIX ?= /opt/xpack/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-

# C library.
#
# Not every RISC-V toolchain brings one. Debian/Ubuntu's
# gcc-riscv64-unknown-elf ships the compiler and libgcc only -- no
# headers, no libc -- so anything using printf/malloc needs picolibc
# alongside it (picolibc-riscv64-unknown-elf), selected with a specs
# file. Toolchains that bundle newlib (xPack, a self-built
# riscv-gnu-toolchain) need nothing extra.
#
# *** picolibc support is INCOMPLETE -- newlib is the supported libc. ***
#
# Zeitlos is written against newlib and picolibc is not a drop-in for
# it. Three separate incompatibilities have already turned up, and the
# third is not a flag away:
#
#   1. tinystdio does not define stdin/stdout/stderr; the application
#      must. Handled -- see the __PICOLIBC__ blocks in sw/os/kruntime.c
#      and sw/common/zeitlos.c.
#   2. picolibc.specs contains %{!T:-Tpicolibc.ld}, so it injects its
#      OWN linker script unless gcc can see a -T. -Wl,-T is invisible
#      to that test, so picolibc.ld was silently added and won,
#      relinking the kernel away from riscv-os.ld's 0x40000000 and
#      producing an image that could not boot. Handled -- every
#      Makefile now passes -T directly rather than through -Wl.
#   3. picolibc supplies its own sbrk() wanting __heap_start/__heap_end
#      from its linker script, which collides with this tree's own
#      _sbrk() in kruntime.c/zeitlos.c. NOT handled. This one needs a
#      decision about which heap owns memory, not a flag.
#
# So this detection exists to make the flags correct IF you deliberately
# choose picolibc, not to recommend it. With the default newlib prefix
# above it resolves to LIBC=newlib and none of this applies.
#
# Detected by asking the compiler whether it can find picolibc's specs
# file: -print-file-name echoes the name back unchanged when it can't,
# so a '/' in the answer means it was found. Override explicitly with
# LIBC=newlib or LIBC=picolibc.
LIBC ?= $(if $(findstring /,$(shell $(RISCV_PREFIX)gcc -print-file-name=picolibc.specs 2>/dev/null)),picolibc,newlib)

ifeq ($(LIBC),picolibc)
  LIBC_FLAGS = --specs=picolibc.specs
else
  LIBC_FLAGS =
endif

# The specs file is needed at LINK time too, not just when compiling --
# it supplies crt0 and the library paths, so without it the link fails
# with "cannot find crt0.o". Some Makefiles here define no LDFLAGS at
# all, hence the default; any that do set LDFLAGS themselves override
# this, and they append $(LIBC_FLAGS) explicitly.
LDFLAGS ?= -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
