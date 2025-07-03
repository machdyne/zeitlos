
PREFIX = riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = bounce.o

bounce: bounce.elf bounce.bin

bounce.o:
	$(CC) $(CFLAGS) -c bounce.c -o bounce.o
	$(CC) $(CFLAGS) -c bounce.c -S -o bounce.asm

bounce.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,bounce.map -o bounce.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble bounce.elf > bounce.dasm

bounce.bin:
	$(PREFIX)objcopy -O binary bounce.elf bounce.bin

clean:
	rm -f bounce.elf bounce.bin bounce.asm bounce.dasm bounce.map bounce.o *.o *.d

.PHONY: bounce clean

