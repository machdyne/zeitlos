# sw/common/zgl/tests/bench_frames -- ZGL_FRAMES in the simulator: how
# many instructions a frame costs. make -C sw/common/zgl bench
APP = framebench
SRC_DIRS = $(COMMON_DIR)/zgl
APP_CFLAGS = -I$(COMMON_DIR)/zgl
OBJS = zeitlos.o zobj.o zgl.o zgl_lines.o zgl_frames.o zgl_chunky.o zgl_lists.o zgl_t2.o framebench.o
APP_STACK = 16K
include ../../../app.mk
