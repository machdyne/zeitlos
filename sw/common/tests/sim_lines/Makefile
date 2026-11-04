# sw/common/tests/sim_lines -- zgfx's lines by the CPU, on a bitstream
# without the rasterizer (ZSIM_CONSOLE=1), against a model of
# rtl/gpu/gpu_raster.v's steps, pixel for pixel.
#   make -C sw/common/tests/sim_lines test
APP = linetest
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o linetest.o
APP_STACK = 16K
include ../../app.mk

test: $(APP)
	$(MAKE) -C ../../../../sim zsim-headless
	ZSIM_CONSOLE=1 ../../../../sim/zsim-headless -n 0 linetest.bin | grep linetest
