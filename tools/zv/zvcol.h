// zvcol.h - the encoder's colour layer (format: sw/common/zv.h)
#ifndef ZVCOL_H
#define ZVCOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct zvc zvc_t;

// cell: 8 or 4 (zv.h, the layer's header)
zvc_t *zvc_new(int w, int h, int cell);
// a frame's planes (w x h each, full range; u and v centred on 128);
// key starts a new group (one a seek point)
void zvc_frame(zvc_t *c, const uint8_t *y, const uint8_t *u, const uint8_t *v, int key);
// the whole section; *coloured: the share of cells not grey
size_t zvc_finish(zvc_t *c, uint8_t **out, double *coloured);
// The whole film's chroma: its weighted mean in *u, *v (-128..127,
// 0 is grey), and how far the cells lie off the line from grey through
// that mean (RMS; about grey, from grey). A small spread is one hue over
// everything: a black and white film on a
// tinted or yellowed print, which is better shown in greys.
double zvc_tint(const zvc_t *c, double *u, double *v);
void zvc_free(zvc_t *c);

#endif
