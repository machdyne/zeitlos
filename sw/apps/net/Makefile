
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS =
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zstream.o enc28j60.o eth.o arp.o ip.o udp.o tftp.o net.o

net: net.elf net.bin

# each rule below lists its own .c file as a prerequisite -- without
# this, `make` has no way to know a source file changed (it was only
# checking whether the .o file existed at all), so it would happily
# keep linking a stale .o built from an older version of that file
# after any edit. bit us for real with sw/os/fs/fs.c during this
# session's zstream/TFTP work -- see docs/networking.md.

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zstream.o: ../../common/zstream.c ../../common/zstream.h
	$(CC) $(CFLAGS) -c ../../common/zstream.c -o zstream.o

enc28j60.o: enc28j60.c enc28j60.h
	$(CC) $(CFLAGS) -c enc28j60.c -o enc28j60.o

eth.o: eth.c eth.h
	$(CC) $(CFLAGS) -c eth.c -o eth.o

arp.o: arp.c arp.h
	$(CC) $(CFLAGS) -c arp.c -o arp.o

ip.o: ip.c ip.h
	$(CC) $(CFLAGS) -c ip.c -o ip.o

udp.o: udp.c udp.h
	$(CC) $(CFLAGS) -c udp.c -o udp.o

tftp.o: tftp.c tftp.h
	$(CC) $(CFLAGS) -c tftp.c -o tftp.o

net.o: net.c
	$(CC) $(CFLAGS) -c net.c -o net.o
	$(CC) $(CFLAGS) -c net.c -S -o net.asm

net.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,net.map -o net.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble net.elf > net.dasm

net.bin: net.elf
	@END=$$($(PREFIX)nm net.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	echo "net.bin: padding to _end=$$END (includes .bss -- see docs/networking.md, \"objcopy truncation bug\")"; \
	$(PREFIX)objcopy -O binary --pad-to=$$END net.elf net.bin

clean:
	rm -f net.elf net.bin net.asm net.dasm net.map *.o *.d

.PHONY: net clean
