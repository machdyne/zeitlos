# sw/apps/sheet -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = sheet

# sheet_core.o is the spreadsheet itself -- cells, formulas, files --
# kept free of any I/O so the test suite below can build it for the
# host. zfix.o is the fixed-point arithmetic, shared with sw/apps/calc.
# ztype.o is only for z_ftype_ext(), which is how a .CSV is told from a
# .ZSS on the way in and out.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o zfix.o ztype.o sheet_core.o sheet.o

sheet_core.o: sheet_core.c sheet_core.h
	$(CC) $(CFLAGS) -c sheet_core.c -o sheet_core.o

# Host builds, with the host's own compiler -- not $(CC), which is the
# cross compiler and produces something this machine cannot run.
#
#   make test        the spreadsheet model: arithmetic, formulas,
#                    evaluation, the file format
#   make test-layout the window geometry: the hit-test round trip
#                    across window sizes, fonts and column widths
#   make test-files  this app's own file paths: the chunked reader,
#                    the streamed writer, and which format runs
#   make render      draws the panel to /tmp/sheet.pbm so it can be
#                    LOOKED AT before it reaches a screen
#
# `test` is worth running after ANY change to sheet_core.c: a
# spreadsheet that is subtly wrong is worse than no spreadsheet,
# because nobody re-checks its arithmetic.
#
# The other two need -no-pie and vm.mmap_min_addr=0 -- see
# sw/common/tests/ztramp.h -- and exit 77 (the skip convention) where
# that isn't available.
HOSTSRC = ../../common/zwin.c ../../common/zwidget.c \
	../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c \
	../../common/zfix.c sheet_core.c

test:
	cc --std=gnu99 -Wall -Wextra -o sheet_test \
		tests/test_core.c sheet_core.c ../../common/zfix.c
	./sheet_test

test-layout:
	cc --std=gnu99 -Wall -no-pie -I ../../common -o sheet_layout \
		tests/test_layout.c $(HOSTSRC)
	./sheet_layout

# ztype.c is linked for real here rather than stubbed, because which
# writer runs is decided by z_ftype_ext() and that decision is part of
# what this tests.
test-files:
	cc --std=gnu99 -Wall -no-pie -I ../../common -o sheet_files \
		tests/test_files.c ../../common/ztype.c $(HOSTSRC)
	./sheet_files

render:
	cc --std=gnu99 -Wall -no-pie -I ../../common -o sheet_render \
		tests/render.c $(HOSTSRC)
	./sheet_render /tmp/sheet.pbm

.PHONY: test test-layout test-files render

CLEAN_EXTRA = sheet_test sheet_layout sheet_files sheet_render

include ../../common/app.mk
