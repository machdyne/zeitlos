# libz -- the Zeitlos runtime blob for zcc-compiled programs.
#
# Built with GCC, once, and copied verbatim into every image zcc
# produces. See docs/libz.md.

PREFIX ?= $(RISCV_PREFIX)
include ../../../common/arch.mk
CC = $(PREFIX)gcc

# -mno-relax and -msmall-data-limit=0 are REQUIRED, not tuning.
#
# gp-relative addressing resolves through whatever `gp` the CALLER
# has, and libz is called from zcc-compiled code that never sets gp at
# all. That is the same hazard docs/app_runtime.md documents for the
# kernel's syscall path, where it took three separate bugs to find --
# a global read through the wrong gp does not crash, it silently reads
# somebody else's memory. Turning the addressing mode off entirely
# costs one instruction per global access and removes the whole class.
#
# -ffreestanding because there is no libc here; libz IS the libc.
# -fno-builtin so that GCC does not rewrite a loop in memcpy() into a
# call to memcpy(), which it will happily do.
COMMON = ../../../common

override CFLAGS += -Os -std=gnu99 -Wall -Wextra -MD \
	-march=$(ARCH) -mabi=$(ABI) \
	-mno-relax -msmall-data-limit=0 \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-ffunction-sections -fdata-sections

LDFLAGS = -march=$(ARCH) -mabi=$(ABI) -nostdlib -mno-relax \
	-Wl,--build-id=none

OBJS = start.o syscall.o mem.o str.o fmt.o glue.o \
	zgfx.o zwin.o zobj.o zkbd.o zfont_data.o

# libz.bin and libz.sym, NOT libz.blob and libz.syms.
#
# The SD card is FatFs with FF_USE_LFN 0 (sw/os/fs/fatfs/ffconf.h), so
# it stores 8.3 SHORT names only -- eight characters of base and THREE
# of extension. A four-character extension cannot be written to the
# card at all, so the original names were ones the machine that needs
# these files could never hold. Caught while adding them to
# tools/tftp-dist.sh, which already warns about base names over eight
# characters and would not have warned about this.
all: libz.bin libz.sym

# The graphics half of the runtime is the tree's OWN sources, compiled
# unmodified. Nothing in sw/common changed to make this work; what it
# needed was five shim headers (shim/) supplying the freestanding
# <stdio.h>, <stdlib.h>, <string.h>, <stddef.h> and <math.h> those
# files include, declaring only the names they actually call.
#
# -nostdinc, and the shims come FIRST on the include path. Without
# -nostdinc the compiler's own <stdint.h> is found and everything
# still builds -- right up until something reaches for a header that
# assumes a hosted environment. Failing at the #include is better than
# failing at the link.
#
# The shims deliberately do NOT #include libz.h. Doing so redeclares
# maskirq() and z_msg_send() alongside zeitlos.h's own versions of the
# same names, with different signatures, and every one of these files
# includes zeitlos.h.
SHIMFLAGS = -nostdinc -I shim -I $(COMMON)

glue.o: glue.c
	$(CC) $(OWN_CFLAGS) $(SHIMFLAGS) -c glue.c -o glue.o

zgfx.o: $(COMMON)/zgfx.c
	$(CC) $(CFLAGS) $(SHIMFLAGS) -c $< -o $@

zwin.o: $(COMMON)/zwin.c
	$(CC) $(CFLAGS) $(SHIMFLAGS) -c $< -o $@

zobj.o: $(COMMON)/zobj.c
	$(CC) $(CFLAGS) $(SHIMFLAGS) -c $< -o $@

zkbd.o: $(COMMON)/zkbd.c
	$(CC) $(CFLAGS) $(SHIMFLAGS) -c $< -o $@

zfont_data.o: $(COMMON)/zfont_data.c
	$(CC) $(CFLAGS) $(SHIMFLAGS) -c $< -o $@


start.o: start.S libz.h libz_table.def
	$(CC) $(CFLAGS) -x assembler-with-cpp -c start.S -o start.o

# -Werror=implicit-function-declaration, on libz's OWN sources only.
#
# Not blanket -Werror: the rules below also compile sw/common sources,
# whose warnings vary by toolchain version and which this build has no
# business failing over. But an implicit declaration is never a style
# question -- it means a call whose prototype the compiler is GUESSING,
# and the guess is `int`, so a function returning a pointer silently
# becomes a truncated integer. GCC 15 makes it an error by default;
# older ones did not, which is how syscall.c reached a user calling
# malloc() with no declaration in scope, on a target where a truncated
# pointer is a wild store.
OWN_CFLAGS = $(CFLAGS) -Werror=implicit-function-declaration \
	-Werror=int-conversion

# syscall.c dereferences a fixed address near zero -- reg_kernel at
# 0x0c, which the kernel wrote a function pointer into before this code
# ever ran. GCC reads a constant address that low as a likely null
# dereference and says so; on a hosted target it would be right. The
# warning is disabled for this one file rather than tree-wide, so that
# a real null dereference anywhere else still gets caught.
syscall.o: syscall.c libz_int.h
	$(CC) $(OWN_CFLAGS) $(SHIMFLAGS) -Wno-array-bounds -c syscall.c -o syscall.o

# mem.c, str.c, fmt.c -- libz's own, so they get OWN_CFLAGS too.
%.o: %.c libz.h libz_int.h
	$(CC) $(OWN_CFLAGS) $(SHIMFLAGS) -c $< -o $@

# NOTE: no --gc-sections. Every table entry is reached only through a
# `j` in an assembly section, and the linker cannot see that those are
# uses -- it would drop the whole runtime and leave a table of jumps to
# nowhere. KEEP() on the table alone is not enough, because what needs
# keeping is everything the table points AT.
# -lgcc, at the end, after -nostdlib has removed the default libraries.
#
# libgcc is the COMPILER's support library, not the C library: it holds
# the routines GCC emits calls to for operations the ISA has no
# instruction for. zobj.c's z_obj_equal() compares Z_FLOAT32 values,
# which on a core with no F extension becomes a call to __ltsf2 -- so
# the link failed on a soft-float comparison in a runtime that has no
# floating point of its own. Everything here is still freestanding;
# libgcc is not libc.
libz.elf: $(OBJS) libz.ld
	$(CC) $(LDFLAGS) -T libz.ld -Wl,-Map,libz.map -o libz.elf $(OBJS) -lgcc
	$(PREFIX)objdump -d libz.elf > libz.dasm

# --pad-to=_libz_end, so libz's own .bss is present in the blob as
# literal zeros. It has to be: the blob sits at the FRONT of the image
# and the ZEXE header describes exactly one .bss, at the end. See
# libz.ld's own comment.
libz.bin: libz.elf
	$(PREFIX)objcopy -O binary --pad-to=$$($(PREFIX)nm libz.elf | \
		awk '$$3=="_libz_end"{print "0x"$$1}') libz.elf libz.bin

libz.sym: libz.elf libz_table.def libz.h mksyms.py
	python3 mksyms.py $(PREFIX)nm libz.elf libz_table.def libz.sym

clean:
	rm -f libz.elf libz.bin libz.sym libz.map libz.dasm *.o *.d

.PHONY: all clean
-include $(wildcard *.d)
