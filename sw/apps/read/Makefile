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
DOCS ?= ../../../docs/*.md

test:
	cc --std=gnu99 -Wall -Wextra -o md_test md_test.c md.c
	./md_test $(DOCS)

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

.PHONY: test render

CLEAN_EXTRA = md_test render_test

include ../../common/app.mk
