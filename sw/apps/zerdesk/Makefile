# sw/apps/zerdesk -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The remote desktop, served by Zeitlos itself. net accepts the TCP
# connections and relays them here (Z_NET_LISTEN). The viewer page is
# the ESP32's, embedded unchanged: one file, both paths.

APP = zerdesk
APP_LISTING = 0
APP_MAIN_RULE = 0

OBJS = zeitlos.o zobj.o zport.o zcfg.o zsha1.o zws.o zscreen.o zinput.o zerdesk.o

PAGE_SRC = ../../../esp32/zeitlos-nic/web/index.html

page.h: $(PAGE_SRC)
	python3 -c "import sys;d=open(sys.argv[1],'rb').read();print('/* generated from %s: do not edit */'%sys.argv[1]);print('#define PAGE_LEN %d'%len(d));print('static const unsigned char page_html[PAGE_LEN] = {');[print(','.join(str(b) for b in d[i:i+24])+',') for i in range(0,len(d),24)];print('};')" $(PAGE_SRC) > page.h

zerdesk.o: zerdesk.c page.h deskcfg.h ../../common/zscreen.h ../../common/zinput.h ../../common/zws.h
	$(CC) $(CFLAGS) -c zerdesk.c -o zerdesk.o

HOSTCC ?= cc
.PHONY: test
test:
	$(HOSTCC) --std=gnu99 -Wall -Wextra -I. -o /tmp/test_deskcfg tests/test_deskcfg.c
	/tmp/test_deskcfg

CLEAN_EXTRA = page.h

include ../../common/app.mk
