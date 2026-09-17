/*
 * Zeitlos roulette -- the wheel, the layout, and what each bet pays.
 * See rl_table.h for the invariant that checks every payout at once.
 */

#include "rl_table.h"

int rl_pockets(int wheel)
{
    return wheel == RL_AMERICAN ? 38 : 37;
}

/* -- red --------------------------------------------------------------
 *
 * The eighteen red numbers, written out.
 *
 * There is a rule of thumb -- odd numbers are red in the first and
 * third dozen, even in the second -- and it is nearly right, which is
 * worse than useless. It fails at 10 and 11 (both black) and again at
 * 28 and 29 (both black), because the wheel's colouring alternates
 * around the RIM and the printed table is laid out numerically. A
 * derived version of this function would put 10 and 29 in the wrong
 * colour and still look plausible.
 *
 * So the set is a list, and the test asserts the properties that
 * matter: eighteen of them, eighteen black, no overlap, and together
 * exactly 1..36.
 */
static const uint8_t red_numbers[18] = {
    1, 3, 5, 7, 9, 12, 14, 16, 18,
    19, 21, 23, 25, 27, 30, 32, 34, 36
};

bool rl_is_red(int pocket)
{
    int i;

    if (pocket < 1 || pocket > 36) return false;

    for (i = 0; i < 18; i++)
        if (red_numbers[i] == pocket) return true;

    return false;
}

/* -- splits and corners -----------------------------------------------
 *
 * The printed table is twelve rows of three:
 *
 *      1  2  3
 *      4  5  6
 *      ...
 *     34 35 36
 *
 * A SPLIT is two numbers sharing an edge on that grid: horizontal
 * within a row (1-2, 2-3) or vertical between rows (1-4). 3 and 4 are
 * consecutive numbers and are NOT a split -- they sit at opposite ends
 * of different rows and share no edge. That is the mistake a
 * "consecutive numbers" implementation makes, and it is invisible
 * until somebody's winning bet does not pay.
 *
 * Zero splits (0-1, 0-2, 0-3) exist on a real table. They are left out
 * here along with the other zero-adjacent bets, because their geometry
 * differs between the European and American layouts and supporting
 * them on one but not the other would be worse than supporting them on
 * neither. A straight-up zero covers the same ground at better odds.
 *
 * A CORNER is the four numbers meeting at an interior grid point:
 * rows r and r+1, columns c and c+1.
 */
#define RL_SPLIT_H (12 * 2)          /* two per row               */
#define RL_SPLIT_V (11 * 3)          /* three per gap, eleven gaps */

int rl_n_splits(void)
{
    return RL_SPLIT_H + RL_SPLIT_V;
}

int rl_n_corners(void)
{
    return 11 * 2;                   /* two interior points per gap */
}

int rl_split_pair(int sel, int *out)
{
    if (sel < 0) return 0;

    if (sel < RL_SPLIT_H) {
        int row = sel / 2, col = sel % 2;    /* col 0: a-b, col 1: b-c */
        out[0] = row * 3 + col + 1;
        out[1] = row * 3 + col + 2;
        return 2;
    }

    sel -= RL_SPLIT_H;
    if (sel < RL_SPLIT_V) {
        int gap = sel / 3, col = sel % 3;
        out[0] = gap * 3 + col + 1;
        out[1] = (gap + 1) * 3 + col + 1;
        return 2;
    }

    return 0;
}

int rl_corner_set(int sel, int *out)
{
    int gap, col;

    if (sel < 0 || sel >= rl_n_corners()) return 0;

    gap = sel / 2;
    col = sel % 2;

    out[0] = gap * 3 + col + 1;
    out[1] = gap * 3 + col + 2;
    out[2] = (gap + 1) * 3 + col + 1;
    out[3] = (gap + 1) * 3 + col + 2;

    return 4;
}

/* -- payouts ----------------------------------------------------------
 *
 * The total returned per unit staked on a win, stake included.
 */
int rl_returns(int type)
{
    switch (type) {
    case RL_STRAIGHT: return 36;     /* 35:1 */
    case RL_SPLIT:    return 18;     /* 17:1 */
    case RL_STREET:   return 12;     /* 11:1 */
    case RL_CORNER:   return 9;      /*  8:1 */
    case RL_SIXLINE:  return 6;      /*  5:1 */
    case RL_COLUMN:   return 3;      /*  2:1 */
    case RL_DOZEN:    return 3;      /*  2:1 */
    case RL_RED:
    case RL_BLACK:
    case RL_ODD:
    case RL_EVEN:
    case RL_LOW:
    case RL_HIGH:     return 2;      /*  1:1 */
    case RL_BASKET:   return 7;      /*  6:1 -- American, and a bad bet */
    }
    return 0;
}

int rl_sel_count(int wheel, int type)
{
    switch (type) {
    case RL_STRAIGHT: return rl_pockets(wheel);
    case RL_SPLIT:    return rl_n_splits();
    case RL_STREET:   return 12;
    case RL_CORNER:   return rl_n_corners();
    case RL_SIXLINE:  return 11;
    case RL_COLUMN:   return 3;
    case RL_DOZEN:    return 3;
    case RL_BASKET:   return wheel == RL_AMERICAN ? 1 : 0;
    case RL_RED:
    case RL_BLACK:
    case RL_ODD:
    case RL_EVEN:
    case RL_LOW:
    case RL_HIGH:     return 1;
    }
    return 0;
}

bool rl_bet_valid(int wheel, int type, int sel)
{
    if (type < 0 || type >= RL_NTYPES) return false;
    if (sel < 0) return false;
    return sel < rl_sel_count(wheel, type);
}

bool rl_covers(int wheel, int type, int sel, int pocket)
{
    int set[4], n, i;

    if (!rl_bet_valid(wheel, type, sel)) return false;
    if (pocket < 0 || pocket >= RL_MAX_POCKETS) return false;
    if (pocket == RL_DOUBLE_ZERO && wheel != RL_AMERICAN) return false;

    switch (type) {

    case RL_STRAIGHT:
        return pocket == sel;

    case RL_SPLIT:
        n = rl_split_pair(sel, set);
        for (i = 0; i < n; i++) if (set[i] == pocket) return true;
        return false;

    case RL_CORNER:
        n = rl_corner_set(sel, set);
        for (i = 0; i < n; i++) if (set[i] == pocket) return true;
        return false;

    case RL_STREET:
        return pocket >= sel * 3 + 1 && pocket <= sel * 3 + 3;

    case RL_SIXLINE:
        return pocket >= sel * 3 + 1 && pocket <= sel * 3 + 6;

    case RL_COLUMN:
        /* Column 0 is 1, 4, 7 ... 34, i.e. n % 3 == 1. The third
         * column is 3, 6, 9 ... 36, where n % 3 == 0 -- which is why
         * this is written as a comparison against sel + 1 modulo 3
         * rather than as n % 3 == sel. */
        if (pocket < 1 || pocket > 36) return false;
        return (pocket - 1) % 3 == sel;

    case RL_DOZEN:
        return pocket >= sel * 12 + 1 && pocket <= sel * 12 + 12;

    case RL_RED:
        return rl_is_red(pocket);

    case RL_BLACK:
        return pocket >= 1 && pocket <= 36 && !rl_is_red(pocket);

    case RL_ODD:
        /* ZERO IS NOT ODD AND NOT EVEN. It loses every even-money bet,
         * which is the entire house edge on those bets and the one
         * place a `% 2` shortcut silently gives the house's money
         * away. */
        return pocket >= 1 && pocket <= 36 && (pocket % 2) == 1;

    case RL_EVEN:
        return pocket >= 1 && pocket <= 36 && (pocket % 2) == 0;

    case RL_LOW:
        return pocket >= 1 && pocket <= 18;

    case RL_HIGH:
        return pocket >= 19 && pocket <= 36;

    case RL_BASKET:
        return pocket == RL_ZERO || pocket == RL_DOUBLE_ZERO ||
            (pocket >= 1 && pocket <= 3);
    }

    return false;
}

int rl_coverage(int wheel, int type, int sel)
{
    int p, n = 0, np = rl_pockets(wheel);

    for (p = 0; p < np; p++) {
        int pocket = (p == 37) ? RL_DOUBLE_ZERO : p;
        if (rl_covers(wheel, type, sel, pocket)) n++;
    }

    /* The American wheel's 38th pocket is RL_DOUBLE_ZERO (37), which
     * the loop above reaches as p == 37. */
    return n;
}

char *rl_pocket_str(int pocket, char *buf)
{
    if (pocket == RL_DOUBLE_ZERO) {
        buf[0] = '0'; buf[1] = '0'; buf[2] = '\0';
        return buf;
    }

    if (pocket < 0 || pocket > 36) {
        buf[0] = '?'; buf[1] = '\0';
        return buf;
    }

    if (pocket >= 10) {
        buf[0] = (char)('0' + pocket / 10);
        buf[1] = (char)('0' + pocket % 10);
        buf[2] = '\0';
    } else {
        buf[0] = (char)('0' + pocket);
        buf[1] = '\0';
    }

    return buf;
}

const char *rl_type_name(int type)
{
    switch (type) {
    case RL_STRAIGHT: return "straight up";
    case RL_SPLIT:    return "split";
    case RL_STREET:   return "street";
    case RL_CORNER:   return "corner";
    case RL_SIXLINE:  return "six line";
    case RL_COLUMN:   return "column";
    case RL_DOZEN:    return "dozen";
    case RL_RED:      return "red";
    case RL_BLACK:    return "black";
    case RL_ODD:      return "odd";
    case RL_EVEN:     return "even";
    case RL_LOW:      return "1 to 18";
    case RL_HIGH:     return "19 to 36";
    case RL_BASKET:   return "basket";
    }
    return "?";
}

const char *rl_type_odds(int type)
{
    switch (type) {
    case RL_STRAIGHT: return "35:1";
    case RL_SPLIT:    return "17:1";
    case RL_STREET:   return "11:1";
    case RL_CORNER:   return "8:1";
    case RL_SIXLINE:  return "5:1";
    case RL_COLUMN:
    case RL_DOZEN:    return "2:1";
    case RL_BASKET:   return "6:1";
    default:          return "1:1";
    }
}

/* -- a round ----------------------------------------------------------- */

void rl_round_clear(rl_round_t *r, int wheel)
{
    int i;
    for (i = 0; i < (int)sizeof(rl_round_t); i++) ((char *)r)[i] = 0;
    r->wheel = wheel;
}

static int find_bet(const rl_round_t *r, int type, int sel)
{
    int i;
    for (i = 0; i < r->nbets; i++)
        if (r->bet[i].type == type && r->bet[i].sel == sel) return i;
    return -1;
}

bool rl_bet_place(rl_round_t *r, int type, int sel, int32_t amount)
{
    int i;

    if (amount <= 0) return false;
    if (!rl_bet_valid(r->wheel, type, sel)) return false;

    /* Stacked onto an existing bet rather than added as a second
     * entry. Placing a chip is a click, and a person clicking the same
     * square ten times must not exhaust RL_MAX_BETS -- nor see ten
     * identical lines in the bet list. */
    i = find_bet(r, type, sel);
    if (i >= 0) {
        r->bet[i].amount += amount;
        r->staked += amount;
        return true;
    }

    if (r->nbets >= RL_MAX_BETS) return false;

    r->bet[r->nbets].type = (uint8_t)type;
    r->bet[r->nbets].sel = (uint8_t)sel;
    r->bet[r->nbets].amount = amount;
    r->nbets++;
    r->staked += amount;

    return true;
}

int32_t rl_bet_remove(rl_round_t *r, int type, int sel, int32_t amount)
{
    int i = find_bet(r, type, sel);
    int32_t took;

    if (i < 0 || amount <= 0) return 0;

    took = amount < r->bet[i].amount ? amount : r->bet[i].amount;
    r->bet[i].amount -= took;
    r->staked -= took;

    if (r->bet[i].amount == 0) {
        r->bet[i] = r->bet[r->nbets - 1];
        r->nbets--;
    }

    return took;
}

int32_t rl_bet_on(const rl_round_t *r, int type, int sel)
{
    int i = find_bet(r, type, sel);
    return i < 0 ? 0 : r->bet[i].amount;
}

int32_t rl_round_returns(const rl_round_t *r, int pocket)
{
    int32_t total = 0;
    int i;

    for (i = 0; i < r->nbets; i++) {
        if (!rl_covers(r->wheel, r->bet[i].type, r->bet[i].sel, pocket))
            continue;
        total += r->bet[i].amount * (int32_t)rl_returns(r->bet[i].type);
    }

    return total;
}
