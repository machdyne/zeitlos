
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# MOD_MAX_FILE bounds the static module buffer -- see track.c on why the
# module lives in .bss and not in malloc(). Override for a board with
# more than 1MB:  make MOD_MAX_FILE=$$((1024*1024))
MOD_MAX_FILE ?= $(shell echo $$((192*1024)))
# Output volume at startup, 0..100. The 1-bit DAC drives headphones
# directly with no attenuator, so full scale is very loud -- see the
# note on `volume` in track.c. '-' and '=' adjust it at runtime.
MOD_VOLUME ?= 35
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT -DMOD_MAX_FILE=$(MOD_MAX_FILE) -DMOD_VOLUME_DEFAULT=$(MOD_VOLUME) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zsoc.o zfsapp.o zkbd.o zgfx.o zwin.o zfont_data.o modplay.o track.o

track: track.elf track.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zsoc.o:
	$(CC) $(CFLAGS) -c ../../common/zsoc.c -o zsoc.o

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

modplay.o:
	$(CC) $(CFLAGS) -c modplay.c -o modplay.o

track.o:
	$(CC) $(CFLAGS) -c track.c -S -o track.asm
	$(CC) $(CFLAGS) -c track.c -o track.o

track.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,track.map -o track.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble track.elf > track.dasm

track.bin: track.elf
	@EDATA=$$($(PREFIX)nm track.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm track.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary track.elf track.data; \
	python3 ../../../tools/mkexec.py track.data track.bin $$(($$END - $$EDATA)); \
	rm -f track.data

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

clean:
	rm -f track.elf track.bin track.data track.asm track.dasm track.map *.o *.d
	rm -f modplay_test *.wav

.PHONY: track clean test test-wav

# Header dependency tracking -- see sw/apps/hello_win/Makefile for the
# full explanation. Short version: CFLAGS carries -MD and the object
# rules have no prerequisites, so without this an existing .o is
# considered permanently up to date and edits silently link stale
# objects.
-include $(wildcard *.d)
