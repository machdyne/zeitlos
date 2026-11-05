# sw/apps/mausrun3d/tests/simbench -- mausrun3d's frames in the simulator
# with its cycle model, T2 there as its model (ZSIM_GTRI=2): where the
# CPU's cycles go. make -C sw/apps/mausrun3d simbench (docs/mausrun3d.md)
APP = simbench
SRC_DIRS = $(COMMON_DIR)/zgl ../.. ../../../mausrun
APP_CFLAGS = -I$(COMMON_DIR)/zgl -DMR3_SIMBENCH
OBJS = zeitlos.o zobj.o zgl.o zgl_lines.o zgl_frames.o zgl_chunky.o zgl_lists.o zgl_t2.o \
	world.o world_builtin.o models.o vfont.o sprites.o mausrun3d.o simbench.o
APP_STACK = 8M

# the game itself, its main() renamed: simbench.c's main() runs it
mausrun3d.o: ../../mausrun3d.c
	$(CC) $(CFLAGS) -Dmain=game_main -c $< -o $@

include ../../../../common/app.mk
