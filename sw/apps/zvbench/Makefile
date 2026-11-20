# sw/apps/zvbench -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# How fast this machine decodes ZV (docs/zv.md, "zvbench"). Not in
# sw/apps/Makefile's APPS and not on any card: `make -C sw/apps/zvbench`,
# copy zvbench.bin over, and a clip as /zvbench.zv.

APP = zvbench

OBJS = zeitlos.o zobj.o zgfx.o zfsapp.o zv.o zvbench.o

# the record buffer (the clip's largest record and two chunks)
APP_STACK = 256K

# the decoder is the program
APP_CFLAGS = -O2

include ../../common/app.mk
