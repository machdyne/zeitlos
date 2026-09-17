/*
 * kidgames -- the runtime layer. See kgui.h.
 */

#include <stdio.h>		// puts() only -- see the Makefile's noprintf target
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"

#include "kgui.h"
#include "kgpad.h"

static z_win_t win;
static bool game_mode;
static bool game_offered;	/* has the "no game mode" note been printed */

static kg_repaint_fn repaint_fn;
static void *repaint_user;

static char last_char;
static int click_x, click_y;

/* Pointer state, so a press is an EDGE rather than a level. wm sends
 * Z_WM_MOUSE whenever position or buttons change, so without this a
 * finger resting on the button would fire a key per pixel of drift. */
static uint32_t last_buttons;

/* Debounce, per kgpad.h: the last key accepted and when. */
static kg_key_t last_key_sym;
static char last_key_ch;
static uint32_t last_key_at;

#define FONT (&z_font_5x8)
#define FONT_H 8
#define FONT_W 5

/* -- where the playfield actually is -------------------------------- */

/*
 * Origin of the 320x240 playfield in screen coordinates, and the clip
 * rectangle every draw is confined to.
 *
 * Called before EVERY primitive rather than cached, which is
 * deliberate. In windowed mode z_win_content_rect() has a side effect
 * that matters as much as its return value: it loads this window's
 * visible region into zgfx, so a subsequent z_fb_* draw is scissored
 * to the part of the window that is actually on screen rather than
 * painting over whatever is stacked on top. Caching the rectangle
 * would keep the coordinates and lose the scissor -- which is the
 * quiet half of the bug, and the half that only shows up when two
 * windows overlap.
 *
 * In game mode the region has to be explicitly CLEARED for the
 * opposite reason: nothing is in front of a full-screen page, and a
 * region left over from the windowed path would clip every draw to
 * wherever this app's window happens to sit on the desktop behind.
 */
static void target(z_clip_t *out)
{
	/*
	 * Always the window's content rect -- in game mode too.
	 *
	 * This used to branch: game mode drew to framebuffer (0,0) and
	 * the windowed path to the content rect. That is what broke the
	 * mouse, and the reason is worth keeping written down.
	 *
	 * wm delivers Z_WM_MOUSE to the focused window ONLY WHILE THE
	 * CURSOR IS OVER IT -- dispatch_mouse() in wm.c hit-tests in
	 * framebuffer coordinates and drops anything that misses. Game
	 * mode does not change that: it is a camera, not a mode change,
	 * and every window is still exactly where it was. So a playfield
	 * drawn at framebuffer (0,0) while the window sat somewhere else
	 * on the desktop received no clicks at all except by coincidence.
	 *
	 * The fix is to point the camera AT THE WINDOW rather than move
	 * the drawing to the camera -- see enter_game_mode(). The
	 * viewport is set to this rectangle's origin, so the 320x240 it
	 * magnifies is exactly this window's content. Drawing and hit
	 * testing then share one coordinate system in both modes, the
	 * branch disappears from every primitive, and the mouse code
	 * needs no special case whatsoever.
	 *
	 * It also loads this window's visible region into zgfx as a side
	 * effect, which is why it is called before EVERY primitive rather
	 * than cached: cache the rectangle and you keep the coordinates
	 * and lose the scissor, and the scissor is what stops a draw
	 * landing on whatever is stacked on top.
	 */
	z_win_content_rect(&win, out);
}

/* Clamp a playfield rectangle to the playfield, then translate it to
 * screen coordinates. Returns false if nothing is left.
 *
 * The clamp is not belt and braces: z_fb_hw_fill_rect() clamps to the
 * SCREEN, not to a window, so an oversized rectangle handed to it
 * paints over every other window on the desktop (zwin.c says so where
 * z_win_fill_rect does the same clamp). In game mode it is what keeps
 * a stray draw inside the viewport instead of in the off-screen pages
 * where the sprite art lives. */
static bool xlate(const z_clip_t *t, int *x, int *y, int *w, int *h)
{
	int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > KG_W) x1 = KG_W;
	if (y1 > KG_H) y1 = KG_H;

	if (x1 <= x0 || y1 <= y0) return false;

	*x = t->x0 + x0;
	*y = t->y0 + y0;
	*w = x1 - x0;
	*h = y1 - y0;

	return true;
}

/* -- drawing -------------------------------------------------------- */

void kg_fill(int x, int y, int w, int h, int color)
{
	z_clip_t t;

	target(&t);
	if (!xlate(&t, &x, &y, &w, &h)) return;

	z_fb_hw_fill_rect(x, y, w, h, color);
}

void kg_shade(int x, int y, int w, int h, int level)
{
	z_clip_t t;

	target(&t);
	if (!xlate(&t, &x, &y, &w, &h)) return;

	z_fb_hw_fill_shade(x, y, w, h, level);
}

void kg_pattern(int x, int y, int w, int h, const uint8_t *pat)
{
	z_clip_t t;

	target(&t);
	if (!xlate(&t, &x, &y, &w, &h)) return;

	z_fb_hw_fill_pattern(x, y, w, h, pat);
}

bool kg_sprite_available(void)
{
	return z_fb_hw_blit_mem_available();
}

bool kg_sprite(int x, int y, const uint8_t *bits, int stride, int w, int h)
{
	z_clip_t t;
	int dx = x, dy = y, dw = w, dh = h;
	int src_x, src_y;

	target(&t);

	if (!xlate(&t, &dx, &dy, &dw, &dh)) return true;

	/* The clamp may have cut the rectangle, and the source rectangle
	 * has to move with it or a sprite near an edge is drawn shifted
	 * rather than cropped. z_fb_hw_blit_mem() clips to the SCREEN and
	 * takes no clip argument, so this is ours to get right -- zgfx.h
	 * says as much where it documents the call. */
	src_x = (dx - t.x0) - x;
	src_y = (dy - t.y0) - y;

	/* Cleared first, then copied over: see kgui.h. */
	z_fb_hw_fill_rect(dx, dy, dw, dh, 0);

	return z_fb_hw_blit_mem(bits, stride, src_x, src_y, dx, dy, dw, dh);
}

void kg_clear(void)
{
	kg_fill(0, 0, KG_W, KG_H, 0);
}

void kg_frame(int x, int y, int w, int h, int color)
{
	if (w <= 0 || h <= 0) return;

	/* Four fills, not z_win_hw_box(). The rasterizer and the blitter
	 * do not order against each other, and a frame with text inside it
	 * -- which is nearly every frame in this app -- comes up
	 * intermittently missing where the two share a 32-bit VRAM word.
	 * docs/widgets.md has the worked example; the fix there was
	 * exactly this. */
	kg_fill(x, y, w, 1, color);
	kg_fill(x, y + h - 1, w, 1, color);
	kg_fill(x, y, 1, h, color);
	kg_fill(x + w - 1, y, 1, h, color);
}

int kg_text_w(const char *s)
{
	/* z_fb_draw_text advances by exactly font->w per character --
	 * there is no inter-character gap and no kerning (zgfx.c). */
	return s ? (int)strlen(s) * FONT_W : 0;
}

void kg_text(int x, int y, const char *s, int color)
{
	z_clip_t t;

	if (!s) return;
	target(&t);

	z_fb_draw_text(t.x0 + x, t.y0 + y, s, color, FONT, &t);
}

void kg_text2(int x, int y, const char *s, int fg, int bg)
{
	z_clip_t t;

	if (!s) return;
	target(&t);

	z_fb_draw_text2(t.x0 + x, t.y0 + y, s, fg, bg, FONT, &t);
}

void kg_center_text(int y, const char *s, int color)
{
	kg_text((KG_W - kg_text_w(s)) / 2, y, s, color);
}

void kg_center_text2(int y, const char *s, int fg, int bg)
{
	kg_text2((KG_W - kg_text_w(s)) / 2, y, s, fg, bg);
}

/* -- header, status, tries ------------------------------------------ */

void kg_header(const char *name, int level, int score)
{
	char buf[40];

	kg_fill(0, 0, KG_W, KG_HDR_H, 0);

	if (name) kg_text(2, 2, name, 1);

	if (level > 0 || score > 0) {
		buf[0] = '\0';
		kg_append(buf, sizeof(buf), "LVL ");
		kg_append_num(buf, sizeof(buf), level);
		kg_append(buf, sizeof(buf), "  SCORE ");
		kg_append_num(buf, sizeof(buf), score);
		kg_text(KG_W - 2 - kg_text_w(buf), 2, buf, 1);
	}

	kg_fill(0, KG_RULE_Y, KG_W, 1, 1);
}

void kg_status(const char *hint)
{
	/* With the keyboard up there is no bottom row to put this in, and
	 * the keyboard is a better hint than a sentence anyway. Silently
	 * dropping it here rather than at every call site keeps the games
	 * from each having to ask. */
	if (kg_pad_visible()) return;

	kg_fill(0, KG_H - FONT_H - 2, KG_W, FONT_H + 2, 0);
	if (hint) kg_center_text(KG_H - FONT_H - 1, hint, 1);
}

void kg_tries_left(int y, int tries_left, int tries_total)
{
	int i, x;
	int star_w = 9;
	int total_w;

	if (tries_total <= 0) return;
	if (tries_total > 20) tries_total = 20;
	if (tries_left < 0) tries_left = 0;

	kg_fill(0, y, KG_W, 12, 0);

	total_w = tries_total * star_w;
	x = (KG_W - total_w) / 2;

	for (i = 0; i < tries_total; i++) {

		int cx = x + i * star_w + 1;

		if (i < tries_left) {
			/* A remaining try: a solid diamond. Drawn rather than
			 * written as '*' because the 5x8 asterisk sits high in
			 * the cell and a row of them reads as specks. */
			kg_fill(cx + 2, y + 1, 3, 7, 1);
			kg_fill(cx, y + 3, 7, 3, 1);
		} else {
			/* A used try: an outline of the same size. Shape, not
			 * shade -- see kg.h. */
			kg_frame(cx + 1, y + 2, 5, 5, 1);
		}

	}
}

/* -- mode ------------------------------------------------------------ */

bool kg_in_game_mode(void) { return game_mode; }

void *kg_window(void) { return &win; }

/* Point the viewport at this window's content area. Called on entry
 * and again whenever the window moves. */
static void aim_viewport(void)
{
	z_clip_t c;

	if (!game_mode) return;

	z_win_content_rect(&win, &c);

	/*
	 * Always in range, and not by luck. The window is KG_WIN_W x
	 * KG_WIN_H (324x255) and wm keeps it on a 640x480 desktop, so its
	 * origin is at most (316, 225) and its content origin at most
	 * (318, 238). The viewport's own clamp -- wrap is off -- tops out
	 * at (320, 240), which is exactly the point past which a 320x240
	 * camera would run off the framebuffer. A fixed-size, unresizable
	 * window is what makes that hold; a resizable one would need the
	 * clamp checked here.
	 */
	z_game_set_view((uint32_t)c.x0, (uint32_t)c.y0);
}

static void enter_game_mode(void)
{
	if (!z_game_available()) {
		if (!game_offered) {
			game_offered = true;
			puts("kidgames: no game mode in this bitstream -- "
				"running in a window. Rebuild the gateware with GAME "
				"in rtl/boards.vh for the 320x240 viewport.");
		}
		return;
	}

	game_mode = true;

	/*
	 * Clamp, not wrap. Wrap makes the framebuffer a torus, which is
	 * what a side-scroller wants and is actively wrong here.
	 *
	 * Note what is NOT done any more: the whole framebuffer used to be
	 * cleared to black first, on the theory that the first frame
	 * scanned out should be this app's background rather than a
	 * window of somebody's desktop. With the viewport aimed at our own
	 * content that is already true, and the clear was destroying every
	 * other window's pixels for nothing -- which is also why leaving
	 * had to ask wm to repaint the world. It still does, harmlessly,
	 * in case anything else scribbled.
	 */
	z_game_set_enabled(true, false);
	aim_viewport();

	kg_repaint();
}

static void leave_game_mode(void)
{
	if (!game_mode) return;

	z_game_set_enabled(false, false);
	game_mode = false;

	/*
	 * No Z_WM_REPAINT to wm on the way out, and that is a change.
	 *
	 * It used to be necessary and now is not. The old game-mode path
	 * drew at framebuffer (0,0) and cleared the whole 640x480 on
	 * entry, so every other window's pixels were destroyed without
	 * their owners knowing -- wm repairs damage it caused itself, and
	 * that damage came from outside it, so it had to be told.
	 *
	 * With the viewport aimed at this window's own content
	 * (aim_viewport), nothing outside this window's clip is ever
	 * written in either mode. The desktop underneath is exactly as it
	 * was, so asking for a full repaint would be a few hundred
	 * windows' worth of redrawing to fix nothing. Our own content
	 * needs no repaint either -- it is still on the glass -- but it is
	 * cheap and covers the case where wm moved us while the camera
	 * was up.
	 */
	kg_repaint();
}

void kg_toggle_mode(void)
{
	if (game_mode) leave_game_mode();
	else enter_game_mode();
}

/* -- repaint --------------------------------------------------------- */

void kg_set_repaint(kg_repaint_fn fn, void *user)
{
	repaint_fn = fn;
	repaint_user = user;
}

kg_repaint_fn kg_get_repaint(void **user_out)
{
	if (user_out) *user_out = repaint_user;
	return repaint_fn;
}

void kg_repaint(void)
{
	if (repaint_fn) repaint_fn(repaint_user);
}

/* -- lifecycle ------------------------------------------------------- */

bool kg_init(void)
{
	/*
	 * Fixed size, no Z_WIN_FLAG_RESIZABLE: the whole app is laid out
	 * against constants (kg.h) and there is no reflow path. Without
	 * the resizable flag wm draws no corner grip, which is also why
	 * nothing in this app has to dodge Z_WIN_GRIP_INSET the way
	 * sw/apps/text does.
	 *
	 * CLOSE_KILLS_OWNER: one window for the app's whole life, so the
	 * titlebar close icon should just end it. Without the flag wm
	 * sends Z_WM_CLOSE and expects us to decide, which for a
	 * single-window app is a message to handle for no benefit.
	 */
	if (z_win_create_flags(&win, "kidgames", KG_WIN_W, KG_WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		puts("kidgames: failed to create window");
		return false;
	}

	/* No z_gfx_hw_font_load() here, ever. wm loads z_font_5x8 and
	 * z_font_6x12 once at its own startup and is the only process
	 * board-wide that writes glyph memory; an app loading a font
	 * corrupts every other process's text
	 * (docs/window_manager.md). */

	enter_game_mode();

	return true;
}

void kg_shutdown(void)
{
	if (game_mode) leave_game_mode();
	z_win_destroy(&win);
}

/* -- the pump -------------------------------------------------------- */

/* Translate a Z_WM_KEY keysym into the small set of keys games see.
 * Everything else returns KG_KEY_NONE and is swallowed, which is the
 * original's "fool-proof input" rule: a kid mashing the keyboard
 * cannot reach a state a game did not plan for. */
static kg_key_t map_key(uint32_t keysym, char *ch_out)
{
	*ch_out = 0;

	if (keysym >= 'a' && keysym <= 'z') {
		*ch_out = (char)(keysym - 'a' + 'A');
		return KG_KEY_CHAR;
	}
	if (keysym >= 'A' && keysym <= 'Z') {
		*ch_out = (char)keysym;
		return KG_KEY_CHAR;
	}
	if (keysym >= '0' && keysym <= '9') {
		*ch_out = (char)keysym;
		return KG_KEY_CHAR;
	}

	switch (keysym) {
	case Z_KEY_UP:    return KG_KEY_UP;
	case Z_KEY_DOWN:  return KG_KEY_DOWN;
	case Z_KEY_LEFT:  return KG_KEY_LEFT;
	case Z_KEY_RIGHT: return KG_KEY_RIGHT;
	case ' ':         return KG_KEY_SPACE;
	case '\r':
	case '\n':        return KG_KEY_ENTER;
	case 0x1b:        return KG_KEY_ESC;
	case '\b':
	case 0x7f:        return KG_KEY_BACKSPACE;
	default:          return KG_KEY_NONE;
	}
}

/* Accept a key unless it is a repeat of the last one inside the
 * debounce window. See KG_DEBOUNCE_TICKS. */
static bool debounce_ok(kg_key_t k, char ch, uint32_t now)
{
	if (k == last_key_sym && ch == last_key_ch &&
		(uint32_t)(now - last_key_at) < KG_DEBOUNCE_TICKS)
		return false;

	last_key_sym = k;
	last_key_ch = ch;
	last_key_at = now;

	return true;
}

/*
 * One turn of the message loop.
 *
 * Returns the key produced, or KG_KEY_NONE if the queue is drained
 * without one. Everything wm needs answering is answered here.
 */
static kg_key_t pump_once(void)
{
	z_msg_t msg;
	kg_key_t out = KG_KEY_NONE;
	bool need_redraw = false;

	while (z_msg_read(&msg) == Z_OK) {

		switch (msg.subject) {

		case Z_WM_SET_CLIP:
			/* Not optional and not deferrable. A window that misses
			 * one keeps drawing against a region that is no longer
			 * true; one that has never had one draws nothing at all,
			 * with nothing on the console to say why. */
			z_win_apply_clip(&win, &msg.obj);
			break;

		case Z_WM_REDRAW:
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			/* The damage rectangles are deliberately ignored: this app
			 * redraws a whole 320x240 screen regardless, and the
			 * shared layer clips the repaint to the same rectangles
			 * either way. Ignoring it is documented as CORRECT, only
			 * slower, and only for an app that could have painted less
			 * (docs/app_runtime.md). A quiz screen could not. */
			z_win_damage_ignore(&win);
			need_redraw = true;
			break;

		case Z_WM_WINDOW_MOVED:
			/* In game mode the viewport is aimed at this window's
			 * content, so a move that went unanswered would leave the
			 * camera looking at where the window used to be. */
			/* Keep win.x/y in step; no repaint here. wm's
			 * repair_region() always sends a Z_WM_REDRAW for the same
			 * window in the same call, and treating both as triggers
			 * is a visible double repaint. */
			z_win_parse_rect(&win, &msg.obj);
			aim_viewport();
			break;

		case Z_WM_CLOSE:
			return KG_KEY_QUIT;

		case Z_WM_KEY: {

			uint32_t packed = msg.obj.val.uint32;
			uint32_t keysym = Z_WM_UNPACK_KEY_KEYSYM(packed);
			bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;
			char ch;
			kg_key_t k;

			if (!pressed) {
				/* A release only ever un-lights the pad. Acting on
				 * releases would double every keystroke. */
				if (kg_pad_held() >= 0) kg_pad_set_held(-1, z_uptime_ticks());
				break;
			}

			if (keysym == Z_KEY_F2) { kg_toggle_mode(); break; }

			k = map_key(keysym, &ch);
			if (k == KG_KEY_NONE) break;

			if (!debounce_ok(k, ch, z_uptime_ticks())) break;

			/* Light the matching pad key, so a press on the real
			 * keyboard and a click on the drawn one look identical --
			 * which is the whole reason the pad helps a kid find a
			 * key at all. */
			kg_pad_set_held(kg_pad_find(k, ch), z_uptime_ticks());

			last_char = ch;
			out = k;
			break;

		}

		case Z_WM_MOUSE: {

			uint32_t packed = msg.obj.val.uint32;
			uint32_t buttons = Z_WM_UNPACK_MOUSE_BUTTONS(packed);
			int cx, cy, px, py;

			/* Press edge only. wm coalesces these but still sends one
			 * per pixel of movement, so a held button would otherwise
			 * fire continuously. */
			bool down = (buttons & 1) && !(last_buttons & 1);
			bool up = !(buttons & 1) && (last_buttons & 1);

			last_buttons = buttons;

			if (up) {
				if (kg_pad_held() >= 0) kg_pad_set_held(-1, z_uptime_ticks());
				break;
			}

			if (!down) break;

			/*
			 * One translation, both modes.
			 *
			 * This used to gate on z_win_mouse_content_xy() and then
			 * OVERRIDE its answer in game mode with the raw payload
			 * divided by two, on the belief that the pointer moved in
			 * screen pixels over a doubled viewport. Two errors in
			 * four lines:
			 *
			 *  - the guard ran first, so in game mode every click
			 *    outside the window's content rect was dropped before
			 *    the override could see it; and
			 *
			 *  - the halving was wrong anyway. gpu_cursor.v compares
			 *    against FRAMEBUFFER coordinates, so the pointer is
			 *    positioned in framebuffer space and comes out
			 *    pixel-doubled along with everything else, for free
			 *    (rtl/gpu/gpu_video.v says exactly this). A mouse
			 *    coordinate is already a framebuffer coordinate in
			 *    both modes; there was never anything to scale.
			 *
			 * With the viewport aimed at this window's content
			 * (aim_viewport), the content-relative answer is correct
			 * in game mode too, and is the same answer wm hit-tested
			 * with before it sent this.
			 */
			if (!z_win_mouse_content_xy(&win, packed, &cx, &cy)) break;

			px = cx;
			py = cy;

			if (px < 0 || px >= KG_W || py < 0 || py >= KG_H) break;

			{
				int idx = kg_pad_hit(px, py);

				if (idx >= 0) {
					/* A click on the pad becomes an ordinary key and
					 * never surfaces as KG_KEY_CLICK. This is what
					 * makes the six typing games mouse-playable
					 * without a line of mouse code in any of them. */
					const kg_padkey_t *pk = kg_pad_key(idx);

					if (!debounce_ok(pk->key, pk->ch, z_uptime_ticks()))
						break;

					kg_pad_set_held(idx, z_uptime_ticks());
					last_char = pk->ch;
					out = pk->key;
					break;
				}
			}

			click_x = px;
			click_y = py;
			out = KG_KEY_CLICK;
			break;

		}

		default:
			break;

		}

		if (out != KG_KEY_NONE) break;

	}

	if (need_redraw) {
		kg_repaint();
		/* The ack. wm does not block on it any more, so a missed one
		 * is quiet rather than a frozen screen -- which makes it
		 * easier to forget and no less wrong. */
		z_win_redraw_done(&win);
	}

	return out;
}

kg_key_t kg_getkey_timeout(uint32_t ticks)
{
	uint32_t deadline = z_uptime_ticks() + ticks;

	for (;;) {

		kg_key_t k = pump_once();
		uint32_t now;
		uint32_t wait;

		if (k != KG_KEY_NONE) return k;

		now = z_uptime_ticks();

		if (ticks && (int32_t)(now - deadline) >= 0) return KG_KEY_TIMEOUT;

		/*
		 * Sleep, do not spin. A process that spins is runnable forever
		 * and takes a full scheduler share from whatever is in the
		 * foreground -- measured at a quarter of the frame rate with
		 * four such processes alive (docs/app_runtime.md, "Idling").
		 *
		 * The wait is short only while a key is actually held down,
		 * to drive the pad's pulse. Otherwise it is as long as the
		 * caller's deadline allows, and indefinite when there is
		 * none: every input path wakes us, so there is nothing to
		 * poll for.
		 */
		if (kg_pad_held() >= 0) {
			kg_pad_animate(now);
			wait = Z_TICK_HZ / 10;
		} else if (ticks) {
			wait = deadline - now;
			if (wait > Z_TICK_HZ) wait = Z_TICK_HZ;
		} else {
			wait = 0;
		}

		z_proc_wait(wait);

	}
}

kg_key_t kg_getkey(void)
{
	return kg_getkey_timeout(0);
}

char kg_last_char(void) { return last_char; }
int kg_click_x(void) { return click_x; }
int kg_click_y(void) { return click_y; }

bool kg_key_is_next(kg_key_t k)
{
	return k == KG_KEY_DOWN || k == KG_KEY_RIGHT || k == KG_KEY_SPACE;
}

bool kg_key_is_prev(kg_key_t k)
{
	return k == KG_KEY_UP || k == KG_KEY_LEFT;
}

void kg_pause_ms(int ms)
{
	uint32_t ticks = ((uint32_t)ms * Z_TICK_HZ) / 1000u;
	uint32_t deadline = z_uptime_ticks() + ticks;

	if (!ticks) ticks = 1;

	for (;;) {

		uint32_t now;

		/* Keys pressed during a pause are DISCARDED -- the original
		 * does the same, and it matters more here: these screens are
		 * the ones a pre-reader sits through, and a keypress banked
		 * during one would fire into whatever came next. */
		if (pump_once() == KG_KEY_QUIT) return;

		now = z_uptime_ticks();
		if ((int32_t)(now - deadline) >= 0) return;

		z_proc_wait(deadline - now > Z_TICK_HZ ? Z_TICK_HZ : deadline - now);

	}
}
