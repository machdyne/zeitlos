/*
 * planedemo -- the music on a build host: the song through a model of
 * the mixer, written as a WAV (48 kHz, stereo) to listen to before it
 * reaches a board. make -C sw/apps/planedemo/tests wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pd_music.h"

void pdm_host_mix(int16_t *lr, int frames);

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "planedemo.wav";
	static int16_t buf[800 * 2];
	uint32_t frame, total = PD_END_FRAME, n = total * 800, i;
	long peak = 0, clipped = 0;
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return 1; }
	pdm_init();
	fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 4); fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16); put16(f, 1); put16(f, 2); put32(f, 48000); put32(f, 48000 * 4);
	put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, n * 4);
	for (frame = 0; frame < total; frame++) {
		pdm_frame(frame);
		pdm_host_mix(buf, 800);
		for (i = 0; i < 1600; i++) {
			long v = buf[i] < 0 ? -buf[i] : buf[i];
			if (v > peak) peak = v;
			if (v >= 32767) clipped++;
		}
		fwrite(buf, sizeof(int16_t), 1600, f);
	}
	fclose(f);
	printf("%s: %.1f s, peak %ld, %ld clipped samples\n", path, total / 60.0, peak, clipped);
	return 0;
}
