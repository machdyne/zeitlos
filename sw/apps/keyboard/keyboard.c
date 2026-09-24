/*
 * keyboard -- an on-screen keyboard, for any layout
 *
 *   > run wm
 *   > run keyboard
 *
 * Two jobs: a way to SEE a keyboard layout -- every key labelled with
 * what it types, Shift and AltGr included -- and try it; and a keyboard
 * for a machine with a pointer and no keyboard at all (a touchscreen).
 * docs/keyboard_app.md.
 *
 * -- where the keys go --
 *
 * A key is sent the way a physical one arrives: as a raw USB HID event,
 * a press and a release, through hid_inject() -- the path the ESP32
 * remote desktop already uses. The kernel stamps the active layout into
 * it and wm translates it like any other key. So everything a real
 * keyboard gets, this gets: the layout, dead keys, AltGr, Super+Space,
 * Japanese input, wm's own shortcuts. And the keys go where typed keys
 * always go -- the focused window. The keyboard's own window is
 * Z_WIN_FLAG_NO_FOCUS (zwm.h): tapping it never takes the focus from the
 * window you are typing into.
 *
 * -- labels --
 *
 * Each key is labelled by z_kbd_translate() with the layout, the
 * modifiers set and Caps Lock -- the same call wm makes, from the same
 * generated tables, so the labels cannot disagree with what the key
 * types. No table here knows any layout.
 *
 * -- modifiers --
 *
 * Shift, Ctrl, Alt, AltGr and Super are sticky: tap one, and it holds
 * for the next key, then lets go. Caps Lock stays until tapped again.
 * The Lay key steps through EVERY layout (not just the configured ones)
 * and makes it the active one, for testing.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"	// Z_TICK_HZ
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zutf8.h"

// -- the keys --
//
// Widths in quarters of a key; every row is 60 quarters. A key is a USB
// usage, a modifier bit, or an action.

#define Q		5			// pixels per quarter: a plain key is 20px
#define KEY_H	18
#define MARGIN	2
#define ROWS	5
#define WIN_W	(60 * Q + 2 * MARGIN + 4)
// The window's size includes wm's titlebar and border; the content is
// what is left inside them (z_win_content_rect()).
#define STATUS_H	12
#define WIN_H	(Z_WM_TITLEBAR_H + ROWS * KEY_H + STATUS_H + 2 * MARGIN + 6)

enum { K_KEY, K_MOD, K_CAPS, K_LAYOUT };

typedef struct {
	uint8_t	usage;		// K_KEY: the USB usage; K_MOD: the modifier bit
	uint8_t	w;			// quarters
	uint8_t	kind;
} kbkey_t;

static const kbkey_t rows[ROWS][16] = {
	{ {0x35,4}, {0x1E,4}, {0x1F,4}, {0x20,4}, {0x21,4}, {0x22,4}, {0x23,4},
	  {0x24,4}, {0x25,4}, {0x26,4}, {0x27,4}, {0x2D,4}, {0x2E,4}, {0x2A,8} },
	{ {0x2B,6}, {0x14,4}, {0x1A,4}, {0x08,4}, {0x15,4}, {0x17,4}, {0x1C,4},
	  {0x18,4}, {0x0C,4}, {0x12,4}, {0x13,4}, {0x2F,4}, {0x30,4}, {0x31,6} },
	{ {0,7,K_CAPS}, {0x04,4}, {0x16,4}, {0x07,4}, {0x09,4}, {0x0A,4},
	  {0x0B,4}, {0x0D,4}, {0x0E,4}, {0x0F,4}, {0x33,4}, {0x34,4}, {0x32,4},
	  {0x28,5} },
	{ {Z_KBD_MOD_LSHIFT,5,K_MOD}, {0x64,4}, {0x1D,4}, {0x1B,4}, {0x06,4},
	  {0x19,4}, {0x05,4}, {0x11,4}, {0x10,4}, {0x36,4}, {0x37,4}, {0x38,4},
	  {0x87,4}, {Z_KBD_MOD_LSHIFT,7,K_MOD} },
	{ {0x29,5}, {Z_KBD_MOD_LCTRL,5,K_MOD}, {Z_KBD_MOD_LGUI,5,K_MOD},
	  {Z_KBD_MOD_LALT,5,K_MOD}, {0x2C,15}, {Z_KBD_MOD_RALT,5,K_MOD},
	  {0x50,4}, {0x52,4}, {0x51,4}, {0x4F,4}, {0,4,K_LAYOUT} },
};

static z_win_t win;

static uint8_t mods;		// sticky modifiers, Z_KBD_MOD_* bits
static bool caps;
static int layout;			// the active layout, as last drawn
static int down_r = -1, down_c = -1;	// the key being pressed
static char status[64];

// -- text, without stdio --
//
// snprintf would link newlib's whole formatter -- floating point
// included -- into an app whose job is to be small. The status line
// needs strings and hex, and nothing else.

static char *put_s(char *o, const char *e, const char *s) {
	while (*s && o < e) *o++ = *s++;
	*o = 0;
	return o;
}

static char *put_hex(char *o, const char *e, uint32_t v, int digits) {
	for (int i = digits - 1; i >= 0 && o < e; i--)
		*o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
	*o = 0;
	return o;
}

// -- labels --

static const char *mod_name(uint8_t bit) {
	switch (bit) {
		case Z_KBD_MOD_LSHIFT: return "Shift";
		case Z_KBD_MOD_LCTRL:  return "Ctrl";
		case Z_KBD_MOD_LALT:   return "Alt";
		case Z_KBD_MOD_LGUI:   return "Super";
		case Z_KBD_MOD_RALT:   return "AltGr";
		default:               return "";
	}
}

// What a key shows, UTF-8, into `out`.
static void label(const kbkey_t *k, char *out) {

	out[0] = 0;
	if (k->kind == K_MOD)    { strcpy(out, mod_name(k->usage)); return; }
	if (k->kind == K_CAPS)   { strcpy(out, "Caps"); return; }
	if (k->kind == K_LAYOUT) { strcpy(out, z_kbd_layout_info(layout)->label); return; }

	switch (k->usage) {
		case 0x2A: strcpy(out, "Bksp");  return;
		case 0x2B: strcpy(out, "Tab");   return;
		case 0x28: strcpy(out, "Enter"); return;
		case 0x29: strcpy(out, "Esc");   return;
		case 0x2C: return;
		case 0x50: strcpy(out, "<");     return;
		case 0x52: strcpy(out, "^");     return;
		case 0x51: strcpy(out, "v");     return;
		case 0x4F: strcpy(out, ">");     return;
	}

	// The layout's own answer, with Shift and AltGr as they stand --
	// not Ctrl, which would turn every letter into a control code.
	uint32_t ks = z_kbd_translate(layout, k->usage,
		mods & (Z_KBD_MOD_SHIFT | Z_KBD_MOD_RALT),
		caps ? Z_KBD_LOCK_CAPS : 0, NULL);

	// A dead key shows its accent -- or, for the accents ISO 8859-15
	// dropped (a lone acute, diaeresis, cedilla), the ASCII mark that
	// looks most like it, rather than the missing-glyph box.
	if (Z_KEY_IS_DEAD(ks)) {
		ks = z_kbd_dead_spacing_of(ks);
		if (ks >= 0x80 && !z_cp_to_l9(ks)) {
			switch (ks) {
				case 0x00B4: ks = '\''; break;		// acute
				case 0x00A8: case 0x02DD: ks = '"'; break;
				case 0x00B8: case 0x02DB: ks = ','; break;
				case 0x02C7: ks = 'v'; break;		// caron
				case 0x02D8: ks = 'u'; break;		// breve
				case 0x02D9: ks = '.'; break;
				default: ks = '*';
			}
		}
	}

	if (Z_KEY_IS_TEXT(ks)) out[z_utf8_put(ks, out)] = 0;

}

// -- drawing --

static int key_x(int r, int c) {
	int q = 0;
	for (int i = 0; i < c; i++) q += rows[r][i].w;
	return MARGIN + q * Q;
}

static bool key_on(int r, int c) {
	const kbkey_t *k = &rows[r][c];
	if (r == down_r && c == down_c) return true;
	if (k->kind == K_MOD) return (mods & k->usage) != 0;
	if (k->kind == K_CAPS) return caps;
	return false;
}

static void draw_key(int r, int c) {

	const kbkey_t *k = &rows[r][c];
	if (!k->w) return;

	int x = key_x(r, c), y = MARGIN + r * KEY_H;
	int w = k->w * Q - 1, h = KEY_H - 1;
	bool on = key_on(r, c);

	z_win_fill_rect(&win, x, y, w, h, on ? 1 : 0);
	// z_win_hw_box() takes SCREEN coordinates (it only clips to the
	// content), unlike the fill and the text, which are relative to it.
	z_clip_t cr;
	z_win_content_rect(&win, &cr);
	z_win_hw_box(&win, cr.x0 + x, cr.y0 + y, cr.x0 + x + w - 1, cr.y0 + y + h - 1, 1);

	char s[16];
	label(k, s);
	int cols = 0;
	for (const char *p = s, *e = s + strlen(s); p < e; ) cols += z_cp_width(z_utf8_next(&p, e));
	int tx = x + (w - cols * z_font_5x8.w) / 2;
	int ty = y + (h - z_font_5x8.h) / 2;
	z_win_draw_utf8_2(&win, tx, ty, s, on ? 0 : 1, on ? 1 : 0, &z_font_5x8);

}

static void draw_status(void) {
	int y = MARGIN + ROWS * KEY_H + 2;
	z_win_fill_rect(&win, 0, y, WIN_W, STATUS_H, 0);
	z_win_draw_utf8(&win, MARGIN, y + 2, status, 1, &z_font_5x8);
}

static void draw_all(void) {
	z_win_clear(&win);
	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < 16 && rows[r][c].w; c++) draw_key(r, c);
	draw_status();
}

static void set_status_layout(void) {
	const z_kbd_layout_t *l = z_kbd_layout_info(layout);
	char *o = status, *e = status + sizeof(status) - 1;
	o = put_s(o, e, l->name);
	o = put_s(o, e, "  ");
	put_s(o, e, l->desc);
}

// -- sending --

static void send(uint8_t usage, bool pressed) {
	uint32_t ev = ((caps ? Z_KBD_LOCK_CAPS : 0u) << 17) |
		((uint32_t)mods << 9) | ((uint32_t)usage << 1) | (pressed ? 1u : 0u);
	hid_inject((int32_t)ev);
}

// Says what the key just typed: the character, its codepoint and its
// UTF-8 bytes -- what the keyboard-layout work needs to see.
static void report(uint8_t usage) {

	uint32_t ks = z_kbd_translate(layout, usage, mods,
		caps ? Z_KBD_LOCK_CAPS : 0, NULL);
	char *o = status, *e = status + sizeof(status) - 1;
	char u[Z_UTF8_MAX + 1];

	if (Z_KEY_IS_DEAD(ks)) {
		uint32_t sp = z_kbd_dead_spacing_of(ks);
		u[sp ? z_utf8_put(sp, u) : 0] = 0;
		o = put_s(o, e, "dead key ");
		o = put_s(o, e, u);
		put_s(o, e, " -- the next key takes it");
		return;
	}
	if (!Z_KEY_IS_TEXT(ks)) {
		o = put_s(o, e, z_kbd_layout_info(layout)->name);
		o = put_s(o, e, "  key 0x");
		put_hex(o, e, usage, 2);
		return;
	}

	int k = z_utf8_put(ks, u);
	u[k] = 0;
	o = put_s(o, e, u);
	o = put_s(o, e, "  U+");
	o = put_hex(o, e, ks, ks > 0xFFFF ? 6 : 4);
	o = put_s(o, e, " ");
	for (int i = 0; i < k; i++) {
		o = put_s(o, e, " ");
		o = put_hex(o, e, (uint8_t)u[i], 2);
	}
	if (z_kbd_layout_info(layout)->ime) put_s(o, e, "  (romaji)");

}

// -- input --

static bool hit(int x, int y, int *r, int *c) {
	if (y < MARGIN || y >= MARGIN + ROWS * KEY_H) return false;
	*r = (y - MARGIN) / KEY_H;
	for (int i = 0; i < 16 && rows[*r][i].w; i++) {
		int kx = key_x(*r, i);
		if (x >= kx && x < kx + rows[*r][i].w * Q) { *c = i; return true; }
	}
	return false;
}

static void press(int r, int c) {

	const kbkey_t *k = &rows[r][c];

	if (k->kind == K_MOD) {
		mods ^= k->usage;
		draw_all();						// every label may change
		return;
	}
	if (k->kind == K_CAPS) {
		caps = !caps;
		draw_all();
		return;
	}
	if (k->kind == K_LAYOUT) {
		// Every layout, in table order -- for trying them all -- and
		// made active for everything, exactly as Super+Space would.
		int next = (layout + 1) % z_kbd_layout_count;
		if (z_kbd_set_active_layout(next)) layout = next;
		set_status_layout();
		draw_all();
		return;
	}

	report(k->usage);
	send(k->usage, true);
	send(k->usage, false);

	down_r = r; down_c = c;
	draw_key(r, c);

	// One-shot: the modifiers were for this key.
	if (mods) { mods = 0; draw_all(); }
	else draw_status();

}

static void release(void) {
	if (down_r < 0) return;
	int r = down_r, c = down_c;
	down_r = down_c = -1;
	draw_key(r, c);
}

static uint8_t last_buttons;

static void mouse(uint32_t packed) {

	int x, y, r, c;
	bool inside = z_win_mouse_content_xy(&win, packed, &x, &y);
	uint8_t b = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool was = (last_buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool now = (b & Z_MOUSE_BTN_LEFT) != 0;
	last_buttons = b;

	if (now && !was && inside && hit(x, y, &r, &c)) press(r, c);
	else if (!now && was) release();

}

int main(void) {

	if (z_win_create_flags(&win, "keyboard", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
		Z_WIN_FLAG_NO_FOCUS) != Z_OK) {
		return 1;		// no wm: nothing to be a keyboard for
	}

	layout = z_kbd_active_layout();
	set_status_layout();
	draw_all();

	for (;;) {

		z_msg_t msg;
		bool redraw = false, asked = false;

		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_WM_SET_CLIP) z_win_apply_clip(&win, &msg.obj);
			else if (msg.subject == Z_WM_REDRAW) {
				z_win_apply_redraw(&win, msg.obj.val.uint32);
				redraw = asked = true;
			}
			else if (msg.subject == Z_WM_WINDOW_MOVED) z_win_parse_rect(&win, &msg.obj);
			else if (msg.subject == Z_WM_MOUSE) mouse(msg.obj.val.uint32);
			else if (msg.subject == Z_WM_CLOSE) return 0;
		}

		// Super+Space, or a config reload, may have changed the
		// layout under us: follow it.
		int now = z_kbd_active_layout();
		if (now != layout) {
			layout = now;
			set_status_layout();
			redraw = true;
		}

		if (redraw) {
			draw_all();
			if (asked) z_win_redraw_done(&win);	// only a REDRAW is answered
		}

		z_proc_wait(Z_TICK_HZ / 8);

	}

}
