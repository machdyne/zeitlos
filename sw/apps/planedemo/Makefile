# sw/apps/planedemo -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# A sixty-second film about Zeitlos, in 256 colours, with music
# (docs/planedemo.md). On the arcade runtime (sw/common/games/zarcade.c)
# in full screen; the music on the hardware mixer.

APP = planedemo
APP_LISTING = 0

# ../mausrun: its sprites (sprites.c) -- the mouse and the cat
SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui ../mausrun

# The film's data -- the instruments and every picture -- is made on
# the build host (tests/mkfilm.c, film.pdd) and linked in (pd_blob.c);
# the heap is for a build without it, which makes them as it plays:
# 8MB, the most an app may ask for (docs/executables.md). Made for
# Sergei ML2's 512MB.
APP_STACK = 8M

# Objects, and why each is linked:
#
#   zgfx, zcolor           the blitter, the rasterizer, game-mode colour
#   zwin, zwidget, zspeak,
#   zdialog                the runtime's window and its F1 box
#   zfsapp, zcfg           linked by the runtime
#   zarcade, zarcade_sys   the arcade runtime: full screen, 256 colours
#   zsfx                   linked by the runtime; the film has its own music
#   pd_blob                the film's data, film.pdd (tests/mkfilm.c)
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zdialog.o zfsapp.o \
	zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	pd_synth.o pd_song.o pd_music.o pd_gfx.o \
	sprites.o pd_s_clock.o pd_s_plasma.o pd_s_city.o pd_s_donut.o pd_s_tunnel.o pd_s_vectors.o pd_s_sunrise.o pd_s_ending.o pd_scenes.o planedemo.o pd_blob.o

test:
	$(MAKE) -C tests

CLEAN_EXTRA = film.pdd

clean: clean-tests
clean-tests:
	$(MAKE) -C tests clean

.PHONY: test clean-tests

include ../../common/app.mk

# The film's data: made on the build host by the film's own code
# (needs a host C compiler, as the other host tools do), whenever any
# of it changes.
FILM_SRC = $(wildcard pd_*.c pd_*.h pd.h) ../mausrun/sprites.c tests/mkfilm.c
film.pdd: $(FILM_SRC)
	$(MAKE) -C tests film

pd_blob.o: film.pdd

# The program, packed: 2.8 MB of film, mostly zeros, is about 1.1 MB
# on the card, and every loader unpacks it (docs/executables.md).
MKEXEC = python3 zpack.py
