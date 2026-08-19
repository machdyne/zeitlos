
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32 -DZ_GFX_HW_BLIT
LDFLAGS = -march=$(ARCH) -mabi=ilp32
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zkbd.o dock_icons.o wm.o

# built with -DZ_GFX_HW_BLIT, same as hello_win/term -- the dock's
# icon labels (see wm.c's draw_dock()) go through the hardware glyph
# blitter. wm is now the ONLY process on the board that ever calls
# z_gfx_hw_font_load() (once, in main(), for z_font_5x7 -- see that
# call's own comment) -- hello_win/term no longer load a font
# themselves, and every app is expected to only ever draw with
# z_font_5x7 as a result. This sidesteps the cross-process glyph
# memory race entirely (see docs/window_manager.md, "Hardware glyph
# blitting") rather than just narrowing it: there's nothing left to
# race over once only one process ever writes to glyph memory, and
# only one font's data is ever loaded, board-wide.

wm: wm.elf wm.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

# dock_icons.c/.h are generated -- see sw/data/icons/gen_dock_icon_data.py.
# not regenerated automatically here (this Makefile doesn't depend on
# Python/Pillow being available) -- run that script by hand after
# adding or changing an icon PNG, same as font/logo data (see
# dock_icons.h's own header comment).
dock_icons.o: dock_icons.c dock_icons.h
	$(CC) $(CFLAGS) -c dock_icons.c -o dock_icons.o

wm.o:
	$(CC) $(CFLAGS) -c wm.c -o wm.o
	$(CC) $(CFLAGS) -c wm.c -S -o wm.asm

wm.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,wm.map -o wm.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble wm.elf > wm.dasm

wm.bin: wm.elf
	@END=$$($(PREFIX)nm wm.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary --pad-to=$$END wm.elf wm.bin

clean:
	rm -f wm.elf wm.bin wm.asm wm.dasm wm.map *.o *.d

.PHONY: wm clean
