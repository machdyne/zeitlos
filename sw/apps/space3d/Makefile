
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# -DZ_GFX_HW_BLIT, same as wm/hello_win/term: this app draws its score
# and game-over text with z_font_5x8, so it wants the hardware glyph
# blitter rather than zgfx.c's software fallback. Note it does NOT
# call z_gfx_hw_font_load() -- wm loads z_font_5x8 into the shared
# glyph memory exactly once at its own startup and is the only process
# on the board that ever does (see wm.c's main(), and
# docs/window_manager.md, "Hardware glyph blitting"). That's also why
# z_font_5x8 is the only font this app draws with.
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line value,
# not to this one -- silently dropping -Os, -march, section GC, the
# libc specs and any -D this app needs to build correctly. The result
# still builds and links; it just produces a much larger, slower binary
# that can overrun the space the loader has for it and crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(ARCH_DEFS) $(LIBC_FLAGS)
# -march/-mabi on the LINK line too, not just for compiling: gcc uses
# them to pick which multilib's libc/libgcc to search, and omitting
# them silently falls back to the toolchain default -- which may assume
# an ISA the configured PicoRV32 doesn't have. See docs/user_input.md,
# "Debugging notes".
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o space3d.o

space3d: space3d.elf space3d.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

space3d.o:
	$(CC) $(CFLAGS) -c space3d.c -S -o space3d.asm
	$(CC) $(CFLAGS) -c space3d.c -o space3d.o

space3d.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,space3d.map -o space3d.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble space3d.elf > space3d.dasm

space3d.bin: space3d.elf
	@EDATA=$$($(PREFIX)nm space3d.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm space3d.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary space3d.elf space3d.data; \
	python3 ../../../tools/mkexec.py space3d.data space3d.bin $$(($$END - $$EDATA)); \
	rm -f space3d.data

clean:
	rm -f space3d.elf space3d.bin space3d.data space3d.asm space3d.dasm space3d.map *.o *.d

.PHONY: space3d clean

# Header dependency tracking.
#
# CFLAGS has carried -MD all along, so gcc has been writing .d files
# listing every header each object depends on -- but nothing included
# them, and the object rules below have no prerequisites at all
# (`wm.o:` rather than `wm.o: wm.c`). The effect is that once an object
# file exists, make considers it permanently up to date: editing a .c
# or a .h rebuilds nothing, and the link silently produces a binary
# from stale objects.
#
# That is not a theoretical problem -- it has twice produced hardware
# that behaved like a broken feature when the real cause was simply
# that the change was never compiled in. Including the .d files makes
# every object depend on its own source and headers, so `make` alone is
# correct and `make clean` stops being a required ritual.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
