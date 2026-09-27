# sw/apps/play -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = play

# -- tuning knobs --
#
# Every one of these is a memory-or-latency trade and every one is
# documented where it is used. docs/play_app.md has the arithmetic.

# Ring buffer, in bytes. MUST be a power of two -- stream_init()
# refuses anything else rather than corrupting quietly. This is how
# long the SD card may be late before playback breaks: 32KB is 186ms
# of 44.1kHz 16-bit stereo, or 744ms of IMA ADPCM.
#
#   make -C sw/apps/play PLAY_RING_SIZE=$$((64*1024))   # 32MB boards
PLAY_RING_SIZE ?= $(shell echo $$((32*1024)))

# Bytes per filesystem read. A LATENCY budget, not a throughput knob:
# it is the longest this app can be inside a blocking call, and the
# 1024-frame FIFO (23ms at 44.1kHz) has to survive it. Raising it does
# not make streaming faster; it spends the safety margin.
PLAY_CHUNK ?= 2048

# Startup volume, 0..64. The 1-bit sigma-delta output drives headphones
# directly with no attenuator, so full scale is very loud -- the same
# warning sw/apps/track's MOD_VOLUME carries, for the same hardware.
PLAY_VOLUME ?= 40

# What a headerless .raw/.pcm file is assumed to be. Must match what
# tools/mkaudio.py --raw writes, or the two disagree silently and the
# file plays at the wrong speed.
PLAY_RAW_RATE ?= 22050
PLAY_RAW_CHANNELS ?= 2

# Cycle-level phase profiling. OFF by default -- see prof.h for what it
# does and for the one hazard: it uses rdcycle/rdinstret, which are
# free (already built into both CPUs, no BRAM, no LUTs) but become
# ILLEGAL INSTRUCTIONS on a bitstream built with ENABLE_COUNTERS(0).
# Nothing in this tree does that, and the default in picorv32.v is 1.
#
#   make -C sw/apps/play PLAY_PROF=1
PLAY_PROF ?= 0

APP_CFLAGS = -Wextra -DPLAY_RING_SIZE=$(PLAY_RING_SIZE) -DPLAY_CHUNK=$(PLAY_CHUNK) -DPLAY_VOLUME=$(PLAY_VOLUME) -DPLAY_RAW_RATE=$(PLAY_RAW_RATE) -DPLAY_RAW_CHANNELS=$(PLAY_RAW_CHANNELS) -DPLAY_PROF=$(PLAY_PROF)

# zfsapp.o is needed by zflist.o (the file browser inside the open
# dialog) as well as by play.c's own streaming reads.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zsoc.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o zkbd.o \
	adec.o stream.o play_icons.o prof.o play.o

# -- host-side tests for the codec and the resampler --
#
# adec.c and stream.c include nothing from sw/common and touch no
# hardware, so their arithmetic can be checked on a machine where a
# wrong answer prints as a number instead of sounding slightly off.
# Two real bugs were caught this way before any of this reached a
# board; see docs/play_app.md.
#
# Same idea, same reason, as sw/apps/track/modplay_test.c and
# sw/apps/calc/calc_test.c.
#
#   make test        run the tests
#   make test-wav    ...and dump each case as a .wav to listen to
HOSTCC ?= cc
HOSTCFLAGS = --std=gnu99 -O2 -Wall -Wextra

play_test: adec.c stream.c play_test.c adec.h stream.h
	$(HOSTCC) $(HOSTCFLAGS) -o play_test adec.c stream.c play_test.c -lm

test: play_test
	./play_test

test-wav: play_test
	./play_test -w

.PHONY: test test-wav

CLEAN_EXTRA = play_test *.wav

include ../../common/app.mk
