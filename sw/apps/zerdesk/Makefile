# sw/apps/zerdesk -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The remote desktop, served by Zeitlos itself. net accepts the TCP
# connections and relays them here (Z_NET_LISTEN). The viewer page is
# not built in: it is read from the card, /zerdesk/index.html, where the
# card image puts the ESP32's esp32/zeitlos-nic/web/index.html
# (release/lib/mkfatimg.py). One file, both paths.

APP = zerdesk
APP_LISTING = 0
APP_MAIN_RULE = 0

OBJS = zeitlos.o zobj.o zport.o zcfg.o zfsapp.o zsha1.o zws.o zscreen.o zinput.o zerdesk.o

zerdesk.o: zerdesk.c deskcfg.h ../../common/zscreen.h ../../common/zinput.h ../../common/zws.h
	$(CC) $(CFLAGS) -c zerdesk.c -o zerdesk.o

HOSTCC ?= cc
.PHONY: test
test:
	$(HOSTCC) --std=gnu99 -Wall -Wextra -I. -o /tmp/test_deskcfg tests/test_deskcfg.c
	/tmp/test_deskcfg

# 32KB. zport keeps a heap copy of every stripe in flight, up to the
# 6KB budget, and the page is read while that is live. The unnamed
# default is 16KB, which left almost nothing past three viewers.
APP_TIER = MEDIUM

include ../../common/app.mk
