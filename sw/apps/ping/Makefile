
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS =
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o ping.o

ping: ping.elf ping.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

ping.o:
	$(CC) $(CFLAGS) -c ping.c -o ping.o
	$(CC) $(CFLAGS) -c ping.c -S -o ping.asm

ping.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,ping.map -o ping.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble ping.elf > ping.dasm

ping.bin:
	$(PREFIX)objcopy -O binary ping.elf ping.bin

clean:
	rm -f ping.elf ping.bin ping.asm ping.dasm ping.map *.o *.d

.PHONY: ping clean
