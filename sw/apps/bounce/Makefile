
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

bounce.bin: bounce.elf
	@EDATA=$$($(PREFIX)nm bounce.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm bounce.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary bounce.elf bounce.data; \
	python3 ../../../tools/mkexec.py bounce.data bounce.bin $$(($$END - $$EDATA)); \
	rm -f bounce.data

clean:
	rm -f bounce.elf bounce.bin bounce.data bounce.asm bounce.dasm bounce.map bounce.o *.o *.d

.PHONY: bounce clean

