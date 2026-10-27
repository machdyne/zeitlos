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
# stack and malloc's heap: Scheme's strings and the Zeitlos API's
# buffers come from it (docs/slide_scripting.md, "Memory")
APP_STACK = 128K

# PNG on: slides are made of PNGs as often as of JPEGs. zimg's inflate
# window lives in its decoder union (docs/png.md).
APP_CFLAGS = -DZ_IMG_HAVE_PNG=1

# Scheme (Phase 4): Machdyne Scheme and the Zeitlos API, as repl has
# them (sw/apps/repl/Makefile explains the flags). A static heap of
# MS_HEAP_SIZE cells, 24 bytes each here (8000: 192 KB of bss).
MS_DIR = ../../ext/ms
ZAPI_DIR = ../../common/zapi
MS_HEAP_SIZE ?= 8000
MS_PROTECT_STACK_SIZE ?= 384
# MS_HASH_SIZE: ms's lookup tables for symbols and globals (24 KB; off by
# default in ms, for the core apps' budgets). On here: a deck's scripts
# look up the Zeitlos API's hundreds of globals, and every one was a walk
# along the list of them.
MS_HASH_SIZE ?= 2048
MS_SIZE_CFLAGS = -DMS_HEAP_SIZE=$(MS_HEAP_SIZE) -DMS_PROTECT_STACK_SIZE=$(MS_PROTECT_STACK_SIZE) -DMS_HASH_SIZE=$(MS_HASH_SIZE)
MS_CFLAGS = -DLIX -DMS_STATIC_HEAP $(MS_SIZE_CFLAGS) -I../../common/ms_platform -I.

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o ztype.o zimg.o zinflate.o \
	zcanvas.o zcfont.o zpage.o zpaper.o zprint.o zco.o zimgjob.o zsvg.o zpsvg.o \
	zport.o zdns.o zstream.o zcolor.o zrtc.o zrng.o zgpio.o zi2c.o zi2cx.o zspi.o zuart.o \
	ms.o zapi.o zapi_doc.o \
	zpool.o deck.o deckio.o render.o printjob.o edit.o act.o script.o show.o slide.o

ZFMT = 1
ZFMT_FLOAT = 1
LDLIBS = -lm

include ../../common/app.mk

ms_stdlib.h: $(ZAPI_DIR)/gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l
	python3 $(ZAPI_DIR)/gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l ms_stdlib.h

ms.o: ms_stdlib.h $(MS_DIR)/ms.c
	$(CC) $(CFLAGS) $(MS_CFLAGS) -c $(MS_DIR)/ms.c -o ms.o

zapi.o: $(ZAPI_DIR)/zapi.c $(ZAPI_DIR)/zapi.h $(ZAPI_DIR)/ms_api.h $(ZAPI_DIR)/zapi_doc.h
	$(CC) $(CFLAGS) $(MS_SIZE_CFLAGS) -c $(ZAPI_DIR)/zapi.c -o zapi.o

zapi_doc.o: $(ZAPI_DIR)/zapi_doc.c $(ZAPI_DIR)/zapi_doc.h
	$(CC) $(CFLAGS) -c $(ZAPI_DIR)/zapi_doc.c -o zapi_doc.o

zi2c.o: ../../common/zi2c.c
	$(CC) $(CFLAGS) -c ../../common/zi2c.c -o zi2c.o

zspi.o: ../../common/zspi.c
	$(CC) $(CFLAGS) -c ../../common/zspi.c -o zspi.o

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
		$(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/test_slide_deck
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_render tests/test_render.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/test_slide_render
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_edit tests/test_edit.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c edit.c act.c $(COMMON)/zsvg.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/test_slide_edit
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_act tests/test_act.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c act.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/test_slide_act
	mkdir -p /tmp/slide/svg
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_svg tests/test_svg.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c printjob.c $(COMMON)/zpsvg.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/test_slide_svg
	mkdir -p /tmp/slide_ms && python3 $(ZAPI_DIR)/gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l /tmp/slide_ms/ms_stdlib.h
	$(HOSTCC) -std=gnu99 -g -w -DLIX -DMS_STATIC_HEAP -DMS_HEAP_SIZE=8000 -DMS_PROTECT_STACK_SIZE=384 -DMS_HASH_SIZE=2048 \
		-I$(COMMON)/ms_platform -I/tmp/slide_ms -c $(MS_DIR)/ms.c -o /tmp/slide_ms/ms.o
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_slide_script tests/test_script.c tests/zapi_stubs.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c act.c script.c /tmp/slide_ms/ms.o $(COMMON)/zpage.c $(COMMON)/zpaper.c \
		$(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c $(COMMON)/zcfont.c $(COMMON)/zfont_data.c \
		$(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	# (Scheme's own strings are still alive when it exits: not leaks)
	ASAN_OPTIONS=detect_leaks=0 /tmp/test_slide_script

look: test
	mkdir -p /tmp/slide
	/tmp/test_slide_render /tmp/slide

CLEAN_EXTRA = tests/patterns.c ms_stdlib.h

# The window, drawn on the build machine (sw/common/tests/zrender.h):
# /tmp/slide_ui_*.pbm, to look at.
render: tests/patterns.c
	mkdir -p /tmp/slide_ms && python3 $(ZAPI_DIR)/gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l /tmp/slide_ms/ms_stdlib.h
	$(HOSTCC) -std=gnu99 -g -w -no-pie -DLIX -DMS_STATIC_HEAP -DMS_HEAP_SIZE=8000 -DMS_PROTECT_STACK_SIZE=384 -DMS_HASH_SIZE=2048 \
		-I$(COMMON)/ms_platform -I/tmp/slide_ms -c $(MS_DIR)/ms.c -o /tmp/slide_ms/ms_render.o
	$(HOSTCC) -std=gnu99 -g -Wall -no-pie -Wl,--wrap=z_uptime_ticks -Wl,--wrap=z_win_resize -Wl,--wrap=z_win_set_title -I. -I$(COMMON) -DZ_IMG_HAVE_PNG=1 -o /tmp/slide_render_ui \
		tests/render_ui.c tests/zapi_stubs.c /tmp/slide_ms/ms_render.o $(COMMON)/zpool.c deck.c deckio.c render.c printjob.c edit.c act.c script.c show.c $(COMMON)/zsvg.c $(COMMON)/zpsvg.c \
		$(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/zfont_data.c $(COMMON)/zobj.c \
		$(COMMON)/zeitlos.c $(COMMON)/zedit.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zimg.c $(COMMON)/zinflate.c $(COMMON)/zspeak.c $(COMMON)/zprint.c -lm
	/tmp/slide_render_ui ../../..
	/tmp/slide_render_ui ../../.. bench
	/tmp/slide_render_ui ../../.. appstart

# What a click on the calculator example costs, in host instructions
# (bench_* in tests/render_ui.c): a guide to the board, about 1:1.3
bench-click: render
	valgrind --tool=callgrind --callgrind-out-file=/tmp/slide_click.cg /tmp/slide_render_ui ../../.. bench
	callgrind_annotate --inclusive=yes /tmp/slide_click.cg | grep -E "bench_|render_card|ms_eval |update_card|script_eval" | head -20

# Every card of a deck as PGM in /tmp/slide (DECK=path).
DECK ?= ../../data/slide/tour.zsl
look-deck: tests/patterns.c
	mkdir -p /tmp/slide
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/slide_look_deck tests/look_deck.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/slide_look_deck $(DECK) /tmp/slide ../../..

# The deck as a print job, as /print writes it (docs/print.md):
# /tmp/slide/deck.zpp (JOBOUT=...), for sw/apps/print's host tests.
# JOBOPTS="nup=4 cut=1"
JOBOUT ?= /tmp/slide/deck.zpp
job: tests/patterns.c
	mkdir -p /tmp/slide $(dir $(JOBOUT))
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/slide_job_deck tests/job_deck.c tests/hostfs.c tests/patterns.c \
		$(COMMON)/zpool.c deck.c deckio.c render.c printjob.c $(COMMON)/zpage.c $(COMMON)/zpaper.c $(COMMON)/zcanvas.c $(COMMON)/zco.c $(COMMON)/zimgjob.c \
		$(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zimg.c $(COMMON)/zinflate.c -lm
	/tmp/slide_job_deck $(DECK) $(JOBOUT) $(JOBOPTS)
