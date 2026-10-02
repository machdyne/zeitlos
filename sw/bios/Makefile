PREFIX ?= $(RISCV_PREFIX)
# No $(LIBC_FLAGS) here, deliberately: the BIOS is built
# -ffreestanding -nostdlib and has no C library at all. Pulling in
# picolibc's specs would drag its headers in too, and bios.c has its
# own globals called stdin/stdout that then collide with them.
CFLAGS=-DBOARD_$(BOARD) -DFPGA_$(FAMILY) $(ARCH_DEFS)

# DDR3 boards build a minimal BIOS (bios.c, BIOS_DDR3): logo, training,
# boot. The monitor is still in the source but nothing calls it, and
# without section garbage collection it would still take the space the
# training needs. bios.lds KEEPs .init, which holds the reset vector
# and is the only thing that reaches main() and irq(); with nothing
# kept the linker may discard all of it. KEEP is a no-op for every
# other board, whose BIOS is byte-identical with or without it.
# Keep in step with DDR3_BOARDS in the top-level Makefile.
ifneq ($(filter MOZART_ML2 SERGEI_ML2 NOIR KLINGE,$(BOARD)),)
CFLAGS += -ffunction-sections -fdata-sections -Wl,--gc-sections
endif
# ARCH/ABI come from one place -- see that file for why
include ../common/arch.mk
ABI=ilp32

#BRAM_WORDS=1280
#BRAM_WORDS=1536
# Words of BIOS block RAM. Set by the top-level Makefile per board; see
# BRAM_WORDS there. The stack starts at its top (BRAM_BYTES).
BRAM_WORDS ?= 2048
BRAM_BYTES = $(shell echo $$(( $(BRAM_WORDS) * 4 )))
CFLAGS += -DBRAM_BYTES=$(BRAM_BYTES)
# Per-board extra defines (e.g. -DBIOS_NO_XFER), from the top-level Makefile.
BIOS_FLAGS ?=
CFLAGS += $(BIOS_FLAGS)
# Refuse to build a BIOS whose stack has less than this many bytes
# between the image's end and the top of the RAM. 0 = report only.
BIOS_MIN_STACK ?= 0

BOOT=boot_picorv32.S

bios: bios.hex bios.bin bios_seed.hex

# Rebuild when the FLAGS change, not only when a source does: CFLAGS
# carries -DBOARD_x -DFPGA_x, which make cannot see. On families that
# build the BIOS into the gateware at synthesis (xc7, gatemate), a BIOS
# left over from another board would otherwise be baked silently into
# this board's bitstream.
BUILD_FLAGS = $(PREFIX) $(CFLAGS) $(ARCH) $(ABI) $(BRAM_WORDS)
.build-flags: force
	@echo '$(BUILD_FLAGS)' | cmp -s - $@ || echo '$(BUILD_FLAGS)' > $@
force:

bios.elf: bios.lds $(BOOT) bios.c bios_picosoc.c irq.c .build-flags
	$(PREFIX)gcc $(CFLAGS) -Os -march=$(ARCH) -mabi=$(ABI) -ffreestanding -nostdlib -Wall -o bios.elf $(BOOT) bios.c bios_picosoc.c irq.c --std=gnu99 -Wl,-Bstatic,-T,bios.lds,-Map,bios.map,--strip-debug -lgcc
	 $(PREFIX)objdump -S --disassemble bios.elf > bios.asm

bios_seed.hex:
	ecpbram -g bios_seed.hex -w 32 -d $(BRAM_WORDS)

bios.hex: bios.bin
	@# via a temp name: a plain `>` creates bios.hex before makehex runs,
	@# so a BIOS too big for BRAM_WORDS used to leave an empty or stale
	@# bios.hex for synthesis to read without complaint
	rm -f bios.hex
	python3 makehex.py bios.bin $(BRAM_WORDS) > bios.hex.tmp && mv bios.hex.tmp bios.hex
	@echo "BIOS size: $$(grep .. bios.hex | wc -l) / $$(wc -l < bios.hex)"
	@end=$$($(PREFIX)nm bios.elf | awk '$$3=="end"{print $$1}'); room=$$(( $(BRAM_BYTES) - 0x$$end )); \
	echo "BIOS stack room: $$room bytes (image ends at $$((0x$$end)), stack starts at $(BRAM_BYTES))"; \
	if [ $$room -lt $(BIOS_MIN_STACK) ]; then \
		echo "*** BIOS stack room $$room < BIOS_MIN_STACK $(BIOS_MIN_STACK): the stack would run into the"; \
		echo "*** image and overwrite return addresses. Make the BIOS smaller (BIOS_FLAGS)."; \
		rm -f bios.hex; exit 1; fi

bios.bin: bios.elf
	$(PREFIX)objcopy -O binary bios.elf bios.bin

clean:
	rm -f bios.elf bios.hex bios.hex.tmp bios.bin bios.map bios.asm .build-flags

dist_clean: clean
	rm -f bios_seed.hex

.PHONY: bios clean force
