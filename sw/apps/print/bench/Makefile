# Not an app: a benchmark of the print service's work on the RISC-V
# CPU, run under the simulator, which counts retired instructions
# (docs/print.md, "Speed"):
#
#   make -C sw/apps/slide job
#   make -C sw/apps/print/bench
#   sim/zsim-headless -m 33554432 -n 0 sw/apps/print/bench/bench_print.bin
APP = bench_print
APP_STACK = 32K
APP_CFLAGS = -DZ_IMG_HAVE_PNG=1
SRC_DIRS = ..
CLEAN_EXTRA = benchdata.c
OBJS = zeitlos.o zobj.o zimg.o zinflate.o zcanvas.o zcfont.o zfont_data.o zpage.o \
	zpaper.o zpool.o out.o pwg.o job.o raster.o vec.o res.o run.o ipp.o benchdata.o bench.o

include ../../../common/app.mk

benchdata.c: gen_data.py /tmp/slide/deck.zpp
	python3 gen_data.py $@ /tmp/slide/deck.zpp

benchdata.o: benchdata.c
	$(CC) $(CFLAGS) -c benchdata.c -o benchdata.o
