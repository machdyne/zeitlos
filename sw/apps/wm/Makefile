
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -march=$(ARCH) -mabi=ilp32
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zkbd.o wm.o

wm: wm.elf wm.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

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
