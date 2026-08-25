
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
# See sw/apps/repl/Makefile for the full reasoning behind these two --
# in short, sw/common is shared by every app, so each links whole
# objects for the sake of a fraction of them, and section GC drops the
# rest. Measured saving on this app is noted in docs/scheme_api.md's
# memory-budget section. Set GC_SECTIONS=0 to disable.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32 -DZ_GFX_HW_BLIT $(GC_CFLAGS)
LDFLAGS = -march=$(ARCH) -mabi=ilp32 $(GC_LDFLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# which font to build against -- e.g. `make term FONT=z_font_6x12`.
# must be one of the z_font_t externs declared in zfont.h. see term.c's
# own comment on TERM_FONT_NAME for why this exists.
ifdef FONT
CFLAGS += -DTERM_FONT_NAME=$(FONT)
endif

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zkbd.o zvt100.o zport.o term.o

term: term.elf term.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

zvt100.o:
	$(CC) $(CFLAGS) -c ../../common/zvt100.c -o zvt100.o

zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

term.o:
	$(CC) $(CFLAGS) -c term.c -o term.o
	$(CC) $(CFLAGS) -c term.c -S -o term.asm

term.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,term.map -o term.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble term.elf > term.dasm

term.bin: term.elf
	@EDATA=$$($(PREFIX)nm term.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm term.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary term.elf term.data; \
	python3 ../../../tools/mkexec.py term.data term.bin $$(($$END - $$EDATA)); \
	rm -f term.data

clean:
	rm -f term.elf term.bin term.data term.asm term.dasm term.map *.o *.d

.PHONY: term clean
