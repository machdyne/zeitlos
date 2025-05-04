
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = -fPIC --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -fPIC -nostartfiles
LDSCRIPT = ../../common/riscv-app.ld

OBJS = crt0.o bounce.o

bounce: bounce.elf bounce.bin

crt0.o:
	$(AS) $(ASFLAGS) -o crt0.o ../../common/crt0.S

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

