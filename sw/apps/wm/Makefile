# sw/apps/wm -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = wm

# A core app in the flash archive: integer-only printf, about 50KB
# smaller. It prints no floating point. docs/build.md, "Integer-only
# printf (ZFMT)".
ZFMT = 1

# Objects, and why each is linked:
#
#   zspeak.o: the speech keys (Super+S/A/C/W/R) -- docs/tts.md
#
#   zcfg.o: system.keyboard.layouts -- see docs/keyboard_layouts.md
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zkbd.o zspeak.o zcaption.o zcfg.o dock_icons.o win_icons.o wm.o

# dock_icons.c/.h are generated -- see sw/data/icons/gen_dock_icon_data.py.
# not regenerated automatically here (this Makefile doesn't depend on
# Python/Pillow being available) -- run that script by hand after
# adding or changing an icon PNG, same as font/logo data (see
# dock_icons.h's own header comment).
dock_icons.o: dock_icons.c dock_icons.h
	$(CC) $(CFLAGS) -c dock_icons.c -o dock_icons.o

# win_icons.c/.h are hand-edited (binary literals), not generated --
# see win_icons.h's own header comment. Titlebar close icon and any
# future window icon (minimize, open file, save file, ...) live here.
win_icons.o: win_icons.c win_icons.h
	$(CC) $(CFLAGS) -c win_icons.c -o win_icons.o

# -- host tests ------------------------------------------------------
#
# Need only `cc`. No toolchain, no board. tests/test_dock.c includes
# dock_layout.h directly, so it tests the arithmetic that actually
# runs rather than a copy of it -- see that file's own header.
#
# tests/test_region.c predates that arrangement and still carries its
# own lifted copy of the region code; it is built here too so both run
# from one command.

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g

test:
	@$(HOSTCC) $(HOSTFLAGS) -o /tmp/wm_test_dock tests/test_dock.c
	@/tmp/wm_test_dock
	@$(HOSTCC) $(HOSTFLAGS) -o /tmp/wm_test_region tests/test_region.c
	@/tmp/wm_test_region

.PHONY: test

include ../../common/app.mk
