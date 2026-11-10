/*
 * zsys in a page (docs/zsys.md, "In a browser"): the same machine,
 * built with Emscripten, driven by web/zsys.js.
 *
 * Not through SDL: the page owns the canvas, the keyboard, the mouse and
 * the network fetches anyway, so the C side is the machine and a few
 * calls -- run some milliseconds of the machine's time, take the dirty
 * rows as RGBA, give it a key or the pointer. No SDL port to download at
 * build time, and a smaller download for a visitor.
 *
 * Memory the page fills (the images) is malloc'd here (zw_alloc) and
 * handed over: the machine owns it from then on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "../zsys.h"

#define SYSCLK_HZ 48000000u

static zsys_t s;
static uint32_t rgba[ZSYS_SCREEN_W * ZSYS_SCREEN_H];	// what the canvas shows, as ImageData wants it
static int dirty_lo, dirty_hi;

// the console, to the page, a line at a time
EM_JS(void, js_console, (const char *line), { if (Module.onConsole) Module.onConsole(UTF8ToString(line)); });
static char line[512];
static size_t line_n;
static void uart_out(zsys_t *m, uint8_t c) {
	(void)m;
	if (c == '\r') return;
	if (c != '\n' && line_n < sizeof(line) - 1) { line[line_n++] = (char)c; return; }
	line[line_n] = 0;
	js_console(line);
	line_n = 0;
}

EMSCRIPTEN_KEEPALIVE void *zw_alloc(uint32_t n) { return malloc(n ? n : 1); }

// The board, as zsys's main.c builds it (a Lakritz without its USB
// host), from a sim image (flash) and, if card_len, a card image.
// 0 when it is running; otherwise what was wrong.
EMSCRIPTEN_KEEPALIVE int zw_init(uint8_t *flash, uint32_t flash_len, uint8_t *card, uint32_t card_len,
		int host_clock) {
	if (!zsys_init(&s, ZSYS_SOC, 32)) return 1;
	s.uart_out = uart_out;
	s.features = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
		(1u << 9) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 20) | (1u << 21) | (1u << 23) | (1u << 24) |
		(1u << 26) | (1u << 27);		// ... AUDIO, GAME
	s.audio_on = true;
	s.t2_on = true;					// T2, as sergei_ml2's build has it
	s.features2 = (1u << 5) | (1u << 6) | (1u << 15);	// RECONFIG, FLASHW, COLOR
	const uint8_t *bios;
	uint32_t bios_len;
	if (!zsys_load_flash(&s, flash, flash_len, &bios, &bios_len)) return 2;
	if (!bios || !zsys_load_bram(&s, bios, bios_len)) return 3;	// a sim image has its BIOS in it
	free(flash);
	if (card_len && !zsys_sd_insert_mem(&s, card, card_len)) return 4;
	s.rtc_host = host_clock != 0;
	zsys_reset(&s);
	return 0;
}

// Run `ms` of the machine's time. Returns how many of the board's
// instructions that was, for the page's speed meter.
EMSCRIPTEN_KEEPALIVE uint32_t zw_run(uint32_t ms) {
	uint64_t i = s.instret;
	zsys_run(&s, (uint64_t)ms * (SYSCLK_HZ / 1000));
	if (s.stop == ZSYS_TRAPPED) {
		char why[160];
		snprintf(why, sizeof(why), "zsys: the core halted at %08lx: %s", (unsigned long)s.trap_pc, s.trap_why);
		js_console(why);
	}
	return (uint32_t)(s.instret - i);
}

EMSCRIPTEN_KEEPALIVE int zw_halted(void) { return s.stop == ZSYS_TRAPPED; }

// The rows that changed into the RGBA buffer; then zw_dirty_lo/hi say
// which (lo > hi: none) and zw_rgba() where they are.
EMSCRIPTEN_KEEPALIVE void zw_render(void) {
	uint32_t rows[ZSYS_ROWS / 32], argb[ZSYS_SCREEN_W];
	zsys_frame_dirty(&s, rows);
	dirty_lo = ZSYS_SCREEN_H; dirty_hi = -1;
	for (int y = 0; y < ZSYS_SCREEN_H; y++) {
		if (!(rows[y >> 5] & (1u << (y & 31)))) continue;
		zsys_render_row(&s, y, argb, true);
		uint32_t *o = rgba + y * ZSYS_SCREEN_W;
		for (int x = 0; x < ZSYS_SCREEN_W; x++) {	// 0xAARRGGBB to the bytes R, G, B, A
			uint32_t c = argb[x];
			o[x] = (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16);
		}
		if (y < dirty_lo) dirty_lo = y;
		dirty_hi = y;
	}
}
EMSCRIPTEN_KEEPALIVE uint32_t *zw_rgba(void) { return rgba; }
EMSCRIPTEN_KEEPALIVE int zw_dirty_lo(void) { return dirty_lo; }
EMSCRIPTEN_KEEPALIVE int zw_dirty_hi(void) { return dirty_hi; }

EMSCRIPTEN_KEEPALIVE void zw_key(int usage, int down) { if (usage > 0 && usage <= 0xe7) zsys_key(&s, (uint8_t)usage, down != 0); }
EMSCRIPTEN_KEEPALIVE void zw_mouse(int x, int y, int buttons) { zsys_mouse(&s, x, y, (uint8_t)buttons); }
// What the machine played, at the page's rate: up to `max` frames of
// interleaved stereo floats into `out`. How many.
EMSCRIPTEN_KEEPALIVE uint32_t zw_audio_take(float *out, uint32_t max, uint32_t hz) {
	return zsys_audio_take(&s, out, max, hz);
}

EMSCRIPTEN_KEEPALIVE void zw_wheel(int notches) { zsys_wheel(&s, notches); }
EMSCRIPTEN_KEEPALIVE int zw_uart_in(int c) { return zsys_uart_in(&s, (uint8_t)c); }

// What the page keeps between visits, if its owner chooses: the card's
// blocks written since the last zw_card_taken() and the flash (the
// key/value store) when it has been written.
EMSCRIPTEN_KEEPALIVE uint8_t *zw_card(void) { return s.sd.img; }
EMSCRIPTEN_KEEPALIVE uint8_t *zw_card_written(void) { return s.sd.written; }
EMSCRIPTEN_KEEPALIVE uint32_t zw_card_blocks(void) { return (uint32_t)(s.sd.size / 512); }
EMSCRIPTEN_KEEPALIVE uint8_t *zw_flash(void) { return s.flash; }
EMSCRIPTEN_KEEPALIVE uint32_t zw_flash_size(void) { return s.flash_size; }
EMSCRIPTEN_KEEPALIVE uint32_t zw_flash_writes(void) { return (uint32_t)(s.flashw.erases + s.flashw.programs); }

int main(void) { return 0; }
