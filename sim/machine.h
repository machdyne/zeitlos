/*
 * zeitlos-sim: machine.h
 *
 * Ties the CPU core to the Zeitlos memory map: RAM, VRAM, the line
 * rasterizer, the blitter, UART, and a handful of small stub devices
 * (LED, USB HID cursor, SD card, MTU) that are enough to let real,
 * unmodified app binaries run without an OS underneath them.
 *
 * Memory map (matches sw/common/zeitlos.h / rtl/sysctl.v):
 *
 *   0x00000000 - 0x00000fff   low memory (reg_kernel lives at 0x0c)
 *   0x20000000 - ...          VRAM (framebuffer), 512x384x1bpp, 6144 words
 *   0x80000000 - ...          app RAM (app is linked to run here directly;
 *                             we skip the real MTU translation since we
 *                             only ever run one app with no OS underneath)
 *   0x90000000                MTU control (stub)
 *   0xa0000000 - 0xa000003f   GPU line rasterizer registers
 *   0xb0000000                SD card (stub)
 *   0xc0000000 - 0xc000000f   USB HID (only cursor register wired up)
 *   0xd0000000 - 0xd000001f   GPU blitter registers
 *   0xe0000000 - 0xe0000007   LEDs
 *   0xf0000000 - 0xf0000018   UART0 (16550-style register spacing)
 */

#ifndef ZSIM_MACHINE_H
#define ZSIM_MACHINE_H

#include <stdint.h>
#include <stddef.h>
#include "cpu.h"

#define ZS_VRAM_BASE      0x20000000u
#define ZS_VRAM_WORDS     6144            /* 512*384/32, GPU_PIXEL_DOUBLE mode */
#define ZS_SCREEN_W       512
#define ZS_SCREEN_H       384

#define ZS_RAM_BASE       0x80000000u
#define ZS_RAM_DEFAULT_SIZE (4u * 1024 * 1024)

#define ZS_MTU_BASE       0x90000000u
#define ZS_RASTER_BASE    0xa0000000u
#define ZS_SDCARD_BASE    0xb0000000u
#define ZS_USB_BASE       0xc0000000u
#define ZS_BLIT_BASE      0xd0000000u
#define ZS_LED_BASE       0xe0000000u

/* GPIO (rtl/gpio.v) shares the 0xE nibble with the LED registers: the
 * LEDs are words 0 and 1 of the same block, which is why ZS_LED_BASE
 * above and ZS_GPIO_BASE here are the same address. The ports start
 * 4KB in, eight words (32 bytes) each. See docs/gpio.md. */
#define ZS_GPIO_BASE      0xe0000000u
#define ZS_GPIO_PORT_BASE 0xe0001000u
#define ZS_GPIO_PORT_SIZE 0x20u

/* How many ports the simulated machine has. Two rather than the one a
 * real Obst or Lakritz builds, so an app that gets its port indexing
 * wrong fails HERE rather than on hardware -- with a single port,
 * every wrong index still lands on port 0 and looks fine. */
#define ZS_GPIO_NPORTS    2
#define ZS_UART_BASE      0xf0000000u

#define ZS_REG_KERNEL_ADDR 0x0000000cu

/* The address we install into reg_kernel. When the CPU's PC lands here,
 * the run loop intercepts it and performs the syscall in host code rather
 * than executing any instruction there -- see machine_run(). */
#define ZS_SYSCALL_TRAP_PC 0x00000004u

/* --- line rasterizer state (mirrors rtl/gpu/gpu_raster.v) --- */
typedef struct {
	uint32_t x0, y0, x1, y1;
	uint32_t color;
	uint32_t clip_x0, clip_y0, clip_x1, clip_y1;
	uint32_t clip_enable;
	uint32_t pixel_count;
	uint32_t cur_x, cur_y;
} raster_t;

/* --- blitter state (mirrors rtl/gpu/gpu_blit.v) --- */
typedef struct {
	uint32_t dst_x, dst_y, width, height, pattern;
	uint32_t fill, clip_enable;
} blit_t;

/* --- UART state --- */
typedef struct {
	int raw_mode_active;
	int have_pending;
	int pending_byte;
} uart_t;

#define ZS_LOWMEM_SIZE 4096u

typedef struct machine {
	cpu_t cpu;

	uint8_t lowmem[ZS_LOWMEM_SIZE]; /* 0x00000000 .. 0x00000fff, holds reg_kernel */

	uint8_t *ram;
	size_t   ram_size;

	uint32_t vram[ZS_VRAM_WORDS];

	raster_t raster;
	blit_t   blit;
	uart_t   uart;

	uint32_t reg_led, reg_leds;

	/* GPIO port state, one byte per port. See bus_read32()/bus_write32()
	 * in machine.c for the loopback model the `in` side uses. */
	uint8_t gpio_dir[ZS_GPIO_NPORTS];
	uint8_t gpio_out[ZS_GPIO_NPORTS];
	/* What the far end of the connector is driving, and whether it is
	 * driving at all. Nothing sets these yet -- there is no simulated
	 * PMOD -- but they are what a future one would poke, and having the
	 * model read them means it does not have to change when one exists.
	 * Zero-initialised, so by default nothing external drives anything
	 * and the pull-ups win. */
	uint8_t gpio_ext_dir[ZS_GPIO_NPORTS];
	uint8_t gpio_ext_out[ZS_GPIO_NPORTS];
	uint32_t usb_cursor;   /* bits: x[9:0] y[19:10] buttons[23:20] */

	int running;
	int exit_requested;
	int exit_code;

	/* set by the frontend if it wants to know the screen changed;
	 * left NULL this is simply unused (poll-based frontends are fine) */
	void (*on_vram_dirty)(struct machine *m);

	uint64_t total_instructions;
} machine_t;

/* bus access, used by cpu.c */
uint8_t  bus_read8 (machine_t *m, uint32_t addr);
uint16_t bus_read16(machine_t *m, uint32_t addr);
uint32_t bus_read32(machine_t *m, uint32_t addr);
void bus_write8 (machine_t *m, uint32_t addr, uint8_t  val);
void bus_write16(machine_t *m, uint32_t addr, uint16_t val);
void bus_write32(machine_t *m, uint32_t addr, uint32_t val);

/* lifecycle */
int  machine_init(machine_t *m, size_t ram_size);
void machine_destroy(machine_t *m);

/* Loads a raw app image (as produced by `objcopy -O binary`) at ZS_RAM_BASE,
 * matching sw/common/riscv-app.ld's `. = 0x80000000` and the real kernel's
 * process-start convention (pc=0x80000000, sp=top of the process's memory
 * region). Returns 0 on success. */
int machine_load_bin(machine_t *m, const char *path);

/* Runs up to `max_insns` instructions (0 = unlimited) or until the app
 * calls _exit() / hits an illegal instruction. Returns the number of
 * instructions actually executed. */
uint64_t machine_run(machine_t *m, uint64_t max_insns);

/* Convenience: unpack VRAM bit (x,y) -> 0/1 */
static inline int machine_get_pixel(machine_t *m, int x, int y) {
	if (x < 0 || x >= ZS_SCREEN_W || y < 0 || y >= ZS_SCREEN_H) return 0;
	uint32_t bit = (uint32_t)(y * ZS_SCREEN_W + x);
	return (m->vram[bit / 32] >> (bit % 32)) & 1;
}

#endif
