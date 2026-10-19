#ifndef BRICKS_H
#define BRICKS_H

/*
 * bricks -- the app: the screens, the picture, the sound. br_game.[ch]
 * are the rules, br_draw.c the picture, br_sound.c the noise, bricks.c
 * the screens and the loop. See docs/bricks_app.md.
 */

#include "../../common/games/zarcade.h"

#include "br_game.h"

/* -- where things are (the field itself: br_game.h) -- */

#define HUD_H       16          /* the status line along the top */
#define WALL_Y      16          /* the top wall: 16..23 */
#define WALL_T      8           /* wall thickness */

/* -- palette (docs/bricks_app.md, "Colour") -- */
enum {
	BR_C_BLACK = 0,
	BR_C_BACK,          /* the backdrop's pattern: changes with the round */
	BR_C_WALL,
	BR_C_WALL_HI,       /* the walls' light edge, labels */
	BR_C_B1 = 4,        /* 4..9: the six brick colours */
	BR_C_SILVER = 10,
	BR_C_GOLD,          /* shimmers */
	BR_C_PADDLE,
	BR_C_LIGHT,         /* the light in the paddle; flashes when it hits */
	BR_C_GLOW,          /* bolts, trails, capsule gleam: cycles */
	BR_C_WHITE
};

typedef enum {
	ST_TITLE = 0,
	ST_SCORES,
	ST_READY,           /* the bricks arrive; ROUND n */
	ST_PLAY,
	ST_PAUSE,
	ST_LOST,            /* the paddle blows up */
	ST_CLEAR,           /* every brick gone */
	ST_OVER,
	ST_NAME
} br_state_t;

enum { MENU_PLAY = 0, MENU_SCORES, MENU_ITEMS };

#define READY_ROW_FRAMES 3        /* the cascade: a row every 3 frames */
#define READY_FRAMES     110
#define READY_AGAIN      50       /* after losing a paddle: no cascade */
#define LOST_FRAMES      90
#define CLEAR_FRAMES     100

typedef struct {
	br_state_t st;
	uint32_t st_frames;

	br_game_t g;
	int menu;
	int start_level;          /* 1..br_levels() */

	ar_table_t table;
	int rank;
	char name[4];
	int name_pos;

	int ready_rows;           /* ST_READY: rows of bricks shown so far */
	bool ready_quick;         /* ST_READY after a loss: all there */

	/* input */
	int key_speed;            /* the paddle under the arrow keys */
	bool mouse_left;          /* last mouse button state, for clicks */

	/* what the status line shows, to repaint it only when it changes */
	uint32_t shown_score, shown_best;
	int shown_lives, shown_level;

	uint32_t best;
} br_app_t;

extern br_app_t br;

/* br_draw.c */
bool br_draw_init(void);
void br_paint(void *user, const ar_rect_t *r);
void br_draw_frame(uint32_t dt);
void br_draw_events(void);
void br_draw_reset(void);
void br_draw_field_dirty(void);
void br_draw_brick_dirty(int r, int c);
void br_draw_hud_dirty(void);     /* only the parts that changed */
bool br_panel_rect(ar_rect_t *r);
void br_draw_lost(void);          /* the paddle blows up */
void br_draw_level(int level);    /* the backdrop for a round */
void br_mono_inks(ar_ink_t *ink);

extern const uint16_t br_base_palette[16];

#endif
