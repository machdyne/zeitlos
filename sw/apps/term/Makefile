
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# See sw/apps/repl/Makefile for the full reasoning behind these two --
# in short, sw/common is shared by every app, so each links whole
# objects for the sake of a fraction of them, and section GC drops the
# rest. Measured saving on this app is noted in docs/scheme_api.md's
# memory-budget section. Set GC_SECTIONS=0 to disable.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# NOTE: every later append to CFLAGS in this file must also say
# `override`. A plain `CFLAGS += ...` after an override assignment is
# SILENTLY IGNORED by make, which is how -DNET_PHY_$(NET_PHY) went
# missing once: the build still succeeded through compilation and
# failed at link with undefined enc28j60_* symbols, because the header
# had selected one driver and the makefile linked the other. The
# quieter half was worse -- DHCP, NTP, SSH and the static IP settings
# had all reverted to their defaults with nothing said.
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line value,
# not to this one -- silently dropping -Os, -march, section GC, the
# libc specs and any -D this app needs to build correctly. The result
# still builds and links; it just produces a much larger, slower binary
# that can overrun the space the loader has for it and crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# which font to build against -- e.g. `make term FONT=z_font_6x12`.
# must be one of the z_font_t externs declared in zfont.h. see term.c's
# own comment on TERM_FONT_NAME for why this exists.
ifdef FONT
override CFLAGS += -DTERM_FONT_NAME=$(FONT)
endif

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zkbd.o zvt100.o zport.o \
	zdns.o zconnect.o term.o

term: term.elf term.bin

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

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

zvt100.o:
	$(CC) $(CFLAGS) -c ../../common/zvt100.c -o zvt100.o

zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

# The F11 Open bar (term.c) -- shared with repl's port/serial/telnet/
# ssh commands. zdns comes with it: telnet and ssh resolve hostnames.
zdns.o:
	$(CC) $(CFLAGS) -c ../../common/zdns.c -o zdns.o

zconnect.o:
	$(CC) $(CFLAGS) -c ../../common/zconnect.c -o zconnect.o

term.o:
	$(CC) $(CFLAGS) -c term.c -S -o term.asm
	$(CC) $(CFLAGS) -c term.c -o term.o

term.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,term.map -o term.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble term.elf > term.dasm

term.bin: term.elf
	@EDATA=$$($(PREFIX)nm term.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm term.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary term.elf term.data; \
	python3 ../../../tools/mkexec.py term.data term.bin $$(($$END - $$EDATA)); \
	rm -f term.data

clean:
	rm -f term.elf term.bin term.data term.asm term.dasm term.map *.o *.d

.PHONY: term clean

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
