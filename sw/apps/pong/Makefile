
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS =
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o pong.o

pong: pong.elf pong.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

pong.o:
	$(CC) $(CFLAGS) -c pong.c -o pong.o
	$(CC) $(CFLAGS) -c pong.c -S -o pong.asm

pong.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,pong.map -o pong.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble pong.elf > pong.dasm

pong.bin: pong.elf
	@END=$$($(PREFIX)nm pong.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary --pad-to=$$END pong.elf pong.bin

clean:
	rm -f pong.elf pong.bin pong.asm pong.dasm pong.map *.o *.d

.PHONY: pong clean
