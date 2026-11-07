# sw/apps/mauskart3d/tests/simbench -- mauskart3d's frames in the
# simulator with its cycle model, T2 there as its model (ZSIM_GTRI=2):
# where the CPU's cycles go. make here (APP_CFLAGS=... -DFRAMES=n for
# another length; it exits after them), then
#
#   ZSIM_COST=lakritz ZSIM_GTRI=2 ZSIM_GTRI_NOTEX=1 ../../../../../sim/zsim-headless -n 0 simbench.bin
#   (lakritz_dc: with a data cache; ddr3: sergei_ml2's zeitlos32p, with
#   textures: no ZSIM_GTRI_NOTEX). ZSIM_PROF=prof.txt and
#   tools/simprof.py simbench.dasm prof.txt: by function.
APP = simbench
SRC_DIRS = $(COMMON_DIR)/zgl $(COMMON_DIR) ../.. ../../../mausrun3d ../../../mausrun
APP_CFLAGS = -I$(COMMON_DIR)/zgl -DMK_SIMBENCH
OBJS = zeitlos.o zobj.o zgl.o zgl_lines.o zgl_frames.o zgl_chunky.o zgl_lists.o zgl_t2.o \
	zfont_data.o track.o karts.o vfont.o sprites.o mauskart3d.o simbench.o
APP_STACK = 8M

# the game itself, its main() renamed: simbench.c's main() runs it
mauskart3d.o: ../../mauskart3d.c
	$(CC) $(CFLAGS) -Dmain=game_main -c $< -o $@

include ../../../../common/app.mk
