/*
 * planedemo -- the film's data, made on the build host by
 * tests/mkfilm.c (film.pdd) and linked into the program: the
 * instruments and every scene's pictures (pd_gfx.h, "made on the build
 * host"; docs/planedemo.md, "Made before it plays").
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * On the host there is none, unless PD_FILM names a film.pdd to read:
 * the tests play the film both ways.
 */

#include <stdint.h>
#include <stdlib.h>

#ifndef PD_HOST

__asm__(
	"	.section .rodata.pd_film, \"a\"\n"
	"	.balign 16\n"
	"pd_film_data:\n"
	"	.incbin \"film.pdd\"\n"
	"pd_film_end:\n"
	"	.section .data.pd_film, \"aw\"\n"
	"	.balign 4\n"
	"	.globl pd_blob\n"
	"pd_blob:\n"
	"	.word pd_film_data\n"
	"	.globl pd_blob_len\n"
	"pd_blob_len:\n"
	"	.word pd_film_end - pd_film_data\n"
	"	.previous\n");

#else

#include <stdio.h>

const uint8_t *pd_blob;
uint32_t pd_blob_len;

/* PD_FILM: read before main() runs, as the board has it linked in */
__attribute__((constructor)) static void read_film(void) {
	const char *p = getenv("PD_FILM");
	FILE *f;
	long n;
	uint8_t *b;
	if (!p || !(f = fopen(p, "rb"))) return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n);
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { pd_blob = b; pd_blob_len = (uint32_t)n; }
	fclose(f);
}

#endif
