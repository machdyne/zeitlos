# tools/zv -- the ZV encoder, the Linux player and their tests.
# See docs/zv.md.
#
#   make              zvenc, zvplay (with SDL2 when pkg-config finds it)
#   make NOSDL=1      zvplay without SDL2 (headless: --png, --compare, --y4m)
#   make test         host tests of the format and the decoder
#   make examples SRC=film.mp4   a ZV file, its info and a comparison sheet

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -I../../sw/common
LDLIBS  += -lm

ZV      = ../../sw/common/zv.c
ZVH     = ../../sw/common/zv.h ../../sw/common/zv_masks.h

ifeq ($(NOSDL),)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
ifneq ($(SDL_LIBS),)
PLAY_FLAGS = -DZV_SDL $(SDL_CFLAGS)
endif
endif

all: zvenc zvplay

zvenc: zvenc.c zvenc_core.c zvenc_core.h zvcol.c zvcol.h $(ZV) $(ZVH)
	$(CC) $(CFLAGS) -O3 -pthread -o $@ zvenc.c zvenc_core.c zvcol.c $(ZV) $(LDLIBS)

zvplay: zvplay.c $(ZV) $(ZVH)
	$(CC) $(CFLAGS) $(PLAY_FLAGS) -o $@ zvplay.c $(ZV) $(SDL_LIBS) $(LDLIBS)

zv_test: zv_test.c zvenc_core.c zvenc_core.h zvcol.c zvcol.h $(ZV) $(ZVH)
	$(CC) $(CFLAGS) -pthread -o $@ zv_test.c zvenc_core.c zvcol.c $(ZV) $(LDLIBS)

test: zv_test
	./zv_test

SRC ?=
examples: zvenc zvplay
	@test -n "$(SRC)" || (echo "make examples SRC=film.mp4"; exit 2)
	./zvenc --duration 60 "$(SRC)" example.zv
	./zvplay --info example.zv
	./zvplay --compare 240 example_compare.png example.zv

clean:
	rm -f zvenc zvplay zv_test example.zv example_compare.png

.PHONY: all test examples clean
