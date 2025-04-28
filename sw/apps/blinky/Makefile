
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = -fPIC --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -fPIC -nostartfiles
LDSCRIPT = ../../common/riscv-app.ld

OBJS = crt0.o blinky.o

blinky: blinky.elf blinky.bin

crt0.o:
	$(AS) $(ASFLAGS) -o crt0.o ../../common/crt0.S

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

