#ifndef ZSYS_H
#define ZSYS_H
/*
 * zsys -- the Zeitlos SoC, emulated: the real BIOS, kernel, drivers and
 * apps, the same binaries a release flashes (docs/sim_parity.md).
 *
 * Unlike sim/'s single-app emulator (machine.c, simos.c), nothing here
 * stands in for the kernel. The CPU is Zeitlos32's -- RV32IM with
 * picorv32's interrupt ABI and zeitlos32p's IRQ timer -- and the bus
 * follows rtl/sysctl.v's address decode; each device is modelled at
 * its registers, as the RTL defines them.
 *
 * Two machines share the CPU:
 *
 *   ZSYS_SOC  the board: BRAM with the BIOS at 0, main memory at
 *             0x4000_0000, VRAM, the flash window at 0x1000_0000, the
 *             CSRs, the MTU, UART0, the tick (IRQ 3); the GPU's
 *             blitter and line rasterizer, glyph memory, socctl and its
 *             cursor, the RTC, an SD card behind the SPI controller
 *   ZSYS_TB   rtl/cpu/zeitlos32/tests/tb_zeitlos32.v: 64KB at 0, the
 *             test port, the IRQ ports, a console -- for the core's
 *             own test programs (rtl/cpu/zeitlos32/tests/prog and zeitlos32p's)
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { ZSYS_SOC, ZSYS_TB } zsys_kind_t;

// What stopped zsys_run().
enum {
	ZSYS_RUNNING = 0,
	ZSYS_LIMIT,		// the cycle budget ran out
	ZSYS_TRAPPED,		// a fault with its line masked, or in a handler: the core halts
	ZSYS_TB_DONE,		// ZSYS_TB: the program wrote the test port
	ZSYS_IDLE,		// waitirq with nothing that could ever wake it
};

#define ZSYS_UART_FIFO 256
#define ZSYS_GLYPH_SIZE 4096			// rtl/mem/glyph.v, ADDR_WIDTH 12
#define ZSYS_SCREEN_W 640
#define ZSYS_SCREEN_H 480
#define ZSYS_ROWS 480
#define ZSYS_STRIDE 80				// bytes a framebuffer row

// The SD card, in SPI mode (zdev.c): a card image file behind rtl/spim.v.
typedef struct {
	uint8_t *img;			// NULL: the slot is empty
	uint64_t size;
	int fd;				// written through, unless a snapshot (-1)
	bool cs;			// chip select asserted
	bool idle, app;			// in the idle state; the next command is an ACMD
	uint8_t cmd[6];
	int cmd_n;
	uint8_t out[600];		// what the card sends next
	int out_n, out_pos;
	int mode;			// SD_* in zdev.c
	uint32_t block;			// the next block of a multiple read or write
	uint8_t wbuf[514];		// a block being written, and its CRC
	int wn;
	uint64_t reads, writes;		// blocks, for --stats
	uint8_t *written;		// a bit a block written since zsys_sd_insert*(), or NULL
} zsys_sd_t;

// One of rtl/usb_hid.v's two ports, with a device on it: port 0 a
// keyboard, port 1 a mouse (zdev.c).
typedef struct {
	uint8_t typ;			// 0 none, 1 keyboard, 2 mouse, 3 gamepad
	uint8_t mods, keys[4];		// the keyboard's boot report
	uint8_t btn;			// the mouse's buttons
	int8_t dx, dy;			// its last movement
	uint8_t wheel;			// the wheel's notches, counted (the mouse register's top byte)
	uint16_t cx, cy;		// the pointer, as the block keeps it
	uint8_t speed;
	uint64_t reports;
} zsys_hid_t;

// rtl/audio.v and rtl/audio_mixer.v (0x7000_05xx), zdev.c: the FIFO,
// the eight-channel mixer, the sample clock, the system volume.
#define ZSYS_AUDIO_FIFO 1024			// AUDIO_FIFO_LOG2 10
#define ZSYS_AUDIO_OUT  16384			// frames kept for the host, at the machine's rate
typedef struct {
	uint8_t ctrl;
	uint8_t rate, mixvol, spdif_fs, mixpos_sel;
	uint16_t step, mod, wmark, volume;
	bool underrun, head_valid;
	uint32_t head, fifo[ZSYS_AUDIO_FIFO];
	uint32_t f_rd, f_n;			// the FIFO behind head
	uint64_t acc;				// the sample clock: += STEP a cycle, a frame at 128 MOD
	int16_t out_l, out_r;			// the frame the DAC holds
	// the mixer's channels
	uint32_t base[8], len[8], lstart[8], llen[8], cstep[8], cctrl[8], pos[8];
	uint8_t active;
	uint32_t dbg_adr, dbg_dat;
	// what was played, for the host: frames at the machine's rate
	int16_t ring[ZSYS_AUDIO_OUT][2];
	uint32_t r_head, r_n;
	double r_phase;				// the resampler's place between two frames
	uint64_t frames;
} zsys_audio_t;

// rtl/spiflash.v's registers (0x1F00_0000), which write the flash.
typedef struct {
	uint32_t addr, len, status;
	bool armed;
	uint8_t page[256];
	uint64_t erases, programs;
} zsys_flashw_t;

// rtl/gpu/gpu_blit.v's registers, as software sees them.
typedef struct {
	uint32_t ctrl;			// fill, clip, glyph, srcmem, rop, cookie, dither
	uint16_t dst_x, dst_y, width, height, glyph_w, glyph_h;
	uint32_t pattern, glyph_addr, fg, bg, src_addr, src_stride, src_shift, src_b_addr;
	uint16_t clip_x0, clip_y0, clip_x1, clip_y1;
	uint32_t dbg_adr, dbg_dat, dbg_cnt;
	uint64_t blits;
} zsys_blit_t;

// rtl/gpu/gpu_raster.v's.
typedef struct {
	uint16_t x0, y0, x1, y1, cx0, cy0, cx1, cy1, cur_x, cur_y, count;
	uint8_t color;
	bool clip;
	uint64_t lines;
} zsys_raster_t;

typedef struct zsys {

	zsys_kind_t kind;

	// -- the CPU: zeitlos32 (rtl/cpu/zeitlos32/zeitlos32.v) --
	uint32_t x[32], pc;
	uint32_t q[4];
	uint32_t irq_pending, irq_mask, irq_level;	// level: the lines held now
	uint32_t latched;				// LATCHED_IRQ
	bool irq_active, irq_delay;
	bool in_waitirq;				// stalled in waitirq (ST_WAITIRQ)
	uint32_t timer;					// zeitlos32p's IRQ timer, 0 off
	uint64_t cycles, instret;
	uint32_t cpi;					// cycles charged an instruction
	int stop;					// ZSYS_* once something stops it
	uint32_t trap_pc;
	const char *trap_why;

	// -- memory --
	uint8_t *bram;   uint32_t bram_size;		// at 0
	uint8_t *ram;    uint32_t ram_size;		// at 0x4000_0000
	uint8_t *vram;   uint32_t vram_size;		// at 0x2000_0000
	uint8_t *flash;  uint32_t flash_size;		// read at 0x1000_0000
	uint32_t mtu;					// translation base, 0 off

	// -- the board's CSRs (docs/csrs.md) --
	uint32_t mem_mb, features, features2, features3, flash_base;

	// -- UART0, a 16550 (rtl/uart.v) --
	uint8_t uart_ier, uart_lcr, uart_mcr, uart_scr, uart_dll, uart_dlh, uart_fcr;
	bool uart_thre_int;				// THRE interrupt pending
	uint8_t rx[ZSYS_UART_FIFO];
	uint32_t rx_head, rx_tail;
	void (*uart_out)(struct zsys *s, uint8_t c);	// a byte sent
	void *user;
	void (*on_tick)(struct zsys *s);		// each tick, for a profiler

	// -- the tick: rtl/sysctl.v's irq_timer, IRQ 3 every 65536 clocks --
	uint64_t next_tick;

	// -- the SD card's SPI controller (rtl/spim.v) and the card behind it --
	uint32_t spisd_ctrl, spisd_data;
	zsys_sd_t sd;

	// -- the GPU: the blitter, the line rasterizer, glyph memory (zdev.c) --
	zsys_blit_t blit;
	zsys_raster_t raster;
	uint8_t glyph[ZSYS_GLYPH_SIZE];			// at 0x3000_0000

	// -- socctl (rtl/socctl.v): the cursor, the phosphor, reboot --
	uint32_t soc_ctrl;
	uint8_t video_mode, video_bright;
	uint8_t game, cslots, caddr;			// game: bit 0 enable, bit 1 wrap
	uint32_t view;					// { vy[25:16], vx[9:0] }, as written
	// colour (docs/color.md): eight planes, the 256-entry palette, chunky,
	// the colour window and desktop colour's exact path -- a 45F board's
	bool col_en, col_chunky;
	uint8_t col_np;					// planes - 1
	uint8_t col_off;				// planes 1..3: { dy, dx } each, bits 1:0, 3:2, 5:4
	uint16_t palette[256];				// RGB444
	uint16_t win_x0, win_y0, win_x1, win_y1;
	bool win_en, win_desk;
	uint32_t video_gen;				// bumped by every write that changes the picture's mapping
	bool palette_set;				// the power-on palette is in
	uint32_t shown_key;				// zsys_frame_dirty(): what the last frame was drawn with
	uint16_t cursor_rows[64];			// four slots of 16 rows

	// -- the two USB HID ports (rtl/usb_hid.v), IRQ 5 and 6 --
	zsys_hid_t hid[2];

	// -- audio (rtl/audio.v), IRQ 7, a level --
	zsys_audio_t audio;
	bool audio_on;					// FEATURES has AUDIO: the block is there

	// -- writing the flash (rtl/spiflash.v) --
	zsys_flashw_t flashw;
	int flash_fd;					// written through, unless a snapshot (-1)
	bool reboot;					// RECONFIG's key was written
	uint8_t *bios_copy; uint32_t bios_len;		// what a reconfiguration puts back

	// -- the RTC (rtl/rtc.v) --
	uint32_t rtc_sec, rtc_sub_pre;			// rtc_sec at rtc_cycles
	uint32_t rtc_sub_staged;			// SUB, waiting for the next SEC write
	uint64_t rtc_cycles;
	bool rtc_valid;
	bool rtc_host;					// set from the host's clock, at every reset
	int16_t rtc_tz;

	// -- which framebuffer rows have changed since zsys_dirty_take() --
	uint32_t dirty[ZSYS_ROWS / 32];

	// -- the LEDs, for what they are worth --
	uint32_t led, leds;

	// -- ZSYS_TB --
	uint32_t tb_result;

	// -- counts, for --stats --
	uint64_t irq_count[32], waitirqs, timers, reboots;

	// -- what was touched that is not there (each said once) --
	uint32_t unmapped_said;

} zsys_t;

// Make a machine. ZSYS_SOC: `ram_mb` of main memory; ZSYS_TB: 64KB.
bool zsys_init(zsys_t *s, zsys_kind_t kind, uint32_t ram_mb);
void zsys_free(zsys_t *s);

// The BIOS into BRAM (ZSYS_SOC), or a test program at 0 (ZSYS_TB).
bool zsys_load_bram(zsys_t *s, const uint8_t *data, uint32_t len);

// A flash image (docs/sim_parity.md, "Images"): a release's
// zeitlos-<board>.img, whose gateware region the emulator ignores, or
// a sim image, whose gateware region holds the BIOS instead -- returned
// in *bios / *bios_len when there is one (pointing into the image).
bool zsys_load_flash(zsys_t *s, const uint8_t *data, uint32_t len,
	const uint8_t **bios, uint32_t *bios_len);

// The CPU from reset: pc 0, sp at the top of BRAM, everything masked.
void zsys_reset(zsys_t *s);

// Run up to `cycles` clocks, or until something stops it. Returns
// ZSYS_* (ZSYS_LIMIT when the budget ran out).
int zsys_run(zsys_t *s, uint64_t cycles);

// A byte typed at the console. False if the FIFO is full.
bool zsys_uart_in(zsys_t *s, uint8_t c);

// -- the devices (zdev.c) --

// Power-on state of the GPU, socctl, the RTC (counting from 0, not
// valid) and the card's protocol state. zsys_reset() calls it.
void zdev_reset(zsys_t *s);

// An SD card image in the slot: `path` read whole, and written through
// as the machine writes it unless `snapshot` (then writes stay in
// memory). False, and the slot left empty, if it cannot be read or is
// smaller than 512KB.
bool zsys_sd_insert(zsys_t *s, const char *path, bool snapshot);

// Input, as the USB devices would send it. A key by its USB HID usage
// (SDL's scancodes are the same numbers; 0xE0-0xE7 are the modifiers):
// the keyboard's report changes and IRQ 5 pends. The mouse at an
// absolute position, buttons as HID numbers them (bit 0 left, 1 right,
// 2 middle): its report, with the movement since the last, and IRQ 6.
void zsys_key(zsys_t *s, uint8_t usage, bool down);
void zsys_mouse(zsys_t *s, int x, int y, uint8_t buttons);
// The wheel turned `notches`: positive away from the user (scroll up),
// the HID convention. Counted in the mouse register's top byte, as the
// USB host's mouse slot counts a wheel the descriptor confirmed, and
// reported on IRQ 6; wm sends the difference to the window.
void zsys_wheel(zsys_t *s, int notches);
// Where the hardware cursor is drawn: the mouse port's pointer.
static inline void zsys_pointer(const zsys_t *s, int *x, int *y) { *x = s->hid[1].cx; *y = s->hid[1].cy; }

// The flash image's file, to write the flash's erases and programs to
// (-1, the default: they stay in memory).
static inline void zsys_flash_file(zsys_t *s, int fd) { s->flash_fd = fd; }

// The same from memory: `img` (malloc'd, `size` bytes) is the card's
// now, freed with the machine. No file: writes stay in it, and each
// block written is marked in sd.written -- what a page keeps between
// visits (web/).
bool zsys_sd_insert_mem(zsys_t *s, uint8_t *img, uint64_t size);

// What the machine played since the last call, resampled to the host's
// rate: up to `max` stereo frames of floats (-1..1, left then right)
// into `out`. Returns how many. A front end calls it as its device asks;
// when the machine has played less than that, the rest is its to fill
// (silence), and when it has played more than ZSYS_AUDIO_OUT the oldest
// is gone.
uint32_t zsys_audio_take(zsys_t *s, float *out, uint32_t max, uint32_t out_hz);
// The machine's sample rate now, in Hz (FSDIV: CLK_HZ STEP / (128 MOD)).
double zsys_audio_hz(const zsys_t *s);
// Frames the machine has played that the host has not taken.
static inline uint32_t zsys_audio_pending(const zsys_t *s) { return s->audio.r_n; }

// internal: the sample clock, from zsys.c's clocks()
void zdev_audio_frames(zsys_t *s);
static inline void zsys_audio_clock(zsys_t *s, uint32_t n) {
	if (!s->audio_on) return;
	s->audio.acc += (uint64_t)n * s->audio.step;
	if (s->audio.acc >= (uint64_t)s->audio.mod * 128u) zdev_audio_frames(s);
}
bool zdev_audio_irq(const zsys_t *s);
uint64_t zdev_audio_next_frame(const zsys_t *s);	// cycles to the next frame

// The RTC set as though the board had a clock: the host's time, valid.
void zsys_rtc_from_host(zsys_t *s);

// The picture as the video output would send it: 640x480 RGB, three
// bytes a pixel, the phosphor's ink, paper's inversion and the
// hardware cursor (when `cursor`) at the mouse's pointer included.
void zsys_screen_rgb(const zsys_t *s, uint8_t *rgb, bool cursor);

// The rows a front end must draw again for this frame: the dirty rows,
// or all of them when the picture's mapping changed (the phosphor, the
// cursor's shape, game mode, the view, colour, the palette) or while
// game mode or the colour window is on -- then any VRAM row can be
// anywhere on the screen.
void zsys_frame_dirty(zsys_t *s, uint32_t rows[ZSYS_ROWS / 32]);

// One row of it, 0xAARRGGBB: what a window redraws, row by dirty row.
void zsys_render_row(const zsys_t *s, int y, uint32_t *argb, bool cursor);

// The same, written as a PNG. False on an I/O error.
bool zsys_screenshot(const zsys_t *s, const char *path, bool cursor);

// Rows changed since the last call, as bits (row r: bit r % 32 of word
// r / 32), into `rows`, and cleared. What a front end redraws.
void zsys_dirty_take(zsys_t *s, uint32_t rows[ZSYS_ROWS / 32]);

// -- internal: zsys.c <-> zdev.c --
uint32_t zdev_read(zsys_t *s, uint32_t pa, bool *ok);
bool zdev_write(zsys_t *s, uint32_t pa, uint32_t v, uint32_t sel);
static inline void zsys_vram_dirty(zsys_t *s, uint32_t off) {
	uint32_t row = (off % (ZSYS_STRIDE * ZSYS_ROWS)) / ZSYS_STRIDE;
	s->dirty[row >> 5] |= 1u << (row & 31);
}

// The sim image's mark, at the start of the gateware region.
#define ZSYS_IMG_MAGIC "ZSIMBIOS"

#endif
