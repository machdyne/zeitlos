/*
 * Host test for sw/apps/term: the REAL term.c, driven through scripted
 * sessions, with the screen checked against an independent answer
 * after every frame.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/term_render \
 *      sw/apps/term/tests/render.c sw/common/zwin.c sw/common/zwidget.c \
 *      sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
 *      sw/common/zvt100.c sw/common/zport.c sw/common/zcfg.c
 *   /tmp/term_render /tmp/term          # writes /tmp/term-*.pbm
 *
 * Exit 0 all checks passed, 1 a check failed, 77 skipped (the host
 * cannot map the fixed addresses -- see zrender.h/ztramp.h).
 *
 * -- what it checks --
 *
 * term keeps a shadow of what is on the glass and draws only where the
 * shadow and the wanted picture differ, and it moves pixels with a
 * hardware scroll and shifts the shadow to match. Both halves can go
 * wrong silently: a stale shadow draws nothing, and a blit that
 * disagrees with its shift leaves wrong text that nothing will ever
 * repaint. On hardware either looks like "sometimes the terminal shows
 * old text".
 *
 * check_glass() therefore recomputes, WITHOUT term's own helpers, what
 * every cell not covered by an overlay should show -- history or
 * screen at the current offset, inverted for the selection and the
 * cursor -- and compares it with BOTH term's shadow and the actual
 * pixels in VRAM. The pixel comparison is the one that matters: it is
 * what catches a blit and a shift that disagree.
 *
 * The render harness's z_fb_hw_scroll() is a software copy with the
 * hardware's contract, including refusing a partly covered window, so
 * the occluded-window scenario exercises term's fallback exactly when
 * hardware would.
 *
 * The scenarios are the ones this rewrite exists for: the Open bar
 * that stayed on screen after connecting, the start panel, scrollback
 * with output arriving, selection and copy across history.
 */

#include "../../../common/tests/zrender.h"

#include <stdarg.h>

#define main term_main_unused
#include "../term.c"
#undef main

// ---------------------------------------------------------------
// a scripted kernel
// ---------------------------------------------------------------
//
// The same trick as ztramp.h (a stub at reg_kernel), but with enough
// behaviour to connect: a mailbox, a pid registry, a clock, and a
// provider that answers CONNECT.

#define PID_WM     1
#define PID_REPL   3
#define PID_POSIX  4
#define PID_NET    2

static z_obj_t k_ok, k_fail;

static z_msg_t mailbox[32];
static int mb_head, mb_count;

static uint32_t ticks = 1000;

static bool reg_repl, reg_posix;
static bool refuse_next;

static char clipboard[Z_WM_CLIP_MAX];

// What the scripted kernel's config store holds for
// apps.term.auto_connect; NULL means the file does not set it.
static const char *cfg_auto_connect;
static int closes_sent;

static void post(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
	if (mb_count >= 32) return;
	z_msg_t *m = &mailbox[(mb_head + mb_count++) % 32];
	memset(m, 0, sizeof(*m));
	m->from = from;
	m->to = 10;
	m->subject = subject;
	m->tag = tag;
	m->obj = obj;
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {

	(void)b;

	switch (id) {

	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks++;
		return (uint32_t *)&k_ok;

	case Z_SYS_PROC_WAIT:
		ticks += 4;
		return (uint32_t *)&k_ok;

	case Z_SYS_CFG_GET: {
		z_cfg_get_args_t *a = (z_cfg_get_args_t *)args;
		a->generation = 1;
		a->found = 0;
		if (a->key && cfg_auto_connect &&
			!strcmp(a->key, "apps.term.auto_connect")) {
			snprintf(a->val, a->vallen, "%s", cfg_auto_connect);
			a->found = 1;
		}
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		const char *n = o->val.str;
		uint32_t pid = 0;
		if (!strcmp(n, "wm0")) pid = PID_WM;
		else if (!strcmp(n, "repl0") && reg_repl) pid = PID_REPL;
		else if (!strcmp(n, "posix0") && reg_posix) pid = PID_POSIX;
		else if (!strcmp(n, "net0")) pid = PID_NET;
		if (!pid) { o->type = Z_NONE; return (uint32_t *)&k_fail; }
		o->type = Z_UINT32;
		o->val.uint32 = pid;
		return (uint32_t *)&k_ok;
	}

	case Z_SYS_MSG_READ:
		if (!mb_count) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = mailbox[mb_head];
		mb_head = (mb_head + 1) % 32;
		mb_count--;
		return (uint32_t *)&k_ok;

	case Z_SYS_MSG_SEND: {
		z_msg_t *m = (z_msg_t *)args;
		if (m->subject == Z_PORT_CONNECT) {
			if (refuse_next) {
				refuse_next = false;
				post(m->to, Z_PORT_REFUSED, 0, z_obj_str("busy"));
			} else {
				post(m->to, Z_PORT_CONNECTED, 0, z_obj_uint32(7));
			}
		} else if (m->subject == Z_PORT_CLOSE) {
			closes_sent++;
		} else if (m->subject == Z_WM_CLIP_SET && m->obj.type == Z_STR) {
			snprintf(clipboard, sizeof(clipboard), "%s", m->obj.val.str);
		}
		return (uint32_t *)&k_ok;
	}

	default:
		return (uint32_t *)&k_ok;

	}

}

static bool k_install(void) {

	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;

	void *page = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (page == MAP_FAILED) return false;

	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;

	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;

}

// ---------------------------------------------------------------
// zconnect, reduced to `port <name>` -- the network kinds are net's
// business and not what this tests.
// ---------------------------------------------------------------

bool z_conn_kind_from_word(const char *word, z_conn_kind_t *out) {
	if (!strcmp(word, "port")) { *out = Z_CONN_PORT; return true; }
	if (!strcmp(word, "telnet")) { *out = Z_CONN_TELNET; return true; }
	return false;
}

const char *z_conn_kind_name(z_conn_kind_t kind) {
	return kind == Z_CONN_PORT ? "port" : "telnet";
}

bool z_conn_prepare(z_conn_kind_t kind, const char *text,
	z_conn_target_t *out, char *err, size_t errlen) {
	memset(out, 0, sizeof(*out));
	if (kind != Z_CONN_PORT) {
		snprintf(err, errlen, "telnet: net is not running");
		return false;
	}
	snprintf(out->provider, sizeof(out->provider), "%s", text);
	out->arg = z_obj_none();
	out->timeout_ticks = Z_CONN_TIMEOUT_LOCAL_TICKS;
	snprintf(out->detail, sizeof(out->detail), "%s", text);
	return true;
}

// ---------------------------------------------------------------
// checks
// ---------------------------------------------------------------

static int failures;
static int checks;

static void fail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	printf("  FAIL: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	failures++;
}

static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) fail("%s", what);
}

// Independent of term.c's sel_prepare()/sel_contains().
static bool want_selected(uint32_t id, int col) {

	if (!sel_active) return false;

	uint32_t i0 = sel_a_id, i1 = sel_c_id;
	int c0 = sel_a_col, c1 = sel_c_col;
	if (i1 < i0 || (i1 == i0 && c1 < c0)) {
		uint32_t ti = i0; i0 = i1; i1 = ti;
		int tc = c0; c0 = c1; c1 = tc;
	}

	uint32_t first = vt_history_pushed(&vt) - vt_history_count(&vt);
	if (i1 < first) return false;
	if (i0 < first) { i0 = first; c0 = 0; }

	if (id < i0 || id > i1) return false;
	if (id == i0 && col < c0) return false;
	if (id == i1 && col > c1) return false;
	return true;

}

static bool covered(int row, int col) {
	if (bar_active && row == VT_ROWS - 1) return true;
	if (panel_visible && row >= PANEL_R0 && row <= PANEL_R1 &&
		col >= PANEL_C0 && col <= PANEL_C1) return true;
	return false;
}

static void check_glass(const char *where) {

	z_clip_t clip;
	z_win_content_rect(&win, &clip);

	int count = vt_history_count(&vt);
	uint32_t pushed = vt_history_pushed(&vt);
	int bad_shadow = 0, bad_pixels = 0;
	int first_r = -1, first_c = -1;

	for (int row = 0; row < VT_ROWS; row++) {
		for (int col = 0; col < VT_COLS; col++) {

			if (covered(row, col)) continue;

			int doc = count - view_off + row;
			uint32_t id = pushed - (uint32_t)view_off + (uint32_t)row;
			uint8_t b = vt_doc_cell(&vt, doc, col);

			bool inv = VT_PACK_REV(b);
			if (want_selected(id, col)) inv = !inv;
			if (port.connected) {
				int cc = vt.cursor_x >= VT_COLS ? VT_COLS - 1 : vt.cursor_x;
				if (row == vt.cursor_y + view_off && col == cc) inv = !inv;
			}

			uint16_t want = (uint16_t)((uint8_t)VT_PACK_CH(b) | (inv ? 0x100 : 0));
			if (shadow[row][col] != want) {
				if (!bad_shadow++ && first_r < 0) { first_r = row; first_c = col; }
			}

			char ch = VT_PACK_CH(b);
			const uint8_t *g = NULL;
			if ((uint8_t)ch >= TERM_FONT.first && (uint8_t)ch <= TERM_FONT.last)
				g = TERM_FONT.glyphs + ((uint8_t)ch - TERM_FONT.first) * TERM_FONT.h;

			bool cell_bad = false;
			for (int j = 0; j < cell_h && !cell_bad; j++)
				for (int i = 0; i < cell_w; i++) {
					int bit = g ? ((g[j] & (0x80 >> i)) != 0) : 0;
					int px = inv ? !bit : bit;
					if (z_render_get(clip.x0 + col * cell_w + i,
						clip.y0 + row * cell_h + j) != px) {
						cell_bad = true;
						break;
					}
				}
			if (cell_bad && !bad_pixels++ && first_r < 0) {
				first_r = row; first_c = col;
			}

		}
	}

	checks++;
	if (bad_shadow || bad_pixels)
		fail("%s: %d shadow and %d pixel mismatches (first at row %d col %d)",
			where, bad_shadow, bad_pixels, first_r, first_c);

}

// ---------------------------------------------------------------
// driving it
// ---------------------------------------------------------------

static void frame_check(const char *where) {
	frame();
	check_glass(where);
}

static void key(uint32_t keysym, uint8_t mods) {
	handle_key_event(Z_WM_PACK_KEY(keysym, mods, 1));
	handle_key_event(Z_WM_PACK_KEY(keysym, mods, 0));
}

static void type(const char *s) {
	for (; *s; s++) key((uint8_t)*s, 0);
}

// Pointer at a content-relative pixel.
static void mouse(int cx, int cy, uint8_t buttons) {
	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	handle_mouse_event(Z_WM_PACK_MOUSE(clip.x0 + cx, clip.y0 + cy, buttons, 1));
}

static void output(const char *s) {
	vt_feed(&vt, (const uint8_t *)s, (uint32_t)strlen(s));
}

static void output_lines(int from, int to, int per_frame, const char *where) {
	char line[96];
	for (int i = from; i <= to; i++) {
		snprintf(line, sizeof(line), "line %04d  the quick brown fox jumps over the lazy dog\r\n", i);
		output(line);
		if ((i - from) % per_frame == per_frame - 1) frame_check(where);
	}
	frame_check(where);
}

static char row_text_buf[VT_COLS + 1];

// Text currently wanted at display row `row`, trailing blanks dropped.
static const char *display_row(int row) {
	int last = -1;
	for (int c = 0; c < VT_COLS; c++) {
		uint8_t b = vt_doc_cell(&vt, vt_history_count(&vt) - view_off + row, c);
		row_text_buf[c] = VT_PACK_CH(b);
		if (row_text_buf[c] != ' ') last = c;
	}
	row_text_buf[last + 1] = 0;
	return row_text_buf;
}

static void write_pbm(const char *prefix, const char *name) {
	char path[256];
	snprintf(path, sizeof(path), "%s-%s.pbm", prefix, name);
	z_render_write(path, &win, 2);
}

int main(int argc, char **argv) {

	const char *prefix = argc > 1 ? argv[1] : "/tmp/term";

	term_setup();

	if (!z_render_open(&win, term_win_w, term_win_h)) {
		printf("term render: skipped (cannot map the VRAM address)\n");
		return 77;
	}
	if (!k_install()) {
		printf("term render: skipped (needs -no-pie and vm.mmap_min_addr=0)\n");
		return 77;
	}

	// Away from the screen's top-left, so a drag can go above the
	// content area -- mouse coordinates are unsigned.
	win.x = 40;
	win.y = 60;

	printf("term render: window %dx%d, scrollback %d lines\n",
		term_win_w, term_win_h, TERM_HIST_LINES);

	// -- 1. starts disconnected, on the panel --
	printf("1. start panel\n");
	term_start();
	frame_check("startup");
	expect(panel_visible, "panel visible at startup");
	expect(!port.connected, "not connected at startup");
	expect(!panel_items[PB_REPL].enabled, "REPL disabled while repl0 is not registered");
	expect(panel_items[PB_OPEN].enabled, "OPEN always enabled");
	expect(panel_set.focused == PB_OPEN, "focus on OPEN while no shell is ready");
	write_pbm(prefix, "1-start");

	// -- 2. repl0 registers; its button comes alive; click it --
	printf("2. repl0 appears, click REPL\n");
	reg_repl = true;
	ticks += Z_TICK_HZ;
	frame_check("after repl0 registers");
	expect(panel_items[PB_REPL].enabled, "REPL enabled once repl0 registers");
	expect(!panel_items[PB_POSIX].enabled, "POSIX still disabled");
	expect(panel_set.focused == PB_REPL, "focus follows the first ready shell");
	write_pbm(prefix, "2-repl-ready");

	{
		z_widget_t *w = &panel_items[PB_REPL];
		mouse(w->x + w->w / 2, w->y + w->h / 2, Z_MOUSE_BTN_LEFT);
		mouse(w->x + w->w / 2, w->y + w->h / 2, 0);
	}
	frame_check("after connecting to repl0");
	expect(port.connected && port.peer_pid == PID_REPL, "connected to repl0");
	expect(!panel_visible, "panel gone once connected");

	// -- 3. output scrolls, in bursts and a line at a time --
	printf("3. output (hardware scroll path)\n");
	output("repl -- Zeitlos command interpreter\r\n> ");
	frame_check("banner");
	output_lines(0, 60, 1, "one line per frame");
	output_lines(61, 140, 7, "seven lines per frame");
	expect(vt_history_count(&vt) > 100, "history filling");

	// -- 4. the Open bar: Esc, and connecting through it --
	printf("4. Open bar dismissal (the original bug)\n");
	key(Z_KEY_F11, 0);
	frame_check("bar open");
	expect(bar_active, "F11 opens the bar");
	type("port pos");
	frame_check("bar typing");
	write_pbm(prefix, "4-bar");
	key(0x1b, 0);
	frame_check("bar dismissed with Esc");
	expect(!bar_active, "Esc dismisses");

	reg_posix = true;
	key(Z_KEY_F11, 0);
	type("port posix0");
	key(0x0d, 0);
	frame_check("bar dismissed by a successful connect");
	expect(!bar_active, "bar gone after connecting");
	expect(port.connected && port.peer_pid == PID_POSIX, "connected to posix0");
	output("posix -- a Unix-shaped shell for Zeitlos\r\n$ ");
	output_lines(141, 150, 3, "output after the bar");

	// Output written UNDER the bar while it is up must not show
	// through, and must be there when it goes.
	key(Z_KEY_F11, 0);
	output_lines(151, 160, 2, "output under the bar");
	key(0x1b, 0);
	frame_check("bar gone, output under it restored");

	// -- 5. scrollback --
	printf("5. scrollback\n");
	key(Z_KEY_PAGEUP, Z_KBD_MOD_LSHIFT);
	frame_check("Shift+PgUp");
	expect(view_off == VT_ROWS - 1, "a page is one line short of a screen");
	key(Z_KEY_PAGEUP, Z_KBD_MOD_LSHIFT);
	frame_check("Shift+PgUp twice");
	for (int i = 0; i < 5; i++) {
		key(Z_KEY_UP, Z_KBD_MOD_LSHIFT);
		frame_check("Shift+Up");
	}
	key(Z_KEY_DOWN, Z_KBD_MOD_LSHIFT);
	frame_check("Shift+Down");
	write_pbm(prefix, "5-scrolled");

	char pinned[VT_COLS + 1];
	snprintf(pinned, sizeof(pinned), "%s", display_row(0));
	output_lines(161, 170, 3, "output while scrolled back");
	expect(!strcmp(pinned, display_row(0)), "view stays on the same text while output arrives");

	key(Z_KEY_HOME, Z_KBD_MOD_LSHIFT);
	frame_check("Shift+Home");
	expect(view_off == vt_history_count(&vt), "Shift+Home reaches the oldest line");

	// -- 6. selection across history, and copy --
	printf("6. selection and copy\n");
	key(Z_KEY_END, Z_KBD_MOD_LSHIFT);
	frame_check("Shift+End");
	key(Z_KEY_PAGEUP, Z_KBD_MOD_LSHIFT);
	frame_check("back a page to select");

	mouse(0 * cell_w + 1, 2 * cell_h + 1, Z_MOUSE_BTN_LEFT);
	mouse(9 * cell_w + 1, 2 * cell_h + 1, Z_MOUSE_BTN_LEFT);
	frame_check("selection started");
	// drag above the content: scrolls back while extending
	for (int i = 0; i < 6; i++) {
		mouse(9 * cell_w + 1, -4, Z_MOUSE_BTN_LEFT);
		frame_check("drag-scroll above the top");
	}
	mouse(9 * cell_w + 1, 0, 0);
	frame_check("selection released");
	expect(sel_active, "selection kept after release");
	write_pbm(prefix, "6-selection");

	mouse(40, 40, Z_MOUSE_BTN_RIGHT);
	mouse(40, 40, 0);
	{
		int lines = 0;
		for (const char *c = clipboard; *c; c++) if (*c == '\n') lines++;
		expect(lines == 8, "copied 9 rows (8 newlines)");
		// Dragged UPWARD from the anchor, so the far end is the start:
		// the copy begins at column 9 of the upper row (just past
		// "line NNNN") and ends at the anchor's column 0.
		size_t len = strlen(clipboard);
		expect(!strncmp(clipboard, "  the quick", 11), "copy starts at the dragged-to column");
		expect(len >= 2 && !strcmp(clipboard + len - 2, "\nl"), "copy ends at the anchor column");
		expect(strstr(clipboard, "dog\n") != NULL, "full rows between the ends");
		expect(clipboard[strlen(clipboard) - 1] != ' ', "trailing blanks stripped");
	}

	// the selection follows its text as more output arrives
	uint32_t before = sel_a_id;
	output_lines(171, 180, 5, "output with a selection up");
	expect(sel_a_id == before && sel_active, "selection still anchored to its line id");

	key('x', 0);
	frame_check("typing clears the selection and returns to live");
	expect(!sel_active, "selection cleared by typing");
	expect(view_off == 0, "typing returns to live");

	// -- 7. a partly covered window: no blit, still correct --
	printf("7. occluded window\n");
	{
		z_clip_t c, r[2];
		z_win_content_rect(&win, &c);
		r[0] = c; r[0].x1 = c.x0 + 100;
		r[1] = c; r[1].x0 = c.x0 + 101;
		z_gfx_set_visible(r, 2);
		output_lines(181, 200, 3, "occluded, scrolling");
		key(Z_KEY_PAGEUP, Z_KBD_MOD_LSHIFT);
		frame_check("occluded, Shift+PgUp");
		key(Z_KEY_END, Z_KBD_MOD_LSHIFT);
		frame_check("occluded, Shift+End");
		z_gfx_clear_visible();
	}

	// -- 8. a wm redraw with the bar up --
	printf("8. redraw with the bar up\n");
	key(Z_KEY_F11, 0);
	frame_check("bar before redraw");
	z_render_clear();
	handle_redraw(Z_WM_PACK_MOUSE(0, 0, 0, 0) /* id/flags unused here */);
	win.x = 40; win.y = 60;
	frame_check("after redraw");
	{
		z_clip_t c;
		z_win_content_rect(&win, &c);
		// the bar is reverse video: its first cell's top-left pixel is ink
		expect(z_render_get(c.x0, c.y0 + (VT_ROWS - 1) * cell_h) == 1,
			"bar repainted after a redraw");
	}
	key(0x1b, 0);
	frame_check("bar dismissed after redraw");

	// -- 9. F12 and a far-end close both return to the panel --
	printf("9. F12, a refused connect, and a close from the far end\n");
	key(Z_KEY_F12, 0);
	frame_check("F12");
	expect(!port.connected && panel_visible, "F12 disconnects to the panel");
	expect(closes_sent >= 1, "F12 told the far end");
	write_pbm(prefix, "9-disconnected");

	refuse_next = true;
	key(0x09, 0);		// Tab to POSIX... focus order is REPL, POSIX, OPEN
	{
		int f = panel_set.focused;
		while (panel_set.focused != PB_POSIX) {
			key(0x09, 0);
			if (panel_set.focused == f) break;
		}
	}
	key(0x0d, 0);
	frame_check("refused");
	expect(!port.connected && panel_visible, "refused connect leaves the panel up");
	expect(strstr(panel_status, "refused") != NULL, "status says refused");

	key(0x0d, 0);		// again, accepted this time
	frame_check("reconnect");
	expect(port.connected, "Enter on POSIX connects");

	post(PID_POSIX, Z_PORT_CLOSE, port.conn_id, z_obj_none());
	{
		z_msg_t m;
		z_msg_read(&m);
		if (m.subject == Z_PORT_CLOSE && port.connected &&
			m.tag == port.conn_id && m.from == port.peer_pid) {
			port.connected = false;
			term_disconnect("connection closed by the other end");
		}
	}
	frame_check("closed by the far end");
	expect(panel_visible, "a far-end close shows the panel");

	key(0x1b, 0);
	frame_check("panel hidden with Esc");
	expect(!panel_visible, "Esc hides the panel");
	key('p', 0);
	frame_check("any key shows it again");
	expect(panel_visible && !bar_active, "a key brings the panel back first");
	key('p', 0);
	frame_check("typing on the panel starts the bar");
	expect(bar_active && bar_len == 1, "typing on the panel opens the bar with it");
	key(0x1b, 0);
	frame_check("bar dismissed");

	// -- 10. apps.term.auto_connect --
	printf("10. auto-connect\n");
	expect(!port.connected, "disconnected before auto-connect tests");

	cfg_auto_connect = "port repl0";		// registered: connects at once
	auto_begin();
	frame_check("auto-connect to a registered provider");
	expect(port.connected && port.peer_pid == PID_REPL, "auto-connected to repl0");
	expect(!panel_visible, "panel gone after auto-connect");

	key(Z_KEY_F12, 0);
	frame_check("F12 after auto-connect");
	expect(panel_visible && !port.connected && !auto_pending,
		"F12 lands on the panel and does NOT auto-connect again");

	// Not registered yet: waits, then connects when it appears.
	reg_posix = false;
	cfg_auto_connect = "port posix0";
	auto_begin();
	frame_check("auto-connect waiting");
	expect(auto_pending && panel_visible, "waiting while posix0 is absent");
	expect(strstr(panel_status, "auto-connect") != NULL, "panel says it is waiting");
	write_pbm(prefix, "10-auto-waiting");
	ticks += Z_TICK_HZ * 2;
	reg_posix = true;
	frame_check("posix0 appears");
	expect(port.connected && port.peer_pid == PID_POSIX,
		"connected once the provider registered");

	// Never appears: gives up with a reason.
	key(Z_KEY_F12, 0);
	cfg_auto_connect = "port nothing0";
	auto_begin();
	frame_check("waiting for a provider that never comes");
	ticks += TERM_AUTO_WAIT_TICKS + Z_TICK_HZ;
	frame_check("auto-connect timed out");
	expect(!auto_pending && !port.connected, "gave up after the wait");
	expect(strstr(panel_status, "did not appear") != NULL, "says why");

	// Esc stops the wait; the next Esc hides the panel as usual.
	auto_begin();
	frame_check("waiting again");
	key(0x1b, 0);
	frame_check("Esc cancels the wait");
	expect(!auto_pending && panel_visible, "Esc cancelled, panel still up");
	key(0x1b, 0);
	expect(!panel_visible, "second Esc hides the panel");
	key(0x1b, 0);		// brings it back (any key)

	// Nonsense is reported, not attempted.
	cfg_auto_connect = "bogus thing";
	auto_begin();
	frame_check("bad auto-connect value");
	expect(!auto_pending && strstr(panel_status, "try port") != NULL,
		"a bad value is explained on the panel");

	cfg_auto_connect = "none";
	auto_begin();
	expect(!auto_pending, "none means the panel");

	frame_check("final");

	printf("\nterm render: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;

}
