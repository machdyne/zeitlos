# learn -- a school on the machine. docs/learn.md, docs/learn_format.md.
#
#   make -C sw/apps/learn            the app, learn.bin
#   make -C sw/apps/learn test       host tests: the format, the checker,
#                                    answers, records, and every screen
#   make -C sw/apps/learn render     just the screens (tests/out)
#
# learncheck, the course checker, is tools/learn/learncheck.

APP = learn
# stack and malloc's heap: Scheme's strings come from it (figures)
APP_STACK = 256K		# 48K of it: the slow work's coroutine (learn.c, main()); a power of two
APP_CFLAGS = -I../../common
LDLIBS = -lm
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o zrtc.o zcfg.o \
	zdialog.o zfsapp.o zedit.o zflist.o zcanvas.o zcfont.o zco.o \
	lsx.o lmath.o lformat.o lquiz.o lstore.o lcat.o ldraw.o ltext.o lmathdraw.o \
	ms.o lfig.o lreview.o lapp.o lstep.o learn.o

# Figures are Machdyne Scheme (sw/ext/ms) without the Zeitlos API: a
# MS_HASH_SIZE: ms's lookup tables for symbols and globals (24 KB, off
# by default in ms; on here: global lookups were the top of a figure's
# profile, docs/learn_figures.md "Speed").
# static heap of MS_HEAP_SIZE cells (16 bytes each on the board): 5,300 live with the library, the rest so a frame seldom collects.
MS_DIR = ../../ext/ms
MS_HEAP_SIZE ?= 20000
MS_DEFS = -DLIX -DMS_STATIC_HEAP -DMS_HEAP_SIZE=$(MS_HEAP_SIZE) -DMS_PROTECT_STACK_SIZE=384 -DMS_HASH_SIZE=2048

HOSTCC ?= cc
COMMON = ../../common
HOST_CFLAGS = -std=gnu99 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize=shift -I $(COMMON)
FORMAT = lformat.c lsx.c lmath.c lcheck.c
APPSRC = lapp.c lstep.c ldraw.c ltext.c lmathdraw.c lquiz.c lstore.c lcat.c lformat.c lsx.c lmath.c lfig.c lreview.c
HOSTLIB = $(COMMON)/zcanvas.c $(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zrtc.c
HOSTMS = /tmp/learn_ms/ms.o

# the figure library, built in as a C string
figlib.h: figlib.scm
	python3 -c "import json,sys; s=''.join(l for l in open('figlib.scm') if not l.lstrip().startswith(';')); open('figlib.h','w').write('// made from figlib.scm by the Makefile\\nstatic const char figlib_scm[] = ' + json.dumps(s) + ';\\n')"

ms_stdlib.h: $(COMMON)/zapi/gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l
	python3 $(COMMON)/zapi/gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l ms_stdlib.h

# The code a key press runs through -- drawing the page (zcanvas,
# zcfont, ldraw, ltext), the lesson's layout (lstep) and a figure's frame
# (ms, lfig) -- at -O2: on an in-order core the cost is instructions, and
# -Os keeps the inner loops rolled and the small calls uninlined. The
# rest of learn stays -Os. (About 60 KB more code; docs/learn.md.)
FAST_CFLAGS = -O2

ms.o: ms_stdlib.h $(MS_DIR)/ms.c
	$(CC) $(CFLAGS) $(FAST_CFLAGS) $(MS_DEFS) -I$(COMMON)/ms_platform -I. -w -c $(MS_DIR)/ms.c -o ms.o

lfig.o: lfig.c figlib.h
	$(CC) $(CFLAGS) $(FAST_CFLAGS) -c lfig.c -o lfig.o

zcanvas.o: $(COMMON)/zcanvas.c
	$(CC) $(CFLAGS) $(FAST_CFLAGS) -c $(COMMON)/zcanvas.c -o zcanvas.o

zcfont.o: $(COMMON)/zcfont.c
	$(CC) $(CFLAGS) $(FAST_CFLAGS) -c $(COMMON)/zcfont.c -o zcfont.o

ldraw.o: ldraw.c
	$(CC) $(CFLAGS) $(FAST_CFLAGS) -c ldraw.c -o ldraw.o

ltext.o: ltext.c
	$(CC) $(CFLAGS) $(FAST_CFLAGS) -c ltext.c -o ltext.o

lstep.o: lstep.c
	$(CC) $(CFLAGS) $(FAST_CFLAGS) -c lstep.c -o lstep.o

$(HOSTMS): ms_stdlib.h $(MS_DIR)/ms.c
	mkdir -p /tmp/learn_ms
	$(HOSTCC) -std=gnu99 -g -w $(MS_DEFS) -I$(COMMON)/ms_platform -I. -c $(MS_DIR)/ms.c -o $@

test: figlib.h $(HOSTMS)
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_math tests/test_math.c lmath.c lsx.c -lm
	/tmp/learn_test_math
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_format tests/test_format.c tests/hostfs.c lformat.c lsx.c -lm
	/tmp/learn_test_format tests/courses
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_check tests/test_check.c tests/hostfs.c $(FORMAT) -lm
	/tmp/learn_test_check tests/courses
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_quiz tests/test_quiz.c lquiz.c lmath.c lsx.c -lm
	/tmp/learn_test_quiz
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_store tests/test_store.c tests/hostplat.c lstore.c $(COMMON)/zrtc.c -lm
	/tmp/learn_test_store
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_review tests/test_review.c tests/hostplat.c lreview.c lstore.c $(COMMON)/zrtc.c -lm
	/tmp/learn_test_review
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_test_fig tests/test_fig.c lfig.c ldraw.c lmathdraw.c lmath.c lsx.c $(HOSTLIB) $(HOSTMS) -lm
	mkdir -p tests/out
	/tmp/learn_test_fig tests/out ../../data/font
	$(MAKE) -s -C ../../../tools/learn/learncheck
	/tmp/learncheck --quiet tests/courses/t-good ../../data/learn/edu-zeitlos
	# figures that go wrong: each mistake found, with the control or time that shows it
	/tmp/learncheck --quiet tests/courses/t-figs > /tmp/learn_figs.out; test $$? = 1
	grep -q "13: error: figure: rect: what is 'purple" /tmp/learn_figs.out
	grep -q "21: error: figure, with n at 0: /: division by zero" /tmp/learn_figs.out
	grep -q "39: error: figure, at 7.50 seconds" /tmp/learn_figs.out
	grep -q "47: error: figure, with every point in one place" /tmp/learn_figs.out
	grep -q "words go past its edge" /tmp/learn_figs.out
	grep -q "5 errors" /tmp/learn_figs.out
	$(MAKE) -s render

# every screen, driven by a script, as pictures in tests/out (and checks)
render: figlib.h $(HOSTMS)
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/learn_render tests/render.c tests/hostplat.c $(COMMON)/zco.c $(APPSRC) $(HOSTLIB) $(HOSTMS) -lm
	mkdir -p tests/out
	/tmp/learn_render tests/out ../../data/font
	-python3 ../../../tools/learn/topng.py tests/out

# what each kind of key costs, in instructions (docs/learn_figures.md, "Speed")
bench: figlib.h $(HOSTMS)
	$(HOSTCC) -std=gnu99 -O2 -g -I $(COMMON) -o /tmp/learn_bench tests/bench.c tests/hostplat.c $(APPSRC) $(HOSTLIB) $(HOSTMS) -lm
	for m in "40 fig type" "40 fig slide" "40 fig scroll" "40 plain type"; do \
		valgrind --tool=callgrind --callgrind-out-file=/tmp/learn_bench.cg /tmp/learn_bench $$m 2>&1 | grep Collected | sed "s/^/$$m: /"; done

clean-host:
	rm -f /tmp/learn_test_fig $(HOSTMS) /tmp/learn_test_math /tmp/learn_test_format /tmp/learn_test_check /tmp/learn_test_quiz /tmp/learn_test_store /tmp/learn_render
	rm -rf tests/out

.PHONY: test render bench clean-host

include ../../common/app.mk
