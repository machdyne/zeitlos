/* Host-only shim: the five zfsapp.h calls stl.c uses, over stdio.
 * Not part of the shipped app. */
#ifndef STL_HOST_STUBS_H
#define STL_HOST_STUBS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_fp = NULL;

static int fs_size(char *name) {
	FILE *f = fopen(name, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fclose(f);
	return (int)n;
}

static int fs_open_read(const char *name) {
	g_fp = fopen(name, "rb");
	return g_fp ? 3 : -1;
}

static int fs_read_chunk(int h, void *buf, int maxlen) {
	(void)h;
	if (!g_fp) return -1;
	size_t n = fread(buf, 1, (size_t)maxlen, g_fp);
	return (int)n;
}

static int fs_seek(int h, uint32_t off) {
	(void)h;
	if (!g_fp) return 0;
	return fseek(g_fp, (long)off, SEEK_SET) == 0 ? 1 : 0;
}

static int fs_close_handle(int h) {
	(void)h;
	if (g_fp) { fclose(g_fp); g_fp = NULL; }
	return 1;
}

#endif
