
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32 -DZ_GFX_HW_BLIT
LDFLAGS =
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o hello_win.o

hello_win: hello_win.elf hello_win.bin

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

hello_win.o:
	$(CC) $(CFLAGS) -c hello_win.c -o hello_win.o
	$(CC) $(CFLAGS) -c hello_win.c -S -o hello_win.asm

hello_win.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,hello_win.map -o hello_win.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble hello_win.elf > hello_win.dasm

hello_win.bin:
	$(PREFIX)objcopy -O binary hello_win.elf hello_win.bin

clean:
	rm -f hello_win.elf hello_win.bin hello_win.asm hello_win.dasm hello_win.map *.o *.d

.PHONY: hello_win clean
