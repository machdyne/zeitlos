# Zeitlos kidgames.

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why. Never
# hardcode them here: some boards are rv32i and a hardcoded rv32im
# produces a binary that traps on its first multiply.
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

# sw/common is shared by every app, so each links whole objects for the
# sake of a fraction of them, and section GC drops the rest. See
# sw/apps/repl/Makefile for the full reasoning.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT, same as wm/chess/poker/cal/clock: text goes through
# the hardware glyph blitter. This app does NOT call
# z_gfx_hw_font_load() itself -- wm is the only process board-wide that
# writes glyph memory, and it loads z_font_5x8 at startup, which is the
# only font used here. See docs/window_manager.md, "Fonts in glyph
# memory".
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment REPLACES the makefile's CFLAGS
# outright, silently dropping -Os, -march, section GC and the libc
# specs. The result still links; it is just three times the size, and
# past a certain size it overruns the loader's space and crashes on
# start with nothing in the build output to say so
# (docs/app_runtime.md). For a one-off option use:
#
#     make clean && make EXTRA_CFLAGS=-DSOMETHING=1
#
# EVERY LATER APPEND IN THIS FILE MUST ALSO SAY `override`. A plain
# `CFLAGS +=` after an override assignment is silently ignored by make,
# which is how sw/apps/net ended up compiling against one PHY header
# and linking the other.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) $(EXTRA_CFLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# zfsapp.o is kgsave.c's: fs_read_file/fs_write_file/fs_mkdir are
# DECLARED in sw/common/zfsapp.h and DEFINED in sw/common/zfsapp.c, so
# using them needs the object in the link. It was missing until the
# first real RISC-V link -- see `linkcheck` below for why nothing on
# this side noticed, and what now does.
COMMONOBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zrng.o zfsapp.o

APPOBJS = kgui.o kgwidget.o kgpad.o kgfont.o kgfmt.o \
	kgsave.o kgrand.o wordlist.o levelup.o kground.o \
	games_word.o games_number.o games_guess.o games_memory.o games_animal.o \
	animals.o kgart.o kgsound.o games.o kidgames.o

OBJS = $(COMMONOBJS) $(APPOBJS)

kidgames: kidgames.elf kidgames.bin

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

# The ChaCha20 generator, seeded from rtl/trng.v where the board has
# one. A board without a TRNG still gets a different sequence each
# boot; see zrng.h.
zrng.o:
	$(CC) $(CFLAGS) -c ../../common/zrng.c -o zrng.o

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

kidgames.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,kidgames.map -o kidgames.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble kidgames.elf > kidgames.dasm

# A .zexe rather than a raw --pad-to binary: the header carries .bss as
# a NUMBER instead of writing it out as literal zeros to be read off
# the SD card on every launch. See sw/common/zexec.h.
kidgames.bin: kidgames.elf
	@EDATA=$$($(PREFIX)nm kidgames.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm kidgames.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary kidgames.elf kidgames.data; \
	python3 ../../../tools/mkexec.py kidgames.data kidgames.bin $$(($$END - $$EDATA)); \
	rm -f kidgames.data

# -- host-side tests ----------------------------------------------------
#
# Native cc, no RISC-V toolchain, and the REAL sources in every target.
# There is no second copy of the layout, the font or the keyboard
# anywhere in this tree.
HOSTCC ?= cc

HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2

# -I ../../common, for the panel targets ONLY.
#
# It is here rather than in HOSTFLAGS for the reason sw/apps/chess's
# Makefile records: with the flag in a shared variable a bare
# #include "zgfx.h" in an app source compiles in every host target and
# then fails on the first line of the TARGET build, which has no such
# -I. The flag exists purely for sw/common/tests/zrender.h's own sake.
# -DKG_SAVE_HOSTED for the same reason the data tests use it: the panel
# targets link kground.c, which saves progress, and there is no
# filesystem on a build machine. The parser and serialiser are still
# the shipped ones -- only the two functions that call fs_* are
# compiled out. See kgsave.h.
PANELFLAGS = $(HOSTFLAGS) -I ../../common -DKG_SAVE_HOSTED -DKG_SOUND_HOSTED

# Everything the panel targets need: this app's drawing, plus the real
# zwin/zfont/zobj/zeitlos. zgfx.c is deliberately absent -- zrender.h
# supplies software pixel plotting instead, because the real one
# programs GPU registers that are unmapped MMIO on a build machine.
# kgwidget.c is NOT here. tests/render.c #includes it, so that it can
# call its static draw functions and show a menu or a message box
# WITHOUT running the input loop inside them -- which would block on a
# message queue that does not exist on a build machine. Linking it as
# well would be a duplicate definition of every one of them.
PANELSRC = kgui.c kgpad.c kgfont.c kgfmt.c \
	../../common/zwin.c ../../common/zfont_data.c \
	../../common/zobj.c ../../common/zeitlos.c

# The unattended tests link the widgets and the registry normally --
# they call the public entry points, not the statics.
TESTSRC = $(PANELSRC) kgwidget.c games.c levelup.c wordlist.c \
	kground.c kgsave.c games_word.c games_number.c games_guess.c \
	games_memory.c games_animal.c animals.c kgart.c kgsound.c \
	tests/kg_rand_host.c

# The pure half: a parser, a serialiser and some word tables, with no
# Zeitlos headers and no framebuffer anywhere in them. Built with
# -DKG_SAVE_HOSTED so kgsave.c's two filesystem functions are not
# compiled, and with the deterministic generator rather than the TRNG
# one -- see kgrand.h for why that is not merely a stub.
DATASRC = kgsave.c kgfmt.c wordlist.c animals.c kgart.c kgsound.c \
	tests/kg_rand_host.c
DATAFLAGS = $(HOSTFLAGS) -DKG_SAVE_HOSTED -DKG_SOUND_HOSTED

# Every source the target build compiles, checked with the host
# compiler and NO -I, which is the header search the target build
# actually has. -fsyntax-only, so this needs no RISC-V toolchain.
#
# DERIVED from APPOBJS rather than listed, and that is not tidiness:
# the two were written out separately and drifted. kgart.c was in
# APPOBJS and missing here, so the generated art was the one target
# source no check ever looked at. `linkcheck` found it by failing on
# eight undefined kg_art_* symbols -- a strange way to discover that a
# list was stale, and a good argument for there being only one list.
TARGETSRC = $(APPOBJS:.o=.c)

includecheck:
	@for f in $(TARGETSRC); do \
		$(HOSTCC) -std=gnu99 -Wall -fsyntax-only $$f || exit 1; \
	done
	@echo "includecheck: every target source resolves its headers with no -I"

# -- the missing-object trap --
#
# fs_read_file(), fs_write_file() and fs_mkdir() are declared in a
# header and defined in sw/common/zfsapp.c, so using them needs
# zfsapp.o in the link -- and nothing on this side noticed when it was
# absent. `includecheck` only resolves headers, and every host test
# builds kgsave.c with -DKG_SAVE_HOSTED, which compiles those three
# calls out entirely. The first thing to complain was the real RISC-V
# link: the one step that needs a cross toolchain, and so the one step
# that could not be run while writing this.
#
# This closes that. It compiles every target source AND every
# sw/common source in COMMONOBJS with the HOST compiler, then checks
# that no symbol of ours is left undefined across the lot. It is the
# link step in miniature, and it needs no cross toolchain.
#
# KG_SAVE_HOSTED and KG_SOUND_HOSTED are deliberately NOT defined
# here: those two files are exactly the ones whose real bodies this
# has to see.
#
# COMMON_HOSTABLE is COMMONOBJS minus zrng.c, which has RISC-V inline
# asm (`rdcycle`) and cannot be assembled on an x86 build machine. Its
# exports are allow-listed by prefix instead -- one narrow hole, named
# rather than papered over.
COMMON_HOSTABLE = zeitlos zobj zgfx zfont_data zwin zfsapp

# Only symbols with one of these prefixes are ours; anything else
# undefined is libc (memcpy, strlen, puts) and resolves at the real
# link. z_rng_ is the zrng.c hole above.
OURS = '^(kg_|z_|fs_|game_|games_|wordlist_|animals_|levelup_)'
ALLOWED = '^z_rng_'

linkcheck:
	@rm -rf /tmp/kg_link && mkdir -p /tmp/kg_link
	@for f in $(TARGETSRC); do \
		$(HOSTCC) -std=gnu99 -w -c $$f -o /tmp/kg_link/$$(basename $$f .c).o \
			|| exit 1; \
	done
	@for f in $(COMMON_HOSTABLE); do \
		$(HOSTCC) -std=gnu99 -w -c ../../common/$$f.c -o /tmp/kg_link/$$f.o \
			|| exit 1; \
	done
	@nm --defined-only /tmp/kg_link/*.o 2>/dev/null \
		| awk '{print $$3}' | sort -u > /tmp/kg_link/defined
	@nm -u /tmp/kg_link/*.o 2>/dev/null \
		| awk '{print $$2}' | sort -u > /tmp/kg_link/undefined
	@comm -23 /tmp/kg_link/undefined /tmp/kg_link/defined \
		| grep -E $(OURS) | grep -vE $(ALLOWED) > /tmp/kg_link/missing || true
	@if [ -s /tmp/kg_link/missing ]; then \
		echo "linkcheck: FAILED -- undefined at link time:"; \
		sed 's/^/  /' /tmp/kg_link/missing; \
		echo "  add the sw/common .c that defines these to COMMONOBJS"; \
		exit 1; \
	fi
	@echo "linkcheck: every symbol this app uses is in the link"

# -- the floating-point trap --
#
# Some boards here are rv32i (sw/common/arch.mk), with no FPU and no
# soft-float linked by default. One `double` pulls in the whole
# soft-float runtime -- tens of kilobytes, on an app that crashes on
# start if it outgrows the loader's space. Nothing in the build output
# would mention it.
#
# Every number in this app is an integer, and this checks that it stays
# that way. LEXICAL, and honest about it: the definitive check is
# `nmcheck` below, which needs the toolchain. This one runs everywhere
# and catches the mistake where somebody makes it.
#
# COMMENTS ARE STRIPPED FIRST (-fpreprocessed -dD -E) so that macros
# survive and prose does not -- otherwise this fails on its own
# explanation of why there is no floating point here.
nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			grep -nE '[0-9]\.[0-9]'; then \
			echo "nofloat: FAILED -- a decimal literal in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: kidgames is integer-only"

# -- the printf trap --
#
# An app whose printf calls are all plain strings does not link the
# formatter: the compiler rewrites printf("literal\n") into puts(), and
# --gc-sections drops vfprintf. ONE conversion specifier links it back
# in and costs on the order of 100KB -- enough to push this app past
# the loader's space, with the binary tripling looking entirely
# unrelated to the one-line debug print that caused it. sw/apps/read
# hit this twice in a week.
#
# So: no printf with a specifier, and no fputs/stdout either (a FILE
# drags in ~40KB of stdio on its own). Numbers are formatted by
# kg_utoa/kg_itoa and emitted with puts(). See docs/app_runtime.md.
# COMMENTS ARE STRIPPED FIRST, exactly as `nofloat` above does it, and
# for exactly the same reason -- this check failed on its own
# explanation of why there is no printf here the first time it ran,
# which is a funny way to find out that a check reads its own
# documentation as evidence against itself. -fpreprocessed keeps the
# includes unexpanded, so this stays fast and needs no -I.
noprintf:
	@for f in $(TARGETSRC); do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			grep -nE '(^|[^_[:alnum:]])(printf|fprintf|sprintf|snprintf|fputs|vprintf)[[:space:]]*\(' ; then \
			echo "noprintf: FAILED in $$f -- use kg_utoa()/kg_itoa() and puts()"; \
			exit 1; \
		fi; \
	done
	@echo "noprintf: no formatter is linked"

# The DEFINITIVE version of nofloat, needing the toolchain and a
# completed build. Scoped to this app's own objects: sw/common/zobj.c
# has a float member in z_obj_t and is linked by every app in the tree,
# which is a property of the shared object system and a separate
# question from whether THIS app kept its arithmetic integer. Failing
# on somebody else's float would make the check something people turn
# off.
nmcheck:
	@if $(PREFIX)nm $(APPOBJS) | \
		grep -E 'U __(add|sub|mul|div|fix|float|cmp|unord)[ds]f'; then \
		echo "nmcheck: FAILED -- soft-float reached this app's objects"; \
		exit 1; \
	fi
	@echo "nmcheck: no soft-float in this app's target objects"
	@$(PREFIX)size kidgames.elf 2>/dev/null || true

# Unattended, and fast -- there is no search or simulation in this app.
#
#   includecheck  the headers resolve the way the target build needs
#   linkcheck     every symbol this app uses is actually in the link --
#                 the check that would have caught zfsapp.o missing
#   nofloat       no floating point reaches an rv32i board
#   noprintf      no printf formatter, no stdio FILE
#   test_layout   the band geometry, the big-font run decomposition,
#                 and -- the one that earns its keep -- that nothing is
#                 drawn outside the 320x240 playfield
#   test_pad      every key reachable, no overlaps, clicks and
#                 keystrokes landing on the same key, and the debounce
#   test_data     the save file against every way a hand edit can
#                 break it, and the word tiers against their own
#                 length rules
test: includecheck linkcheck nofloat noprintf
	$(HOSTCC) $(PANELFLAGS) -o /tmp/kg_layout tests/test_layout.c $(TESTSRC)
	$(HOSTCC) $(PANELFLAGS) -o /tmp/kg_pad tests/test_pad.c $(TESTSRC)
	$(HOSTCC) $(DATAFLAGS) -o /tmp/kg_data tests/test_data.c $(DATASRC)
	/tmp/kg_layout
	/tmp/kg_pad
	/tmp/kg_data

# LOOK AT THE OUTPUT before changing any layout.
#
# sw/apps/logic's panel shipped wrong three times against a passing
# arithmetic test, and the third bug was found in one look at a render.
# The whole reason this app has a fixed 320x240 playfield is so that
# what comes out here is exactly what a board shows.
#
#   make render                 the menu
#   make render WHAT=pad        the on-screen keyboard, alpha
#   make render WHAT=digits     the on-screen keyboard, digits
#   make render WHAT=field      a question with a text field
#   make render WHAT=msg        a message box
#   make render WHAT=font       the big font, every glyph
WHAT ?= menu

render:
	$(HOSTCC) $(PANELFLAGS) -o /tmp/kg_render tests/render.c $(PANELSRC) \
		kgsave.c wordlist.c levelup.c animals.c kgart.c kgsound.c \
		tests/kg_rand_host.c
	/tmp/kg_render /tmp/kidgames.pbm $(WHAT)
	@echo "wrote /tmp/kidgames.pbm ($(WHAT)) -- others: make render \
WHAT=pad|digits|field|msg|font"

clean:
	rm -f *.o *.d kidgames.elf kidgames.bin kidgames.data kidgames.map \
		kidgames.dasm

.PHONY: kidgames test clean includecheck linkcheck nofloat noprintf nmcheck render

# Header dependency tracking -- see sw/apps/wm/Makefile. Without this,
# editing a .c or .h rebuilds nothing once the object exists, and the
# link silently produces a binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
