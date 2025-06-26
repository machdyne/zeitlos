PREFIX = riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = gpu3d.o

gpu3d: gpu3d.elf gpu3d.bin

gpu3d.o:
	$(CC) $(CFLAGS) -c gpu3d.c -o gpu3d.o
	$(CC) $(CFLAGS) -c gpu3d.c -S -o gpu3d.asm

gpu3d.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,gpu3d.map -o gpu3d.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpu3d.elf > gpu3d.dasm

gpu3d.bin:
	$(PREFIX)objcopy -O binary gpu3d.elf gpu3d.bin

clean:
	rm -f gpu3d.elf gpu3d.bin gpu3d.asm gpu3d.dasm gpu3d.map gpu3d.o *.o *.d

.PHONY: gpu3d clean

