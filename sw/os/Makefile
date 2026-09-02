PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../common/arch.mk
CC = $(PREFIX)gcc

# Section garbage collection.
#
# Every app Makefile under sw/apps has done this since sw/apps/repl;
# the kernel had not, and it is the same argument. The kernel links
# whole objects from ../common (zobj, zstream, zdns, zsoc) for the sake
# of a fraction of each, plus all of FatFs, plus whatever libc drags
# in -- and without -ffunction-sections nothing can be dropped, because
# the linker's unit of granularity is the whole .o.
#
# ../common/riscv-os.ld already collects *(.text.*), so no linker
# script change is needed for this to work.
#
# Measured on this tree (rv32im, picolibc, so the absolute figures
# differ from a newlib build but the delta does not):
#
#   as-is                     84884 bytes text+data
#   +gc-sections              79284      -5600
#   -fPIC removed             83152      -1732
#   both                      73968     -10916
#
# That matters because the BIOS copies a FIXED ROM_OS_SIZE of 256KB
# (sw/bios/bios.c) into main memory, so 256KB is a hard ceiling on
# kernel.bin rather than a soft one.
#
# Set GC_SECTIONS=0 to disable, e.g. when chasing a linker problem
# where a section being dropped is itself the suspect.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -fPIC on a kernel linked at a fixed 0x40000000 (../common/riscv-os.ld)
# costs 1732 bytes of GOT indirection for relocatability nothing uses.
# It is LEFT IN because this change is scoped to section GC and the two
# should be measured separately -- flip it and reflash to find out:
#
#   make -C sw/os KERNEL_PIC=0
KERNEL_PIC ?= 1
ifeq ($(KERNEL_PIC),1)
PIC_FLAGS = -fPIC
endif

CFLAGS = $(PIC_FLAGS) --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -I../common $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = $(PIC_FLAGS) -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../common/riscv-os.ld

OBJS = kernel.o kruntime.o mem.o zar.o \
	fs/fs.o fs/fatfs/sdmm.o fs/fatfs/ff.o \
	uart.o hid.o sh.o xfer.o xmodem.o ui.o msg.o pidreg.o fsapi.o procapi.o zobj.o zstream.o zdns.o zsoc.o logo.o

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
	uart.c hid.c sh.c xfer.c xmodem.c ui.c msg.c pidreg.c fsapi.c procapi.c logo.c \
	../common/zobj.c ../common/zstream.c ../common/zdns.c ../common/zsoc.c

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
	$(CC) $(CFLAGS) -c xmodem.c -o xmodem.o
	$(CC) $(CFLAGS) -c ui.c -o ui.o
	$(CC) $(CFLAGS) -c uart.c -o uart.o
	$(CC) $(CFLAGS) -c hid.c -o hid.o
	$(CC) $(CFLAGS) -c logo.c -o logo.o
	$(CC) $(CFLAGS) -c msg.c -o msg.o
	$(CC) $(CFLAGS) -c pidreg.c -o pidreg.o
	$(CC) $(CFLAGS) -c fsapi.c -o fsapi.o
	$(CC) $(CFLAGS) -c procapi.c -o procapi.o
	$(CC) $(CFLAGS) -c ../common/zobj.c -o zobj.o
	$(CC) $(CFLAGS) -c ../common/zstream.c -o zstream.o
	$(CC) $(CFLAGS) -c ../common/zdns.c -o zdns.o
	$(CC) $(CFLAGS) -c kernel.c -S -o kernel.asm
	$(CC) $(CFLAGS) -c ../common/zsoc.c -o zsoc.o
	$(CC) $(CFLAGS) -c sh.c -S -o sh.asm

kernel.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,kernel.map -o kernel.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble kernel.elf > kernel.dasm

kernel.bin: kernel.elf
	@END=$$($(PREFIX)nm kernel.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)strip kernel.elf; \
	$(PREFIX)objcopy -O binary --pad-to=$$END kernel.elf kernel.bin

clean:
	rm -f kernel.elf kernel.bin kernel.asm kernel.dasm kernel.map sh.asm *.o *.d fs/*.o fs/*.d fs/fatfs/*.o fs/fatfs/*.d

.PHONY: kernel clean

# Header dependency tracking.
#
# CFLAGS has carried -MD all along, so gcc has been writing .d files
# listing every header each object depends on -- but nothing included
# them, and the object rules below have no prerequisites at all
# (`wm.o:` rather than `wm.o: wm.c`). The effect is that once an object
# file exists, make considers it permanently up to date: editing a .c
# or a .h rebuilds nothing, and the link silently produces a binary
# from stale objects.
#
# That is not a theoretical problem -- it has twice produced hardware
# that behaved like a broken feature when the real cause was simply
# that the change was never compiled in. Including the .d files makes
# every object depend on its own source and headers, so `make` alone is
# correct and `make clean` stops being a required ritual.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
