# preview: every step of a course drawn as learn draws it, a PNG a step.
#
#   make -C tools/learn/preview                     builds /tmp/learn_preview
#   make -C tools/learn/preview run COURSE=zeit-tc  sw/data/learn/edu-zeitlos/zeit-tc
#                                                   into tools/learn/out/preview/zeit-tc
#
# (or /tmp/learn_preview COURSE-FOLDER OUT-FOLDER [FONT-FOLDER], from the tree's top)

LEARN = ../../../sw/apps/learn
COMMON = ../../../sw/common
SRC = preview.c $(LEARN)/tests/hostplat.c \
      $(LEARN)/lapp.c $(LEARN)/lstep.c $(LEARN)/ldraw.c $(LEARN)/ltext.c $(LEARN)/lmathdraw.c \
      $(LEARN)/lquiz.c $(LEARN)/lstore.c $(LEARN)/lcat.c $(LEARN)/lformat.c $(LEARN)/lsx.c \
      $(LEARN)/lmath.c $(LEARN)/lfig.c $(LEARN)/lreview.c \
      $(COMMON)/zcanvas.c $(COMMON)/zcfont.c $(COMMON)/zfont_data.c $(COMMON)/zrtc.c
MS = /tmp/learn_ms/ms.o
PACK ?= edu-zeitlos
COURSE ?= zeit-tc
OUT = ../out/preview/$(COURSE)

/tmp/learn_preview: $(SRC) $(wildcard $(LEARN)/*.h) $(LEARN)/figlib.scm
	$(MAKE) -s -C $(LEARN) figlib.h $(MS)
	cc -std=gnu99 -O2 -Wall -I $(LEARN) -I $(LEARN)/tests -I $(COMMON) -o $@ $(SRC) $(MS) -lm

run: /tmp/learn_preview
	rm -rf $(OUT) && mkdir -p $(OUT)
	cd ../../.. && /tmp/learn_preview sw/data/learn/$(PACK)/$(COURSE) tools/learn/out/preview/$(COURSE) sw/data/font
	python3 ../topng.py $(OUT) && rm -f $(OUT)/*.pgm

clean:
	rm -f /tmp/learn_preview

.PHONY: run clean
