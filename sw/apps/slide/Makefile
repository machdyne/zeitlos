# sw/apps/slide -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# slide: cards, slides and pages (docs/slide_app.md). A card app (not in
# flash): it draws with sw/common/zcanvas.c and zcfont.c, and keeps its
# deck in the arena.
#
#   make              the app
#   make test         the model, the file, the renderer (host, ASan/UBSan)
#   make look         the same, writing the test card as PGM in /tmp/slide

APP = slide
APP_STACK = 64K

# PNG on: slides are made of PNGs as often as of JPEGs. zimg's inflate
# window lives in its decoder union (docs/png.md).
APP_CFLAGS = -DZ_IMG_HAVE_PNG=1

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o ztype.o zimg.o zinflate.o \
	zcanvas.o zcfont.o zpage.o zpaper.o zprint.o zco.o zimgjob.o \
	zpool.o deck.o deckio.o render.o printjob.o edit.o show.o slide.o

include ../../common/app.mk

# -- host tests --------------------------------------------------------------

HOSTCC ?= cc
HOST_CFLAGS = -std=gnu99 -g -Wall -Wextra -fsanitize=address,undefined \
	-fno-sanitize=shift -I. -DZ_IMG_HAVE_PNG=1
COMMON = ../../common

# zwidget.c's pattern table, without the rest of zwidget.c (which wants
# a window system): one table, never a copy kept in step by hand.
tests/patterns.c: $(COMMON)/zwidget.c
	( echo '#include <stdint.h>'; echo '#define Z_PATTERN_COUNT 16'; \
	  sed -n '/^const uint8_t z_pattern_table\[Z_PATTERN_COUNT\]\[8\] = {/,/^};/p' $(COMMON)/zwidget.c ) > $@

test: tests/patterns.c
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_deck tests/test_deck.c tests/hostfs.c tests/patterns.c $(COMMON)/zpool.c deck.c deckio.c printjob.c render.c \
		$(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c $(COMMON)/zcfont.c $(COMMON)/zfont_data.c \
		$(COMMON)/zimg.c $(COMMON)/zinflate.c
	/tmp/test_slide_deck
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_render tests/test_render.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c
	/tmp/test_slide_render
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_edit tests/test_edit.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c edit.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c
	/tmp/test_slide_edit

look: test
	mkdir -p /tmp/slide
	/tmp/test_slide_render /tmp/slide

CLEAN_EXTRA = tests/patterns.c

# The window, drawn on the build machine (sw/common/tests/zrender.h):
# /tmp/slide_ui_*.pbm, to look at.
render: tests/patterns.c
	$(HOSTCC) -std=gnu99 -g -Wall -no-pie -Wl,--wrap=z_uptime_ticks -I. -I$(COMMON) -DZ_IMG_HAVE_PNG=1 -o /tmp/slide_render_ui \
		tests/render_ui.c $(COMMON)/zpool.c deck.c deckio.c render.c printjob.c edit.c show.c \
		$(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/zfont_data.c $(COMMON)/zobj.c \
		$(COMMON)/zeitlos.c $(COMMON)/zedit.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zimg.c $(COMMON)/zinflate.c $(COMMON)/zspeak.c $(COMMON)/zprint.c
	/tmp/slide_render_ui ../../..

# Every card of a deck as PGM in /tmp/slide (DECK=path).
DECK ?= ../../data/slide/tour.zsl
look-deck: tests/patterns.c
	mkdir -p /tmp/slide
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/slide_look_deck tests/look_deck.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c
	/tmp/slide_look_deck $(DECK) /tmp/slide ../../..

# The deck as a print job, as /print writes it (docs/print.md):
# /tmp/slide/deck.zpp (JOBOUT=...), for sw/apps/print's host tests.
# JOBOPTS="nup=4 cut=1"
JOBOUT ?= /tmp/slide/deck.zpp
job: tests/patterns.c
	mkdir -p /tmp/slide $(dir $(JOBOUT))
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/slide_job_deck tests/job_deck.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c printjob.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c
	/tmp/slide_job_deck $(DECK) $(JOBOUT) $(JOBOPTS)
