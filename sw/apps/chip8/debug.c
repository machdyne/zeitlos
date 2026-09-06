/*
 * chip8 -- the debugger pane. See debug.h.
 */

#include <stdio.h>
#include <string.h>

#include "../../common/zfont.h"
#include "../../common/zgfx.h"

#include "debug.h"
#include "disasm.h"

#define LINE_H 8            /* z_font_5x8 */
#define CHAR_W 5

/* What is currently on screen, one padded row per line. Diffed against
 * the newly formatted line so only changed cells are redrawn -- see
 * debug.h for why nothing is ever blanked first. */
static char shown[C8_DBG_LINES][C8_DBG_COLS + 1];
static bool shown_valid;

void c8_debug_invalidate(void) {
	shown_valid = false;
}

/* Format into a fixed-width, space-padded row.
 *
 * The padding is load-bearing, not tidiness: a line that gets shorter
 * (a four-byte "i := long 0x1234" replaced by "clear") has to overwrite
 * its own tail, and trailing spaces are what does that. Without them
 * the leftover reads as a garbled instruction rather than as stale
 * pixels, which is a much more confusing thing to look at in a
 * debugger. */
static void row(char *dst, const char *src) {

	int i = 0;

	while (src[i] && i < C8_DBG_COLS) { dst[i] = src[i]; i++; }
	while (i < C8_DBG_COLS) dst[i++] = ' ';

	dst[C8_DBG_COLS] = '\0';

}

/* Draw only the cells that differ, as runs.
 *
 * Runs rather than per character because each z_win_draw_text() call
 * carries a fixed setup cost; a register whose two hex digits both
 * changed is one call, not two. */
static void flush_line(const z_win_t *win, int x, int y, int n,
	const char *want, bool force) {

	char *have = shown[n];
	int i = 0;
	int ly = y + n * LINE_H;

	if (force) {
		z_win_draw_text(win, x, ly, want, 1, &z_font_5x8);
		memcpy(have, want, C8_DBG_COLS + 1);
		return;
	}

	while (i < C8_DBG_COLS) {

		int start, len;
		char seg[C8_DBG_COLS + 1];

		if (want[i] == have[i]) { i++; continue; }

		start = i;
		while (i < C8_DBG_COLS && want[i] != have[i]) i++;
		len = i - start;

		memcpy(seg, want + start, (size_t)len);
		seg[len] = '\0';

		z_win_draw_text(win, x + start * CHAR_W, ly, seg, 1, &z_font_5x8);
		memcpy(have + start, want + start, (size_t)len);

	}

}

void c8_debug_draw(const z_win_t *win, int x, int y, const c8_t *vm,
	bool paused, int ipf, const char *profile_name, bool force) {

	char buf[96];
	char want[C8_DBG_COLS + 1];
	char dis[C8_DISASM_MAX];
	uint16_t pc;
	int i, n = 0;

	if (!shown_valid) {
		force = true;
		shown_valid = true;
	}

	#define LINE(fmt, ...) do { \
		snprintf(buf, sizeof(buf), fmt, __VA_ARGS__); \
		row(want, buf); \
		flush_line(win, x, y, n++, want, force); \
	} while (0)

	LINE("PC %04X I %04X SP %X DT %02X ST %02X %s",
		vm->pc, vm->i, vm->sp, vm->dt, vm->st,
		vm->halted ? "HALT" : paused ? "PAUSE" : "RUN");

	LINE("V0-7 %02X %02X %02X %02X %02X %02X %02X %02X",
		vm->v[0], vm->v[1], vm->v[2], vm->v[3],
		vm->v[4], vm->v[5], vm->v[6], vm->v[7]);

	LINE("V8-F %02X %02X %02X %02X %02X %02X %02X %02X",
		vm->v[8], vm->v[9], vm->v[10], vm->v[11],
		vm->v[12], vm->v[13], vm->v[14], vm->v[15]);

	/* Disassembly forward from PC.
	 *
	 * Forward only, and no attempt to show what came BEFORE it.
	 * CHIP-8 instructions are not all the same length (XO-CHIP's long
	 * load is four bytes) and code and data share one address space,
	 * so there is no way to walk backwards that is right more often
	 * than it is wrong. A listing that is confidently wrong above the
	 * cursor is worse than no listing at all. */
	pc = vm->pc;
	for (i = 0; i < C8_DBG_DIS_LINES; i++) {

		uint16_t a = (uint16_t)(pc & vm->q.addr_mask);
		uint16_t op = (uint16_t)((vm->ram[a] << 8) |
			vm->ram[(uint16_t)(a + 1) & vm->q.addr_mask]);
		int len = c8_disasm(vm->ram, pc, vm->q.addr_mask, dis, sizeof(dis));

		LINE("%c %04X %04X %s", i == 0 ? '>' : ' ', a, op, dis);

		pc = (uint16_t)(pc + len);

	}

	LINE("%s %d/f  plane %d  %s", profile_name, ipf, vm->plane_mask,
		vm->waiting ? "wait key" : vm->wait_frame ? "wait frame" : "");

	if (vm->halted)
		LINE("%s", "exited (00FD).  F5 resets, ESC quits");
	else
		LINE("%s", "F10 panel  F11 step  F12 run/pause  ESC quit");

	#undef LINE

}
