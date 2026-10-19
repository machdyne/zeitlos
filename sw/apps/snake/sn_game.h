#ifndef SN_GAME_H
#define SN_GAME_H

/*
 * snake -- the rules. No drawing, no sound, no Zeitlos: a grid, one or
 * two snakes, food, and what happens on each step. The host tests drive
 * this file directly (tests/test_rules.c), and so does the drawing --
 * which reads the state and the events, never the other way round.
 *
 * -- smooth movement --
 *
 * A snake moves one cell per MOVE, and a move takes some frames: its
 * progress runs from 0 to SN_CELL_PX px (in 8.8 fixed point) and the
 * drawing slides the head into the next cell and the tail out of the
 * last one in between. Everything that is a rule -- where the head is
 * going, whether that kills it -- is decided when a move STARTS, so
 * the head never visibly pokes into a wall it then turns away from.
 * Eating happens when the move completes, when the head is on the food.
 *
 * Turns are queued (two deep) and taken one per move, so pressing UP
 * then LEFT quickly makes both turns rather than losing the first. A
 * turn back into the body (reversing) is ignored.
 */

#include <stdint.h>
#include <stdbool.h>

#define SN_GW 40               /* grid, in cells */
#define SN_GH 28
#define SN_CELL_PX 8
#define SN_MAXLEN (SN_GW * SN_GH)
#define SN_PLAYERS 2

/* cell contents */
enum {
	SN_EMPTY = 0,
	SN_WALL,
	SN_BODY,        /* + owner in sn_cell_t.who */
	SN_BLOCKED      /* invisible wall (the title's menu panel) */
};

enum { SN_RIGHT = 0, SN_DOWN, SN_LEFT, SN_UP, SN_NODIR = 0xff };

typedef struct {
	uint8_t what;
	uint8_t who;            /* SN_BODY: player */
	uint8_t col;            /* SN_BODY: colour slot 0..3 */
	uint8_t links;          /* SN_BODY: bit d = connected in direction d */
} sn_cell_t;

typedef struct {
	bool alive, active;
	uint8_t dir;            /* direction of the current move */
	uint8_t queue[2];       /* turns waiting */
	uint8_t nq;
	/* body: a ring of cell indices, head at [head], tail at [tail] */
	uint16_t body[SN_MAXLEN];
	uint16_t head, tail, len;
	uint16_t next;          /* the cell the current move enters */
	bool tail_moves;        /* the tail leaves its cell this move */
	uint16_t grow;          /* cells still to grow */
	uint16_t progress;      /* 8.8 px into the current move */
	uint16_t speed;         /* 8.8 px per frame */
	uint8_t colour_next;    /* rotating colour slot for new cells */
	uint32_t score;
	uint16_t eaten;
	uint8_t wins;
} sn_snake_t;

typedef enum {
	SN_MODE_CLASSIC = 0,    /* walls around the edge, faster per food */
	SN_MODE_OPEN,           /* no walls: the edges wrap */
	SN_MODE_MAZE,           /* levels with inner walls */
	SN_MODE_VERSUS,         /* two players */
	SN_MODE_TITLE,          /* the title screen's self-playing snake */
	SN_MODES
} sn_mode_t;

/* What happened during a step, for the drawing and the sound. */
enum {
	SN_EV_EAT = 1,          /* p, cell, value */
	SN_EV_BONUS_EAT,        /* p, cell, value */
	SN_EV_BONUS_SHOW,       /* cell */
	SN_EV_BONUS_GONE,       /* cell */
	SN_EV_FOOD,             /* cell: new food placed */
	SN_EV_DIE,              /* p, cell where it hit */
	SN_EV_MOVE,             /* p: a move started */
	SN_EV_LEVEL_DONE,       /* maze level eaten through */
	SN_EV_SPEEDUP           /* p */
};

typedef struct {
	uint8_t type, p;
	uint16_t cell;
	uint16_t value;
} sn_event_t;

#define SN_MAX_EV 16

typedef struct {
	sn_mode_t mode;
	uint8_t players;
	uint8_t level;          /* maze: 0.. */
	bool wrap;
	sn_cell_t grid[SN_GW * SN_GH];
	sn_snake_t s[SN_PLAYERS];

	uint16_t food;          /* cell, or 0xffff */
	uint16_t bonus;         /* cell, or 0xffff */
	uint16_t bonus_left;    /* frames until it goes */
	uint16_t bonus_total;
	uint16_t foods_to_bonus;
	uint16_t level_goal;    /* maze: foods to clear the level */

	uint32_t frame;
	bool over;              /* every snake that can die has */

	sn_event_t ev[SN_MAX_EV];
	int nev;

	/* the source of randomness: the game's, injected for the tests */
	uint32_t (*rand_below)(uint32_t n);
} sn_game_t;

#define SN_NONE 0xffffu

static inline int sn_cx(uint16_t c) { return c % SN_GW; }
static inline int sn_cy(uint16_t c) { return c / SN_GW; }
static inline uint16_t sn_cell(int x, int y) { return (uint16_t)(y * SN_GW + x); }

/* Set up a game. `level` matters for the maze. */
void sn_new(sn_game_t *g, sn_mode_t mode, int level,
	uint32_t (*rand_below)(uint32_t n));

/* A turn from the player. */
void sn_turn(sn_game_t *g, int p, int dir);

/* Advance one frame. Events land in g->ev (cleared first). */
void sn_step(sn_game_t *g);

/* The neighbour of `c` in direction d, wrapping or not; SN_NONE off
 * the grid. */
uint16_t sn_neighbour(const sn_game_t *g, uint16_t c, int d);

/* The direction from a to its neighbour b, or SN_NODIR. */
int sn_dir_between(const sn_game_t *g, uint16_t a, uint16_t b);

/* Cell index of segment i (0 = tail) of player p. */
uint16_t sn_segment(const sn_snake_t *s, int i);

/* Remove the head segment of a dead snake (the dissolve): its cell, or
 * SN_NONE when nothing is left. */
uint16_t sn_pop_head(sn_game_t *g, int p);

/* Number of maze levels. */
int sn_levels(void);

/* The title's autopilot: choose a turn for player p. */
void sn_autopilot(sn_game_t *g, int p);

/* Speed in 8.8 px per frame for cells per second. */
uint16_t sn_speed_for(int cells_per_sec_x10);

#endif
