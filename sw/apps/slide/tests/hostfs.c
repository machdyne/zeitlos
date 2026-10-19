/*
 * The board's file calls and clock, on the build machine: stdio, and a
 * clock that moves one tick a call (so a time slice is a few steps).
 * For the host tests that link render.c (its pictures load through
 * sw/common/zimgjob.c).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// The repository, for the board's /media/images (the pictures the card
// image carries): hostfs_root = argv[...] in a test that needs them.
const char *hostfs_root = "../../..";

static FILE *hf[8];

static const char *host_path(const char *p, char *q, int n) {
	if (!strncmp(p, "/media/images/", 14)) {
		snprintf(q, (size_t)n, "%s/sw/data/images/%s", hostfs_root, p + 14);
		return q;
	}
	return p;
}

int fs_open_read(const char *p) {
	char q[512];
	int i;
	for (i = 0; i < 8; i++)
		if (!hf[i]) { hf[i] = fopen(host_path(p, q, sizeof(q)), "rb"); return hf[i] ? i : -1; }
	return -1;
}
int fs_read_chunk(int h, void *b, int n) {
	if (h < 0 || h >= 8 || !hf[h]) return -1;
	return (int)fread(b, 1, (size_t)n, hf[h]);
}
int fs_seek(int h, uint32_t o) {
	if (h < 0 || h >= 8 || !hf[h]) return -1;
	return fseek(hf[h], (long)o, SEEK_SET);
}
int fs_close_handle(int h) {
	if (h >= 0 && h < 8 && hf[h]) { fclose(hf[h]); hf[h] = NULL; }
	return 0;
}
int fs_size(char *p) {
	char q[512];
	FILE *f = fopen(host_path(p, q, sizeof(q)), "rb");
	long n;
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return (int)n;
}
int hostfs_open_count(void) {
	int i, n = 0;
	for (i = 0; i < 8; i++) if (hf[i]) n++;
	return n;
}

static uint32_t hostfs_clock;
uint32_t z_uptime_ticks(void) { return hostfs_clock++; }
