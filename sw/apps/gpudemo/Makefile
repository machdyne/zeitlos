
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = gpudemo.o

gpudemo: gpudemo.elf gpudemo.bin

gpudemo.o:
	$(CC) $(CFLAGS) -c gpudemo.c -o gpudemo.o
	$(CC) $(CFLAGS) -c gpudemo.c -S -o gpudemo.asm

gpudemo.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,gpudemo.map -o gpudemo.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpudemo.elf > gpudemo.dasm

gpudemo.bin:
	$(PREFIX)objcopy -O binary gpudemo.elf gpudemo.bin

clean:
	rm -f gpudemo.elf gpudemo.bin gpudemo.asm gpudemo.dasm gpudemo.map gpudemo.o *.o *.d

.PHONY: gpudemo clean

