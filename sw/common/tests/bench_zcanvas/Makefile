# Not an app: a benchmark of sw/common/zcanvas.c and zcfont.c on the
# RISC-V CPU, run under the simulator, which counts retired
# instructions (docs/zcanvas.md, "Speed"):
#
#   make -C sw/common/tests/bench_zcanvas
#   sim/zsim-headless -n 0 sw/common/tests/bench_zcanvas/bench_zcanvas.bin
#
# `make APP_CFLAGS=-DONLY_CACHED` draws only the cached card, eleven
# times, for profiling the common case on its own.
APP = bench_zcanvas
APP_STACK = 32K
CLEAN_EXTRA = fontdata.c
OBJS = zeitlos.o zobj.o zcanvas.o zcfont.o zfont_data.o fontdata.o bench.o

include ../../app.mk

fontdata.c: gen_fontdata.py ../../../data/font/zhf/sans.zhf ../../../data/font/zhf/serif.zhf ../../../data/font/zfn/m9x15.zfn
	python3 gen_fontdata.py $@

fontdata.o: fontdata.c
	$(CC) $(CFLAGS) -c fontdata.c -o fontdata.o
