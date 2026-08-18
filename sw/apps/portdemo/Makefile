
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -march=$(ARCH) -mabi=ilp32
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zport.o portdemo.o

portdemo: portdemo.elf portdemo.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

portdemo.o:
	$(CC) $(CFLAGS) -c portdemo.c -o portdemo.o
	$(CC) $(CFLAGS) -c portdemo.c -S -o portdemo.asm

portdemo.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,portdemo.map -o portdemo.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble portdemo.elf > portdemo.dasm

portdemo.bin: portdemo.elf
	@END=$$($(PREFIX)nm portdemo.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary --pad-to=$$END portdemo.elf portdemo.bin

clean:
	rm -f portdemo.elf portdemo.bin portdemo.asm portdemo.dasm portdemo.map *.o *.d

.PHONY: portdemo clean
