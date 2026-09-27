# sw/apps/irc -- an IRC client. See docs/irc_app.md.
#
#   make            irc.bin
#   make test       the protocol tests, on the build machine
#   make render     the window, drawn on the build machine
#
# Build rules are shared: sw/common/app.mk, docs/build.md.

APP = irc

# irc_core.o is the protocol, with no I/O -- tests/test_irc.c builds it
# for the host. zrtc.o and zcfg.o: timestamps in system.rtc.timezone,
# and the apps.irc.* keys. zdns.o: the server's name.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zedit.o zport.o \
	zdns.o zcfg.o zrtc.o irc_core.o irc.o

HOSTCC ?= cc

test:
	$(HOSTCC) --std=gnu99 -Wall -Wextra -o /tmp/test_irc tests/test_irc.c irc_core.c
	/tmp/test_irc

# The window, drawn by the real repaint() on the build machine: LOOK at
# it before changing the layout (sw/common/tests/zrender.h). Needs
# -no-pie and vm.mmap_min_addr=0, as the other renders do.
C = ../../common
render:
	$(HOSTCC) -std=gnu99 -w -no-pie -I $(C) -I $(C)/tests -o /tmp/irc_render \
		tests/render.c irc_core.c $(C)/zwin.c $(C)/zedit.c $(C)/zfont_data.c \
		$(C)/zobj.c $(C)/zeitlos.c $(C)/zport.c $(C)/zdns.c $(C)/zcfg.c \
		$(C)/zrtc.c $(C)/tests/zspeak_stub.c
	/tmp/irc_render /tmp/irc
	@echo "wrote /tmp/irc.pbm -- look at it"

.PHONY: test render

include ../../common/app.mk
