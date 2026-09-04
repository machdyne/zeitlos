/*
 * mmod -- read, write and verify an SPI memory module in an MMOD
 * socket (github.com/machdyne/mmod).
 *
 * The console identify tool this grew out of is still the thing to
 * run first on a new board; see sw/apps/mmod/mmod.c's history and
 * docs/mmod.md. This is the working app.
 *
 * -- Layout, and why it is built the way it is --
 *
 * Every coordinate below comes from layout(), which walks a cursor
 * and records the frames it draws into strip-style rect_t values that
 * the draw functions then read. Nothing is placed with a literal
 * offset at a draw call.
 *
 * That is not style. sw/apps/logic's panel was laid out wrong twice --
 * once treating window coordinates as content coordinates, once with
 * widgets sitting across the frames they belonged inside -- and both
 * times its arithmetic test passed, because a geometry assertion only
 * checks the relationship somebody thought to write down.
 *
 * So this app is checked two ways. sw/apps/mmod/tests/test_layout.c
 * asserts the relationships (containment, overlap, window-vs-content).
 * sw/apps/mmod/tests/render.c DRAWS IT on the build machine and writes
 * a PNM, because rendering checks every relationship at once,
 * including the ones nobody anticipated. Run the second one and look
 * at it before changing anything here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zgpio.h"
#include "../../common/zspi.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"

// Content coordinates; the window is derived from them.
//
// z_win_content_rect() insets 2px per content edge and the titlebar
// takes Z_WM_TITLEBAR_H off the top, so a window is 4px wider and 15px
// taller than the area an app may draw in. Getting that backwards is
// what broke sw/apps/logic the first time.
#define CONTENT_W 576
#define CONTENT_H 232

#define WIN_W (CONTENT_W + 4)
#define WIN_H (CONTENT_H + Z_WM_TITLEBAR_H + 4)

#define MARGIN 6
#define FH     (z_font_5x8.h)
#define FW     (z_font_5x8.w)
#define LINE   (FH + 2)

#define ROW_H  18
#define BTN_H  13

static z_win_t win;

typedef struct { int16_t x, y, w, h; } rect_t;

// The three drawn frames. Recorded rather than computed at the draw
// call, so the layout test can assert widgets stay inside them.
static rect_t dev_f, file_f, prog_f;

// Row cursors, filled by layout() and read by draw_*().
static int16_t dev_lbl_x, dev_val_x, file_lbl_x, file_val_x;
static int16_t row_y[7];
static int16_t act_y, prog_bar_y, prog_txt_y;

// -- state ------------------------------------------------------

typedef enum { C_UNKNOWN = 0, C_NOR, C_FRAM, C_EEPROM } class_t;
static const char *class_name[] = { "?", "NOR", "FRAM", "EEPROM" };

// 2, 3 or 4 address bytes. Three, not two: a 32MB NOR (capacity 0x19,
// which is what the first module tested here reported) cannot be
// reached with 24 bits at all -- 3 bytes tops out at 16MB, and the
// upper half needs the 4-byte opcodes or EN4B. Defaulting to 3 on
// such a part silently accesses the bottom half while appearing to
// address the whole device, which on a write is data loss rather than
// a wrong reading.
static const uint8_t addr_widths[] = { 2, 3, 4 };
#define ADDR_COUNT 3
static int addr_idx = 1;

static class_t dev_class;
static uint8_t dev_id[3];
static uint32_t dev_size;
static uint32_t dev_page = 256;
static uint32_t dev_erase = 4096;
static bool dev_detected;
static bool ss_ok;

static uint32_t range_start, range_len;

// The name shown in the panel, and the full path operations use.
// Split because the FILE well is not wide enough for a path and the
// directory is not what anyone checks at a glance.
static char file_name[40];
static char file_path[160];
static uint32_t file_size;

static char status[96];
static char detail[96];
static int progress;			// -1 = idle, else 0..100

// Labels the widgets borrow. z_widget_t.label is read at draw time,
// so these have to stay valid and be ours to rewrite.
static char lbl_id[16], lbl_class[10], lbl_size[12], lbl_addr[10];
static char lbl_port[8];
static char lbl_page[14], lbl_erase[14], lbl_ss[10];
static char lbl_file[44], lbl_bytes[16], lbl_start[12], lbl_len[12];
static char lbl_range[40], lbl_backend[20], lbl_rate[20];

enum {
	W_PORT, W_DETECT,
	W_CLASS, W_SIZE, W_ADDR, W_PROBE,
	W_START, W_LEN, W_ALL,
	W_READ, W_WRITE, W_VERIFY, W_ERASE, W_CANCEL,
	W_COUNT
};

static z_widget_t widgets[W_COUNT];
static z_widget_set_t wset;

// -- layout -----------------------------------------------------

static int put_btn(int cx, int y, int idx, int w, int h) {
	widgets[idx].x = (int16_t)cx;
	widgets[idx].y = (int16_t)y;
	widgets[idx].w = (int16_t)w;
	widgets[idx].h = (int16_t)h;
	return cx + w + 4;
}

static void layout(void) {

	int i, cx;
	int col_w = (CONTENT_W - 2 * MARGIN - 8) / 2;
	int left_x = MARGIN;
	int right_x = MARGIN + col_w + 8;

	// Headings sit ABOVE the frames with clear air between, and every
	// control lives INSIDE a frame. Both of sw/apps/logic's layout
	// bugs were controls on a heading line drifting into the box
	// below, so there is no heading line here to drift into.
	int head_y = 2;
	int top = head_y + FH + 4;			// 14

	for (i = 0; i < 7; i++) row_y[i] = (int16_t)(top + 6 + i * ROW_H);

	dev_f.x = (int16_t)left_x;
	dev_f.y = (int16_t)top;
	dev_f.w = (int16_t)col_w;
	dev_f.h = (int16_t)(6 + 7 * ROW_H - (ROW_H - BTN_H) + 6);

	file_f.x = (int16_t)right_x;
	file_f.y = (int16_t)top;
	file_f.w = (int16_t)col_w;
	file_f.h = dev_f.h;

	// One grid for both columns: label at +6, value at +6 + 6 glyphs
	// + 8. Every well and button starts on that line, so the two
	// columns read as a table rather than as two piles.
	dev_lbl_x = (int16_t)(left_x + 6);
	dev_val_x = (int16_t)(left_x + 6 + 6 * FW + 8);
	file_lbl_x = (int16_t)(right_x + 6);
	file_val_x = (int16_t)(right_x + 6 + 6 * FW + 8);

	cx = put_btn(dev_val_x, row_y[0], W_PORT, 20, BTN_H);
	put_btn(cx, row_y[0], W_DETECT, 54, BTN_H);

	put_btn(dev_val_x, row_y[2], W_CLASS, 60, BTN_H);
	put_btn(dev_val_x, row_y[3], W_SIZE, 60, BTN_H);
	cx = put_btn(dev_val_x, row_y[4], W_ADDR, 60, BTN_H);
	put_btn(cx, row_y[4], W_PROBE, 46, BTN_H);

	put_btn(file_val_x, row_y[3], W_START, 76, BTN_H);
	cx = put_btn(file_val_x, row_y[4], W_LEN, 76, BTN_H);
	put_btn(cx, row_y[4], W_ALL, 34, BTN_H);

	act_y = (int16_t)(top + dev_f.h + 8);
	cx = MARGIN;
	cx = put_btn(cx, act_y, W_READ, 62, 19);
	cx = put_btn(cx, act_y, W_WRITE, 62, 19);
	cx = put_btn(cx, act_y, W_VERIFY, 62, 19);
	put_btn(cx, act_y, W_ERASE, 62, 19);
	put_btn(CONTENT_W - MARGIN - 62, act_y, W_CANCEL, 62, 19);

	prog_f.x = MARGIN;
	prog_f.y = (int16_t)(act_y + 19 + 8);
	prog_f.w = (int16_t)(CONTENT_W - 2 * MARGIN);
	prog_f.h = (int16_t)(CONTENT_H - MARGIN - prog_f.y);

	prog_bar_y = (int16_t)(prog_f.y + 6);
	prog_txt_y = (int16_t)(prog_bar_y + 13);

	z_widget_invalidate(&wset);

}

// -- labels -----------------------------------------------------

static void human_size(char *buf, size_t n, uint32_t bytes) {
	if (!bytes) snprintf(buf, n, "-");
	else if (bytes >= 1024u * 1024u)
		snprintf(buf, n, "%lu MB", (unsigned long)(bytes / (1024u * 1024u)));
	else snprintf(buf, n, "%lu KB", (unsigned long)(bytes / 1024u));
}

static void relabel(void) {

	int i;

	if (dev_detected)
		snprintf(lbl_id, sizeof(lbl_id), "%02x %02x %02x",
			dev_id[0], dev_id[1], dev_id[2]);
	else
		snprintf(lbl_id, sizeof(lbl_id), "--");

	snprintf(lbl_class, sizeof(lbl_class), "%s", class_name[dev_class]);
	human_size(lbl_size, sizeof(lbl_size), dev_size);
	snprintf(lbl_addr, sizeof(lbl_addr), "%d-byte", addr_widths[addr_idx]);
	snprintf(lbl_page, sizeof(lbl_page), "%lu", (unsigned long)dev_page);
	snprintf(lbl_erase, sizeof(lbl_erase), "%luK",
		(unsigned long)(dev_erase / 1024u));

	// The SS check is on the panel permanently, not in a log line: it
	// is the single thing standing between a marginal chip select and
	// a corrupted module, and it gates WRITE and ERASE below.
	snprintf(lbl_ss, sizeof(lbl_ss), "%s",
		!dev_detected ? "?" : (ss_ok ? "ok" : "FAILED"));

	snprintf(lbl_file, sizeof(lbl_file), "%s",
		file_name[0] ? file_name : "(none -- open or save icon)");
	if (file_name[0])
		snprintf(lbl_bytes, sizeof(lbl_bytes), "%lu",
			(unsigned long)file_size);
	else
		snprintf(lbl_bytes, sizeof(lbl_bytes), "-");

	snprintf(lbl_start, sizeof(lbl_start), "%08lx",
		(unsigned long)range_start);
	snprintf(lbl_len, sizeof(lbl_len), "%08lx", (unsigned long)range_len);

	snprintf(lbl_range, sizeof(lbl_range), "%08lx - %08lx",
		(unsigned long)range_start,
		(unsigned long)(range_start + (range_len ? range_len - 1 : 0)));

	// WRITE and ERASE are disabled until the device is identified AND
	// the SS check passed. An interlock, not a hint: if SS cannot
	// deassert, the module is permanently selected and a write lands
	// somewhere nobody chose.
	widgets[W_WRITE].enabled = dev_detected && ss_ok && file_name[0] != 0;
	widgets[W_ERASE].enabled = dev_detected && ss_ok && dev_class != C_FRAM;
	widgets[W_VERIFY].enabled = dev_detected && file_name[0] != 0;
	widgets[W_READ].enabled = dev_detected;
	widgets[W_CANCEL].enabled = (progress >= 0);

	for (i = 0; i < W_COUNT; i++) widgets[i].dirty = true;

}

// -- drawing ----------------------------------------------------

// z_win_hw_line() and z_win_hw_box() take ABSOLUTE SCREEN COORDINATES.
// Everything else an app draws with -- z_win_fill_rect(),
// z_win_draw_text(), zwidget.c -- is content-relative. zwin.h says so;
// it is easy to miss because the two families sit next to each other
// and their arguments look identical.
//
// Getting it wrong does not fail loudly: the frames land at the
// window's screen position instead of its content origin, so they are
// off by (2, Z_WM_TITLEBAR_H+2) when the window is at the top-left of
// the screen and off by the whole window position anywhere else. On a
// panel whose frames are meant to contain its controls, that is every
// box in the wrong place.
//
// So frames go through these two wrappers, which translate. Nothing in
// this file calls z_win_hw_* directly.
static void abs_box(int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	z_win_hw_box(&win, clip.x0 + x0, clip.y0 + y0,
		clip.x0 + x1, clip.y0 + y1, color);
}

static void abs_line(int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	z_win_hw_line(&win, clip.x0 + x0, clip.y0 + y0,
		clip.x0 + x1, clip.y0 + y1, color);
}

static void frame_raised(int x, int y, int w, int h) {
	abs_box(x, y, x + w - 1, y + h - 1, 1);
	abs_line(x + 1, y + h - 2, x + w - 2, y + h - 2, 1);
	abs_line(x + w - 2, y + 1, x + w - 2, y + h - 2, 1);
}

static void frame_well(int x, int y, int w, int h) {
	abs_box(x, y, x + w - 1, y + h - 1, 1);
	abs_line(x + 1, y + 1, x + w - 2, y + 1, 1);
	abs_line(x + 1, y + 1, x + 1, y + h - 2, 1);
}

// Row labels are centred on the row's control, not on its top edge.
static void label(int x, int y, const char *s) {
	z_win_draw_text(&win, x, y + (BTN_H - FH) / 2 + 1, s, 1, &z_font_5x8);
}

// A heading above a frame, with a rule running out to the frame's
// right edge.
static void heading(int x, int y, int w, const char *s) {
	int tw = (int)strlen(s) * FW;
	z_win_draw_text(&win, x, y, s, 1, &z_font_5x8);
	if (w > tw + 8)
		abs_line(x + tw + 5, y + FH / 2, x + w - 1, y + FH / 2, 1);
}

// A read-only value, in a recessed well so it reads as a display
// rather than as something to press.
// A read-only field, drawn on the same grid line a button at the same
// row_y would occupy -- so a well and a button in adjacent rows line
// up rather than being two pixels out.
//
// Draws its text DIRECTLY rather than through label(). label() applies
// the same vertical centring, so going through it applied the offset
// twice and pushed every well's contents down into the row below --
// which is precisely what the first render of this panel showed, and
// what no containment assertion would ever have caught.
static void value(int x, int y, int w, const char *s) {
	frame_well(x, y, w, BTN_H);
	z_win_draw_text(&win, x + 4, y + (BTN_H - FH) / 2 + 1, s, 1,
		&z_font_5x8);
}

static void draw_device(void) {

	heading(dev_f.x, 2, dev_f.w, "DEVICE");
	frame_raised(dev_f.x, dev_f.y, dev_f.w, dev_f.h);

	label(dev_lbl_x, row_y[0], "PORT");

	label(dev_lbl_x, row_y[1], "ID");
	value(dev_val_x, row_y[1], 84, lbl_id);

	label(dev_lbl_x, row_y[2], "CLASS");
	label(dev_lbl_x, row_y[3], "SIZE");
	label(dev_lbl_x, row_y[4], "ADDR");

	label(dev_lbl_x, row_y[5], "PAGE");
	value(dev_val_x, row_y[5], 46, lbl_page);
	label(dev_val_x + 54, row_y[5], "SECT");
	value(dev_val_x + 54 + 4 * FW + 6, row_y[5], 42, lbl_erase);

	label(dev_lbl_x, row_y[6], "SS");
	value(dev_val_x, row_y[6], 60, lbl_ss);

}

static void draw_file(void) {

	int wide = file_f.w - (file_val_x - file_f.x) - 8;

	heading(file_f.x, 2, file_f.w, "FILE / RANGE");
	frame_raised(file_f.x, file_f.y, file_f.w, file_f.h);

	label(file_lbl_x, row_y[0], "FILE");
	value(file_val_x, row_y[0], wide, lbl_file);

	label(file_lbl_x, row_y[1], "BYTES");
	value(file_val_x, row_y[1], 84, lbl_bytes);

	label(file_lbl_x, row_y[2], "RANGE");
	value(file_val_x, row_y[2], wide, lbl_range);

	label(file_lbl_x, row_y[3], "START");
	label(file_lbl_x, row_y[4], "LEN");

	// The two rows the range controls do not need carry what the
	// operation is actually doing -- which backend, and how fast it
	// is really going. Both are things you otherwise have to guess
	// at, and an 18x difference between backends is confusing to
	// debug silently.
	label(file_lbl_x, row_y[5], "VIA");
	value(file_val_x, row_y[5], 96, lbl_backend);

	label(file_lbl_x, row_y[6], "RATE");
	value(file_val_x, row_y[6], 96, lbl_rate);

}

static void draw_progress(void) {

	int inner_w = prog_f.w - 12;
	int fill;

	frame_well(prog_f.x, prog_f.y, prog_f.w, prog_f.h);

	// The bar is drawn as an empty trough always, so the well never
	// looks broken when idle -- an instrument with a missing element
	// reads as a fault.
	abs_box(prog_f.x + 6, prog_bar_y,
		prog_f.x + 6 + inner_w - 1, prog_bar_y + 8, 1);

	if (progress > 0) {
		fill = ((inner_w - 2) * progress) / 100;
		if (fill > 0)
			z_win_fill_rect(&win, prog_f.x + 7, prog_bar_y + 1,
				fill, 7, 1);
	}

	z_win_draw_text(&win, prog_f.x + 6, prog_txt_y, status, 1, &z_font_5x8);
	z_win_draw_text(&win, prog_f.x + 6, prog_txt_y + LINE, detail, 1,
		&z_font_5x8);

}

static void repaint(void) {
	z_win_clear(&win);
	draw_device();
	draw_file();
	draw_progress();
	z_widget_draw_all(&wset, true);
}

static void repaint_progress(void) {
	z_win_fill_rect(&win, prog_f.x + 1, prog_f.y + 1,
		prog_f.w - 2, prog_f.h - 2, 0);
	draw_progress();
}

// -- widgets ----------------------------------------------------

static void widgets_init(void) {

	int i;

	memset(widgets, 0, sizeof(widgets));

	for (i = 0; i < W_COUNT; i++) {
		widgets[i].type = Z_WIDGET_BUTTON;
		widgets[i].enabled = true;
	}

	widgets[W_PORT].label = lbl_port;
	widgets[W_DETECT].label = "DETECT";
	widgets[W_CLASS].label = lbl_class;
	widgets[W_SIZE].label = lbl_size;
	widgets[W_ADDR].label = lbl_addr;
	widgets[W_PROBE].label = "PROBE";
	widgets[W_START].label = lbl_start;
	widgets[W_LEN].label = lbl_len;
	widgets[W_ALL].label = "ALL";
	widgets[W_READ].label = "READ";
	widgets[W_WRITE].label = "WRITE";
	widgets[W_VERIFY].label = "VERIFY";
	// A plain button, not a two-press toggle: erase confirms through
	// a dialog that names the sector-snapped range it is about to
	// destroy, which a toggle cannot do.
	widgets[W_ERASE].label = "ERASE";
	widgets[W_CANCEL].label = "CANCEL";

	z_widget_set_init(&wset, widgets, W_COUNT, &win);

}

// Set up enough state that a render or a fresh start shows a sensible
// panel rather than a grid of dashes.
static void state_init(void) {
	snprintf(lbl_port, sizeof(lbl_port), "0");
	snprintf(lbl_backend, sizeof(lbl_backend), "bit-bang");
	snprintf(lbl_rate, sizeof(lbl_rate), "-");
	dev_class = C_UNKNOWN;
	dev_size = 0;
	range_start = 0;
	range_len = 0x10000;
	progress = -1;
	file_name[0] = 0;
	snprintf(status, sizeof(status),
		"Press DETECT to identify the module.");
	snprintf(detail, sizeof(detail),
		"WRITE and ERASE stay locked until the SS check passes.");
}
