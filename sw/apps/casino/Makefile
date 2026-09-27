# sw/apps/casino -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = casino
APP_LISTING = 0
# Zeitlos Casino -- the front desk.

COMMON = ../../common
GAMES = $(COMMON)/games

include $(COMMON)/arch.mk

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zfsapp.o zbank.o \
	cs_banner.o casino.o

zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o

# REPLACE THE BANNER: drop your own banner.png in here and run this.
# The generated C is committed, so re-running with the same image must
# produce the same file.
banner:
	python3 gen_banner.py

# -- host-side --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2
TARGETSRC = casino.c cs_banner.c

includecheck:
	@for f in $(TARGETSRC) *.h; do \
		$(HOSTCC) -std=gnu99 -Wall -fsyntax-only $$f || exit 1; \
	done
	@echo "includecheck: every target source resolves its headers with no -I"

nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			sed 's/"[^"]*"//g' | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: the front desk is integer-only"

test: includecheck nofloat
	$(MAKE) -C $(GAMES) test

.PHONY: test includecheck nofloat banner

include ../../common/app.mk
