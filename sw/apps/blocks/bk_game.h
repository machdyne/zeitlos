#ifndef BK_GAME_H
#define BK_GAME_H

/*
 * blocks -- the rules. No drawing, no sound, no Zeitlos: a well, seven
 * shapes of four cells, and what happens each frame. The host tests
 * drive this file directly (tests/test_rules.c), and so does the
 * picture, which reads the state and the events.
 *
 * -- the well --
 *
 * 10 columns by 22 rows, row 0 at the TOP. Rows 0 and 1 are above the
 * visible well: pieces appear there and drop into view, and a stack
 * that reaches them is the end. Rows 2..21 are what is drawn.
 *
 * -- the shapes --
 *
 * Seven, named by the letter each looks like: I O T S Z J L. Each turns
 * in a box (4x4 for I, 2x2 for O, 3x3 for the rest) about the box's
 * centre, and a turn that does not fit is tried again at up to four
 * nearby offsets -- the wall kicks -- before it is refused. The kick
 * tables are the widely used ones, so a player's habits carry over.
 *
 * -- the order of pieces --
 *
 * A shuffled bag of all seven, dealt out, then a new bag: never more
 * than twelve pieces between two of the same, never a drought.
 *
 * -- clearing --
 *
 * When a piece locks and completes rows, the rows are NOT removed at
 * once: they are marked (g->clear_rows) and the game waits, so the
 * picture can flash them and drop the stack. bk_clear_done() removes
 * them and brings the next piece. The tests call it straight away.
 */

#include <stdint.h>
#include <stdbool.h>

#define BK_W 10
#define BK_H 22
#define BK_HIDDEN 2
#define BK_VISIBLE (BK_H - BK_HIDDEN)
#define BK_QUEUE 5

enum { BK_EMPTY = 0, BK_I, BK_O, BK_T, BK_S, BK_Z, BK_J, BK_L, BK_GREY,
	/* little pieces (BK_MODE_LITTLE): one, two and three cells */
	BK_DOT, BK_DUO, BK_I3, BK_V3,
	/* the narrower well's side walls: never cleared, never moved */
	BK_WALL,
	BK_TYPES };

typedef enum {
	BK_MODE_ENDLESS = 0,   /* score; a level every ten lines */
	BK_MODE_40,            /* forty lines against the clock */
	BK_MODE_3MIN,          /* score in three minutes */
	BK_MODE_LITTLE,        /* endless, little pieces, a well 8 wide */
	BK_MODES
} bk_mode_t;

enum {
	BK_EV_SPAWN = 1,       /* type */
	BK_EV_MOVE,
	BK_EV_ROTATE,          /* value: kick used (0 = none) */
	BK_EV_LAND,            /* the piece touched down (first time) */
	BK_EV_LOCK,            /* type; x, y of the piece */
	BK_EV_HARD_DROP,       /* value: rows fallen */
	BK_EV_HOLD,
	BK_EV_CLEAR,           /* value: rows; flags below */
	BK_EV_LEVEL,           /* value: the new level */
	BK_EV_OVER,            /* topped out */
	BK_EV_DONE,            /* the mode's goal: 40 lines, or time */
	BK_EV_RESCUE           /* easy: topped out, the top half cleared;
	                          value: rescues left */
};

/* How hard: the values of zarcade's AR_SKILL_*, kept apart so the rules
 * need nothing from the runtime. */
enum { BK_SKILL_EASY = 0, BK_SKILL_NORMAL, BK_SKILL_HARD };

#define BK_RESCUES      3      /* easy: top-outs forgiven per game */
#define BK_LITTLE_L     1      /* little pieces: the walls' columns */
#define BK_LITTLE_R     (BK_W - 2)

#define BK_F_TWIST  1      /* the T turned into place: a twist */
#define BK_F_B2B    2      /* back to back: four, or twist, after another */
#define BK_F_PERFECT 4     /* the well is empty after it */

typedef struct {
	uint8_t type, flags;
	uint8_t piece, rot;    /* the piece as it was at the event */
	int8_t x, y;
	int16_t value;
	int16_t combo;
	uint32_t points;
} bk_event_t;

#define BK_MAX_EV 16

typedef struct {
	uint8_t type;          /* BK_I .. BK_L, 0 none */
	uint8_t rot;           /* 0..3 */
	int8_t x, y;           /* box top-left, well coordinates */
} bk_piece_t;

typedef struct {
	bk_mode_t mode;
	uint8_t skill;         /* BK_SKILL_* */
	bool little;           /* BK_MODE_LITTLE: walls in columns 0 and 9 */
	uint8_t rescues;       /* easy: top-outs still forgiven */
	uint8_t max_level;
	uint8_t well[BK_H][BK_W];

	bk_piece_t cur;
	uint8_t hold;
	bool hold_used;        /* once per piece */
	uint8_t queue[BK_QUEUE];
	uint8_t bag[7];
	uint8_t bag_n;

	uint32_t score;
	uint16_t lines;
	uint8_t level, start_level;
	int16_t combo;         /* -1 when the last piece cleared nothing */
	bool b2b;              /* the last clear was a four or a twist */

	uint32_t fall;         /* 8.8 rows accumulated toward the next drop */
	bool soft;             /* soft drop held */
	uint8_t lock_timer;
	uint8_t lock_resets;
	bool landed;           /* on the stack (or floor) now */
	bool last_rotate;      /* the last move was a turn (for twists) */
	uint8_t last_kick;

	uint32_t clear_rows;   /* rows waiting to go (bit r = row r) */
	uint8_t clear_count;

	uint32_t frames;       /* time in play */
	bool over, done;

	bk_event_t ev[BK_MAX_EV];
	int nev;

	uint32_t (*rand_below)(uint32_t n);
} bk_game_t;

/* The four cells of a shape in a rotation, box coordinates (y down). */
void bk_cells(int type, int rot, int8_t out[4][2]);

/* A game at normal skill: as it always was. */
void bk_new(bk_game_t *g, bk_mode_t mode, int start_level,
	uint32_t (*rand_below)(uint32_t n));
void bk_new_skill(bk_game_t *g, bk_mode_t mode, int start_level, int skill,
	uint32_t (*rand_below)(uint32_t n));

/* Cells a shape really has (1..4; bk_cells() always fills four, the
 * little ones repeating their last). */
int bk_ncells(int type);

/* Lines for the next level, and the frames a piece may lie before it
 * locks, at this game's skill. */
int bk_lines_per_level(const bk_game_t *g);
int bk_lock_frames(const bk_game_t *g);
/* Rows a frame x256 for this game, now. */
uint32_t bk_gravity_now(const bk_game_t *g);
/* The playable columns: 0..9, or 1..8 with little pieces. */
int bk_col0(const bk_game_t *g);
int bk_col1(const bk_game_t *g);

/* Player actions; true if something moved. */
bool bk_move(bk_game_t *g, int dx);
bool bk_rotate(bk_game_t *g, int dir);         /* +1 clockwise, -1 not */
bool bk_hold(bk_game_t *g);
void bk_hard_drop(bk_game_t *g);
void bk_soft(bk_game_t *g, bool on);

/* One frame: gravity, lock delay, the clock. Events go to g->ev, as the
 * actions' do; bk_clear_events() empties it, once a frame, before the
 * input is applied. */
void bk_step(bk_game_t *g);
void bk_clear_events(bk_game_t *g);

/* After the picture has shown a clear: remove the rows, next piece. */
void bk_clear_done(bk_game_t *g);
bool bk_clearing(const bk_game_t *g);

/* Where the current piece would land (its y). */
int bk_ghost_y(const bk_game_t *g);

/* Does `p` fit in the well? */
bool bk_fits(const bk_game_t *g, const bk_piece_t *p);

/* Rows a frame, x256, at a level. */
uint32_t bk_gravity(int level);

/* Lines to the goal (40) or frames left (3 minutes), for the HUD. */
int bk_lines_goal(const bk_game_t *g);
uint32_t bk_frames_left(const bk_game_t *g);

/* -- the autopilot: where should this piece go? -- */
typedef struct {
	uint8_t rot;
	int8_t x;
	bool use_hold;
} bk_plan_t;

bool bk_ai_plan(const bk_game_t *g, bk_plan_t *out);

#define BK_FRAMES_3MIN (3u * 60u * 60u)
#define BK_GOAL_40 40

#endif
