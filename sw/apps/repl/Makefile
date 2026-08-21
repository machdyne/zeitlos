
PREFIX = /opt/riscv32i/bin/riscv32-unknown-elf-
ARCH = rv32i
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=ilp32
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=ilp32
LDFLAGS = -march=$(ARCH) -mabi=ilp32
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

OBJS = zeitlos.o zobj.o zport.o zline.o zrepl.o zdns.o ms.o repl.o

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

# regenerated whenever the submodule's stdlib source is newer than the
# last build's header -- see gen_ms_stdlib.py's own header comment for
# why this isn't committed to the repo instead.
ms_stdlib.h: gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l
	python3 gen_ms_stdlib.py $(MS_DIR)/ms_stdlib.l ms_stdlib.h

ms.o: ms_stdlib.h
	$(CC) $(CFLAGS) $(MS_CFLAGS) -c $(MS_DIR)/ms.c -o ms.o

repl.o:
	$(CC) $(CFLAGS) $(MS_SIZE_CFLAGS) -c repl.c -o repl.o
	$(CC) $(CFLAGS) $(MS_SIZE_CFLAGS) -c repl.c -S -o repl.asm

repl.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-T$(LDSCRIPT),-Map,repl.map -o repl.elf $(OBJS) -lm
	$(PREFIX)objdump -S --disassemble repl.elf > repl.dasm

repl.bin: repl.elf
	@END=$$($(PREFIX)nm repl.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary --pad-to=$$END repl.elf repl.bin

clean:
	rm -f repl.elf repl.bin repl.asm repl.dasm repl.map ms_stdlib.h *.o *.d

.PHONY: repl clean
