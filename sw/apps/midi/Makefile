
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

# See sw/apps/repl/Makefile for the full reasoning -- in short,
# sw/common is shared by every app, so each links whole objects for the
# sake of a fraction of them, and section GC drops the rest.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -- tuning knobs --

# Largest .MID accepted, in bytes.
#
# Loaded WHOLE, with no streaming. A MIDI file is a SCORE rather than
# audio -- ten to a hundred KB is typical -- and the format 1 track
# merge needs random access to every track at once anyway. 256KB covers
# everything short of a full orchestral export, and a larger file is
# refused with a message rather than truncated, because half a MIDI
# file plays perfectly right up until it stops.
MIDI_MAX_FILE ?= $(shell echo $$((256*1024)))

# Startup volume, 0..64. The 1-bit sigma-delta output drives headphones
# directly with no attenuator, so full scale is very loud -- the same
# warning sw/apps/track's MOD_VOLUME carries, for the same hardware.
MIDI_VOLUME ?= 40

# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright, silently dropping -Os, -march, section GC and the libc
# specs. See sw/apps/play/Makefile for the longer version.
override CFLAGS += --std=gnu99 -Os -MD -Wall -Wextra -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT \
	-DMIDI_MAX_FILE=$(MIDI_MAX_FILE) \
	-DMIDI_VOLUME=$(MIDI_VOLUME) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)

LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# zfsapp.o is needed by zflist.o (the file browser inside the open
# dialog) as well as by midi.c's own file loading.
OBJS = zeitlos.o zobj.o zsoc.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o zkbd.o \
	smf.o synth.o midi_icons.o midi.o

midi: midi.elf midi.bin

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

smf.o:
	$(CC) $(CFLAGS) -c smf.c -o smf.o
synth.o:
	$(CC) $(CFLAGS) -c synth.c -o synth.o
midi_icons.o:
	$(CC) $(CFLAGS) -c midi_icons.c -o midi_icons.o

midi.o:
	$(CC) $(CFLAGS) -c midi.c -S -o midi.asm
	$(CC) $(CFLAGS) -c midi.c -o midi.o

midi.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,midi.map -o midi.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble midi.elf > midi.dasm

midi.bin: midi.elf
	@EDATA=$$($(PREFIX)nm midi.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm midi.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary midi.elf midi.data; \
	python3 ../../../tools/mkexec.py midi.data midi.bin $$(($$END - $$EDATA)); \
	rm -f midi.data

# -- host-side tests for the sequencer and the synth --
#
# smf.c and synth.c include nothing from sw/common and touch no
# hardware, so the parsing, timing and pitch arithmetic can be checked
# on a machine where a wrong answer prints as a number instead of
# playing slightly wrong. Same idea, same reason, as
# sw/apps/track/modplay_test.c and sw/apps/play/play_test.c.
HOSTCC ?= cc
HOSTCFLAGS = --std=gnu99 -O2 -Wall -Wextra

midi_test: smf.c synth.c midi_test.c smf.h synth.h
	$(HOSTCC) $(HOSTCFLAGS) -o midi_test smf.c synth.c midi_test.c -lm

test: midi_test
	./midi_test

clean:
	rm -f midi.elf midi.bin midi.data midi.asm midi.dasm midi.map *.o *.d
	rm -f midi_test

.PHONY: midi clean test

# Header dependency tracking -- see sw/apps/hello_win/Makefile. Short
# version: CFLAGS carries -MD and the object rules have no
# prerequisites, so without this an existing .o is considered
# permanently up to date and edits silently link stale objects.
-include $(wildcard *.d)
