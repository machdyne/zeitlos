PREFIX ?= /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
ABI = ilp32
CC = $(PREFIX)gcc
CFLAGS = -fPIC --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -I../common
LDFLAGS = -fPIC -march=$(ARCH) -mabi=$(ABI)
LDSCRIPT = ../common/riscv-os.ld

OBJS = kernel.o kruntime.o mem.o \
	fs/fs.o fs/fatfs/sdmm.o fs/fatfs/ff.o \
	uart.o hid.o sh.o xfer.o ui.o msg.o pidreg.o fsapi.o zobj.o zstream.o zdns.o logo.o logo_data.o

# kernel.o's recipe below builds every object in one go (they're not
# independent processes, and mostly don't need to be) -- but for
# `make` to actually know when to rerun that recipe, kernel.o needs
# every source file that feeds it listed as a prerequisite here.
# without this, `make` only checks whether kernel.o already exists,
# not whether any of these are newer than it -- so it'll happily keep
# linking stale objects built from an older version of a file after
# any edit. bit us for real with fs/fs.c during this session's
# zstream/TFTP work -- see docs/networking.md.
KSRCS = kernel.c kruntime.c mem.c \
	fs/fs.c fs/fatfs/sdmm.c fs/fatfs/ff.c \
	uart.c hid.c sh.c xfer.c ui.c msg.c pidreg.c fsapi.c logo.c logo_data.c \
	../common/zobj.c ../common/zstream.c ../common/zdns.c

kernel: kernel.elf kernel.bin

kernel.o: $(KSRCS)
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
	$(CC) $(CFLAGS) -c kruntime.c -o kruntime.o
	$(CC) $(CFLAGS) -c mem.c -o mem.o
	$(CC) $(CFLAGS) -c fs/fs.c -o fs/fs.o
	$(CC) $(CFLAGS) -c fs/fatfs/sdmm.c -o fs/fatfs/sdmm.o
	$(CC) $(CFLAGS) -c fs/fatfs/ff.c -o fs/fatfs/ff.o
	$(CC) $(CFLAGS) -c sh.c -o sh.o
	$(CC) $(CFLAGS) -c xfer.c -o xfer.o
	$(CC) $(CFLAGS) -c ui.c -o ui.o
	$(CC) $(CFLAGS) -c uart.c -o uart.o
	$(CC) $(CFLAGS) -c hid.c -o hid.o
	$(CC) $(CFLAGS) -c logo.c -o logo.o
	$(CC) $(CFLAGS) -c logo_data.c -o logo_data.o
	$(CC) $(CFLAGS) -c msg.c -o msg.o
	$(CC) $(CFLAGS) -c pidreg.c -o pidreg.o
	$(CC) $(CFLAGS) -c fsapi.c -o fsapi.o
	$(CC) $(CFLAGS) -c ../common/zobj.c -o zobj.o
	$(CC) $(CFLAGS) -c ../common/zstream.c -o zstream.o
	$(CC) $(CFLAGS) -c ../common/zdns.c -o zdns.o
	$(CC) $(CFLAGS) -c kernel.c -S -o kernel.asm
	$(CC) $(CFLAGS) -c sh.c -S -o sh.asm

kernel.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,kernel.map -o kernel.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble kernel.elf > kernel.dasm

kernel.bin: kernel.elf
	@END=$$($(PREFIX)nm kernel.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)strip kernel.elf; \
	$(PREFIX)objcopy -O binary --pad-to=$$END kernel.elf kernel.bin

clean:
	rm -f kernel.elf kernel.bin kernel.asm kernel.dasm kernel.map sh.asm *.o *.d fs/*.o fs/fatfs/*.o

.PHONY: kernel clean
