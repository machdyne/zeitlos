# sw/apps/cal -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = cal

# It matters more than usual here: this app links zrtc.o for
# z_time_to_tm()/z_tm_to_time()/z_month_name_long(), and section GC is
# what stops the long month-name table being carried by every OTHER
# app that links zrtc.o for the register accessors alone.

# cal_core.o is the month geometry and the year-entry state machine,
# kept free of any I/O so the test suite below can build it for the
# host -- see tests/cal_test.c. It has no date arithmetic of its own;
# that is zrtc.o, which is the same code the kernel and clock use.

# Objects, and why each is linked:
#
#   zcfg.o: system.rtc.timezone -- docs/config.md
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o zrtc.o \
	zcfg.o cal_core.o cal.o

# -- host-side tests --
#
# Native cc, no RISC-V toolchain involved. cal.c, cal_core.c, zrtc.c,
# zwin.c and zwidget.c are the REAL sources in all three targets; only
# the pixel plotting is software (sw/common/tests/zrender.h).
#
# zrtc.c compiles on a host unmodified -- its MMIO accessors are
# static inlines that nothing in the tests calls -- which is what lets
# the calendar arithmetic under test be the shipped calendar
# arithmetic rather than a copy of it.
HOSTCC ?= cc

# Two lists, because the two kinds of test need different things and
# using one for both does not link.
#
# CORESRC is the logic: the grid, the year entry and the calendar
# arithmetic under them. Nothing here draws, so nothing here needs the
# window layer -- and it must NOT have it, because zwin.c and
# zwidget.c reference the z_fb_* pixel primitives that only
# sw/common/tests/zrender.h supplies. Linking them into a test that
# does not include that header fails with a page of undefined
# references to functions the test never calls.
CORESRC = ../../common/zrtc.c cal_core.c

# PANELSRC adds the real window and widget layers, for the two tests
# that link cal.c -- and zcfg.c, which cal.c calls for
# system.rtc.timezone (docs/config.md). With no kernel behind them the
# config calls read as "not set", so these still exercise UTC.
#
# that do include zrender.h and therefore do have the software pixel
# primitives. zgfx.c is deliberately absent -- see zrender.h's header
# for why the plotting is stubbed and the layout is not.
PANELSRC = $(CORESRC) ../../common/zwin.c ../../common/zwidget.c \
	../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c \
	../../common/zcfg.c ../../common/tests/zspeak_stub.c

HOSTFLAGS = -std=gnu99 -Wall -no-pie -I ../../common

# The layout render (sw/common/tests/zrender.h) -- see
# docs/window_manager.md, "Rendering a panel on the build machine".
# LOOK at the output before changing the grid.
#
# Four states, because three of them are awkward to reach on hardware:
#
#   make render                 today marked, ordinary five-row month
#   make render WHAT=sixrow     31 days from a Saturday -- all six rows
#   make render WHAT=epoch      no clock: 1970, no mark, dead `<`
#   make render WHAT=nortc      no RTC in the bitstream at all
#   make render WHAT=entry      a year being typed
WHAT ?= today

render:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/cal_render tests/render.c $(PANELSRC)
	/tmp/cal_render /tmp/cal.pbm $(WHAT)
	@echo "wrote /tmp/cal.pbm ($(WHAT)) -- other states: make render WHAT=sixrow|epoch|nortc|entry"

# Unattended. cal_test covers the grid arithmetic and the year-entry
# state machine across every month from 1970 to 2105; test_layout
# covers the geometry that a render would show but only if somebody
# looks.
test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/cal_test tests/cal_test.c $(CORESRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/cal_test_layout tests/test_layout.c $(PANELSRC)
	/tmp/cal_test
	/tmp/cal_test_layout

.PHONY: render test

include ../../common/app.mk
