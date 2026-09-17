/*
 * Zeitlos craps -- typed commands and mouse clicks.
 */

#include "cr_input.h"

static bool is_space(char c) { return c == ' ' || c == '\t'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool word_is(const char *s, int n, const char *w)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!w[i]) return false;
        if (lower(s[i]) != w[i]) return false;
    }
    return w[n] == '\0';
}

static int next_word(const char **p, const char **start)
{
    const char *s = *p;
    int n = 0;
    while (*s && is_space(*s)) s++;
    *start = s;
    while (s[n] && !is_space(s[n])) n++;
    *p = s + n;
    return n;
}

static bool to_int(const char *s, int n, int32_t *out)
{
    int32_t v = 0;
    int i;
    if (n <= 0) return false;
    for (i = 0; i < n; i++) {
        if (!is_digit(s[i])) return false;
        v = v * 10 + (s[i] - '0');
        if (v > 1000000) return false;
    }
    *out = v;
    return true;
}

static void say(cr_view_t *v, const char *s)
{
    int i = 0;
    while (s[i] && i < CR_MSG_LEN - 1) { v->message[i] = s[i]; i++; }
    v->message[i] = '\0';
}

static void say2(cr_view_t *v, const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < CR_MSG_LEN - 1) { v->message[i] = a[i]; i++; }
    cr_num(n, t);
    for (k = 0; t[k] && i < CR_MSG_LEN - 1; k++) v->message[i++] = t[k];
    for (k = 0; b[k] && i < CR_MSG_LEN - 1; k++) v->message[i++] = b[k];
    v->message[i] = '\0';
}

bool cr_can_afford(const cr_view_t *v, int32_t amount)
{
    /* WHAT IS ON THE FELT IS SPENT. It has not left the bank yet -- the
     * app only settles when the table empties -- so the check has to
     * count it, or a player could put their whole stack down twice. */
    return amount > 0 && cr_at_risk(v->g) + amount <= v->chips;
}

static ci_action_t place(cr_view_t *v, int type, int sel, int32_t amount)
{
    if (v->rolling) return CI_NONE;

    if (!cr_can_afford(v, amount)) {
        say2(v, "you have ", v->chips - cr_at_risk(v->g), " left to bet");
        return CI_REDRAW;
    }

    if (!cr_can_place(v->g, type, sel, amount)) {
        /* Says WHY where the reason is a phase rather than a typo --
         * "no" on a craps table is usually about when, not what. */
        if (type == CR_PASS || type == CR_DONT_PASS)
            say(v, "the line is closed once a point is on -- use come");
        else if (type == CR_COME || type == CR_DONT_COME)
            say(v, "come bets need a point first");
        else if (type == CR_PASS_ODDS || type == CR_DONT_ODDS)
            say(v, "odds go behind a line bet, once there is a point");
        else
            say(v, "not a bet you can make right now");
        return CI_REDRAW;
    }

    cr_place(v->g, type, sel, amount);
    return CI_REDRAW;
}

const char *cr_input_help_line(int i)
{
    static const char *const lines[] = {
        "Return rolls. bet 25 sets the stake; click the felt to place it",
        "pass, dont, come, field, place 6, hard 8, odds",
        "any7, craps, eleven, two, twelve -- all one-roll, all terrible",
        "odds are the only fair bet here: back the line with them",
        "down 6 takes a bet back; the line is contract and stays",
        "bank, buyin, game, quit -- F1 cycles this help",
        0
    };
    if (i < 0) return 0;
    return lines[i];
}

ci_action_t cr_input_command(cr_view_t *v, const char *line)
{
    const char *p = line, *w;
    int n, m;
    int32_t val;

    n = next_word(&p, &w);
    if (n == 0) return CI_NONE;

    if (word_is(w, n, "roll") || word_is(w, n, "r")) return CI_ROLL;

    if (word_is(w, n, "bet") || word_is(w, n, "b")) {
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val < 1) {
            say(v, "bet takes an amount");
            return CI_REDRAW;
        }
        v->bet = val;
        say2(v, "betting ", val, " a spot");
        return CI_REDRAW;
    }

    {
        static const struct { const char *a, *b; int type; } flat[] = {
            { "pass",   "p",  CR_PASS },
            { "dont",   "d",  CR_DONT_PASS },
            { "come",   "c",  CR_COME },
            { "dontcome", "dc", CR_DONT_COME },
            { "field",  "f",  CR_FIELD },
            { "any7",   "7",  CR_ANY7 },
            { "craps",  "cr", CR_ANY_CRAPS },
            { "eleven", "11", CR_ELEVEN },
            { "two",    "2",  CR_TWO },
            { "twelve", "12", CR_TWELVE }
        };
        int i;
        for (i = 0; i < 10; i++) {
            int32_t amt = v->bet;
            if (!word_is(w, n, flat[i].a) && !word_is(w, n, flat[i].b))
                continue;
            m = next_word(&p, &w);
            if (m && !to_int(w, m, &amt)) {
                say(v, "that is not an amount");
                return CI_REDRAW;
            }
            return place(v, flat[i].type, 0, amt);
        }
    }

    if (word_is(w, n, "place") || word_is(w, n, "pl")) {
        int32_t amt = v->bet;
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val)) { say(v, "place takes a number"); return CI_REDRAW; }
        m = next_word(&p, &w);
        if (m) (void)to_int(w, m, &amt);
        return place(v, CR_PLACE, (int)val, amt);
    }

    if (word_is(w, n, "hard") || word_is(w, n, "h")) {
        int32_t amt = v->bet;
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val)) { say(v, "hard takes a number"); return CI_REDRAW; }
        m = next_word(&p, &w);
        if (m) (void)to_int(w, m, &amt);
        return place(v, CR_HARD, (int)val, amt);
    }

    if (word_is(w, n, "odds") || word_is(w, n, "o")) {
        int32_t amt = v->bet;
        int32_t room;
        m = next_word(&p, &w);
        if (m) (void)to_int(w, m, &amt);
        room = cr_odds_room(v->g, CR_PASS_ODDS, v->g->point);
        if (room <= 0) {
            say(v, "odds go behind a line bet, once there is a point");
            return CI_REDRAW;
        }
        /* Clamped rather than refused, so "odds 9999" means "the most
         * the house will let me" -- which is what a player wanting max
         * odds actually means. */
        if (amt > room) amt = room;
        return place(v, CR_PASS_ODDS, v->g->point, amt);
    }

    if (word_is(w, n, "down")) {
        int32_t back;
        m = next_word(&p, &w);
        if (m && to_int(w, m, &val)) back = cr_take_down(v->g, CR_PLACE, (int)val);
        else back = cr_take_down(v->g, CR_FIELD, 0);
        if (back > 0) say2(v, "took ", back, " back");
        else say(v, "nothing there to take down");
        return CI_REDRAW;
    }

    if (word_is(w, n, "bank")) { say2(v, "you have ", v->chips, " chips"); return CI_REDRAW; }
    if (word_is(w, n, "buyin")) return CI_BUYIN;
    if (word_is(w, n, "game") || word_is(w, n, "full")) return CI_GAME_MODE;
    if (word_is(w, n, "quit") || word_is(w, n, "q")) return CI_QUIT;

    if (word_is(w, n, "help") || word_is(w, n, "?")) {
        say(v, cr_input_help_line(0));
        return CI_REDRAW;
    }

    say(v, "unknown -- F1 for help");
    return CI_REDRAW;
}

ci_action_t cr_input_key(cr_view_t *v, uint32_t keysym)
{
    if (keysym == '\r' || keysym == '\n') {

        if (v->cmd_len == 0) {
            /* Return rolls. It commits nothing -- the bets are already
             * on the felt -- so it is safe as a single key, which is
             * what the other five concluded about their own one verb. */
            return CI_ROLL;
        }

        {
            char l[CR_CMD_LEN];
            int i;
            for (i = 0; i < v->cmd_len && i < CR_CMD_LEN - 1; i++) l[i] = v->cmd[i];
            l[i] = '\0';
            v->cmd_len = 0;
            v->cmd[0] = '\0';
            return cr_input_command(v, l);
        }
    }

    if (keysym == 0x08 || keysym == 0x7f) {
        if (v->cmd_len > 0) v->cmd[--v->cmd_len] = '\0';
        return CI_STATUS;
    }

    if (keysym == 0x1b) { v->cmd_len = 0; v->cmd[0] = '\0'; return CI_STATUS; }

    if (keysym >= 0x20 && keysym < 0x7f && v->cmd_len < CR_CMD_LEN - 1) {
        v->cmd[v->cmd_len++] = (char)keysym;
        v->cmd[v->cmd_len] = '\0';
        /* Only the two text rows changed. */
        return CI_STATUS;
    }

    return CI_NONE;
}

ci_action_t cr_input_click(cr_view_t *v, const cr_layout_t *L, int x, int y)
{
    int i = cr_board_spot_at(L, x, y);
    int sel;

    if (v->rolling) return CI_NONE;

    /* PICKING UP THE DICE ROLLS THEM. They are the most obvious thing
     * on the table to click and they did nothing, which is worse than
     * not drawing them. */
    if (cr_board_dice_at(L, x, y)) return CI_ROLL;

    if (i == CR_SPOT_NONE) return CI_NONE;

    /* The odds boxes take their number from the point, which moves. */
    sel = cr_spot_sel(L, v, i);

    return place(v, L->spot[i].type, sel, v->bet);
}
