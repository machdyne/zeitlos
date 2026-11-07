# picorv32's rotates (ENABLE_ROT, rtl/boards.vh's `CPU_ROT): rot.S under
# iverilog, with them and without. Needs iverilog and a RISC-V assembler
# that knows Zbb (xPack GCC's does).
#
#   make            both
CROSS ?= riscv-none-elf-
# the OS's toolchain, when it is not on PATH (xPack GCC, docs/build.md)
XPACK := $(firstword $(wildcard /opt/xpack/*/bin))
ifneq ($(XPACK),)
export PATH := $(PATH):$(XPACK)
endif

all: rot.hex
	iverilog -g2012 -DROT=1 -o /tmp/tb_rot1 tb_rot.v ../picorv32.v && vvp -n /tmp/tb_rot1
	iverilog -g2012 -DROT=0 -o /tmp/tb_rot0 tb_rot.v ../picorv32.v && vvp -n /tmp/tb_rot0

rot.hex: rot.S
	$(CROSS)as -march=rv32i_zbb -mabi=ilp32 -o /tmp/rot.o rot.S
	$(CROSS)ld -m elf32lriscv -Ttext=0 -o /tmp/rot.elf /tmp/rot.o
	$(CROSS)objcopy -O binary /tmp/rot.elf /tmp/rot.bin
	python3 -c "import sys; d=open('/tmp/rot.bin','rb').read(); d+=b'\0'*(-len(d)%4); \
open('rot.hex','w').write('\n'.join('%08x'%int.from_bytes(d[i:i+4],'little') for i in range(0,len(d),4))+'\n')"

clean:
	rm -f rot.hex

test: all
.PHONY: all clean test
