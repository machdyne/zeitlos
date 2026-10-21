#ifndef SNAKE_H
#define SNAKE_H

/*
 * snake -- the app: what is on screen and why. sn_game.[ch] are the
 * rules, sn_draw.c the picture, sn_sound.c the noise, snake.c the
 * screens and the loop. See docs/snake_app.md.
 */

#include "../../common/games/zarcade.h"

#include "sn_game.h"

/* Where the grid is: 8x8 cells, below a 16-pixel status line. */
#define SN_PLAY_Y   16
#define SN_PX(c)    (sn_cx(c) * SN_CELL_PX)
#define SN_PY(c)    (SN_PLAY_Y + sn_cy(c) * SN_CELL_PX)

/* -- palette (docs/snake_app.md, "Colour") -- */
enum {
	SN_C_BG = 0,        /* black: also the frame the shake shows */
	SN_C_GRID,          /* the dots of the floor */
	SN_C_WALL,
	SN_C_LIGHT,         /* wall mortar, labels */
	SN_C_P1 = 4,        /* 4..7: player 1's body, cycled */
	SN_C_P2 = 8,        /* 8..11: player 2's */
	SN_C_FOOD = 12,
	SN_C_GOLD,          /* the bonus, titles */
	SN_C_FIRE,          /* sparks */
	SN_C_WHITE
};

typedef enum {
	ST_TITLE = 0,
	ST_SCORES,
	ST_READY,
	ST_PLAY,
	ST_PAUSE,
	ST_DYING,
	ST_LEVEL,
	ST_ROUND,
	ST_OVER,
	ST_NAME
} sn_state_t;

/* The menu, in order. */
enum {
	MENU_SKILL = 0,         /* EASY / NORMAL / HARD, Left and Right */
	MENU_CLASSIC,
	MENU_OPEN,
	MENU_MAZE,
	MENU_VERSUS,
	MENU_SCORES,
	MENU_ITEMS
};

#define SN_WINS_NEEDED 3

typedef struct {
	sn_state_t st;
	uint32_t st_frames;       /* frames in this state */

	sn_game_t g;              /* the game, or the title's demo */
	int menu;
	sn_mode_t mode;           /* the mode being played */
	int score_view;           /* ST_SCORES: which mode's table */

	ar_table_t table;
	int rank;
	char name[4];
	int name_pos;

	/* dying: who, and the countdown to the next segment */
	bool dissolving[SN_PLAYERS];
	int dissolve_wait;
	int round_winner;         /* -1 draw */

	bool hud_dirty;
	uint32_t hiscore;
} sn_app_t;

extern sn_app_t sn;

/* sn_draw.c */
bool sn_draw_init(void);
void sn_paint(void *user, const ar_rect_t *r);
void sn_draw_frame(uint32_t dt);
void sn_draw_events(void);           /* effects for sn.g.ev */
void sn_draw_dirty_snakes(void);     /* the cells the snakes touch now */
void sn_draw_cell_dirty(uint16_t c);
void sn_draw_flash(int frames);
void sn_draw_reset(void);            /* popups and sparks gone */
void sn_draw_pop(uint16_t c, int p); /* a dissolving segment */
void sn_draw_celebrate(int frames);  /* walls cycle colours */
bool sn_panel_rect(ar_rect_t *r);    /* the current panel, if any */

extern const char *const sn_mode_names[];

#endif
