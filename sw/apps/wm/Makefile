# sw/apps/wm -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = wm

# A core app in the flash archive: integer-only printf, about 50KB
# smaller. It prints no floating point. docs/build.md, "Integer-only
# printf (ZFMT)".
ZFMT = 1
# ...and stdout without newlib's FILE layer, and no 64-bit printing
# (docs/build.md, "ZFMT_LITE", "ZFMT_NO64"): about 15KB of RAM and of
# flash, which pays for the dock's icons being read from dock.zic into
# a buffer (docs/dock.md). wm prints whole lines to stdout only, and
# never a 64-bit number.
ZFMT_LITE = 1
ZFMT_NO64 = 1

# Objects, and why each is linked:
#
#   zspeak.o: the speech keys (Super+S/A/C/W/R) -- docs/tts.md
#
#   zcfg.o: system.keyboard.layouts -- see docs/keyboard_layouts.md
#
#   dock_cfg.o: reads wm.cfg and dock.zic, the dock's arrangement and
#   icons -- docs/dock.md
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zkbd.o zspeak.o zcaption.o zcfg.o zaccent.o dock_cfg.o win_icons.o workspace.o wm.o

# The dock's icons are not compiled in: they are /data/wm/dock.zic, made
# by sw/data/icons/gen_dock_icon_data.py and put in the flash archive by
# the top-level Makefile (CORE_FILES). docs/dock.md.

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
	@# workspace.c: the workspace decisions (docs/window_manager.md)
	@$(HOSTCC) $(HOSTFLAGS) -o /tmp/wm_test_workspace \
		tests/test_workspace.c workspace.c
	@/tmp/wm_test_workspace
	@# dock_cfg.c: wm.cfg and dock.zic (docs/dock.md), with the real files
	@$(HOSTCC) $(HOSTFLAGS) -o /tmp/wm_test_dock_cfg tests/test_dock_cfg.c dock_cfg.c
	@/tmp/wm_test_dock_cfg ../../data/wm.cfg ../../data/icons/dock.zic
	@# the real wm.c building its dock from those files; needs the
	@# page at address 0 (sw/common/tests/ztramp.h), so exit 77 = skipped
	@$(HOSTCC) -std=gnu99 -w -no-pie -I ../../common -o /tmp/wm_test_dock_files \
		tests/test_dock_files.c ../../common/zwin.c ../../common/zwidget.c \
		../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c \
		../../common/zkbd.c ../../common/zspeak.c ../../common/zcaption.c \
		../../common/zcfg.c ../../common/zaccent.c dock_cfg.c win_icons.c workspace.c
	@/tmp/wm_test_dock_files ../../data/wm.cfg ../../data/icons/dock.zic /tmp/wm_test_dock \
		> /tmp/wm_test_dock.log; r=$$?; tail -1 /tmp/wm_test_dock.log; \
		[ $$r -eq 0 ] || [ $$r -eq 77 ]
	@# kbd_accent.h: a dead key with Ctrl, Super or Alt is not an accent
	@$(HOSTCC) $(HOSTFLAGS) -o /tmp/wm_test_kbd_accent tests/test_kbd_accent.c
	@/tmp/wm_test_kbd_accent
	@# locking: freeze, empty region, then the lock screen (tests/test_lock.c)
	@$(HOSTCC) -std=gnu99 -w -no-pie -I../../common -o /tmp/wm_test_lock tests/test_lock.c \
		../../common/zwin.c ../../common/zwidget.c ../../common/zfont_data.c \
		../../common/zobj.c ../../common/zeitlos.c ../../common/zkbd.c \
		../../common/zspeak.c ../../common/zcaption.c ../../common/zcfg.c \
		../../common/zaccent.c dock_cfg.c win_icons.c workspace.c
	@/tmp/wm_test_lock /tmp/wm_test_lock; r=$$?; [ $$r -eq 0 ] || [ $$r -eq 77 ]
	@# desktop colour: the accent plane, the hand-over (tests/test_accent.c)
	@$(HOSTCC) -std=gnu99 -w -no-pie -I../../common -o /tmp/wm_test_accent tests/test_accent.c \
		../../common/zwin.c ../../common/zwidget.c ../../common/zfont_data.c \
		../../common/zobj.c ../../common/zeitlos.c ../../common/zkbd.c \
		../../common/zspeak.c ../../common/zcaption.c ../../common/zcfg.c \
		../../common/zaccent.c dock_cfg.c win_icons.c workspace.c
	@/tmp/wm_test_accent /tmp/wm_test_accent > /tmp/wm_test_accent.log; r=$$?; \
		tail -1 /tmp/wm_test_accent.log; [ $$r -eq 0 ] || [ $$r -eq 77 ]

.PHONY: test

# 8KB. A message loop, and the clip payload no longer leaks.
# This is the size that left room for a second term on a 1MB board.
APP_STACK = 8K

include ../../common/app.mk
