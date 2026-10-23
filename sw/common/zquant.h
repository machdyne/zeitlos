#ifndef ZQUANT_H
#define ZQUANT_H

/*
 * Zeitlos -- choosing a few colours for a picture: median cut over a
 * histogram of RGB444 colours, and the nearest of them for every
 * colour. Pure C. (pix's importer, sw/apps/pix/px_import.c, has its own
 * copy with its palette's extras; view's screen face uses this one.)
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdint.h>

/* At most `want` (1..256) colours for the pixels counted in `hist`
 * (4096 counts, one per RGB444 colour), into `out`: boxes of RGB444
 * space, the one with the most pixels times its longest side split at
 * its median, each box's colour its pixels' mean. Returns how many --
 * fewer when the picture has fewer. A histogram with every colour of
 * the picture in at most `want` entries gives exactly those. */
int zq_median_cut(const uint32_t *hist, uint16_t *out, int want);

/* For every RGB444 colour, the index of the nearest of the n colours
 * (green weighted most, blue least). With `only` (a histogram), only
 * the colours it counts: the rest of the table is left as it was. */
void zq_nearest_lut(uint8_t *lut, const uint16_t *pal, int n,
	const uint32_t *only);

/* Its luminance, 0..15. */
int zq_lum(uint16_t rgb444);

#endif
