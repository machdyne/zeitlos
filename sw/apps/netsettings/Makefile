# sw/apps/netsettings -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# net.cfg for netx (docs/netsettings.md). cfgfile.c is the file model;
# its host test reads what it writes with netx's own parser.

APP = netsettings
ZFMT = 1

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zdialog.o zflist.o zedit.o ztype.o cfgfile.o netsettings.o

HOSTCC ?= cc
.PHONY: test
test:
	$(HOSTCC) -std=gnu99 -Wall -DNETX -I. -I../../common -o /tmp/test_cfgfile \
		tests/test_cfgfile.c cfgfile.c ../net/netcfg.c
	/tmp/test_cfgfile

include ../../common/app.mk
