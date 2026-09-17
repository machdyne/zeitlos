/*
 * Zeitlos slots -- the reels and what they pay.
 * See sl_reels.h for why the house edge here is an exact integer.
 */

#include "sl_reels.h"

/* -- the strips --------------------------------------------------------
 *
 * The ORDER around a strip does not affect the return -- every stop is
 * equally likely, so only the COUNTS matter -- but it does affect what
 * a near miss looks like. These are scattered, with no paying symbol
 * adjacent to another of its kind, so nothing slides past in clumps.
 *
 * The counts are the machine's design and the edge falls out of them:
 *
 *            cherry  BAR  BARBAR  3BAR  bell  7   blank
 *   reel 1      4     6      4      3     3   2    10
 *   reel 2      3     6      4      3     3   2    11
 *   reel 3      2     6      4      3     2   2    13
 *
 * Cherries thin out left to right and so do the bells. That is the
 * classic shape: the leftmost reel is generous because cherries pay
 * from the left, and the rightmost is stingy because it is the one
 * still spinning when the other two have already matched -- which is
 * where the tension in a slot machine comes from.
 *
 * WRITTEN OUT, NOT GENERATED AT RUNTIME. These numbers were arrived at
 * by pricing the paytable against the exact frequencies they produce;
 * change one and the house edge moves, which tests/reel_test.c will
 * say so about.
 */
const uint8_t sl_strip[SL_REELS][SL_STOPS] = {
    {   /* reel 1 */
        SL_BLANK,  SL_BELL,   SL_BLANK,  SL_BAR2,   SL_BELL,   SL_CHERRY,
        SL_BLANK,  SL_BAR,    SL_BLANK,  SL_BLANK,  SL_BAR,    SL_BAR3,
        SL_BAR,    SL_BAR2,   SL_BAR3,   SL_BAR,    SL_BLANK,  SL_BLANK,
        SL_BLANK,  SL_BLANK,  SL_CHERRY, SL_BAR2,   SL_BAR,    SL_BLANK,
        SL_CHERRY, SL_BAR,    SL_BAR3,   SL_SEVEN,  SL_CHERRY, SL_SEVEN,
        SL_BELL,   SL_BAR2,
    },
    {   /* reel 2 */
        SL_BAR,    SL_BLANK,  SL_BLANK,  SL_BAR,    SL_BLANK,  SL_BAR3,
        SL_BELL,   SL_BLANK,  SL_BAR,    SL_BLANK,  SL_BAR,    SL_SEVEN,
        SL_BAR3,   SL_BAR,    SL_SEVEN,  SL_CHERRY, SL_BELL,   SL_BLANK,
        SL_BLANK,  SL_BELL,   SL_BLANK,  SL_BAR3,   SL_BAR2,   SL_BLANK,
        SL_BAR2,   SL_BLANK,  SL_BAR,    SL_BLANK,  SL_BAR2,   SL_CHERRY,
        SL_BAR2,   SL_CHERRY,
    },
    {   /* reel 3 */
        SL_BLANK,  SL_BAR3,   SL_BLANK,  SL_BAR2,   SL_BAR,    SL_BLANK,
        SL_BAR,    SL_BLANK,  SL_BLANK,  SL_BLANK,  SL_SEVEN,  SL_BAR3,
        SL_BLANK,  SL_BAR,    SL_BELL,   SL_BLANK,  SL_BAR,    SL_BLANK,
        SL_BAR2,   SL_BLANK,  SL_BAR,    SL_BAR2,   SL_BAR3,   SL_SEVEN,
        SL_BAR2,   SL_BLANK,  SL_CHERRY, SL_BLANK,  SL_BAR,    SL_CHERRY,
        SL_BLANK,  SL_BELL,
    }
};

/* -- the paytable ------------------------------------------------------
 *
 * Priced against the frequencies, not by feel. Three sevens come up
 * once in 4,096 line-bets and pay 800, which is a fifth of everything
 * the machine gives back -- a jackpot has to be most of the return or
 * it is not a jackpot, and it has to be rare or it is not an edge.
 */
static int pay_three(int sym)
{
    switch (sym) {
    case SL_SEVEN:  return 800;
    case SL_BELL:   return 150;
    case SL_CHERRY: return 100;
    case SL_BAR3:   return 100;
    case SL_BAR2:   return 50;
    case SL_BAR:    return 20;
    }
    return 0;
}

int sl_pay_three(int sym)
{
    return pay_three(sym);
}

static bool is_bar(int s)
{
    return s == SL_BAR || s == SL_BAR2 || s == SL_BAR3;
}

int sl_pay(int a, int b, int c)
{
    if (a == b && b == c) {
        int p = pay_three(a);
        if (p) return p;
    }

    /* Any three bars, of whatever kind. Common -- once in seventeen
     * line-bets -- so it pays 2, and even at that it is an eighth of
     * the machine's return. */
    if (is_bar(a) && is_bar(b) && is_bar(c)) return 2;

    /* LEFT TO RIGHT. A cherry anywhere on the line was the first
     * version and it put the RTP at 133%: cherries are the most common
     * paying symbol, so paying them from any position pays almost every
     * spin. */
    if (a == SL_CHERRY && b == SL_CHERRY) return 5;
    if (a == SL_CHERRY) return 1;

    return 0;
}

/* -- the window --------------------------------------------------------- */

void sl_window(int reel, int stop, uint8_t *out)
{
    int k;

    if (reel < 0 || reel >= SL_REELS) {
        for (k = 0; k < SL_ROWS; k++) out[k] = SL_BLANK;
        return;
    }

    stop %= SL_STOPS;
    if (stop < 0) stop += SL_STOPS;

    for (k = 0; k < SL_ROWS; k++)
        out[k] = sl_strip[reel][(stop + k) % SL_STOPS];
}

/* The five paylines: the three rows, then the two diagonals. */
void sl_line_rows(int line, int *rows)
{
    switch (line) {
    case 0: rows[0] = 0; rows[1] = 0; rows[2] = 0; break;
    case 1: rows[0] = 1; rows[1] = 1; rows[2] = 1; break;
    case 2: rows[0] = 2; rows[1] = 2; rows[2] = 2; break;
    case 3: rows[0] = 0; rows[1] = 1; rows[2] = 2; break;
    default: rows[0] = 2; rows[1] = 1; rows[2] = 0; break;
    }
}

void sl_line(const int *stops, int line, uint8_t *out)
{
    int rows[SL_REELS], r;

    sl_line_rows(line, rows);

    for (r = 0; r < SL_REELS; r++) {
        uint8_t w[SL_ROWS];
        sl_window(r, stops[r], w);
        out[r] = w[rows[r]];
    }
}

int32_t sl_evaluate(const int *stops, int32_t per_line, int lines,
    int32_t *line_pays)
{
    int32_t total = 0;
    int i;

    if (lines < 0) lines = 0;
    if (lines > SL_LINES) lines = SL_LINES;

    for (i = 0; i < SL_LINES; i++) {
        uint8_t s[SL_REELS];
        int32_t p = 0;

        /* Lines that were not paid for are evaluated as zero rather
         * than skipped, so a display can dim them instead of guessing
         * which ones existed. */
        if (i < lines) {
            sl_line(stops, i, s);
            p = (int32_t)sl_pay(s[0], s[1], s[2]) * per_line;
        }

        if (line_pays) line_pays[i] = p;
        total += p;
    }

    return total;
}

const char *sl_sym_name(int sym)
{
    switch (sym) {
    case SL_BLANK:  return "-";
    case SL_CHERRY: return "cherry";
    case SL_BAR:    return "BAR";
    case SL_BAR2:   return "BARBAR";
    case SL_BAR3:   return "3BAR";
    case SL_BELL:   return "bell";
    case SL_SEVEN:  return "7";
    }
    return "?";
}
