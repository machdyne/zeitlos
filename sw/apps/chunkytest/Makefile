# sw/apps/chunkytest -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# A hardware test of chunky game mode (docs/color.md, "Chunky"). Not in
# sw/apps/Makefile's APPS and not on any card: `make -C sw/apps/chunkytest`
# and copy chunkytest.bin over.

APP = chunkytest

# 76,800 bytes of picture in .bss
OBJS = zeitlos.o zobj.o chunkytest.o

# the heap: on a `VRAM_SDRAM console the chunky picture itself is in
# main memory, 76,800 bytes more (z_color_set_chunky() allocates it)
APP_STACK = 256K

include ../../common/app.mk
