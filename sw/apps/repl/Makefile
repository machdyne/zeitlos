
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
# -ffunction-sections/-fdata-sections + --gc-sections (below) let the
# linker drop code and data nothing actually references. This is worth
# real money here: sw/common is shared by every app, so each one links
# whole objects for the sake of a fraction of them -- repl pulls in
# zfont_data.o for the single font `text` uses and gets all four, and
# zobj.o/zgfx.o/zstream.o are each used partially. Measured on this
# app: ~7KB. wm and term save roughly twice that, since a larger share
# of what they link is unreferenced.
#
# Set GC_SECTIONS=0 to turn this off. It's on by default because it's
# a pure win on every build tested -- but section GC on a bare-metal
# target can in principle drop something reachable only from assembly
# or referenced solely by the linker script, and if an app starts
# misbehaving in a way that smells like missing code, this is the
# first thing to rule out.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32 $(GC_CFLAGS)
LDFLAGS = -march=$(ARCH) -mabi=ilp32 $(GC_LDFLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# sw/ext/ms -- Machdyne Scheme, vendored directly (already patched
# for Zeitlos, not upstream-plus-a-patch-step) -- see docs/scheme.md.
MS_DIR = ../../ext/ms
MS_PLATFORM_DIR = ../../common/ms_platform

# Cell heap / GC-protect-stack sizing -- overridable at the command
# line, e.g. `make MS_HEAP_SIZE=8000`. See docs/scheme.md for the
# memory-budget reasoning behind the defaults below; 4000 cells left
# comfortable headroom for the full stdlib plus interactive use in
# testing, against this app's actual process memory budget.
# Passed to BOTH ms.o and repl.o below (repl.c's own boot log prints
# these) from these same two variables specifically so the two
# translation units can never see different values for the same
# build -- see repl.c's own comment on why that matters.
MS_HEAP_SIZE ?= 4000
MS_PROTECT_STACK_SIZE ?= 192

MS_SIZE_CFLAGS = -DMS_HEAP_SIZE=$(MS_HEAP_SIZE) \
	-DMS_PROTECT_STACK_SIZE=$(MS_PROTECT_STACK_SIZE)

# -DLIX / -DMS_STATIC_HEAP only apply to ms.c's own compile -- repl.c
# doesn't include ms.c or need either macro itself, only the sizes
# above (for its boot-log printf and nothing else).
MS_CFLAGS = -DLIX -DMS_STATIC_HEAP $(MS_SIZE_CFLAGS) \
	-I$(MS_PLATFORM_DIR) -I.

# sw/ext/te -- a small VT100-based text editor, vendored as a git
# submodule (see docs/editor.md and te's own README.md) -- built here
# with -DEMBEDDED (te's own fs.h/getch() contract, satisfied by
# te_support/fs.h + sw/common/zfsapp.c) and -DTE_HOST_IO (repl-side
# I/O indirection -- te_support/te_host_io.h, implemented by
# te_bridge.c -- see that submodule's own top-of-file comment on this
# flag for why it exists and why it doesn't affect any OTHER build of
# te.c). TE_DEFAULT_ROWS/COLS match `term`'s own fixed VT100 screen
# size exactly (VT_ROWS/VT_COLS, sw/common/zvt100.h) rather than te's
# own smaller built-in defaults, so `te` uses the whole window.
TE_DIR = ../../ext/te
TE_SUPPORT_DIR = te_support
TE_DEFAULT_ROWS ?= 25
TE_DEFAULT_COLS ?= 80

# how large a file `te` will open -- see te_bridge.c's own comment on
# why this is deliberately conservative relative to repl's 64KB heap,
# and docs/editor.md for the arithmetic behind the default.
# Overridable, e.g. `make TE_MAX_FILE_SIZE=4096`.
TE_MAX_FILE_SIZE ?= 2048

TE_CFLAGS = -DEMBEDDED -DTE_HOST_IO \
	-DTE_DEFAULT_ROWS=$(TE_DEFAULT_ROWS) -DTE_DEFAULT_COLS=$(TE_DEFAULT_COLS) \
	-I$(TE_SUPPORT_DIR)

OBJS = zeitlos.o zobj.o zport.o zline.o zrepl.o zdns.o zfsapp.o zstream.o \
	zgfx.o zfont_data.o zwin.o ms.o \
	te.o te_bridge.o page.o zapi.o repl.o

repl: repl.elf repl.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

zline.o:
	$(CC) $(CFLAGS) -c ../../common/zline.c -o zline.o

zrepl.o:
	$(CC) $(CFLAGS) -c ../../common/zrepl.c -o zrepl.o

zdns.o:
	$(CC) $(CFLAGS) -c ../../common/zdns.c -o zdns.o

# needed for `tget`/`tput` (zapi.c's own "Networking" section) --
# repl didn't link this before this API existed either.
zstream.o:
	$(CC) $(CFLAGS) -c ../../common/zstream.c -o zstream.o

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

# the window/graphics stack -- see zapi.c's own "Windows" section for
# why repl needs this now (it didn't before this API existed). Same
# three objects/build rules sw/apps/hello_win's own Makefile already
# uses for the same three source files -- copied here verbatim rather
# than reinvented, this is the established way any app links against
# z_win_*.
zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

# regenerated whenever the submodule's stdlib source is newer than the
# last build's header -- see gen_ms_stdlib.py's own header comment for
# why this isn't committed to the repo instead.
ms_stdlib.h: gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l
	python3 gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l ms_stdlib.h

ms.o: ms_stdlib.h
	$(CC) $(CFLAGS) $(MS_CFLAGS) -c $(MS_DIR)/ms.c -o ms.o

# te.c itself is never edited in place here -- if a fix is ever needed
# upstream, edit $(TE_DIR)/te.c directly (it's a git submodule) and
# commit it there; this rule just compiles whatever's currently
# checked out, same as ms.o does for sw/ext/ms above.
te.o: $(TE_DIR)/te.c $(TE_SUPPORT_DIR)/fs.h $(TE_SUPPORT_DIR)/te_host_io.h
	$(CC) $(CFLAGS) $(TE_CFLAGS) -c $(TE_DIR)/te.c -o te.o

te_bridge.o: te_bridge.c te_bridge.h $(TE_SUPPORT_DIR)/te_host_io.h
	$(CC) $(CFLAGS) -DTE_MAX_FILE_SIZE=$(TE_MAX_FILE_SIZE) -c te_bridge.c -o te_bridge.o

# needs MS_SIZE_CFLAGS as of this revision -- the `free` procedure
# reports MS_HEAP_SIZE, so this translation unit has to see the same
# value ms.o and repl.o were built with. Same
# one-variable-feeds-every-compile reasoning documented above.
zapi.o: zapi.c zapi.h ms_api.h ../../common/zfsapp.h ../../common/zwin.h \
	../../common/zdns.h ../../common/znet.h ../../common/zstream.h \
	../../common/zproc.h
	$(CC) $(CFLAGS) $(MS_SIZE_CFLAGS) -c zapi.c -o zapi.o

# the `page` command's VT100 file viewer -- see page.h. No ms/te flags
# needed: it talks to zport and zfsapp only, and knows nothing about
# either Scheme or the editor.
page.o: page.c page.h ../../common/zfsapp.h ../../common/zport.h
	$(CC) $(CFLAGS) -c page.c -o page.o

repl.o:
	$(CC) $(CFLAGS) $(MS_SIZE_CFLAGS) -c repl.c -o repl.o
	$(CC) $(CFLAGS) $(MS_SIZE_CFLAGS) -c repl.c -S -o repl.asm

repl.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,repl.map -o repl.elf $(OBJS) -lm
	$(PREFIX)objdump -S --disassemble repl.elf > repl.dasm

repl.bin: repl.elf
	@EDATA=$$($(PREFIX)nm repl.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm repl.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary repl.elf repl.data; \
	python3 ../../../tools/mkexec.py repl.data repl.bin $$(($$END - $$EDATA)); \
	rm -f repl.data

clean:
	rm -f repl.elf repl.bin repl.data repl.asm repl.dasm repl.map ms_stdlib.h *.o *.d

.PHONY: repl clean
