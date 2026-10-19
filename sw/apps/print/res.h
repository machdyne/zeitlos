#ifndef PRINT_RES_H
#define PRINT_RES_H

/*
 * print -- what a job names and does not carry: fonts by name (from
 * /sys/fonts, or compiled in) and pictures by path, decoded to grey once
 * and kept while the job prints. The loaders zpaper's player and the
 * PDF/PostScript writer call. docs/print.md, "Memory".
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zcfont.h"

// Reads a whole file into the pool (zpool.h), or NULL.
typedef uint8_t *(*res_load_fn)(const char *path, uint32_t *len);

// `image_budget`: bytes of decoded pictures kept at once (older ones
// are dropped and decoded again if wanted).
void res_init(res_load_fn load, uint32_t image_budget);

bool res_font(void *ctx, const char *name, const zc_font_t **out);
const zc_font_t *res_fallback(void);

bool res_image(void *ctx, const char *path, int contrast, int bright,
	int maxw, int maxh, const uint8_t **pix, int *w, int *h);

// What could not be loaded, for the message at the end: a count, and
// the first name.
int res_missing(char *first, int n);

#endif
