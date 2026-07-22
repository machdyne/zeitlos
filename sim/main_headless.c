/* Headless test frontend: no display, just runs the machine and dumps
 * the framebuffer as a PPM every N instructions. Used for validating
 * the emulator core without a display server available. Not meant to
 * be the end-user CLI (see main_sdl.c for that). */

#include <stdio.h>
#include <stdlib.h>
#include "machine.h"

static void dump_ppm(machine_t *m, const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	fprintf(f, "P4\n%d %d\n", ZS_SCREEN_W, ZS_SCREEN_H);
	/* P4 = binary PBM (1bpp), MSB-first per byte, 1=black by PBM convention */
	for (int y = 0; y < ZS_SCREEN_H; y++) {
		uint8_t byte = 0;
		int nbits = 0;
		for (int x = 0; x < ZS_SCREEN_W; x++) {
			int on = machine_get_pixel(m, x, y);
			byte = (uint8_t)((byte << 1) | (on ? 1 : 0));
			nbits++;
			if (nbits == 8) { fputc(byte, f); byte = 0; nbits = 0; }
		}
		if (nbits) { byte <<= (8 - nbits); fputc(byte, f); }
	}
	fclose(f);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <app.bin> [total_insns] [dump_every] [outdir]\n", argv[0]);
		return 1;
	}
	uint64_t total = argc > 2 ? strtoull(argv[2], NULL, 0) : 2000000;
	uint64_t every = argc > 3 ? strtoull(argv[3], NULL, 0) : 200000;
	const char *outdir = argc > 4 ? argv[4] : "/tmp/zsim_frames";

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "mkdir -p %s", outdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "zeitlos-sim: warning: could not create %s\n", outdir);
	}

	machine_t m;
	if (machine_init(&m, 0) != 0) return 1;
	if (machine_load_bin(&m, argv[1]) != 0) return 1;

	int frame = 0;
	uint64_t done = 0;
	while (done < total && m.running && !m.exit_requested) {
		uint64_t chunk = (every < (total - done)) ? every : (total - done);
		uint64_t ran = machine_run(&m, chunk);
		done += ran;

		char path[600];
		snprintf(path, sizeof(path), "%s/frame_%04d.pbm", outdir, frame++);
		dump_ppm(&m, path);

		if (ran < chunk) break; /* app exited or trapped */
	}

	fprintf(stderr, "zeitlos-sim(headless): ran %llu instructions, %d frames -> %s\n",
		(unsigned long long)done, frame, outdir);
	if (m.exit_requested) fprintf(stderr, "app called _exit()\n");

	machine_destroy(&m);
	return 0;
}
