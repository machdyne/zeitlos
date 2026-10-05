# sw/apps/term -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = term

# A core app in the flash archive: integer-only printf, about 50KB
# smaller. It prints no floating point. docs/build.md, "Integer-only
# printf (ZFMT)".
ZFMT = 1

# NOTE: every later append to CFLAGS in this file must also say
# `override`. A plain `CFLAGS += ...` after an override assignment is
# SILENTLY IGNORED by make, which is how -DNET_PHY_$(NET_PHY) went
# missing once: the build still succeeded through compilation and
# failed at link with undefined enc28j60_* symbols, because the header
# had selected one driver and the makefile linked the other. The
# quieter half was worse -- DHCP, NTP, SSH and the static IP settings
# had all reverted to their defaults with nothing said.

# which font to build against -- e.g. `make term FONT=z_font_6x12`.
# must be one of the z_font_t externs declared in zfont.h. see term.c's
# own comment on TERM_FONT_NAME for why this exists.
ifdef FONT
override CFLAGS += -DTERM_FONT_NAME=$(FONT)
endif

# Scrollback depth in lines -- `make term SCROLLBACK=1000`. Each line is
# VT_COLS (80) bytes of .bss, which is RAM for every term instance's
# whole lifetime (docs/boot.md, "Memory budget"): 200 is eight screens
# and 16,000 bytes. term.c refuses anything outside 25..8000. See
# docs/terminal.md, "Scrollback".
SCROLLBACK ?= 200
override CFLAGS += -DTERM_HIST_LINES=$(SCROLLBACK)

# Font -- `make term FONT=z_font_6x12` for a terminal Japanese can be
# drawn in (with sw/apps/jfont running; docs/terminal.md, "UTF-8").
# 5x8 has no Japanese font small enough to read: wide characters are
# boxes there, two columns wide, and copy as themselves all the same.
FONT ?= z_font_5x8
override CFLAGS += -DTERM_FONT_NAME=$(FONT)

# Objects, and why each is linked:
#
#   zspeak.o: The start panel's buttons and the scrollback scrollbar. Section GC
#   drops the widget kinds term never uses (sliders, swatches).
#
#   zdns.o: The F11 Open bar (term.c) -- shared with repl's port/serial/telnet/
#   ssh commands. zdns comes with it: telnet and ssh resolve hostnames.
#
#   zcfg.o: apps.term.auto_connect -- docs/config.md
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o zsayall.o zkbd.o zvt100.o \
	zport.o zdns.o zconnect.o zcfg.o term.o

# 8KB, same as wm. Port sends outlive the call and are bounded.
APP_STACK = 8K

include ../../common/app.mk
