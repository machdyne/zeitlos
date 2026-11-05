# T2's geometry unit, for its zeitlos32g core, which rtl/gpu/gpu_t2.v
# loads into the core's code memory (docs/zgl_t2.md):
#
#   t2fw.hex      without textures (-DT2_NO_TEX): 12 KB of code, 4 KB of
#                 data -- an engine built without texture memory
#   t2fw_tex.hex  with textures: 16 KB and 8 KB (gpu_t2.v's TEX_LOG2)
include ../common/arch.mk
CROSS ?= riscv64-unknown-elf-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
CFLAGS = -march=$(ARCH) -mabi=$(ABI) -O2 -ffreestanding -fno-builtin -nostdlib \
	-Wall -Wextra -I../common/zgl -msmall-data-limit=0 -DT2_CORE
SRCS = crt0.S io.c ../common/zgl/t2fw.c

all: t2fw.hex t2fw_tex.hex

t2fw.elf: $(SRCS) link.ld ../common/zgl/zgl_t2.h
	$(CC) $(CFLAGS) -DT2_NO_TEX -T link.ld -o $@ $(SRCS) -lgcc
	$(CROSS)size $@

t2fw_tex.elf: $(SRCS) link_tex.ld ../common/zgl/zgl_t2.h
	$(CC) $(CFLAGS) -T link_tex.ld -o $@ $(SRCS) -lgcc
	$(CROSS)size $@

%.bin: %.elf
	$(OBJCOPY) -O binary -j .text $< $@

# one 32-bit word a line, for $readmemh
%.hex: %.bin
	python3 -c "import sys,struct; d=open('$<','rb').read(); d+=b'\0'*(-len(d)%4); \
	[print('%08x'%w) for w in struct.unpack('<%dI'%(len(d)//4), d)]" > $@

clean:
	rm -f t2fw.elf t2fw.bin t2fw.hex t2fw_tex.elf t2fw_tex.bin t2fw_tex.hex
.PHONY: all clean
.PRECIOUS: %.bin %.elf
