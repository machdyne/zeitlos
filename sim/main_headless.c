/*
 * zeitlos-sim: headless frontend.
 *
 * No display. Runs an app image against the machine model, optionally
 * dumping the framebuffer as PBM frames, and reports what happened on
 * exit. This is the frontend meant for automated testing -- it is what
 * a compiler test suite would run its output under -- so it takes real
 * options and returns a real exit status. See main_sdl.c for the
 * interactive one.
 *
 * The exit status is the point of the rewrite. It used to return 0
 * unconditionally, which is fine for a human watching stderr and
 * useless as a test harness: a binary that executed one illegal
 * instruction and stopped looked exactly like one that ran to
 * completion.
 *
 *   0  the app called _exit(), or ran its whole instruction budget
 *   1  bad usage, or the image would not load
 *   2  illegal instruction (cpu.trapped == 1)
 *   3  ECALL/EBREAK (cpu.trapped == 2)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "simos.h"

static void dump_pbm(machine_t *m, const char *path) {
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

static void usage(const char *argv0) {
	fprintf(stderr,
		"usage: %s [options] <app.bin>\n"
		"\n"
		"  -r, --root DIR      host directory backing the guest filesystem.\n"
		"                      Without it every FS_* syscall fails, which is\n"
		"                      deliberate -- see sim/simos.h.\n"
		"  -m, --ram BYTES     RAM size (default %u)\n"
		"  -n, --insns N       instruction budget, 0 = unlimited (default 2000000)\n"
		"  -f, --frames        write PBM frames (off by default)\n"
		"      --every N       instructions between frames (default 200000)\n"
		"  -o, --outdir DIR    where frames go (default /tmp/zsim_frames)\n"
		"  -q, --quiet         no summary on stderr\n"
		"\n"
		"Frames are OFF by default now: writing ten PBMs of a program that\n"
		"only printed to the UART was the common case, and nobody wanted the\n"
		"files.\n",
		argv0, (unsigned)ZS_RAM_DEFAULT_SIZE);
}

int main(int argc, char **argv) {

	const char *image = NULL;
	const char *root = NULL;
	const char *outdir = "/tmp/zsim_frames";
	size_t ram = 0;
	uint64_t total = 2000000, every = 200000;
	int frames = 0, quiet = 0;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int has_next = (i + 1 < argc);

		if (!strcmp(a, "-r") || !strcmp(a, "--root")) {
			if (!has_next) { usage(argv[0]); return 1; }
			root = argv[++i];
		} else if (!strcmp(a, "-m") || !strcmp(a, "--ram")) {
			if (!has_next) { usage(argv[0]); return 1; }
			ram = (size_t)strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(a, "-n") || !strcmp(a, "--insns")) {
			if (!has_next) { usage(argv[0]); return 1; }
			total = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--every")) {
			if (!has_next) { usage(argv[0]); return 1; }
			every = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(a, "-o") || !strcmp(a, "--outdir")) {
			if (!has_next) { usage(argv[0]); return 1; }
			outdir = argv[++i];
		} else if (!strcmp(a, "-f") || !strcmp(a, "--frames")) {
			frames = 1;
		} else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
			quiet = 1;
		} else if (a[0] == '-' && a[1]) {
			fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
			usage(argv[0]);
			return 1;
		} else {
			image = a;
		}
	}

	if (!image) { usage(argv[0]); return 1; }
	if (!every) every = 200000;

	if (frames) {
		char cmd[1024];
		snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", outdir);
		if (system(cmd) != 0)
			fprintf(stderr, "zeitlos-sim: warning: could not create %s\n", outdir);
	}

	simos_set_root(root);

	machine_t m;
	if (machine_init(&m, ram) != 0) return 1;
	if (machine_load_bin(&m, image) != 0) { machine_destroy(&m); return 1; }

	int frame = 0;
	uint64_t done = 0;

	for (;;) {
		uint64_t budget = total ? (total - done) : every;
		uint64_t chunk = (every < budget) ? every : budget;
		uint64_t ran;

		if (total && done >= total) break;
		if (!m.running || m.exit_requested) break;

		ran = machine_run(&m, chunk);
		done += ran;

		if (frames) {
			char path[1200];
			snprintf(path, sizeof(path), "%s/frame_%04d.pbm", outdir, frame++);
			dump_pbm(&m, path);
		}

		if (ran < chunk) break;		/* app exited or trapped */
	}

	int rc = 0;

	if (m.cpu.trapped == 1) rc = 2;
	else if (m.cpu.trapped == 2) rc = 3;

	if (!quiet) {
		fprintf(stderr, "zeitlos-sim: %llu instructions",
			(unsigned long long)done);
		if (frames) fprintf(stderr, ", %d frames -> %s", frame, outdir);
		fprintf(stderr, "\n");
		if (m.exit_requested)
			fprintf(stderr, "app called _exit()\n");
		if (m.cpu.trapped == 1)
			fprintf(stderr, "ILLEGAL INSTRUCTION at pc=0x%08x\n", m.cpu.trap_pc);
		if (m.cpu.trapped == 2)
			fprintf(stderr, "ECALL/EBREAK at pc=0x%08x\n", m.cpu.trap_pc);
	}

	machine_destroy(&m);
	return rc;
}
