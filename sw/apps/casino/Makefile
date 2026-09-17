# Zeitlos Casino -- the front desk.

COMMON = ../../common
GAMES = $(COMMON)/games

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- never hardcode them.
include $(COMMON)/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = $(COMMON)/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zfsapp.o zbank.o \
	cs_banner.o casino.o

casino: casino.elf casino.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zeitlos.c -o zeitlos.o
zobj.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zobj.c -o zobj.o
zgfx.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zgfx.c -o zgfx.o
zfont_data.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zfont_data.c -o zfont_data.o
zwin.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zwin.c -o zwin.o
zfsapp.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zfsapp.c -o zfsapp.o
zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o
cs_banner.o:
	$(CC) $(CFLAGS) -c cs_banner.c -o cs_banner.o
casino.o:
	$(CC) $(CFLAGS) -c casino.c -o casino.o

casino.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,casino.map -o casino.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble casino.elf > casino.dasm

casino.bin: casino.elf
	@EDATA=$$($(PREFIX)nm casino.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm casino.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary casino.elf casino.data; \
	python3 ../../../tools/mkexec.py casino.data casino.bin \
		$$(($$END - $$EDATA)); \
	rm -f casino.data

# REPLACE THE BANNER: drop your own banner.png in here and run this.
# The generated C is committed, so re-running with the same image must
# produce the same file.
banner:
	python3 gen_banner.py

# -- host-side --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2
TARGETSRC = casino.c cs_banner.c

includecheck:
	@for f in $(TARGETSRC) *.h; do \
		$(HOSTCC) -std=gnu99 -Wall -fsyntax-only $$f || exit 1; \
	done
	@echo "includecheck: every target source resolves its headers with no -I"

nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			sed 's/"[^"]*"//g' | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: the front desk is integer-only"

test: includecheck nofloat
	$(MAKE) -C $(GAMES) test

clean:
	rm -f *.o *.d casino.elf casino.bin casino.data casino.map casino.dasm

.PHONY: casino test clean includecheck nofloat banner

-include $(wildcard *.d)
