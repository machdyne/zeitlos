# sw/common/zgl -- host tests of zgl (docs/zgl.md). The library itself
# is built into each program that uses it (SRC_DIRS, sw/common/app.mk).
HOSTCC ?= cc

test: tests/test_zgl tests/test_frames tests/test_t2 t2fw
	tests/test_zgl
	tests/test_frames
	tests/test_t2
	$(MAKE) -C tests/pages

tests/test_zgl: tests/test_zgl.c zgl.c zgl.h zgl_int.h GL/gl.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers -O1 -g \
		-fsanitize=address,undefined -I. -o $@ tests/test_zgl.c zgl.c -lm

# the front end's cost, and a ZGL_FRAMES frame's, in the simulator (sim/zsim-headless)
bench:
	$(MAKE) -C tests/bench
	$(MAKE) -C tests/bench_frames
	$(MAKE) -C ../../../sim zsim-headless
	../../../sim/zsim-headless -n 0 tests/bench/zglbench.bin
	../../../sim/zsim-headless -n 0 tests/bench_frames/framebench.bin

tests/test_frames: tests/test_frames.c zgl.c zgl_frames.c zgl_lists.c zgl.h zgl_int.h GL/gl.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers -O1 -g \
		-fsanitize=address,undefined -I. -o $@ tests/test_frames.c zgl.c zgl_frames.c zgl_lines.c zgl_lists.c -lm

# T2's geometry unit, built for its zeitlos32g core (the RISC-V cross compiler)
t2fw:
	-$(MAKE) -C ../../t2fw

tests/test_t2: tests/test_t2.c tests/t2_model.c tests/t2_raster.c tests/t2g_iss.c t2fw.c zgl.c zgl_frames.c zgl_lists.c zgl_lines.c zgl_t2.c zgl.h zgl_int.h zgl_t2.h GL/gl.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers -O1 -g \
		-fsanitize=address,undefined -I. -o $@ tests/test_t2.c tests/t2_model.c tests/t2_raster.c tests/t2g_iss.c t2fw.c zgl.c zgl_frames.c zgl_lines.c zgl_lists.c zgl_t2.c -lm

# ZGL_CHUNKY's software strips in the simulator (VRAM read back)
simtest:
	$(MAKE) -C tests/sim_chunky
	$(MAKE) -C ../../../sim zsim-headless
	../../../sim/zsim-headless -n 0 tests/sim_chunky/chunkytest.bin
	ZSIM_GTRI=2 ../../../sim/zsim-headless -n 0 tests/sim_chunky/chunkytest.bin
	ZSIM_CONSOLE=1 ../../../sim/zsim-headless -n 0 tests/sim_chunky/chunkytest.bin
	ZSIM_CONSOLE=1 ZSIM_GTRI=2 ../../../sim/zsim-headless -n 0 tests/sim_chunky/chunkytest.bin

clean:
	rm -f tests/test_zgl tests/test_frames tests/test_t2
	$(MAKE) -C tests/bench clean
	$(MAKE) -C tests/bench_frames clean
	$(MAKE) -C tests/sim_chunky clean

.PHONY: test bench simtest clean t2fw
