PREFIX = riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = 
LDSCRIPT = ../../common/riscv-app.ld

OBJS = gpublit.o

gpublit: gpublit.elf gpublit.bin

gpublit.o:
	$(CC) $(CFLAGS) -c gpublit.c -o gpublit.o
	$(CC) $(CFLAGS) -c gpublit.c -S -o gpublit.asm

gpublit.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,gpublit.map -o gpublit.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpublit.elf > gpublit.dasm

gpublit.bin:
	$(PREFIX)objcopy -O binary gpublit.elf gpublit.bin

clean:
	rm -f gpublit.elf gpublit.bin gpublit.asm gpublit.dasm gpublit.map gpublit.o *.o *.d

.PHONY: gpublit clean

