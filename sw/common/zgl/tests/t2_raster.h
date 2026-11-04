/* T2's raster, modelled (t2_raster.c) */
#include <stdint.h>
#include "../zgl_t2.h"
void t2r_frame(int32_t x0, int32_t x1, int32_t y0, int32_t y1, uint8_t clear);
volatile uint32_t *t2r_rec(int n);
void t2r_draw(uint8_t pic[240][320], t2_stats_t *st);
const uint32_t *t2r_list(uint32_t *nwords);
