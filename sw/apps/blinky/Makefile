
PREFIX = riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDSCRIPT = ../../common/riscv-app.ld

OBJS = blinky.o

blinky: blinky.elf blinky.bin

blinky.o:
	$(CC) $(CFLAGS) -c blinky.c -o blinky.o
	$(CC) $(CFLAGS) -c blinky.c -S -o blinky.asm

blinky.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,blinky.map -o blinky.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble blinky.elf > blinky.dasm

blinky.bin:
	$(PREFIX)objcopy -O binary blinky.elf blinky.bin

clean:
	rm -f blinky.elf blinky.bin blinky.asm blinky.dasm blinky.map blinky.o *.o *.d

.PHONY: blinky clean

