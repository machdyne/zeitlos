/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The rescue console -- see rescue.h and docs/rescue_console.md.
 *
 * A terminal of 80x60 characters drawn straight into VRAM: z_font_8x8,
 * one byte store per glyph row (as the panic screen's 5x8 font is,
 * uart.c), the blitter to scroll and clear. What it shows is the
 * console log (uart.c), so it needs no output path of its own: every
 * byte the serial console gets is logged, and pid 0 draws the log from
 * where it last stopped. Drawing is pid 0's alone, and never in an
 * interrupt, so it needs no lock and never holds IRQs off for a scroll.
 * Keys from a USB keyboard are turned into characters here (US layout,
 * enough for a shell) and put into the serial receive ring, so the
 * shell reads them as if typed on the serial console.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rescue.h"
#include "../common/zfont.h"

#ifndef RESCUE_HOST_TEST
#include <stdio.h>
#include "../common/zeitlos.h"
#include "../common/zsoc.h"
#include "../common/zconsole.h"
#include "../common/zkbd.h"
#include "kernel.h"
#include "uart.h"
#include "pidreg.h"
#include "hid.h"
#define R_VRAM		((volatile uint8_t *)0x20000000)
#define R_VRAM_PHYS	0x20000000u
#endif

#define R_W			640
#define R_H			480
#define R_STRIDE	(R_W / 8)		// bytes per scanline
#define R_COLS		80				// 40 x 30 on composite video
#define R_ROWS		60
#define R_BACK		2400			// log shown on entering: ~a screenful

// The boot window (rescue_boot_poll()).
#define R_ESC		0x29			// HID usage of ESC
#define R_SETTLE	(Z_TICK_HZ * 4 / 10)	// a port enabled -> its keyboard set up
#define R_WAIT_MAX	(Z_TICK_HZ * 5 / 2)	// never past this
#define R_PORTSTAT	(*(volatile uint32_t *)0xc0000104u)	// usbh_hw.h, Z_USBH_PORTSTAT
#define R_PS_CONN	1u
#define R_PS_EN		2u

volatile bool k_rescue_on;
static int r_cols = R_COLS, r_rows = R_ROWS;	// the grid on screen
static int r_col, r_row;
static bool r_cur;				// the cursor is drawn
static uint8_t r_esc;			// 0, 1 after ESC, 2 in a CSI sequence
static uint8_t r_np;			// the CSI parameter being read (0 or 1)
static uint16_t r_par[2];

// -- the screen --

#ifndef RESCUE_HOST_TEST
static bool r_stuck;	// the blitter never went idle: stop drawing

static void r_blit_idle(void) {
	uint32_t n = 0;
	while ((gpu_blit_status & 1) && !r_stuck)
		if (++n > 10000000u) r_stuck = true;
}

// IRQs masked from the idle check through START, as zgfx.c's
// gpu_blit_acquire() does, so a program still drawing (wm has died, its
// clients have not) cannot program the blitter between our writes.
// Returns the mask for r_blit_go().
static uint32_t r_blit_begin(int x, int y, int w, int h) {
	uint32_t m;
	for (;;) {
		r_blit_idle();
		m = maskirq(0xFFFFFFFF);
		if (!(gpu_blit_status & 1) || r_stuck) break;
		maskirq(m);
	}
	gpu_blit_clip_x0 = 0;
	gpu_blit_clip_y0 = 0;
	gpu_blit_clip_x1 = R_W;
	gpu_blit_clip_y1 = R_H;
	gpu_blit_dst_x = (uint32_t)x;
	gpu_blit_dst_y = (uint32_t)y;
	gpu_blit_width = (uint32_t)w;
	gpu_blit_height = (uint32_t)h;
	return m;
}

static void r_blit_go(uint32_t ctrl, uint32_t m) {
	gpu_blit_ctrl = ctrl;
	maskirq(m);
	r_blit_idle();
}

// Scanlines y..y+h-1 to black.
static void r_clear(int y, int h) {
	if (h <= 0) return;
	uint32_t m = r_blit_begin(0, y, R_W, h);
	gpu_blit_pattern = 0;
	r_blit_go(GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL | GPU_BLIT_CTRL_CLIP, m);
}

// Everything up one text row; the bottom row cleared. Content moving up
// is one safe copy (zgfx.h, z_fb_hw_scroll()).
static void r_scroll(void) {
	uint32_t m = r_blit_begin(0, 0, R_W, r_rows * 8 - 8);
	gpu_blit_src_addr = R_VRAM_PHYS + 8u * R_STRIDE;
	gpu_blit_src_stride = R_STRIDE;
	gpu_blit_src_shift = 0;
	r_blit_go(GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_CLIP, m);	// copy, from VRAM
	r_clear(r_rows * 8 - 8, 8);
}
#endif

// The framebuffer's leftmost pixel is its lowest bit; the font's is its
// highest (uart.c, k_rev8()).
static uint8_t r_rev8(uint8_t b) {
	b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
	b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
	return (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
}

static void r_glyph(int col, int row, uint8_t c) {
	const z_font_t *f = &z_font_8x8;	// full cells: 5x8 left 3px gaps, hard to read
	int gi = z_font_index(f, c);
	if (gi < 0) gi = z_font_index(f, '?');
	const uint8_t *g = f->glyphs + gi * f->h;
	volatile uint8_t *p = R_VRAM + row * 8 * R_STRIDE + col;
	for (int j = 0; j < 8; j++) p[j * R_STRIDE] = r_rev8(g[j]);
}

// An underline in the cell; drawing it twice takes it away.
static void r_cursor(void) {
	int col = r_col < r_cols ? r_col : r_cols - 1;
	R_VRAM[(r_row * 8 + 7) * R_STRIDE + col] ^= 0xFF;
	r_cur = !r_cur;
}

static void r_clear_eol(void) {
	for (int j = 0; j < 8; j++)
		for (int x = r_col; x < r_cols; x++)
			R_VRAM[(r_row * 8 + j) * R_STRIDE + x] = 0;
}

static void r_newline(void) {
	r_col = 0;
	if (++r_row >= r_rows) {
		r_scroll();
		r_row = r_rows - 1;
	}
}

static int r_clamp(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

// The VT100 subset the kernel's own output uses (zeitlos.h, VT100_*):
// cursor moves, home, erase line and screen. Anything else is dropped.
static void r_csi(uint8_t c) {
	int n = r_par[0] ? r_par[0] : 1;
	switch (c) {
	case 'A': r_row = r_clamp(r_row - n, 0, r_rows - 1); break;
	case 'B': r_row = r_clamp(r_row + n, 0, r_rows - 1); break;
	case 'C': r_col = r_clamp(r_col + n, 0, r_cols - 1); break;
	case 'D': r_col = r_clamp((r_col < r_cols ? r_col : r_cols - 1) - n, 0, r_cols - 1); break;
	case 'E': r_newline(); break;
	case 'H': case 'f':
		r_row = r_clamp(r_par[0] ? r_par[0] - 1 : 0, 0, r_rows - 1);
		r_col = r_clamp(r_par[1] ? r_par[1] - 1 : 0, 0, r_cols - 1);
		break;
	case 'K': if (r_col < r_cols) r_clear_eol(); break;
	case 'J':
		if (r_par[0] == 2) { r_clear(0, r_rows * 8); break; }
		if (r_col < r_cols) r_clear_eol();
		r_clear((r_row + 1) * 8, (r_rows - 1 - r_row) * 8);
		break;
	default: break;
	}
}

static void r_put(uint8_t c) {
	if (r_esc == 1) {
		r_esc = 0;
		if (c == '[') {
			r_esc = 2;
			r_np = 0;
			r_par[0] = r_par[1] = 0;
		}
		return;
	}
	if (r_esc == 2) {
		if (c >= '0' && c <= '9') {
			if (r_par[r_np] < 1000) r_par[r_np] = (uint16_t)(r_par[r_np] * 10 + (c - '0'));
		} else if (c == ';') {
			r_np = 1;
		} else if (c >= 0x40 && c <= 0x7e) {
			r_esc = 0;
			r_csi(c);
		}
		return;
	}
	switch (c) {
	case 0x1b: r_esc = 1; return;
	case '\n': r_newline(); return;
	case '\r': r_col = 0; return;
	case '\b': if (r_col > 0) r_col = (r_col < r_cols ? r_col : r_cols) - 1; return;
	case '\t':
		r_col = (r_col + 8) & ~7;
		if (r_col > r_cols) r_col = r_cols;
		return;
	default: break;
	}
	if (c < 0x20 || c == 0x7f) return;
	if (r_col >= r_cols) r_newline();
	r_glyph(r_col++, r_row, c);
}

// -- keys --

// HID usages 0x04-0x38 (USB HID Usage Tables, 10), unshifted and
// shifted; 0x32 is the non-US key next to Enter.
static const char r_lo[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\#;'`,./";
static const char r_hi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|~:\"~<>?";

// The character a key press makes, or 0.
static int r_ascii(uint8_t usage, uint8_t mods, bool caps) {
	if (usage == 0x4c) return 0x7f;				// Delete
	if (usage < 0x04 || usage > 0x38) return 0;
	int i = usage - 0x04;
	bool shift = (mods & 0x22) != 0;			// either Shift
	bool letter = i < 26;
	if (letter && caps) shift = !shift;
	int c = (uint8_t)(shift ? r_hi[i] : r_lo[i]);
	if ((mods & 0x11) && letter) c &= 0x1f;		// either Ctrl
	if (c == 0x1b) return 0;	// ESC: what brought the console up; the shell has no use for it
	return c;
}

#ifndef RESCUE_HOST_TEST

// -- hooks --

static volatile bool r_dirty;	// output logged and not drawn yet
static volatile bool r_fresh;	// entered; the screen not set up yet
static uint32_t r_pos;			// next log byte to draw
static bool r_skip;				// drop the partial first line
static bool r_busy;

int rescue_boot_poll(uint32_t elapsed) {
	static bool seen_en[2];
	static uint32_t settle;

	if (!z_soc_has_feature(Z_FEATURE_GPU)) return RESCUE_POLL_NO;
	bool usb_host = z_soc_has_feature2(Z_FEATURE2_USB_HOST);
	if (!usb_host && !z_soc_has_feature(Z_FEATURE_USB_HID)) return RESCUE_POLL_NO;

	// The key registers hold what is down now, so a held key is simply
	// there: no events, no repeat.
	bool kbd = false;
	for (int p = 0; p < 2; p++) {
		uint32_t info = p ? reg_usb1_info : reg_usb0_info;
		uint32_t keys = p ? reg_usb1_keys : reg_usb0_keys;
		if (((info >> 24) & 3) != 1) continue;
		kbd = true;
		for (int k = 0; k < 4; k++)
			if (((keys >> (k * 8)) & 0xff) == R_ESC) return RESCUE_POLL_ESC;
	}

	// The USB_HID controller sets a keyboard up by itself, long before
	// this. Behind the USB host, `usb` does, and may not have yet: a
	// device on a root port that is not enabled is still being reset,
	// and once it is, its keyboard takes a moment more. A keyboard
	// already there, or nothing plugged in, ends the window on time.
	if (!usb_host || kbd || elapsed >= R_WAIT_MAX) return RESCUE_POLL_NO;
	uint32_t ps = R_PORTSTAT;
	for (int p = 0; p < 2; p++) {
		uint32_t s = (ps >> (p * 8)) & 0xff;
		if (!(s & R_PS_CONN)) continue;
		if (!(s & R_PS_EN)) return RESCUE_POLL_WAIT;
		if (!seen_en[p]) {
			seen_en[p] = true;
			if (elapsed + R_SETTLE > settle) settle = elapsed + R_SETTLE;
		}
	}
	return elapsed < settle ? RESCUE_POLL_WAIT : RESCUE_POLL_NO;
}

// Asked for by wm (Ctrl+Alt+Esc, Z_SYS_RESCUE): wm has hidden its
// windows and is waiting to put them back. Ctrl+Alt+Esc, Ctrl+Alt+Del
// (wm's reboot dialog) and every key release then go on to wm rather
// than to the shell; `exit` asks wm too.
static bool r_by_wm;

static void r_enter(const char *why, bool by_wm) {
	if (k_rescue_on || !z_soc_has_feature(Z_FEATURE_GPU)) return;
	uint32_t end = k_klog_end();
	r_pos = end > R_BACK ? end - R_BACK : 0;
	r_skip = r_pos != 0;
	r_fresh = true;
	r_by_wm = by_wm;
	// ESC's press, held to get here, is not for a wm that starts later.
	// One that asked keeps its keys: it is waiting for their releases.
	if (!by_wm) k_hid_flush();
	k_rescue_on = true;
	printf("rescue: %s\n", why);
	uint32_t old = maskirq(0xFFFFFFFF);
	r_dirty = true;
	k_uart_wake_waiter();		// pid 0, if it waits for input, draws
	maskirq(old);
}

void rescue_enter(const char *why) {
	r_enter(why, false);
}

bool rescue_by_wm(void) {
	return k_rescue_on && r_by_wm;
}

// Z_SYS_RESCUE: Z_UINT32 1 on, 0 off -- wm's, around hiding and
// redrawing its windows.
z_obj_t *k_rescue(z_obj_t *args) {
	if (!args || args->type != Z_UINT32) return &z_fail;
	if (args->val.uint32) {
		r_enter("Ctrl+Alt+Esc -- Ctrl+Alt+Esc again, or `exit`, returns to the desktop",
			true);
		return k_rescue_on ? &z_ok : &z_fail;
	}
	if (k_rescue_on) {
		k_rescue_on = false;		// wm redraws everything from here
		r_by_wm = false;
	}
	return &z_ok;
}

static bool r_is_wm(const char *name) {
	if (!name) return false;
	const char *b = strrchr(name, '/');
	b = b ? b + 1 : name;
	return !strcmp(b, "wm") || !strcmp(b, "wm0");
}

// wm is known by the pid it was created as, not only by its registered
// name: a broken wm can crash before main() registers "wm0".
static uint32_t r_wm_pid;		// 0: none (pid 0 is the kernel)

void rescue_exec(const char *name, uint32_t pid) {
	if (!r_is_wm(name)) {
		if (pid == r_wm_pid) r_wm_pid = 0;	// the slot was reused
		return;
	}
	r_wm_pid = pid;
	if (k_rescue_on) {
		k_rescue_on = false;	// wm clears the screen and takes it
		r_by_wm = false;
		printf("rescue: handing the screen to wm\n");
	}
}

void rescue_ended(uint32_t pid) {
	bool wm = pid == r_wm_pid || r_is_wm(k_pidreg_name_for(pid));
	if (pid == r_wm_pid) r_wm_pid = 0;
	if (wm) r_by_wm = false;	// on already: there is no desktop to go back to
	if (wm && !k_rescue_on) rescue_enter("wm ended -- `run wm` starts it again");
}

void rescue_note(void) {
	r_dirty = true;
	if (z_pid != 0 || k_uart_polled) k_uart_wake_waiter();
}

bool rescue_pending(void) {
	return k_rescue_on && r_dirty && !r_stuck;
}

static void r_setup(void) {
	// Composite video shows 320x240 of the framebuffer, always (zsoc.h):
	// a 40x30 grid in its top left corner. Elsewhere game mode off, so
	// the whole 640x480 is shown.
	if (z_video_is_composite()) {
		r_cols = R_COLS / 2;
		r_rows = R_ROWS / 2;
		z_game_set_view(0, 0);
	} else {
		r_cols = R_COLS;
		r_rows = R_ROWS;
		if (z_game_present()) reg_socctl_game = 0;
	}
	r_clear(0, R_H);
	r_col = r_row = 0;
	r_cur = false;
	r_esc = 0;
}

// One chunk of the log a call: pid 0 waiting for input goes back to look
// at the keyboard between chunks (uart.c, k_uart_wait_rx()), so output
// from a busy process cannot keep it from reading what is typed.
void rescue_pump(void) {
	static uint8_t buf[128];
	if (!k_rescue_on || r_busy || k_uart_polled || z_pid != 0 || r_stuck) return;
	// Not with the timer masked (pid 0 printing inside a critical
	// section): a scroll must not hold interrupts off. Drawn later.
	uint32_t m = maskirq(0xFFFFFFFF);
	maskirq(m);
	if (m & (1u << Z_IRQ_KTIMER)) return;
	r_busy = true;
	if (r_fresh) {
		r_fresh = false;
		r_setup();
	}
	if (r_cur) r_cursor();
	r_dirty = false;	// first: a byte logged meanwhile sets it again
	z_klog_args_t a;
	a.pos = r_pos;
	a.buf = buf;
	a.len = sizeof(buf);
	a.n = 0;
	a.lost = 0;
	k_klog_read((z_obj_t *)&a);
	r_pos = a.pos;
	for (uint32_t i = 0; i < a.n && k_rescue_on; i++) {
		if (r_skip) {
			if (buf[i] == '\n') r_skip = false;
			continue;
		}
		r_put(buf[i]);
	}
	if (r_pos != k_klog_end()) r_dirty = true;	// more: the next call
	if (k_rescue_on) r_cursor();
	r_busy = false;
}

// Ctrl (either) + Alt (either) + ESC, or + Delete (the reboot dialog,
// wm's too), as a press.
static bool r_hotkey(uint32_t ev) {
	uint8_t mods = (uint8_t)(ev >> 9), u = (uint8_t)(ev >> 1);
	return (ev & 1) && (u == R_ESC || u == 0x4C || u == 0x63) &&
		(mods & 0x11) && (mods & 0x44);
}

bool rescue_key(uint32_t ev) {
	if (!k_rescue_on) return false;
	// For the wm that is waiting to come back: its hotkey, and every
	// release, so it does not believe a key held when it hid is still
	// down. Neither types anything here.
	if (r_by_wm && (r_hotkey(ev) || !(ev & 1))) return false;
	// With no desktop behind it there is no dialog to ask with.
	if (r_hotkey(ev) && (uint8_t)(ev >> 1) != R_ESC) {
		printf("\nCtrl+Alt+Del: type `reboot` to restart\n");
		return true;
	}
	if (ev & 1) {
		int c = r_ascii((uint8_t)(ev >> 1), (uint8_t)(ev >> 9),
			((ev >> 17) & Z_KBD_LOCK_CAPS) != 0);
		// Bit 25: injected by a program, not typed (hid.c) -- marked so,
		// as console0's input is (`passwd reset` wants a person).
		if (c > 0) k_uart_rx_put((uint8_t)c, (ev >> 25) & 1);
	}
	return true;
}

#endif
