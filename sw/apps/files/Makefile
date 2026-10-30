# sw/apps/files -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = files

# zflist.o is the browsing widget this app is mostly made of, zdialog.o
# the confirm/prompt dialogs, ztype.o the extension-to-app table.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zfsutil.o zflist.o zdialog.o zedit.o ztype.o zconvert.o zwalk.o zfsc.o files.o

# -- host-side test --
#
# files.c itself (main renamed), with zwalk.c, zflist.c, zwin.c and
# zwidget.c, over a real directory tree in /tmp: find, folder copy,
# move and delete, stopping part-way. Needs -no-pie and
# vm.mmap_min_addr=0 (sw/common/tests/ztramp.h); skips (77) without.
# TEST_PBM=/some/dir also writes renders of the window.
HOSTCC ?= cc
HOSTSRC = ../../common/zwin.c ../../common/zwidget.c ../../common/zflist.c \
	../../common/zwalk.c ../../common/zfsutil.c ../../common/ztype.c \
	../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c \
	../../common/tests/zspeak_stub.c
HOSTFLAGS = -std=gnu99 -Wall -no-pie -I ../../common

test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/files_test_walk tests/test_walk.c $(HOSTSRC)
	/tmp/files_test_walk

.PHONY: test

include ../../common/app.mk
