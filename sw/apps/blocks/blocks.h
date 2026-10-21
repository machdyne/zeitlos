#ifndef BLOCKS_H
#define BLOCKS_H

/*
 * blocks -- the app: the screens, the picture, the sound. bk_game.[ch]
 * are the rules, bk_draw.c the picture, bk_sound.c the noise, blocks.c
 * the screens and the loop. See docs/blocks_app.md.
 */

#include "../../common/games/zarcade.h"

#include "bk_game.h"

/* -- where things are -- */

#define BK_CELL     10
#define WELL_X      110                 /* the visible well: 100 x 200 */
#define WELL_Y      24
#define WELL_W      (BK_W * BK_CELL)
#define WELL_H      (BK_VISIBLE * BK_CELL)

/* row r of the well (r >= BK_HIDDEN) is drawn at this y */
#define ROW_Y(r)    (WELL_Y + ((r) - BK_HIDDEN) * BK_CELL)
#define COL_X(c)    (WELL_X + (c) * BK_CELL)

/* the boxes either side */
#define HOLD_X      20
#define HOLD_Y      34
#define HOLD_W      72
#define HOLD_H      40
#define STATS_X     16
#define STATS_Y     84
#define NEXT_X      228
#define NEXT_Y      34
#define NEXT_W      76
#define NEXT_H      40
#define QUEUE_Y     80
#define QUEUE_H     140
#define SAY_X       12                  /* the announcer: FOUR!, combos */
#define SAY_Y       170
#define SAY_W       88
#define SAY_H       56

/* -- palette (docs/blocks_app.md, "Colour") --
 *
 * Index = the shape's colour in planes 0-2, plus plane 3 for its light
 * edge: shape s (1..7) is drawn in s with an edge in s + 8. That leaves
 * 0 (black) and 8 (the theme: frame, backdrop, grey blocks). 15, the
 * L's edge, is white, and is the white of text and flashes too. */
enum {
	BK_C_BLACK = 0,
	BK_C_THEME = 8,
	BK_C_WHITE = 15
};

typedef enum {
	ST_TITLE = 0,
	ST_SCORES,
	ST_READY,
	ST_PLAY,
	ST_PAUSE,
	ST_FLASH,          /* rows complete: flashing */
	ST_FALL,           /* the stack dropping into the gap */
	ST_CURTAIN,        /* topped out: the well greys over */
	ST_OVER,
	ST_NAME
} bk_state_t;

/* The menu, top to bottom. MENU_SKILL is EASY / NORMAL / HARD. */
enum { MENU_SKILL = 0, MENU_ENDLESS, MENU_LITTLE, MENU_40, MENU_3MIN,
	MENU_SCORES, MENU_ITEMS };

/* The game a menu line starts (MENU_ENDLESS .. MENU_3MIN). */
bk_mode_t bk_menu_mode(int menu);
/* The music: calm at the start, the whole band at higher levels. */
void bk_music(void);

#define FLASH_FRAMES 16
#define FALL_FRAMES  10

typedef struct {
	bk_state_t st;
	uint32_t st_frames;

	bk_game_t g;
	bk_mode_t mode;
	int menu;
	int start_level;          /* endless and little, 1..10 (easy 1..8) */
	int score_view;

	ar_table_t table;
	int rank;
	char name[4];
	int name_pos;

	/* the clear: which rows, and how far each surviving row falls */
	uint32_t rows;            /* g.clear_rows, kept while it plays */
	int fall_px[BK_H];        /* current pixel offset of each row */
	int fall_to[BK_H];        /* rows it falls in all */
	bool copy_failed;

	int curtain_row;          /* ST_CURTAIN: rows greyed so far */

	/* input */
	int das_dir, das_count;

	/* what the panels show, for dirtying when it changes */
	uint32_t shown_score;
	int shown_level, shown_lines;
	uint32_t shown_time;
	bool hud_dirty, queue_dirty;

	uint32_t best;            /* the table's top, for the panel */
} bk_app_t;

extern bk_app_t bk;

/* bk_draw.c */
bool bk_draw_init(void);
void bk_paint(void *user, const ar_rect_t *r);
void bk_before_paint(void *user);
void bk_draw_frame(uint32_t dt);
void bk_draw_events(void);
void bk_draw_reset(void);
void bk_draw_well_dirty(void);
void bk_draw_rows_dirty(uint32_t rows);
void bk_draw_cell_dirty(int x, int y);
void bk_draw_piece_cells_dirty(const bk_piece_t *p);
bool bk_panel_rect(ar_rect_t *r);
void bk_say(const char *big, const char *small, int colour);
void bk_draw_fall_start(void);
bool bk_draw_fall_step(void);       /* false when it has landed */
void bk_theme(int level);
void bk_mono_inks(ar_ink_t *ink);

extern const uint16_t bk_base_palette[16];
extern const char *const bk_mode_names[];

/* blocks.c: time as mm:ss.c for a number of frames */
void bk_format_time(char *out, uint32_t frames);

#endif
