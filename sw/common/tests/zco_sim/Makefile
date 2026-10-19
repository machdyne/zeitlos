# Not an app: zco's register switch on the RISC-V CPU, under the
# simulator (docs/responsive.md):
#
#   make -C sw/common/tests/zco_sim
#   sim/zsim-headless -n 0 sw/common/tests/zco_sim/zco_sim.bin
APP = zco_sim
APP_STACK = 32K
CLEAN_EXTRA = jpgdata.c
OBJS = zeitlos.o zobj.o zimg.o zco.o jpgdata.o zco_sim.o

include ../../app.mk

jpgdata.c: gen.py ../../../data/images/squirrel.jpg
	python3 gen.py $@

jpgdata.o: jpgdata.c
	$(CC) $(CFLAGS) -c jpgdata.c -o jpgdata.o
