#ifndef BR_GAME_H
#define BR_GAME_H

/*
 * bricks -- the rules. No drawing, no sound, no Zeitlos: a wall of
 * bricks, a paddle, up to eight balls, and what happens on each frame.
 * tests/test_rules.c drives this file directly; the picture reads the
 * state and the events, never the other way round.
 *
 * -- coordinates --
 *
 * Pixels of the 320x240 play picture. The field inside the walls is
 * BR_FX0..BR_FX1 across and from BR_FY0 down to the bottom edge; a ball
 * that falls past the bottom is gone. Bricks are 16x8, BR_COLS x BR_ROWS
 * of them from (BR_FX0, BR_BY0), so every brick sits on the runtime's
 * 8x8 tile grid. Balls keep their position in 1/256 pixel.
 *
 * -- the ball's direction is an angle, not a vector --
 *
 * A ball holds one of eight angles from the vertical (8..63 degrees)
 * and two signs. Walls and bricks flip a sign; only the paddle chooses
 * a new angle, by where on the paddle the ball lands: the middle sends
 * it nearly straight up, the ends out wide. The speed is one number for
 * every ball, so speeding up or slowing down cannot bend a path, and no
 * bounce can ever leave a ball flat enough to rattle sideways for ever.
 *
 * -- no tunnelling --
 *
 * A frame's movement is split into steps of at most one pixel on each
 * axis; each step moves across, then down, and a move that would put the
 * ball into a brick or a wall is refused and that axis flips instead.
 * So a ball never overlaps a brick, at any speed (the tests check this
 * after every step of millions).
 */

#include <stdint.h>
#include <stdbool.h>

/* the field */
#define BR_FX0      8
#define BR_FX1      312
#define BR_FY0      24
#define BR_COLS     19
#define BR_ROWS     14
#define BR_BW       16
#define BR_BH       8
#define BR_BY0      32
#define BR_BRICK_X(c) (BR_FX0 + (c) * BR_BW)
#define BR_BRICK_Y(r) (BR_BY0 + (r) * BR_BH)

#define BR_PADDLE_Y 224
#define BR_PADDLE_H 6
#define BR_PADDLE_W 32
#define BR_PADDLE_WIDE 48
#define BR_BALL     6
#define BR_FLOOR    240          /* a ball whose top passes this is lost */

#define BR_MAX_BALLS 8
#define BR_MAX_CAPS  3
#define BR_MAX_SHOTS 6
#define BR_LIVES     3

/* bricks */
enum {
	BR_EMPTY = 0,
	BR_C1, BR_C2, BR_C3, BR_C4, BR_C5, BR_C6,   /* one hit */
	BR_SILVER,                                  /* several hits */
	BR_GOLD,                                    /* never breaks */
	BR_TYPES
};

/* the paddle's state: one at a time, the newest wins */
enum { BR_P_NONE = 0, BR_P_WIDE, BR_P_CATCH, BR_P_LASER };

/* capsules: what a broken brick sometimes lets fall */
enum {
	BR_CAP_NONE = 0,
	BR_CAP_WIDE,       /* E: a longer paddle */
	BR_CAP_MULTI,      /* M: one ball becomes three */
	BR_CAP_SLOW,       /* S: the balls slow down */
	BR_CAP_CATCH,      /* C: the paddle holds the ball */
	BR_CAP_LASER,      /* L: the paddle fires */
	BR_CAP_LIFE,       /* P: another paddle */
	BR_CAPS
};

/* What happened during a step, for the picture and the sound. */
enum {
	BR_EV_WALL = 1,        /* x, y: a ball hit a wall */
	BR_EV_PADDLE,          /* x: a ball came off the paddle */
	BR_EV_HIT,             /* r, c, value: a silver brick took a hit (value: hits left) */
	BR_EV_BREAK,           /* r, c, brick, value: a brick broke (value: chain) */
	BR_EV_GOLD,            /* r, c */
	BR_EV_CAP_DROP,        /* value: capsule type; x, y */
	BR_EV_CAP_TAKE,        /* value: capsule type; x */
	BR_EV_CAP_MISS,        /* a capsule fell past */
	BR_EV_LAUNCH,          /* a held ball left the paddle */
	BR_EV_CATCH,           /* x: the paddle caught a ball */
	BR_EV_FIRE,            /* x: two bolts */
	BR_EV_SHOT_END,        /* x, y: a bolt hit the top */
	BR_EV_BALL_LOST,       /* x: one ball fell, others are still in play */
	BR_EV_LOST,            /* x: the last ball fell */
	BR_EV_CLEAR,           /* every brick that can break is broken */
	BR_EV_LIFE,            /* an extra paddle, by points or capsule */
	BR_EV_SPEED            /* the balls got faster */
};

typedef struct {
	uint8_t type, r, c, value;
	uint8_t brick;         /* BREAK: what broke */
	int16_t x, y;
} br_event_t;

#define BR_MAX_EV 32

typedef struct {
	int32_t x, y;          /* top-left, 1/256 pixel */
	uint8_t ang;           /* 0..7: 8 to 63 degrees from vertical */
	int8_t sx, sy;         /* -1 or 1 */
	bool live;
	bool stuck;            /* held on the paddle */
	int16_t stuck_off;     /* where on it: pixels from its left end */
	int16_t stuck_left;    /* frames until it lets go by itself */
} br_ball_t;

typedef struct {
	int16_t x;
	int32_t y;             /* 1/256 pixel */
	uint8_t type;          /* BR_CAP_*, 0 = none */
} br_cap_t;

typedef struct {
	int16_t x, y;
	bool live;
} br_shot_t;

typedef struct {
	uint8_t brick[BR_ROWS][BR_COLS];
	uint8_t hp[BR_ROWS][BR_COLS];
	int bricks_left;       /* that can break */

	int level;             /* 1.. */
	int px, pw;            /* the paddle: left edge, width */
	int power;             /* BR_P_* */

	br_ball_t ball[BR_MAX_BALLS];
	br_cap_t cap[BR_MAX_CAPS];
	br_shot_t shot[BR_MAX_SHOTS];
	int fire_wait;         /* frames until the laser can fire again */

	int32_t speed;         /* 1/256 pixel a frame */
	int32_t speed_base;    /* this level's serve speed */
	bool top_boost;        /* the top wall's boost, taken this serve */
	int hits;              /* bricks hit this level, for speeding up */
	int chain;             /* bricks hit since the paddle last was */
	int drop_wait;         /* bricks to break before a capsule may fall */

	uint32_t score;
	uint32_t next_life;    /* points for the next extra paddle */
	int lives;             /* paddles left, the one in play included */
	uint32_t frame;

	bool cleared;          /* the level is done: balls frozen */
	bool over;             /* no paddles left */

	br_event_t ev[BR_MAX_EV];
	int nev;

	uint32_t (*rand_below)(uint32_t n);
} br_game_t;

/* A new game from `level`. */
void br_new(br_game_t *g, int level, uint32_t (*rand_below)(uint32_t n));

/* The next level, with the paddle served. */
void br_next_level(br_game_t *g);

/* A fresh ball held on the middle of the paddle (after a loss). */
void br_serve(br_game_t *g);

/* Move the paddle: by dx, or with its middle to x. Held balls go with
 * it. Clamped to the field. */
void br_paddle_move(br_game_t *g, int dx);
void br_paddle_centre(br_game_t *g, int x);

/* The button: let go of held balls, or fire the laser. */
void br_fire(br_game_t *g);

/* Advance one frame. Events are appended to g->ev; the caller clears
 * them with br_clear_events() once it has looked. */
void br_step(br_game_t *g);
void br_clear_events(br_game_t *g);

/* Balls in play, held ones included. */
int br_balls(const br_game_t *g);

/* Points for breaking a brick of `type` on `level`. */
uint32_t br_points(int type, int level);

/* The layouts. */
int br_levels(void);
const char *br_level_name(int level);

/* Which brick, if any, covers pixel (x, y): true and its row and
 * column. */
bool br_brick_at(const br_game_t *g, int x, int y, int *r, int *c);

/* The autopilot: where the paddle's middle should go, and whether to
 * press the button. For the title and the host renders. */
int br_ai_target(const br_game_t *g, bool *fire);

#ifdef BR_TESTS
/* Load an exact layout (rows of BR_COLS characters, NULL-terminated). */
void br_test_layout(br_game_t *g, const char *const *rows);
#endif

#endif
