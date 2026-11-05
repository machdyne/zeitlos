# sw/common/zgl/tests/sim_chunky -- ZGL_CHUNKY's software strips into
# the simulator's VRAM, and T2's (ZSIM_GTRI=2) held to them.
# make -C sw/common/zgl simtest
APP = chunkytest
SRC_DIRS = $(COMMON_DIR)/zgl $(COMMON_DIR)/zgl/tests
APP_CFLAGS = -I$(COMMON_DIR)/zgl
OBJS = zeitlos.o zobj.o zgl.o zgl_lines.o zgl_frames.o zgl_chunky.o zgl_lists.o zgl_t2.o chunkytest.o
APP_STACK = 16K
include ../../../app.mk
