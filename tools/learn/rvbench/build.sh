#!/bin/sh
# tools/learn/rvbench: learn built for the board's CPU (rv32im, the same
# flags as sw/apps/learn) and run in an emulator that counts the
# instructions of each kind of key press (docs/learn_figures.md, "Speed").
#
#   tools/learn/rvbench/build.sh && python3 tools/learn/rvbench/sim.py tools/learn/rvbench/out/simbench.elf
#
# Needs riscv-none-elf-gcc on the PATH, and python3 with unicorn and
# pyelftools (pip install unicorn pyelftools).
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
L=$R/sw/apps/learn
C=$R/sw/common
make -s -C "$L" figlib.h ms_stdlib.h
CC="riscv-none-elf-gcc -march=rv32im -mabi=ilp32 -std=gnu99 -ffunction-sections -fdata-sections -I$L -I$C -DZ_ARCH_HAS_MUL=1 -DZ_ARCH_HAS_DIV=1 -w"
MS_DEFS="-DLIX -DMS_STATIC_HEAP -DMS_HEAP_SIZE=20000 -DMS_PROTECT_STACK_SIZE=384 -DMS_HASH_SIZE=2048"
O=$H/out
mkdir -p "$O"
rm -f "$O"/*.o
fast="lfig ldraw ltext lstep"
for f in lapp lstep ldraw ltext lmathdraw lquiz lstore lcat lformat lsx lmath lfig lreview; do
	opt=-Os; case " $fast " in *" $f "*) opt=-O2;; esac
	$CC $opt -c "$L/$f.c" -o "$O/$f.o"
done
$CC -O2 -c "$C/zcanvas.c" -o "$O/zcanvas.o"
$CC -O2 -c "$C/zcfont.c" -o "$O/zcfont.o"
$CC -Os -c "$C/zfont_data.c" -o "$O/zfont_data.o"
$CC -Os -c "$C/zrtc.c" -o "$O/zrtc.o"
$CC -O2 $MS_DEFS -I"$C/ms_platform" -I"$L" -c "$R/sw/ext/ms/ms.c" -o "$O/ms.o"
$CC -Os -c "$H/simbench.c" -o "$O/simbench.o"
$CC -Os -c "$H/simplat.c" -o "$O/simplat.o"
$CC -Os -c "$H/sys.c" -o "$O/sys.o"
$CC -c "$H/crt0.S" -o "$O/crt0.o"
riscv-none-elf-gcc -march=rv32im -mabi=ilp32 -nostartfiles -specs=nano.specs -Wl,--gc-sections -u _printf_float \
	-o "$O/simbench.elf" "$O"/*.o -lm -lc -lgcc
echo "$O/simbench.elf"
