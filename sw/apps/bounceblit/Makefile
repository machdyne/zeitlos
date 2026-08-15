
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = bounceblit.o

bounceblit: bounceblit.elf bounceblit.bin

bounceblit.o:
	$(CC) $(CFLAGS) -c bounceblit.c -o bounceblit.o
	$(CC) $(CFLAGS) -c bounceblit.c -S -o bounceblit.asm

bounceblit.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,bounceblit.map -o bounceblit.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble bounceblit.elf > bounceblit.dasm

bounceblit.bin: bounceblit.elf
	@END=$$($(PREFIX)nm bounceblit.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary --pad-to=$$END bounceblit.elf bounceblit.bin

clean:
	rm -f bounceblit.elf bounceblit.bin bounceblit.asm bounceblit.dasm bounceblit.map bounceblit.o *.o *.d

.PHONY: bounceblit clean

