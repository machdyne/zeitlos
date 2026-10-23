/*
 * pix -- the real editor on a build host, driven by a script, every
 * screen written out as a picture. make -C sw/apps/pix/tests render
 *
 * pix.c is compiled with -Dmain=game_main and runs unchanged; the
 * arcade runtime's machine half is sw/common/games/tests/ar_host.c,
 * and the boxes (open, save, prompts) answer pix_host_answer.
 *
 *   render_pix DIR icons     the release's dock.zic: drawing, selecting
 *   render_pix DIR font      a card font: going to a glyph
 *   render_pix DIR cursor    the pointer file, with its hot-spot guide
 *   render_pix DIR picture   a new 320 x 240 picture: shapes, fill, undo
 *   render_pix DIR colour    a new 16-colour sheet: drawing in colours,
 *                            fewer colours (F7), the palette (F8), full
 *                            screen
 *   render_pix DIR c256      a 256-colour sheet: every colour, the
 *                            palette, full screen on 8, 6 and 4 planes
 *   render_pix DIR cpic      a new 320 x 240 picture in 64 colours:
 *                            shapes in colours, a page of colours, undo,
 *                            full screen
 *   render_pix DIR import    a PNG opened as a colour picture, another
 *                            brought into it (Ctrl+I), full screen
 *   render_pix DIR cost      what each frame writes while drawing, in
 *                            256 colours full screen
 *   render_pix DIR wcolour   a 16-colour sheet in its colour window
 *                            (32 colours: the art at 16, the controls
 *                            above; 8 on six planes: the art at 4):
 *                            drawing, F7, a box, covered, full screen
 *   render_pix DIR wc256     the 256-colour sheet in its colour window
 *   render_pix DIR anim      mausrun's sprites: onion skin, an
 *                            animation made (Shift+F5) and played (F5),
 *                            the next frame (Ctrl+D)
 */

#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/games/zarcade.h"
#include "../../../common/zkbd.h"

int game_main(void);

/* zimg.c's file source is not used: pix reads a file whole first */
int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int h, void *b, int n) { (void)h; (void)b; (void)n; return -1; }
int fs_seek(int h, uint32_t o) { (void)h; (void)o; return 0; }
int fs_close_handle(int h) { (void)h; return 1; }
extern const char *pix_host_answer;

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define M(f, x, y, b)   { (f), ARH_MOUSE, (b), 0, NULL, (x), (y), 0, 0 }
#define TM(f, k, m)     { (f), ARH_TAP, (k), (m), NULL, 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }
#define WCOLOR(f, on)   { (f), ARH_WCOLOR, (on), 0, NULL, 0, 0, 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }

#define K_HOME     0x110004u
#define K_PGDN     0x110007u
#define K_F3       0x110012u
#define K_F5       0x110014u
#define K_F6       0x110015u
#define K_F7       0x110016u
#define K_F8       0x110017u
#define K_F11      0x11001au

static const ar_host_step_t icons[] = {
	SHOT(10, "01_dock_icons"),
	T(12, ARH_K_RIGHT), T(13, ARH_K_RIGHT), T(14, ARH_K_DOWN),
	T(15, ' '),
	SHOT(20, "02_cursor_and_dot"),
	/* a line with the mouse */
	T(22, 'l'),
	M(24, 60, 60, 1), M(26, 120, 140, 1), M(28, 120, 140, 0),
	SHOT(30, "03_line"),
	/* select, lift, move */
	T(32, 's'),
	M(34, 70, 40, 1), M(36, 150, 110, 1), M(38, 150, 110, 0),
	SHOT(40, "04_selected"),
	T(42, 'h'),
	SHOT(44, "05_flipped"),
	M(46, 100, 70, 1), SHOT(47, "05b_grabbed"), M(48, 130, 90, 1), SHOT(49, "05c_dragged"), M(50, 130, 90, 0),
	SHOT(52, "06_moved"),
	T(54, '\r'),
	/* zoom, the next cells, full screen */
	T(56, '-'), T(57, '-'),
	SHOT(60, "07_zoomed_out"),
	T(62, K_PGDN), T(63, K_PGDN),
	SHOT(66, "08_third_icon"),
	MODE(68, ARH_FULL_MONO),
	SHOT(72, "09_full_mono"),
	MODE(74, ARH_FULL_COLOR),
	SHOT(78, "10_full_color"),
	MODE(80, ARH_WINDOW),
	END(90)
};

static const ar_host_step_t font[] = {
	SHOT(10, "11_font_first"),
	T(12, K_F3),                     /* go to "g": the hook answers */
	SHOT(16, "12_font_g"),
	T(18, 'r'),                      /* a rectangle, to see the guides under it */
	M(20, 40, 40, 1), M(22, 200, 160, 1), SHOT(23, "13_font_rect_preview"), M(24, 200, 160, 0),
	T(26, K_PGDN), T(27, K_PGDN), T(28, K_PGDN),
	SHOT(32, "14_font_later"),
	END(50)
};

static const ar_host_step_t cursor[] = {
	SHOT(10, "21_cursor_normal"),
	T(12, K_PGDN),
	SHOT(16, "22_cursor_busy"),
	T(18, 'g'),                      /* fill the hourglass's top */
	M(20, 122, 30, 1), M(21, 122, 30, 0),   /* inside its top: row 2, column 7 */
	SHOT(24, "23_cursor_filled"),
	END(30)
};

static const ar_host_step_t picture[] = {
	SHOT(10, "31_picture_new"),
	T(12, 'r'),
	M(14, 20, 20, 1), M(16, 180, 140, 1), M(18, 180, 140, 0),
	T(20, 'l'),
	M(22, 20, 140, 1), M(24, 180, 20, 1), M(26, 180, 20, 0),
	T(28, 'g'),
	M(30, 150, 120, 1), M(31, 150, 120, 0),
	SHOT(34, "32_picture_drawn"),
	T(36, '+'),
	SHOT(42, "33_picture_zoomed"),
	TM(44, 0x1a, Z_KBD_MOD_CTRL),    /* Ctrl+Z: the fill goes */
	SHOT(48, "34_picture_undone"),
	END(56)
};

static const ar_host_step_t colour[] = {
	SHOT(8, "41_colour_new"),
	/* a box in red (2) filled with yellow (7), a line in blue (6) */
	T(10, '2'), T(11, 'r'),
	M(12, 40, 30, 1), M(13, 200, 160, 1), M(14, 200, 160, 0),
	T(16, '7'), T(17, 'g'), M(18, 120, 100, 1), M(19, 120, 100, 0),
	T(20, '6'), T(21, 'l'), M(22, 50, 150, 1), M(23, 190, 40, 1), M(24, 190, 40, 0),
	T(25, '5'), T(26, 'p'), M(27, 128, 90, 1), M(28, 140, 90, 1), M(29, 140, 90, 0),
	SHOT(32, "42_colour_drawn"),
	T(34, K_F7), SHOT(38, "43_colour_as4"),
	T(40, K_F7), SHOT(44, "44_colour_as2"),
	T(46, K_F7), SHOT(50, "45_colour_back"),
	T(52, K_F8), SHOT(56, "46_colour_palette"),
	T(58, ARH_K_DOWN), T(59, ARH_K_DOWN), T(60, ARH_K_RIGHT), T(61, ARH_K_RIGHT),
	SHOT(64, "47_colour_redder"),
	T(66, K_F8),
	MODE(68, ARH_FULL_COLOR),
	SHOT(72, "48_colour_full"),
	T(74, K_F8), SHOT(78, "49_colour_full_palette"), T(80, K_F8),
	MODE(82, ARH_WINDOW),
	END(90)
};

static const ar_host_step_t c256[] = {
	SHOT(8, "51_c256_sheet"),
	T(10, K_F8), SHOT(14, "52_c256_palette"), T(16, K_F8),
	MODE(18, ARH_FULL_COLOR),
	SHOT(22, "53_c256_full"),
	T(24, K_F7), SHOT(28, "54_c256_full_as64"),
	T(30, K_F7), SHOT(34, "55_c256_full_as16"),
	T(36, K_F7), T(37, K_F7), T(38, K_F7),
	MODE(40, ARH_WINDOW),
	SHOT(44, "56_c256_window"),
	END(50)
};

/* mausrun's sprites (sw/data/mausrun/sprites.zsp): four colours */
static const ar_host_step_t mausrun[] = {
	SHOT(8, "61_mausrun_sprites"),
	T(10, K_PGDN), T(11, K_PGDN), T(12, K_PGDN), T(13, K_PGDN),
	SHOT(16, "62_mausrun_cat"),
	MODE(18, ARH_FULL_COLOR),
	SHOT(22, "63_mausrun_full"),
	T(24, K_F7), SHOT(28, "64_mausrun_as2"),
	T(30, K_F7),
	MODE(32, ARH_WINDOW),
	END(40)
};

static const ar_host_step_t cpic[] = {
	SHOT(8, "81_cpic_new"),
	T(10, '2'), T(11, 'r'), TM(12, 'r', 0),           /* a filled red box */
	M(14, 30, 30, 1), M(15, 200, 150, 1), M(16, 200, 150, 0),
	TM(18, K_PGDN, Z_KBD_MOD_SHIFT),                  /* colours 16-31 */
	T(20, '5'), T(21, 'g'), M(22, 100, 90, 1), M(23, 100, 90, 0),
	TM(24, K_PGDN, Z_KBD_MOD_SHIFT), T(25, 'a'),      /* colour 42 */
	T(26, 'l'), M(27, 20, 170, 1), M(28, 240, 20, 1), M(29, 240, 20, 0),
	SHOT(32, "82_cpic_drawn"),
	TM(34, 0x1a, Z_KBD_MOD_CTRL),                     /* the line goes */
	SHOT(38, "83_cpic_undone"),
	TM(40, 0x19, Z_KBD_MOD_CTRL),                     /* and comes back */
	MODE(42, ARH_FULL_COLOR),
	SHOT(46, "84_cpic_full"),
	T(48, K_F7), SHOT(52, "85_cpic_full_as16"),
	T(54, K_F7), T(55, K_F7), T(56, K_F7),
	MODE(58, ARH_WINDOW),
	END(64)
};

static const ar_host_step_t import[] = {
	SHOT(8, "91_import_opened"),
	T(10, '+'), T(11, '+'),
	TM(12, 0x09, Z_KBD_MOD_CTRL),    /* Ctrl+I: the hook answers */
	SHOT(16, "92_import_piece"),
	T(18, '\r'),
	MODE(20, ARH_FULL_COLOR),
	SHOT(24, "93_import_full"),
	MODE(26, ARH_WINDOW),
	END(32)
};

/* mausrun's sprites again: its running mouse as an animation */
static const ar_host_step_t anim[] = {
	T(8, K_F3),                      /* to mouse_run2: the hook answers */
	SHOT(12, "71_anim_run2"),
	T(14, K_F6),                     /* onion: the cell before (no line yet: mouse_run1) */
	SHOT(18, "72_anim_onion_before"),
	T(20, K_F6),                     /* and after */
	SHOT(24, "73_anim_onion_both"),
	TM(26, K_F5, Z_KBD_MOD_SHIFT),   /* the animation: the hook answers */
	SHOT(30, "74_anim_made"),
	T(32, K_F6),                     /* onion off */
	T(34, K_F5),                     /* play: 120 ms is 7 frames */
	SHOT(36, "75_anim_play_a"),
	SHOT(43, "76_anim_play_b"),
	SHOT(50, "77_anim_play_c"),
	T(52, K_F5),                     /* stop */
	TM(54, 0x04, Z_KBD_MOD_CTRL),    /* Ctrl+D: mouse_run3, after run2 in "run" */
	T(56, 'p'), M(58, 128, 96, 1), M(59, 128, 96, 0),
	T(60, K_F6),
	SHOT(64, "78_anim_next_frame"),
	TM(66, K_F5, Z_KBD_MOD_SHIFT),   /* shows the line: answered with itself */
	SHOT(70, "79_anim_after_dup"),
	END(80)
};

/* What a frame costs while drawing, full screen in 256 colours: a pen
 * stroke, a line dragged, a box dragged, a selection drawn and moved.
 * The hook prints, a frame at a time, the blitter operations and the
 * framebuffer pixels (a plane each) the frame wrote. */
static const ar_host_step_t cost[] = {
	MODE(4, ARH_FULL_COLOR),
	T(8, '5'), T(9, 'p'),
	M(10, 100, 80, 1), M(11, 104, 80, 1), M(12, 108, 82, 1), M(13, 112, 84, 1), M(14, 112, 84, 0),
	T(16, 'l'),
	M(18, 60, 60, 1), M(19, 70, 66, 1), M(20, 80, 72, 1), M(21, 90, 78, 1), M(22, 90, 78, 0),
	T(24, 'r'),
	M(26, 60, 100, 1), M(27, 70, 106, 1), M(28, 80, 112, 1), M(29, 80, 112, 0),
	T(31, 's'),
	M(33, 40, 40, 1), M(34, 60, 50, 1), M(35, 80, 60, 1), M(36, 80, 60, 0),
	M(38, 50, 45, 1), M(39, 60, 50, 1), M(40, 70, 55, 1), M(41, 70, 55, 0),
	SHOT(44, "91_cost"),
	MODE(46, ARH_WINDOW),
	END(50)
};

/* The real boxes (PIX_BOXES: the screen toolkit, ui/sui.h) on pix's
 * screen, answered by keys: rename, delete, help, "save first?", open,
 * in the window, then full screen in colour, and save. */
#define K_F1 0x110010u
#define K_F2 0x110011u
#define K_DEL 0x110009u
static const ar_host_step_t boxes[] = {
	T(8, K_F2),                         /* rename */
	SHOT(10, "81_box_rename"),
	T(12, '_'), T(13, 'b'),
	SHOT(15, "82_box_rename_typed"),
	T(16, '\r'),
	TM(20, K_DEL, Z_KBD_MOD_SHIFT),     /* delete: Yes / No */
	SHOT(23, "83_box_delete"),
	T(25, 0x1b),                        /* no */
	T(30, K_F1),                        /* help, on pix's screen */
	SHOT(33, "84_box_help"),
	T(35, K_PGDN),
	SHOT(37, "85_box_help_pgdn"),
	T(39, K_F1),
	TM(44, 0x0f, Z_KBD_MOD_CTRL),       /* open: changed, so "save first?" */
	SHOT(47, "86_box_save_first"),
	T(49, 'n'),
	SHOT(52, "87_box_open"),
	T(54, 0x1b),
	MODE(58, ARH_FULL_COLOR),
	T(62, K_F2),
	SHOT(65, "88_box_rename_full"),
	T(67, 0x1b),
	T(70, K_F1),
	SHOT(73, "89_box_help_full"),
	T(75, 0x1b),
	TM(78, 0x13, Z_KBD_MOD_CTRL),       /* save: untitled, so the box */
	SHOT(81, "90_box_save_full"),
	T(83, 0x1b),
	MODE(86, ARH_WINDOW),
	TM(88, 0x11, Z_KBD_MOD_CTRL),       /* quit: "save first?" -- no */
	SHOT(91, "91_box_quit"),
	T(93, 'n'),
	END(96)
};

/* The colour window (docs/color.md): wm gives pix's window the colour,
 * takes it while another window covers it, gives it back. */
#define K_F1W 0x110010u
static const ar_host_step_t wcolour[] = {
	T(10, '2'), T(11, 'r'),
	M(12, 40, 30, 1), M(13, 200, 160, 1), M(14, 200, 160, 0),
	T(16, '7'), T(17, 'g'), M(18, 120, 100, 1), M(19, 120, 100, 0),
	SHOT(22, "a1_mono_window"),
	WCOLOR(24, 1),
	SHOT(28, "a2_colour_window"),
	/* drawing in it: a line in blue (6), a pixel in 13 */
	T(30, '6'), T(31, 'l'), M(32, 50, 150, 1), M(33, 190, 40, 1), M(34, 190, 40, 0),
	T(35, 'p'), T(36, 'd'), M(37, 128, 90, 1), M(38, 140, 90, 1), M(39, 140, 90, 0),
	SHOT(42, "a3_colour_window_drawn"),
	T(44, K_F7), SHOT(48, "a4_colour_window_as4"),
	T(50, K_F7), T(51, K_F7),
	SHOT(54, "a5_colour_window_back"),
	T(56, K_F1W), SHOT(60, "a6_colour_window_help"), T(62, 0x1b),
	COVER(64, 150, 100, 200, 160),
	SHOT(68, "a7_covered"),
	COVER(70, 0, 0, 0, 0),
	SHOT(74, "a8_uncovered"),
	MODE(76, ARH_FULL_COLOR),
	SHOT(80, "a9_full"),
	MODE(82, ARH_WINDOW),
	SHOT(86, "aa_window_again"),
	WCOLOR(88, 0),
	SHOT(92, "ab_focus_elsewhere"),
	TM(93, 0x11, Z_KBD_MOD_CTRL),       /* quit: "save first?" -- no */
	T(95, 'n'),
	END(98)
};

static const ar_host_step_t wc256[] = {
	SHOT(8, "b1_c256_window"),
	WCOLOR(10, 1),
	SHOT(14, "b2_c256_colour_window"),
	T(16, K_F8), SHOT(20, "b3_c256_colour_window_palette"), T(22, K_F8),
	END(30)
};

static const char *scene = "icons";

void ar_host_hook(uint32_t frame) {
	if (!strcmp(scene, "cost")) {
		static uint64_t ops, px;
		if (frame >= 8)
			printf("cost: frame %2u: %4lu ops, %7lu pixels\n", (unsigned)frame,
				(unsigned long)(arh_ops - ops), (unsigned long)(arh_pixels - px));
		ops = arh_ops; px = arh_pixels;
	}
	if (!strcmp(scene, "font") && frame == 11) pix_host_answer = "g";
	if (!strcmp(scene, "import") && frame == 11) pix_host_answer = "out/import/rgba.png";
	if (!strcmp(scene, "anim")) {
		if (frame == 7) pix_host_answer = "mouse_run2";
		if (frame == 25) pix_host_answer = "run 120 mouse_stand mouse_run1 mouse_run2";
		if (frame == 65) pix_host_answer = "=";    /* OK on what the box shows */
	}
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "out";
	setvbuf(stdout, NULL, _IOLBF, 0);
	const ar_host_step_t *s = icons;
	if (argc > 2) scene = argv[2];
	if (!strcmp(scene, "font")) {
		setenv("PIX_OPEN", "../../../data/font/zfn/m8x13.zfn", 1);
		s = font;
	} else if (!strcmp(scene, "cursor")) {
		setenv("PIX_OPEN", "cursor.zsp", 1);
		s = cursor;
	} else if (!strcmp(scene, "picture")) {
		setenv("PIX_NEW", "picture 320 240", 1);
		s = picture;
	} else if (!strcmp(scene, "colour")) {
		setenv("PIX_NEW", "sprite 24 20 16", 1);
		s = colour;
	} else if (!strcmp(scene, "c256")) {
		setenv("PIX_OPEN", "c256.zsp", 1);
		s = c256;
	} else if (!strcmp(scene, "import")) {
		setenv("PIX_OPEN", "out/import/many.png", 1);
		s = import;
	} else if (!strcmp(scene, "cpic")) {
		setenv("PIX_NEW", "picture 320 240 64", 1);
		s = cpic;
	} else if (!strcmp(scene, "anim")) {
		setenv("PIX_OPEN", "../../../data/mausrun/sprites.zsp", 1);
		s = anim;
	} else if (!strcmp(scene, "cost")) {
		setenv("PIX_OPEN", "c256.zsp", 1);
		s = cost;
	} else if (!strcmp(scene, "wcolour")) {
		setenv("PIX_NEW", "sprite 24 20 16", 1);
		setenv("PIX_BOXES", "1", 1);
		s = wcolour;
	} else if (!strcmp(scene, "wc256")) {
		setenv("PIX_OPEN", "c256.zsp", 1);
		s = wc256;
	} else if (!strcmp(scene, "boxes")) {
		setenv("PIX_NEW", "sprite 24 20 16", 1);
		setenv("PIX_BOXES", "1", 1);
		s = boxes;
	} else if (!strcmp(scene, "mausrun")) {
		setenv("PIX_OPEN", "../../../data/mausrun/sprites.zsp", 1);
		s = mausrun;
	} else {
		setenv("PIX_OPEN", "../../../data/icons/dock.zic", 1);
	}
	ar_host_run(s, dir);
	return game_main();
}
