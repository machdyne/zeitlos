/*
 * planedemo -- the film's data, made on the build host: every scene's
 * pictures and the instruments, by the same code the board would run
 * (pd_film_visit(), writing), to film.pdd, which pd_blob.c links in.
 *
 *   mkfilm OUT.pdd
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pd.h"
#include "../pd_gfx.h"
#include "../pd_scenes.h"

#undef main     /* planedemo.c's is game_main here: its helpers are what is wanted */

int main(int argc, char **argv) {
	pd_store_t st;
	FILE *f;
	if (argc < 2) { fprintf(stderr, "usage: mkfilm OUT.pdd\n"); return 2; }
	pd_trig_init();
	pd_planes = 8;
	pd_keep(0xff, 0xff);
	memset(&st, 0, sizeof(st));
	st.writing = true;
	if (!pd_film_visit(&st, true) || st.bad) { fprintf(stderr, "mkfilm: out of memory\n"); return 1; }
	f = fopen(argv[1], "wb");
	if (!f || fwrite(st.buf, 1, st.len, f) != st.len || fclose(f)) { fprintf(stderr, "mkfilm: cannot write %s\n", argv[1]); return 1; }
	printf("mkfilm: %s, %u KB\n", argv[1], (unsigned)(st.len / 1024));
	return 0;
}
