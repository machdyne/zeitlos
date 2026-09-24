#ifndef ZGAMES_BANK_H
#define ZGAMES_BANK_H

/*
 * Zeitlos -- the shared casino bank, /casino.dat.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * One pile of chips that every game in sw/apps spends from and pays
 * into, so that winning at roulette leaves you better off at
 * blackjack. Without it each game is its own sealed world and the
 * chips mean nothing.
 *
 * -- APPLY A DELTA, NEVER WRITE A BALANCE --
 *
 * This is the whole design and everything else follows from it.
 *
 * Two games can be open at once. If roulette writes "chips = 940" from
 * a copy it read two minutes ago, it silently erases whatever poker
 * won in between -- and neither app has done anything obviously wrong.
 * sw/common/zfsapp.h flags exactly this concurrency caveat for the
 * syscalls underneath.
 *
 * So the operation a game performs is zbank_adjust(), which re-reads
 * the file, applies the change to whatever is there NOW, and writes
 * that. A stale in-memory copy then costs nothing. The remaining
 * window is the few milliseconds between that read and that write, and
 * two games would have to land inside it on the same tick.
 *
 * There is no zbank_set_chips(), and that omission is deliberate. A
 * function that can write an absolute balance will eventually be
 * called with a stale one.
 *
 * -- two halves --
 *
 * The parser and the formatter are pure: no filesystem, no syscalls,
 * host-testable. The load/save/adjust wrappers are the thin part that
 * touches zfsapp. Same split sw/common/zcfg.h uses, and for the same
 * reason -- the interesting logic is the part that can be tested
 * without a device.
 *
 * -- the file --
 *
 *   # Zeitlos casino bank
 *   version: 1
 *   chips: 1000
 *   peak: 1450
 *   buyins: 1
 *   game.roulette.net: -120
 *   game.roulette.rounds: 40
 *
 * Text, in the same "key: value" shape as /zeitlos.cfg, so `cat
 * /casino.dat` from the console tells you something and a hand edit is
 * possible when a game gets it wrong. Binary would be smaller and
 * would make both of those false.
 *
 * -- what happens when it is damaged --
 *
 * PARSING IS LENIENT. Unknown lines are ignored, missing keys take
 * their defaults, and a truncated file still yields whatever it
 * contained up to the cut. A power cut mid-write should cost the last
 * round, not the bankroll.
 *
 * A file that exists, is not empty, and has no `chips:` line at all is
 * treated as CORRUPT rather than as a fresh bank. zbank_load() reports
 * it and zbank_adjust() refuses to write over it, because the one
 * unrecoverable outcome is overwriting a real balance with a default
 * one. An app that sees this should say so and offer a reset, not
 * quietly hand out a new stack.
 *
 * A file that is simply MISSING is not corrupt -- it is a first run,
 * and the defaults are correct.
 */

#include <stdint.h>
#include <stdbool.h>

/* The bank lives under /user, with the rest of what belongs to the
 * person rather than to the system. The directory is created on demand
 * -- see zbank_save() -- so a card that predates this still works.
 *
 * ZBANK_OLD_PATH is where it used to be. zbank_load() falls back to it
 * so an existing bankroll is not lost on the move; the next save writes
 * the new location and the old file is then just a stale copy. */
#define ZBANK_DIR         "/user"
#define ZBANK_PATH        "/user/casino.dat"
#define ZBANK_OLD_PATH    "/casino.dat"
#define ZBANK_VERSION     1

/* What a fresh bank hands over. */
#define ZBANK_START       1000

/* -- loans --
 *
 * There is no free money. A bank that tops itself up when it runs dry
 * makes the house edge meaningless: an exactly-computed 5.359% costs
 * nothing if losing is undone by asking.
 *
 * So running out means BORROWING, and a loan is recorded. Borrow 1000
 * and you owe 1200 -- the vig is charged once, at the counter, not as
 * interest that accrues. Nothing forces repayment; what the debt does
 * is sit in the net-worth figure, which is the number sw/apps/casino
 * leads with.
 *
 * zbank_buyin() is now exactly "borrow a starting stack", so the
 * `buyin` command every game already has does the right thing without
 * any of them changing. */
#define ZBANK_LOAN_NUM    6       /* borrow 5, owe 6 */
#define ZBANK_LOAN_DEN    5
#define ZBANK_MAX_DEBT    20000

#define ZBANK_MAX_GAMES   8
#define ZBANK_NAME_MAX    12      /* including the NUL */

/* Enough for the header plus ZBANK_MAX_GAMES worth of lines, with
 * room to spare. The file is deliberately small: a single short write
 * is the least likely thing to be torn by a power cut.
 *
 * This was 512, chosen by eye, and eight games did not fit -- the
 * formatter refused and the bank could not be written at all once the
 * last slot filled. tests/games_test.c fills every slot with the
 * longest values the fields allow and formats it, so the constant is
 * pinned to the documented maximum rather than to a guess. */
#define ZBANK_FILE_MAX    1024

typedef struct {
    char     name[ZBANK_NAME_MAX];
    int32_t  net;         /* lifetime, signed */
    uint32_t rounds;
} zbank_game_t;

typedef struct {
    uint32_t     version;
    int32_t      chips;
    int32_t      peak;    /* the most chips ever held, for bragging */
    int32_t      debt;    /* owed, including the vig */
    uint32_t     buyins;  /* how many loans have been taken */
    int          ngames;
    zbank_game_t game[ZBANK_MAX_GAMES];
} zbank_t;

#define ZBANK_OK       0
#define ZBANK_MISSING  1   /* no file -- a first run, not an error */
#define ZBANK_CORRUPT  2   /* a file with no balance in it */
#define ZBANK_IOERR    3
#define ZBANK_REFUSED  4   /* a loan too large, or nothing to repay */

/* -- the pure half, testable with no filesystem ---------------------- */

void zbank_defaults(zbank_t *b);

/* Parses `len` bytes of `text`. Returns ZBANK_OK or ZBANK_CORRUPT.
 * `b` is filled with defaults first, so a partial file yields defaults
 * for everything it did not mention. */
int zbank_parse(const char *text, int len, zbank_t *b);

/* Writes the file's text into `out`, NUL-terminated. Returns the
 * number of bytes written, or -1 if it did not fit. */
int zbank_format(const zbank_t *b, char *out, int outlen);

/* Finds a game's record, creating it if there is room. NULL only when
 * ZBANK_MAX_GAMES are already in use. */
zbank_game_t *zbank_game(zbank_t *b, const char *name);

/* Applies a round's result to an in-memory bank: chips, the game's
 * net and round count, and the peak. Separated from the file I/O so
 * the arithmetic can be tested on its own. */
void zbank_apply(zbank_t *b, const char *game, int32_t delta);

/* -- the half that touches the filesystem ---------------------------- */

/* Reads /casino.dat. Returns ZBANK_OK, ZBANK_MISSING (defaults are in
 * `b` and that is correct) or ZBANK_CORRUPT (defaults are in `b` and
 * that is NOT correct -- do not write over the file). */
int zbank_load(zbank_t *b);

int zbank_save(const zbank_t *b);

/* THE ONE A GAME SHOULD CALL.
 *
 * Re-reads, applies `delta` to whatever is in the file now, writes it
 * back, and reports the new balance through `out_chips` if that is not
 * NULL. Refuses, changing nothing, if the file is corrupt.
 *
 * `game` is a short name ("roulette", "poker") used only for the
 * per-game record. */
int zbank_adjust(const char *game, int32_t delta, int32_t *out_chips);

/* Borrows `amount`, adding it to the chips and amount*6/5 to the debt.
 * Refuses if the amount is not positive or the debt would pass
 * ZBANK_MAX_DEBT. */
int zbank_borrow(int32_t amount, int32_t *out_chips);

/* Pays `amount` off the debt out of the chips. Pays off no more than is
 * owed and spends no more than is held; reports what actually moved
 * through `out_paid`. */
int zbank_repay(int32_t amount, int32_t *out_chips, int32_t *out_paid);

/* Borrows a starting stack. Refuses if there are still chips -- a
 * top-up that works at any balance is a cheat with a friendly name.
 *
 * This used to be free money. It is a loan now, which is the same
 * command doing an honest thing. */
int zbank_buyin(int32_t *out_chips);

/* What borrowing `amount` adds to the debt: the principal plus the vig,
 * rounded UP. In the pure half so the arithmetic can be tested without
 * a filesystem -- the rounding is the part worth checking, because a
 * rate that rounds to nothing on small loans is not a rate. */
int32_t zbank_owed(int32_t amount);

/* Chips minus debt. The figure that actually says how you are doing,
 * and the reason the debt is worth recording at all. */
int32_t zbank_net(const zbank_t *b);

/* Throws the bank away and starts again. For an app offering a way out
 * of a corrupt file. */
int zbank_reset(void);

const char *zbank_strerror(int rv);

#endif
