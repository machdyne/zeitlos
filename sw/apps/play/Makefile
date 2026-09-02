
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

# See sw/apps/repl/Makefile for the full reasoning -- in short,
# sw/common is shared by every app, so each links whole objects for the
# sake of a fraction of them, and section GC drops the rest. Set
# GC_SECTIONS=0 to disable.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

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

# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line value,
# not to this one -- silently dropping -Os, -march, section GC, the
# libc specs and every -D above. The result still builds and links; it
# is just larger and slower, and can overrun the space the loader has
# for it.
#
# -DZ_GFX_HW_BLIT: every glyph and every fill this app draws goes
# through the hardware blitter. It matters here for a specific reason
# rather than as a default -- a text row costs about 16ms of wall time
# once the CPU is shared three ways (see sw/apps/track's display
# notes), and the audio FIFO's entire margin is 23ms.
override CFLAGS += --std=gnu99 -Os -MD -Wall -Wextra -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT \
	-DPLAY_RING_SIZE=$(PLAY_RING_SIZE) \
	-DPLAY_CHUNK=$(PLAY_CHUNK) \
	-DPLAY_VOLUME=$(PLAY_VOLUME) \
	-DPLAY_RAW_RATE=$(PLAY_RAW_RATE) \
	-DPLAY_RAW_CHANNELS=$(PLAY_RAW_CHANNELS) \
	-DPLAY_PROF=$(PLAY_PROF) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)

LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# zfsapp.o is needed by zflist.o (the file browser inside the open
# dialog) as well as by play.c's own streaming reads.
OBJS = zeitlos.o zobj.o zsoc.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o zkbd.o \
	adec.o stream.o play_icons.o prof.o play.o

play: play.elf play.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zsoc.o:
	$(CC) $(CFLAGS) -c ../../common/zsoc.c -o zsoc.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zwidget.o:
	$(CC) $(CFLAGS) -c ../../common/zwidget.c -o zwidget.o

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

zflist.o:
	$(CC) $(CFLAGS) -c ../../common/zflist.c -o zflist.o

zdialog.o:
	$(CC) $(CFLAGS) -c ../../common/zdialog.c -o zdialog.o

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

adec.o:
	$(CC) $(CFLAGS) -c adec.c -o adec.o

stream.o:
	$(CC) $(CFLAGS) -c stream.c -o stream.o

play_icons.o:
	$(CC) $(CFLAGS) -c play_icons.c -o play_icons.o

prof.o:
	$(CC) $(CFLAGS) -c prof.c -o prof.o

play.o:
	$(CC) $(CFLAGS) -c play.c -S -o play.asm
	$(CC) $(CFLAGS) -c play.c -o play.o

play.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,play.map -o play.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble play.elf > play.dasm

play.bin: play.elf
	@EDATA=$$($(PREFIX)nm play.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm play.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary play.elf play.data; \
	python3 ../../../tools/mkexec.py play.data play.bin $$(($$END - $$EDATA)); \
	rm -f play.data

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

clean:
	rm -f play.elf play.bin play.data play.asm play.dasm play.map *.o *.d
	rm -f play_test *.wav

.PHONY: play clean test test-wav

# Header dependency tracking -- see sw/apps/hello_win/Makefile for the
# full explanation. Short version: CFLAGS carries -MD and the object
# rules have no prerequisites, so without this an existing .o is
# considered permanently up to date and edits silently link stale
# objects.
-include $(wildcard *.d)
