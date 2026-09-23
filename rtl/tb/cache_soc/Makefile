# rtl/tb/cache_soc: builds prog.hex for rtl/tb/tb_cache_soc.v
# same default as the top-level Makefile; e.g. PREFIX=riscv64-unknown-elf-
PREFIX ?= /opt/riscv32i/bin/riscv32-unknown-elf-
CFLAGS = -march=rv32im -mabi=ilp32 -O2 -ffreestanding -nostdlib -fno-builtin \
	-fno-pic -mcmodel=medany -Wall

prog.hex: prog.elf
	$(PREFIX)objcopy -O binary $< prog.bin
	python3 -c "import sys,struct; d=open('prog.bin','rb').read(); d+=b'\0'*(-len(d)%4); \
	[print('%08x'%struct.unpack('<I',d[i:i+4])[0]) for i in range(0,len(d),4)]" > $@

prog.elf: start.S prog.c link.ld
	$(PREFIX)gcc $(CFLAGS) -T link.ld -o $@ start.S prog.c

clean:
	rm -f prog.elf prog.bin prog.hex
