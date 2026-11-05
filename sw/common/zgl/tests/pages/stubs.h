/* the host model's stand-ins for zsoc.h and zeitlos.h (model.c) */
#include <stdint.h>
#include <stdbool.h>
#define Z_GTRI_W_CTRL 0u
#define Z_GTRI_CTRL_VSYNC 2u
#define Z_GTRI_W_STATUS 1u
#define Z_GTRI_W_CMD 2u
#define Z_GTRI_W_VBASE 3u
#define Z_GTRI_W_STRIPS 4u
#define Z_GTRI_W_RESET 5u
#define Z_GTRI_W_ID 7u
#define Z_GTRI_W_CAPS 11u
#define Z_GTRI_CAPS_VBASEQ (1u << 1)
#define Z_GTRI_W_TEXADDR 12u
#define Z_GTRI_W_TEXDATA 13u
#define Z_CHUNKY_W 320
#define Z_CHUNKY_H 240
#define Z_CHUNKY_WORDS (Z_CHUNKY_W * Z_CHUNKY_H / 4)
#define Z_TICK_HZ 732u
uint32_t gtri_rd(uint32_t w);
void gtri_wr(uint32_t w, uint32_t v);
static inline volatile uint32_t *z_gtri_reg(uint32_t w) { static uint32_t d; (void)w; return &d; }
uint32_t sim_frame(void);
#define reg_socctl_frame (sim_frame())
extern volatile uint32_t *z_chunky_mem;
extern uint32_t z_chunky_mem_phys;
void z_chunky_set_pic(void *pic, bool show);
static inline uint32_t z_chunky_phys(void) { return z_chunky_mem ? z_chunky_mem_phys : 0x20000000u; }
uint32_t z_uptime_ticks(void);
static inline bool z_vram_sdram(void) { return true; }
volatile uint32_t *z_chunky_seg(uint32_t y, uint32_t seg);
