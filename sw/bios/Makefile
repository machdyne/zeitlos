PREFIX=/opt/riscv32i/bin/riscv32-unknown-elf-
CFLAGS=-DBOARD_$(BOARD) -DFPGA_$(FAMILY)
ARCH=rv32i
ABI=ilp32

#BRAM_WORDS=1280
#BRAM_WORDS=1536
BRAM_WORDS=2048

BOOT=boot_picorv32.S

bios: bios.hex bios.bin bios_seed.hex

bios.elf: bios.lds $(BOOT) bios.c
	$(PREFIX)gcc $(CFLAGS) -Os -march=$(ARCH) -mabi=$(ABI) -ffreestanding -nostdlib -Wall -o bios.elf $(BOOT) bios.c bios_picosoc.c irq.c --std=gnu99 -Wl,-Bstatic,-T,bios.lds,-Map,bios.map,--strip-debug -lgcc
	 $(PREFIX)objdump -S --disassemble bios.elf > bios.asm

bios_seed.hex:
	icebram -g 32 $(BRAM_WORDS) > bios_seed.hex

bios.hex: bios.bin
	python3 makehex.py bios.bin $(BRAM_WORDS) > bios.hex
	@echo "BIOS size: $$(grep .. bios.hex | wc -l) / $$(wc -l < bios.hex)"

bios.bin: bios.elf
	$(PREFIX)objcopy -O binary bios.elf bios.bin

clean:
	rm -f bios.elf bios.hex bios.bin bios.map bios.asm

dist_clean: clean
	rm -f bios_seed.hex

.PHONY: bios clean
