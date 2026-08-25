
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = draw.o

draw: draw.elf draw.bin

draw.o:
	$(CC) $(CFLAGS) -c draw.c -o draw.o
	$(CC) $(CFLAGS) -c draw.c -S -o draw.asm

draw.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,draw.map -o draw.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble draw.elf > draw.dasm

draw.bin:
	$(PREFIX)objcopy -O binary draw.elf draw.bin

clean:
	rm -f draw.elf draw.bin draw.asm draw.dasm draw.map draw.o *.o *.d

.PHONY: draw clean

