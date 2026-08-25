PREFIX = riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zwin.o gpudemo.o

gpudemo: gpudemo.elf gpudemo.bin

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o: ../../common/zgfx.c ../../common/zgfx.h
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zwin.o: ../../common/zwin.c ../../common/zwin.h
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

gpudemo.o: gpudemo.c
	$(CC) $(CFLAGS) -c gpudemo.c -o gpudemo.o
	$(CC) $(CFLAGS) -c gpudemo.c -S -o gpudemo.asm

gpudemo.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,gpudemo.map -o gpudemo.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpudemo.elf > gpudemo.dasm

gpudemo.bin: gpudemo.elf
	@EDATA=$$($(PREFIX)nm gpudemo.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm gpudemo.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary gpudemo.elf gpudemo.data; \
	python3 ../../../tools/mkexec.py gpudemo.data gpudemo.bin $$(($$END - $$EDATA)); \
	rm -f gpudemo.data

clean:
	rm -f gpudemo.elf gpudemo.bin gpudemo.data gpudemo.asm gpudemo.dasm gpudemo.map *.o *.d

.PHONY: gpudemo clean
