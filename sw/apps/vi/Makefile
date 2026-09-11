# vi -- nextvi (sw/ext/nextvi, ISC) running on Zeitlos.
#
# The editor's own sources are compiled UNMODIFIED from sw/ext/nextvi.
# What this directory adds is vi_zeitlos.c (the port connection, and
# stdin/stdout through the hooks in sw/common/zeitlos.c) and
# posix_stubs.c (the calls upstream makes that this machine does not
# have). See sw/ext/nextvi/ZEITLOS.md.

PREFIX ?= $(RISCV_PREFIX)
include ../../common/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

NEXTVI = ../../ext/nextvi

# -DZEITLOS selects uc_bytemode() in uc.c. -Dmain=nextvi_main renames
# upstream's entry point so that vi_zeitlos.c can own main() and do the
# terminal handoff before the editor starts -- which is what lets the
# editor source stay untouched.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) \
	-Ishim -I../../common -I$(NEXTVI) \
	-DZEITLOS=1

# -Ishim FIRST, and it is not optional.
#
# nextvi includes <poll.h> and <dirent.h>. No embedded newlib or
# picolibc has a poll.h at all -- poll() multiplexes, and these
# libraries have nothing to multiplex -- and newlib's dirent.h for this
# target is a single `#error "<dirent.h> not supported"`.
#
# So these are not "stub it out if you like" headers, they are the
# difference between the editor compiling and not. sw/apps/zcc/libz
# does the same thing for the same reason.
#
# The stubs behind them are in posix_stubs.c, which says what each one
# costs. Only one costs anything: `:e` filename completion.

# -Werror on THIS app's own sources only. sw/ext is vendored upstream
# code and sw/common has warnings that vary by toolchain; neither is
# this app's to fix, and failing the build over them would mean
# patching a vendored tree on every toolchain bump.
APP_CFLAGS = $(CFLAGS) -Werror

LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# ONE object, not nine.
#
# nextvi is a unity build: vi.c #includes conf.c, ex.c, lbuf.c, led.c,
# regex.c, ren.c, term.c and uc.c, and upstream's own cbuild.sh
# compiles `cc vi.c` and nothing else. The individual files do not
# compile standalone -- they rely on vi.h and on each other having been
# included first -- so listing them separately does not build a smaller
# object, it fails.
#
# Found by trying it. Worth stating because the file list looks exactly
# like a set of translation units.
OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zfsapp.o \
	nextvi.o vi_zeitlos.o posix_stubs.o fileio.o

vi: vi.elf vi.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o
zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o
zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o
# zwin for z_launch_arg_take() alone -- how posix hands over a filename.
zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o
zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o
zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o
zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

# The whole editor, from the one translation unit upstream intends.
#
# -Dmain=nextvi_main renames its entry point so vi_zeitlos.c can own
# main() and do the terminal handoff before the editor starts. That
# rename is what lets every line of sw/ext/nextvi stay untouched.
# Three renames, all of them -D rather than an edit to sw/ext:
#
#   main   -> nextvi_main   so vi_zeitlos.c owns main() and can do the
#                           terminal handoff before the editor starts
#
# itoa is renamed too, but NOT here -- see shim/stdlib.h. A -D applies
# to newlib's declaration as well as nextvi's, so renaming both leaves
# the conflict exactly where it was.
#   lstat  -> stat          FAT has no symbolic links, so on this
#                           machine lstat IS stat. newlib declares stat
#                           and not lstat
#
# Checked rather than discovered one build at a time: every name
# nextvi exports was compared against what a C library plausibly
# claims, and `itoa` is the only real collision (`swap` is static).
nextvi.o: $(NEXTVI)/vi.c
	$(CC) $(CFLAGS) -Dmain=nextvi_main -Dlstat=stat \
		-c $(NEXTVI)/vi.c -o nextvi.o

vi_zeitlos.o: vi_zeitlos.c
	$(CC) $(APP_CFLAGS) -c vi_zeitlos.c -o vi_zeitlos.o
posix_stubs.o: posix_stubs.c
	$(CC) $(APP_CFLAGS) -c posix_stubs.c -o posix_stubs.o
# The POSIX descriptor layer over zfsapp -- see fileio.c. Overrides the
# weak _open/_read/_write/_close/_fstat in sw/common/zeitlos.c.
fileio.o: fileio.c
	$(CC) $(APP_CFLAGS) -c fileio.c -o fileio.o

vi.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,vi.map -o vi.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble vi.elf > vi.dasm

vi.bin: vi.elf
	@EDATA=$$($(PREFIX)nm vi.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm vi.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary vi.elf vi.data; \
	python3 ../../../tools/mkexec.py vi.data vi.bin $$(($$END - $$EDATA)); \
	rm -f vi.data; \
	echo "vi.bin: $$(wc -c < vi.bin) bytes"

clean:
	rm -f vi.elf vi.bin vi.data vi.map vi.dasm *.o *.d

.PHONY: vi clean
-include $(wildcard *.d)
