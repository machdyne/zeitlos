# sw/apps/read -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = read

# EXTRA_CFLAGS is the hook for one-off build options:
#
#   make clean && make EXTRA_CFLAGS=-DREAD_PROFILE=1
#
# (clean first: -MD tracks header dependencies, not flag changes, so a
# rebuild without it will happily keep the old objects.) A command-line
# CFLAGS+= works too; sw/common/app.mk appends with `override`, so it
# no longer drops the flags every app needs.
EXTRA_CFLAGS ?=
APP_CFLAGS = $(EXTRA_CFLAGS)

# md.o is the Markdown parser, deliberately free of I/O so the test
# suite below can build it for the host -- see md_test.c.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o zsayall.o \
	zfsapp.o zflist.o zdialog.o zedit.o ztype.o md.o read.o

md.o: md.c md.h
	$(CC) $(CFLAGS) -c md.c -o md.o

# Host builds of the parser and the layout logic, with the host's own
# compiler -- not $(CC), which is the cross compiler.
#
#   make test                 unit tests plus invariants
#   make test DOCS="a.md b.md"   also run them over real documents
#
# The second form is the one that finds things. Real Markdown contains
# constructs nobody thinks to write a case for -- hard-wrapped link
# syntax, nested lists indented like code blocks -- and both of those
# were found exactly this way.
#
# cases/ holds documents found by fuzzing, cut down: each once made the
# parser or the reader misbehave (front_unterminated.md: a blank page;
# skiprows.md: every key read to the end; fence_info_backtick.md: the
# rest of the document became code). They run with the real ones.
#
# rfind_test builds the REAL read.c against stubbed system calls and
# checks its long work: End, links to headings, scrollbar drags and
# launch offsets are finds stepped ~20 ms at a time; Esc leaves the page
# as it was; a failed read is not the end of the file; a redraw is
# acknowledged before any scan; a resize keeps the position valid.
#
# fence_unterminated.md is left out of md_test, whose real-document
# pass fails a document that ends inside a fence: that one does, on
# purpose. rfind_test reads it like the others.
DOCS ?= ../../../docs/*.md
CASES = $(filter-out cases/fence_unterminated.md,$(wildcard cases/*.md))

test:
	cc --std=gnu99 -Wall -Wextra -o md_test md_test.c md.c
	./md_test $(DOCS) $(CASES)
	cc --std=gnu99 -Wall -o rfind_test rfind_test.c md.c
	./rfind_test $(DOCS) cases/fence_unterminated.md
	cc --std=gnu99 -Wall -O2 -o rpic_test rpic_test.c md.c ../../common/zfont_data.c
	@for f in 6 5; do for d in $(SEAM_DOCS); do \
		./rpic_test $$d 316 280 seam $$f seam.pbm 300 || exit 1; done; done

# Pixels: a scroll by blit plus the strip redrawn must equal a fresh
# repaint of the same place (rpic_test.c, "seam"). A difference is
# left in seam.pbm, the two screens side by side. Tables, lists, code
# and headings, in both fonts.
SEAM_DOCS ?= ../../../docs/learn_format.md ../../../docs/usb_host.md ../../../docs/read_app.md

# Renders a document to ASCII using the same parser, wrapping and
# block logic the app uses, and checks that scrolling is REVERSIBLE --
# down N display lines and back up N must land exactly where it
# started, from every position. That property is why positions are
# block-aligned; see read.c.
#
#   make render DOC=file.md            layout, as text
#   make render DOC=file.md MODE=scroll  reversibility check
#   make render DOC=file.md MODE=seam  blit + strip redraw == full
#                                        redraw, pixel row by pixel row
#   make render DOC=file.md MODE=seekcache  parser state is the same
#                                        replayed from any mid-point
#                                        as from the start (the
#                                        position cache's premise)
DOC ?= ../../../docs/welcome.md
MODE ?= render

render:
	cc --std=gnu99 -Wall -o render_test render_test.c md.c
	./render_test $(DOC) 300 $(MODE)

# A picture of the first page of DOC, drawn by the real read.c in the
# real fonts, and what it cost -- see rpic_test.c:
#
#   make pic DOC=file.md                read.pbm, 5x8
#   make pic DOC=file.md FONT=6         6x12
PICW ?= 316
PICH ?= 280
FONT ?= 5
pic:
	cc --std=gnu99 -Wall -O2 -o rpic_test rpic_test.c md.c ../../common/zfont_data.c
	./rpic_test $(DOC) $(PICW) $(PICH) 0 $(FONT) read.pbm

.PHONY: test render pic

CLEAN_EXTRA = md_test render_test rfind_test rpic_test read.pbm seam.pbm

include ../../common/app.mk
