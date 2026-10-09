# sw/apps/zlink -- files and a shell between two machines over a couple
# of wires (docs/zlink_app.md, docs/zlink.md). Build rules are shared:
# sw/common/app.mk.
#
# The source is zlinkapp.c, not zlink.c: sw/common/zlink.c is the link
# layer, and an app source may not share a name with one there
# (docs/build.md). The link layer and its transports have their own
# host tests in sw/common/tests/Makefile.zlink; this app's is
# `make -C sw/apps/zlink/tests`, two copies of it on simulated wires.

APP = zlink
APP_MAIN_RULE = 0

OBJS = zeitlos.o zobj.o zport.o zwin.o zfsapp.o zrng.o zgpio.o zgpio_stream.o \
	zlink.o zlink_soft.o zlink_stream.o zlinkapp.o

# 32KB: the port layer's sends in flight sit on the heap -- up to eight
# for each of a shell, a term and three commands.
APP_STACK = 32K

include ../../common/app.mk
