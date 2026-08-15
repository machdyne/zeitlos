PREFIX = riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zwin.o gpu3d.o

gpu3d: gpu3d.elf gpu3d.bin

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o: ../../common/zgfx.c ../../common/zgfx.h
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zwin.o: ../../common/zwin.c ../../common/zwin.h
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

gpu3d.o: gpu3d.c
	$(CC) $(CFLAGS) -c gpu3d.c -o gpu3d.o
	$(CC) $(CFLAGS) -c gpu3d.c -S -o gpu3d.asm

gpu3d.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,gpu3d.map -o gpu3d.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpu3d.elf > gpu3d.dasm

gpu3d.bin: gpu3d.elf
	@END=$$($(PREFIX)nm gpu3d.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary --pad-to=$$END gpu3d.elf gpu3d.bin

clean:
	rm -f gpu3d.elf gpu3d.bin gpu3d.asm gpu3d.dasm gpu3d.map *.o *.d

.PHONY: gpu3d clean
