# learncheck: checks learn courses on the build machine.
# docs/learn_format.md, "Checks".
#
#   make -C tools/learn/learncheck            builds /tmp/learncheck
#   /tmp/learncheck path/to/edu               every course in a folder
#   /tmp/learncheck --release path/to/course  as a release would

LEARN = ../../../sw/apps/learn
COMMON = ../../../sw/common
# figures are run as learn runs them: lfig.c and Machdyne Scheme (made by
# sw/apps/learn's Makefile, which also writes figlib.h)
SRC = learncheck.c $(LEARN)/lcheck.c $(LEARN)/lformat.c $(LEARN)/lsx.c $(LEARN)/lmath.c \
      $(LEARN)/tests/hostfs.c $(LEARN)/lfig.c $(LEARN)/ldraw.c $(LEARN)/lmathdraw.c \
      $(COMMON)/zcanvas.c $(COMMON)/zcfont.c $(COMMON)/zfont_data.c
MS = /tmp/learn_ms/ms.o

/tmp/learncheck: $(SRC) $(LEARN)/lcheck.h $(LEARN)/lformat.h $(LEARN)/lsx.h $(LEARN)/lmath.h $(LEARN)/lfig.h $(LEARN)/figlib.scm
	$(MAKE) -s -C $(LEARN) figlib.h $(MS)
	cc -std=gnu99 -O2 -Wall -Wextra -I $(LEARN) -I $(COMMON) -o $@ $(SRC) $(MS) -lm

clean:
	rm -f /tmp/learncheck

.PHONY: clean
