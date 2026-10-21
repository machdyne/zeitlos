/*
 * zcolor.c and zgame.c, the real ones, for the host: their frame waits
 * and palette writes go to mr_host.c.
 */
#include "../../../../common/zsoc.h"
void host_wait_frame(void);
void host_set_palette(uint32_t i, uint32_t rgb);
#define z_game_wait_frame host_wait_frame
#define z_color_set_palette host_set_palette
#include "../../../../common/zcolor.c"
#include "../../../../common/zgame.c"
