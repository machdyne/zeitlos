/* the triangle engine in the simulator (gtri.c), with ZSIM_GTRI set */
#include <stdint.h>
struct machine;
#define ZS_GTRI_BASE 0x7C000000u
int gtri_enabled(void);
uint32_t gtri_read(uint32_t w);
void gtri_write(uint32_t *vram, uint32_t w, uint32_t val);
/* ZSIM_CONSOLE (`VRAM_SDRAM): the picture LINEAR in main memory at VBASE,
 * where VBASE is in ram (ram_base .. + size) -- as gpu_t2.v LINEAR */
void gtri_set_linear(uint8_t *ram, uint32_t ram_base, uint32_t size);
