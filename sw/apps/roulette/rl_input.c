/*
 * Zeitlos roulette -- typed commands and mouse clicks.
 * See rl_input.h for why both routes end in the same call.
 */

#include "rl_input.h"

/* Hand-rolled parsing, for the reason docs/app_runtime.md gives: one
 * conversion specifier links picolibc's formatter at a cost of around
 * 100KB, and a command line is exactly where that creeps in. */

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
        if (v > 10000000) return false;
    }
    *out = v;
    return true;
}

static void say(rl_view_t *v, const char *s)
{
    int i = 0;
    while (s[i] && i < RL_MSG_LEN - 1) { v->message[i] = s[i]; i++; }
    v->message[i] = '\0';
}

static void say2(rl_view_t *v, const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < RL_MSG_LEN - 1) { v->message[i] = a[i]; i++; }
    rl_num(n, t);
    for (k = 0; t[k] && i < RL_MSG_LEN - 1; k++) v->message[i++] = t[k];
    for (k = 0; b[k] && i < RL_MSG_LEN - 1; k++) v->message[i++] = b[k];
    v->message[i] = '\0';
}

/* -- staking ------------------------------------------------------------ */

bool rl_can_stake(const rl_view_t *v, int32_t amount)
{
    return amount > 0 && v->round->staked + amount <= v->chips;
}

static ri_action_t place(rl_view_t *v, int type, int sel, int32_t amount)
{
    if (v->spinning) { say(v, "the wheel is turning"); return RI_REDRAW; }

    if (!rl_bet_valid(v->round->wheel, type, sel)) {
        say(v, "not a bet on this wheel");
        return RI_REDRAW;
    }

    /* THE BANK IS THE LIMIT, not the bet. Chips already on the table
     * are spoken for even though they have not left the bank yet --
     * they leave it when the wheel is spun, in one adjustment. Checking
     * against the balance alone would let somebody bet their stack
     * twice over. */
    if (!rl_can_stake(v, amount)) {
        say2(v, "you only have ", v->chips - v->round->staked, " left");
        return RI_REDRAW;
    }

    if (!rl_bet_place(v->round, type, sel, amount)) {
        say(v, "no room for another bet -- clear some");
        return RI_REDRAW;
    }

    return RI_REDRAW;
}

/* -- naming a split or a corner by its members --------------------------
 *
 * Splits and corners have no names, so they are typed by the numbers
 * they cover and looked up here. Searching rather than computing means
 * this cannot disagree with rl_table.c about what a split is -- and
 * "adjacent on the printed table" is not obvious enough to want two
 * implementations of.
 */
static int find_split(int a, int b)
{
    int sel, pair[2];

    for (sel = 0; sel < rl_n_splits(); sel++) {
        rl_split_pair(sel, pair);
        if ((pair[0] == a && pair[1] == b) ||
            (pair[0] == b && pair[1] == a)) return sel;
    }

    return -1;
}

static int find_corner(int top_left)
{
    int sel, set[4];

    for (sel = 0; sel < rl_n_corners(); sel++) {
        rl_corner_set(sel, set);
        if (set[0] == top_left) return sel;
    }

    return -1;
}

/* -- commands ------------------------------------------------------------ */

const char *rl_input_help_line(int i)
{
    static const char *const lines[] = {
        "17 25 bets 25 on 17; a bare number uses the chip value",
        "red/black/odd/even/low/high, dozen 1-3, column 1-3",
        "street 1-12, six 1-11, split 17 20, corner 17",
        "spin (or Return), clear, chip 1|5|25|100",
        "wheel euro|american, bank, buyin, game, quit",
        "click a number, a line between two, or a corner",
        0
    };

    if (i < 0) return 0;
    return lines[i];
}

ri_action_t rl_input_command(rl_view_t *v, const char *line)
{
    const char *p = line, *w;
    int n, m;
    int32_t val, amt;
    int32_t chip = rl_chip_values[v->chip_sel];

    n = next_word(&p, &w);
    if (n == 0) return RI_NONE;

    /* THE DOUBLE ZERO IS CHECKED FIRST, because "00" is a perfectly
     * good integer literal and the bare-number branch below would read
     * it as 0 -- putting the chip on the single zero, silently, on the
     * one bet where the two are genuinely different pockets. */
    if (word_is(w, n, "00")) {
        amt = chip;
        m = next_word(&p, &w);
        if (m && !to_int(w, m, &amt)) {
            say(v, "that is not an amount");
            return RI_REDRAW;
        }
        return place(v, RL_STRAIGHT, RL_DOUBLE_ZERO, amt);
    }

    /* A bare number is a straight-up bet on it. Nothing else somebody
     * says at a roulette table is a bare number. */
    if (to_int(w, n, &val)) {
        amt = chip;
        m = next_word(&p, &w);
        if (m && !to_int(w, m, &amt)) { say(v, "that is not an amount"); return RI_REDRAW; }
        if (val > 36) { say(v, "the numbers run 0 to 36"); return RI_REDRAW; }
        return place(v, RL_STRAIGHT, (int)val, amt);
    }

    /* The even-money bets and the two dozen-sized ones. */
    {
        static const struct { const char *a, *b; int type; } simple[] = {
            { "red",   "r",  RL_RED   },
            { "black", "b",  RL_BLACK },
            { "odd",   "o",  RL_ODD   },
            { "even",  "e",  RL_EVEN  },
            { "low",   "1-18", RL_LOW  },
            { "high",  "19-36", RL_HIGH }
        };
        int i;
        for (i = 0; i < 6; i++) {
            if (!word_is(w, n, simple[i].a) && !word_is(w, n, simple[i].b))
                continue;
            amt = chip;
            m = next_word(&p, &w);
            if (m && !to_int(w, m, &amt)) {
                say(v, "that is not an amount");
                return RI_REDRAW;
            }
            return place(v, simple[i].type, 0, amt);
        }
    }

    /* The ones that take a selector: dozen, column, street, six. */
    {
        static const struct { const char *a, *b; int type; int lo, hi; }
        sel_cmds[] = {
            { "dozen",  "d", RL_DOZEN,   1, 3  },
            { "column", "c", RL_COLUMN,  1, 3  },
            { "street", "s", RL_STREET,  1, 12 },
            { "six",    "6", RL_SIXLINE, 1, 11 }
        };
        int i;
        for (i = 0; i < 4; i++) {
            if (!word_is(w, n, sel_cmds[i].a) && !word_is(w, n, sel_cmds[i].b))
                continue;
            m = next_word(&p, &w);
            if (!m || !to_int(w, m, &val) ||
                val < sel_cmds[i].lo || val > sel_cmds[i].hi) {
                say2(v, "that one takes a number up to ",
                    sel_cmds[i].hi, "");
                return RI_REDRAW;
            }
            amt = chip;
            m = next_word(&p, &w);
            if (m && !to_int(w, m, &amt)) {
                say(v, "that is not an amount");
                return RI_REDRAW;
            }
            /* One-based for the person, zero-based inside. */
            return place(v, sel_cmds[i].type, (int)val - 1, amt);
        }
    }

    if (word_is(w, n, "split")) {
        int32_t a, b;
        int sel;
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &a)) { say(v, "split takes two numbers"); return RI_REDRAW; }
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &b)) { say(v, "split takes two numbers"); return RI_REDRAW; }
        sel = find_split((int)a, (int)b);
        if (sel < 0) {
            /* Not a scolding: "not adjacent" is the useful half, since
             * consecutive numbers are the obvious wrong guess and 3 and
             * 4 look adjacent in a list. */
            say(v, "those two are not next to each other on the table");
            return RI_REDRAW;
        }
        amt = chip;
        m = next_word(&p, &w);
        if (m) (void)to_int(w, m, &amt);
        return place(v, RL_SPLIT, sel, amt);
    }

    if (word_is(w, n, "corner")) {
        int sel;
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val)) {
            say(v, "corner takes the lowest of its four numbers");
            return RI_REDRAW;
        }
        sel = find_corner((int)val);
        if (sel < 0) {
            say(v, "no corner starts at that number");
            return RI_REDRAW;
        }
        amt = chip;
        m = next_word(&p, &w);
        if (m) (void)to_int(w, m, &amt);
        return place(v, RL_CORNER, sel, amt);
    }

    if (word_is(w, n, "chip")) {
        int i;
        m = next_word(&p, &w);
        if (m && to_int(w, m, &val)) {
            for (i = 0; i < RL_NCHIPS; i++)
                if (rl_chip_values[i] == val) {
                    v->chip_sel = i;
                    say2(v, "chips of ", val, " now");
                    return RI_REDRAW;
                }
        }
        say(v, "chip takes 1, 5, 25 or 100");
        return RI_REDRAW;
    }

    if (word_is(w, n, "clear")) {
        rl_round_clear(v->round, v->round->wheel);
        say(v, "table cleared");
        return RI_REDRAW;
    }

    if (word_is(w, n, "spin")) {
        if (v->round->nbets == 0) {
            say(v, "nothing on the table");
            return RI_REDRAW;
        }
        return RI_SPIN;
    }

    if (word_is(w, n, "wheel")) {
        m = next_word(&p, &w);
        if (m && (word_is(w, m, "euro") || word_is(w, m, "european"))) {
            v->round->wheel = RL_EURO;
            say(v, "european wheel -- one zero, 2.7% edge");
            return RI_NEWGAME;
        }
        if (m && (word_is(w, m, "american") || word_is(w, m, "us"))) {
            v->round->wheel = RL_AMERICAN;
            say(v, "american wheel -- two zeros, 5.3% edge");
            return RI_NEWGAME;
        }
        say(v, "wheel takes euro or american");
        return RI_REDRAW;
    }

    if (word_is(w, n, "bank")) {
        say2(v, "you have ", v->chips, " chips");
        return RI_REDRAW;
    }

    if (word_is(w, n, "buyin")) return RI_BUYIN;
    if (word_is(w, n, "game") || word_is(w, n, "full")) return RI_GAME_MODE;
    if (word_is(w, n, "quit") || word_is(w, n, "q")) return RI_QUIT;

    if (word_is(w, n, "help") || word_is(w, n, "?")) {
        say(v, rl_input_help_line(0));
        return RI_REDRAW;
    }

    say(v, "unknown -- F1 for help");
    return RI_REDRAW;
}

/* -- keys ---------------------------------------------------------------- */

ri_action_t rl_input_key(rl_view_t *v, uint32_t keysym)
{
    if (keysym == '\r' || keysym == '\n') {

        if (v->cmd_len == 0) {
            /* Return on an empty line spins, if there is anything to
             * spin for. It is the one action worth a single key: it
             * commits what is already on the table rather than adding
             * to it, so it cannot cost anything that was not already
             * staked deliberately. */
            if (v->round->nbets == 0) {
                say(v, "place a bet first");
                return RI_REDRAW;
            }
            return RI_SPIN;
        }

        {
            char line[RL_CMD_LEN];
            int i;
            for (i = 0; i < v->cmd_len && i < RL_CMD_LEN - 1; i++)
                line[i] = v->cmd[i];
            line[i] = '\0';
            v->cmd_len = 0;
            v->cmd[0] = '\0';
            return rl_input_command(v, line);
        }
    }

    if (keysym == 0x08 || keysym == 0x7f) {
        if (v->cmd_len > 0) v->cmd[--v->cmd_len] = '\0';
        return RI_STATUS;
    }

    if (keysym == 0x1b) {
        v->cmd_len = 0;
        v->cmd[0] = '\0';
        return RI_STATUS;
    }

    /* The chip denomination, without going through the line. */
    if (v->cmd_len == 0 && (keysym == '+' || keysym == '=')) {
        if (v->chip_sel < RL_NCHIPS - 1) v->chip_sel++;
        return RI_REDRAW;
    }
    if (v->cmd_len == 0 && keysym == '-') {
        if (v->chip_sel > 0) v->chip_sel--;
        return RI_REDRAW;
    }

    if (keysym >= 0x20 && keysym < 0x7f && v->cmd_len < RL_CMD_LEN - 1) {
        v->cmd[v->cmd_len++] = (char)keysym;
        v->cmd[v->cmd_len] = '\0';
        return RI_STATUS;
    }

    return RI_NONE;
}

/* -- clicks -------------------------------------------------------------- */

ri_action_t rl_input_click(rl_view_t *v, const rl_layout_t *L,
    int x, int y, bool remove)
{
    int type, sel, c;

    if (v->spinning) return RI_NONE;

    c = rl_board_chip_at(L, x, y);
    if (c >= 0) { v->chip_sel = c; return RI_REDRAW; }

    if (x >= L->spin.x && x < L->spin.x + L->spin.w &&
        y >= L->spin.y && y < L->spin.y + L->spin.h) {
        if (v->round->nbets == 0) {
            say(v, "nothing on the table");
            return RI_REDRAW;
        }
        return RI_SPIN;
    }

    if (x >= L->clear.x && x < L->clear.x + L->clear.w &&
        y >= L->clear.y && y < L->clear.y + L->clear.h) {
        rl_round_clear(v->round, v->round->wheel);
        say(v, "table cleared");
        return RI_REDRAW;
    }

    if (!rl_board_hit(L, v, x, y, &type, &sel)) return RI_NONE;

    if (remove) {
        /* The right button takes a chip back off. Without it the only
         * way to undo a misplaced chip is to clear the whole table,
         * which on a layout this dense is a real nuisance. */
        int32_t took = rl_bet_remove(v->round, type, sel,
            rl_chip_values[v->chip_sel]);
        if (took == 0) return RI_NONE;
        return RI_REDRAW;
    }

    return place(v, type, sel, rl_chip_values[v->chip_sel]);
}
