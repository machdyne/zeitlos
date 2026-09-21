
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# Section GC, same as wm/clock.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS -- see sw/apps/hello_win/Makefile for the full story.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS) $(GC_CFLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS) $(GC_LDFLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zspeak.o ttstest.o

ttstest: ttstest.elf ttstest.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zspeak.o:
	$(CC) $(CFLAGS) -c ../../common/zspeak.c -o zspeak.o

ttstest.o:
	$(CC) $(CFLAGS) -c ttstest.c -o ttstest.o

ttstest.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,ttstest.map -o ttstest.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble ttstest.elf > ttstest.dasm

ttstest.bin: ttstest.elf
	@EDATA=$$($(PREFIX)nm ttstest.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm ttstest.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary ttstest.elf ttstest.data; \
	python3 ../../../tools/mkexec.py ttstest.data ttstest.bin $$(($$END - $$EDATA)); \
	rm -f ttstest.data

clean:
	rm -f ttstest.elf ttstest.bin ttstest.data ttstest.dasm ttstest.map *.o *.d

.PHONY: ttstest clean

# Header dependency tracking -- see sw/apps/hello_win/Makefile.
-include $(wildcard *.d)
