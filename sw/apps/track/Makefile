# sw/apps/track -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = track

# MOD_MAX_FILE bounds the static module buffer -- see track.c on why the
# module lives in .bss and not in malloc(). Override for a board with
# more than 1MB:  make MOD_MAX_FILE=$$((1024*1024))
MOD_MAX_FILE ?= $(shell echo $$((192*1024)))
# Output volume at startup, 0..100. The 1-bit DAC drives headphones
# directly with no attenuator, so full scale is very loud -- see the
# note on `volume` in track.c. '-' and '=' adjust it at runtime.
MOD_VOLUME ?= 35
APP_CFLAGS = -DMOD_MAX_FILE=$(MOD_MAX_FILE) -DMOD_VOLUME_DEFAULT=$(MOD_VOLUME)

OBJS = zeitlos.o zobj.o zsoc.o zfsapp.o zkbd.o zgfx.o zwin.o zfont_data.o modplay.o track.o

# Host-side tests for the playback engine.
#
# Built with the HOST compiler, deliberately: modplay.c includes
# nothing from sw/common and touches no hardware, so its arithmetic can
# be checked on a machine where a wrong answer prints as a number
# instead of sounding slightly off. Three real bugs were caught this
# way before any of this reached a board, including a finetune table
# that overflowed its type and silenced every note.
#
# Same idea, same reason, as sw/apps/calc/calc_test.c.
#
#   make test        run the tests
#   make test-wav    ...and dump each case as a .wav to listen to
HOSTCC ?= cc
HOSTCFLAGS = --std=gnu99 -O2 -Wall -Wextra

modplay_test: modplay.c modplay_test.c modplay.h
	$(HOSTCC) $(HOSTCFLAGS) -o modplay_test modplay.c modplay_test.c -lm

test: modplay_test
	./modplay_test

test-wav: modplay_test
	./modplay_test -w

.PHONY: test test-wav

CLEAN_EXTRA = modplay_test *.wav

include ../../common/app.mk
