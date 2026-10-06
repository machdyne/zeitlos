# sw/common/app.mk -- the build rules every app shares.
#
# See docs/build.md for the full description. In short, an app's
# Makefile sets a few variables and then includes this file, LAST:
#
#   APP = cal
#   APP_CFLAGS = -DCAL_FEATURE=1
#   OBJS = zeitlos.o zobj.o zgfx.o cal_core.o cal.o
#   include ../../common/app.mk
#
# Last, because this file reads APP, OBJS, SRC_DIRS, GC_SECTIONS and
# the other settings below at the moment it is included (a rule's
# prerequisites are expanded when make reads the rule), so every
# `OBJS +=` an app makes under a build option has to have happened by
# then. The app's own rules (tests, renders, special objects) can sit
# above the include; the default goal is pinned to $(APP) regardless.
#
# and gets `make` (cal.elf + cal.bin), `make clean`, header dependency
# tracking, and a rule that compiles any object in OBJS from either the
# app's own directory or sw/common. Everything below that an app might
# want to change is a `?=` default, so an app overrides it by setting
# the variable above the include.
#
# Variables an app sets:
#
#   APP           required. The target name: $(APP).elf and $(APP).bin.
#   OBJS          required. Every object to link, IN LINK ORDER. Link
#                 order decides the layout of the binary, so it is the
#                 app's to state, not this file's to derive.
#   APP_CFLAGS    extra compiler flags (-D, -I) for this app.
#   GFX_HW_BLIT   1 (default) draws text with the hardware glyph
#                 blitter (-DZ_GFX_HW_BLIT); 0 for the software path.
#   ZFMT          0 (default). 1 replaces newlib's printf family with
#                 sw/common/zfmt.c: integers only, ~50KB smaller. For
#                 the core apps in flash; see below.
#   SRC_DIRS      extra directories to search for a .c file, after the
#                 app's own directory and before sw/common (for example
#                 $(COMMON_DIR)/games).
#   GC_SECTIONS   1 (default) links with section garbage collection.
#   APP_LISTING   1 (default) also writes $(APP).asm from $(APP).c.
#   APP_MAIN_RULE 1 (default) this file provides the $(APP).o rule; set
#                 0 when the app compiles its main object itself.
#   LDLIBS        extra link inputs after $(OBJS), e.g. -lm.
#   APP_STACK     stack+heap size, written into the ZEXE flags word as
#                 a size code (sw/common/zexec.h, docs/executables.md):
#                 a power of two from 8K to the kernel's cap, e.g. 64K
#                 or 1M. Unset leaves flags 0, which the loader reads
#                 as "does not ask" (the default, 16KB).
#   CLEAN_EXTRA   extra files or directories for `make clean` to remove.
#                 A clean step that needs a command rather than a file
#                 list (a sub-make, say) is a prerequisite:
#                   clean: clean-tests
#                   clean-tests: ; $(MAKE) -C tests clean
#
# An object that needs different flags (the web browser's crypto at
# -O2, say) gets an explicit rule in the app's Makefile. An explicit
# rule always wins over the pattern rule here.

# The directory this file is in, as the including Makefile named it:
# ../../common from sw/apps/<app>, ../../../common from one level
# deeper. Keeping the RELATIVE path matters -- it is what reaches the
# compiler as the source file name, and so what __FILE__ (assert()'s
# messages) and the debug info say. Resolving it to an absolute path
# would make every binary depend on where the tree was checked out.
# `:=`, not `=`: MAKEFILE_LIST grows as make reads further files (the
# .d includes at the bottom of this one), so the last word has to be
# captured now, while it is still this file.
ifeq ($(origin COMMON_DIR),undefined)
COMMON_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
endif

PREFIX ?= $(RISCV_PREFIX)

# ARCH/ABI come from one place -- see that file for why.
include $(COMMON_DIR)/arch.mk

CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

# Section garbage collection. sw/common is shared by every app, so each
# app links whole objects for the sake of a fraction of what is in
# them; compiling every function and object into its own section lets
# the linker drop the rest. Set GC_SECTIONS=0 to compare.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# Text through the hardware glyph blitter, for every app. See
# docs/window_manager.md, "Hardware glyph blitting": seven register
# writes a glyph against ~5,300 cycles in software, and zgfx.c already
# falls back to software on its own for a font that is not resident in
# glyph memory or a glyph not wholly on screen -- so there is no app
# that needs it off. It is a switch rather than a constant for one
# job: `make GFX_HW_BLIT=0` builds the software path, to tell a
# blitter problem from a layout one. An app that draws no text is
# unaffected either way.
#
# It used to be each app's own -D, which let an app lose it quietly:
# 19 of them never had it, and sw/apps/read prints which path it got
# at startup because a lost flag once cost it all its text speed.
GFX_HW_BLIT ?= 1
ifeq ($(GFX_HW_BLIT),1)
GFX_CFLAGS = -DZ_GFX_HW_BLIT
endif

# Integer-only printf -- docs/build.md, "Integer-only printf (ZFMT)".
#
# OFF by default, on in the core apps that live in the flash archive
# (wm, net, term), whose space is fixed and was nearly full. newlib's
# printf can print a double, so it links a formatting engine, the
# double-to-decimal conversion and soft-float arithmetic whether or not
# a program prints one: about 50KB. zfmt.c is the same family (printf,
# fprintf, snprintf, ...) without floating point, in about 1.5KB, and
# still writes through newlib's FILE layer, so buffering and ordering
# do not change.
#
# The cost: %f, %e, %g and %a print "?". An app that sets ZFMT = 1
# must not need them -- check with the grep in docs/build.md -- and
# zobj.c's float printing switches to an integer-only form (Z_ZFMT).
ZFMT ?= 0
ifeq ($(ZFMT),1)
OBJS += zfmt.o
ZFMT_CFLAGS = -DZ_ZFMT
endif

APP_LISTING ?= 1
APP_MAIN_RULE ?= 1

# `override` so the flags an app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line value,
# not to this one -- which would silently drop -Os, -march, section GC,
# the libc specs and any -D the app needs. The result still builds and
# links; it is just a much larger, slower binary that can overrun the
# space the loader has for it and crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	$(GFX_CFLAGS) $(ZFMT_CFLAGS) $(APP_CFLAGS) $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = $(COMMON_DIR)/riscv-app.ld
MKEXEC = python3 $(COMMON_DIR)/../../tools/mkexec.py

# The app's own directory is searched first (make always looks in the
# current directory before vpath), then SRC_DIRS, then sw/common.
# No app has a source file with the same name as one in sw/common;
# docs/build.md says why that must stay true.
vpath %.c $(SRC_DIRS) $(COMMON_DIR)

# `make` with no target builds the app, even when the app's Makefile
# defines rules of its own (tests, renders) above the include.
.DEFAULT_GOAL := $(APP)

$(APP): $(APP).elf $(APP).bin

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(APP_MAIN_RULE),1)
$(APP).o: $(APP).c
ifeq ($(APP_LISTING),1)
	$(CC) $(CFLAGS) -c $< -S -MF /dev/null -o $(APP).asm
endif
	$(CC) $(CFLAGS) -c $< -o $@
endif

$(APP).elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,$(APP).map -o $@ $(OBJS) $(LDLIBS)
	$(PREFIX)objdump -S --disassemble $@ > $(APP).dasm

# A .zexe rather than a raw --pad-to binary: .bss is carried in the
# header as a NUMBER instead of as literal zeros read off the card on
# every launch. See sw/common/zexec.h and docs/executables.md.
# $(APP_STACK), when the Makefile set one, is the stack+heap size;
# mkexec.py turns it into the code, and a size it cannot encode fails
# the build. Unset, that argument is absent and flags stay 0.
ifdef APP_TIER
$(error APP_TIER is gone: set APP_STACK to a size instead (docs/executables.md))
endif
$(APP).bin: $(APP).elf
	@EDATA=$$($(PREFIX)nm $< | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm $< | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary $< $(APP).data; \
	$(MKEXEC) $(APP).data $@ $$(($$END - $$EDATA)) $(APP_STACK); \
	rc=$$?; rm -f $(APP).data; exit $$rc

clean:
	rm -rf $(APP).elf $(APP).bin $(APP).data $(APP).asm $(APP).dasm \
		$(APP).map *.o *.d $(CLEAN_EXTRA)

.PHONY: $(APP) clean

# Header dependency tracking. CFLAGS carries -MD, so gcc writes a .d
# file per object listing every header it read; including them makes
# each object depend on its own source and headers, so editing a .h
# rebuilds what uses it. Without this, an object once built was never
# rebuilt, and the link silently produced a binary from stale objects
# -- which has twice looked on hardware like a broken feature when the
# change had simply never been compiled in.
#
# Only the .d files of objects this app builds. The old per-app
# Makefiles included */*.d and */*/*.d as well, which in sw/apps/net
# also picked up sw/apps/net/netserve's -- whose paths are relative to
# netserve/, so they named ../../../common/*.c, which does not exist
# from net/, and every rebuild of net after netserve had been built
# stopped with "No rule to make target". An app whose objects live in
# a subdirectory names them that way in OBJS and is covered here.
-include $(wildcard *.d $(OBJS:.o=.d))
