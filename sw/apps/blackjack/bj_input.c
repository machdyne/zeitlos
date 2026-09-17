/*
 * Zeitlos blackjack -- typed commands and mouse clicks.
 * See bj_input.h for why both routes end in the same call.
 */

#include "bj_input.h"

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

static void say(bj_view_t *v, const char *s)
{
    int i = 0;
    while (s[i] && i < BJ_MSG_LEN - 1) { v->message[i] = s[i]; i++; }
    v->message[i] = '\0';
}

static void say2(bj_view_t *v, const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < BJ_MSG_LEN - 1) { v->message[i] = a[i]; i++; }
    bj_num(n, t);
    for (k = 0; t[k] && i < BJ_MSG_LEN - 1; k++) v->message[i++] = t[k];
    for (k = 0; b[k] && i < BJ_MSG_LEN - 1; k++) v->message[i++] = b[k];
    v->message[i] = '\0';
}

const char *bj_input_help_line(int i)
{
    static const char *const lines[] = {
        "h hit   s stand   d double   p split   u surrender",
        "deal (or Return), bet 25, chip 5|25|100|500",
        "insure 10, or `no` to decline",
        "decks 1-8, h17|s17, pays 3:2|6:5, das, surrender",
        "hint toggles basic strategy advice",
        "bank, buyin, game, quit -- F1 cycles this help",
        0
    };
    if (i < 0) return 0;
    return lines[i];
}

/* -- acting --------------------------------------------------------------- */

static bi_action_t act(bj_view_t *v, int action)
{
    bj_options_t o;

    if (v->g->phase != BJ_PHASE_PLAYER) {
        say(v, "not your turn");
        return BI_REDRAW;
    }

    bj_options(v->g, &o);

    /* THE STAKE MUST BE COVERED BEFORE IT IS COMMITTED. Doubling and
     * splitting each put another bet up, and the engine does not know
     * about the bank -- it is the app's business, so it is checked
     * here, on both routes at once. */
    if (action == BJ_DOUBLE || action == BJ_SPLIT) {
        if (v->g->staked + v->g->hand[v->g->active].bet > v->chips) {
            say(v, "not enough chips for that");
            return BI_REDRAW;
        }
    }

    if (!bj_act(v->g, action)) {
        say(v, "you cannot do that here");
        return BI_REDRAW;
    }

    return BI_ACTED;
}

bi_action_t bj_input_command(bj_view_t *v, const char *line)
{
    const char *p = line, *w;
    int n, m;
    int32_t val;

    n = next_word(&p, &w);
    if (n == 0) return BI_NONE;

    if (word_is(w, n, "h") || word_is(w, n, "hit")) return act(v, BJ_HIT);
    if (word_is(w, n, "s") || word_is(w, n, "stand")) return act(v, BJ_STAND);
    if (word_is(w, n, "d") || word_is(w, n, "double")) return act(v, BJ_DOUBLE);
    if (word_is(w, n, "p") || word_is(w, n, "split")) return act(v, BJ_SPLIT);
    if (word_is(w, n, "u") || word_is(w, n, "surrender"))
        return act(v, BJ_SURRENDER);

    if (word_is(w, n, "insure") || word_is(w, n, "i")) {
        int32_t amt = v->g->hand[0].bet / 2;
        m = next_word(&p, &w);
        if (m && !to_int(w, m, &amt)) { say(v, "insure takes an amount"); return BI_REDRAW; }
        if (!bj_insure(v->g, amt)) {
            say2(v, "insurance is capped at ", v->g->hand[0].bet / 2, "");
            return BI_REDRAW;
        }
        return BI_ACTED;
    }

    if (word_is(w, n, "no") || word_is(w, n, "n")) {
        if (!bj_insure(v->g, 0)) { say(v, "nothing to decline"); return BI_REDRAW; }
        return BI_ACTED;
    }

    if (word_is(w, n, "deal")) return BI_DEAL;

    if (word_is(w, n, "bet") || word_is(w, n, "b")) {
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val <= 0) {
            say(v, "bet takes an amount");
            return BI_REDRAW;
        }
        if (val > v->chips) { say2(v, "you only have ", v->chips, ""); return BI_REDRAW; }
        v->bet = val;
        say2(v, "betting ", val, " a hand");
        return BI_REDRAW;
    }

    if (word_is(w, n, "chip")) {
        int i;
        m = next_word(&p, &w);
        if (m && to_int(w, m, &val))
            for (i = 0; i < BJ_NCHIPS; i++)
                if (bj_chip_values[i] == val) { v->chip_sel = i; return BI_REDRAW; }
        say(v, "chip takes 5, 25, 100 or 500");
        return BI_REDRAW;
    }

    /* The rules. Every one of these moves the house edge, which is why
     * they are changeable and why the table prints them. */
    if (word_is(w, n, "decks")) {
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val < 1 || val > Z_DECK_MAX_DECKS) {
            say2(v, "decks takes 1 to ", Z_DECK_MAX_DECKS, "");
            return BI_REDRAW;
        }
        v->g->rules.ndecks = (int)val;
        return BI_NEWGAME;
    }

    if (word_is(w, n, "h17")) { v->g->rules.dealer_hits_soft17 = true;
        say(v, "dealer hits soft 17 -- worse for you"); return BI_REDRAW; }
    if (word_is(w, n, "s17")) { v->g->rules.dealer_hits_soft17 = false;
        say(v, "dealer stands on all 17s"); return BI_REDRAW; }

    if (word_is(w, n, "pays")) {
        m = next_word(&p, &w);
        if (m && word_is(w, m, "3:2")) {
            v->g->rules.bj_pay_num = 3; v->g->rules.bj_pay_den = 2;
            say(v, "blackjack pays 3:2 -- about 0.4% edge");
            return BI_REDRAW;
        }
        if (m && word_is(w, m, "6:5")) {
            v->g->rules.bj_pay_num = 6; v->g->rules.bj_pay_den = 5;
            say(v, "blackjack pays 6:5 -- edge jumps to about 1.8%");
            return BI_REDRAW;
        }
        say(v, "pays takes 3:2 or 6:5");
        return BI_REDRAW;
    }

    if (word_is(w, n, "das")) {
        v->g->rules.double_after_split = !v->g->rules.double_after_split;
        say(v, v->g->rules.double_after_split ?
            "doubling after a split allowed" : "no doubling after a split");
        return BI_REDRAW;
    }

    if (word_is(w, n, "surr")) {
        v->g->rules.surrender = !v->g->rules.surrender;
        say(v, v->g->rules.surrender ? "surrender allowed" : "no surrender");
        return BI_REDRAW;
    }

    if (word_is(w, n, "hint") || word_is(w, n, "hints")) {
        v->hints = !v->hints;
        if (!v->hints) say(v, "hints off");
        else if (bj_hint_exact(&v->g->rules)) say(v, "hints on -- basic strategy");
        else say(v, "hints on -- approximate for these rules");
        return BI_REDRAW;
    }

    if (word_is(w, n, "bank")) { say2(v, "you have ", v->chips, " chips"); return BI_REDRAW; }
    if (word_is(w, n, "buyin")) return BI_BUYIN;
    if (word_is(w, n, "game") || word_is(w, n, "full")) return BI_GAME_MODE;
    if (word_is(w, n, "quit") || word_is(w, n, "q")) return BI_QUIT;

    if (word_is(w, n, "help") || word_is(w, n, "?")) {
        say(v, bj_input_help_line(0));
        return BI_REDRAW;
    }

    say(v, "unknown -- F1 for help");
    return BI_REDRAW;
}

bi_action_t bj_input_key(bj_view_t *v, uint32_t keysym)
{
    if (keysym == '\r' || keysym == '\n') {

        if (v->cmd_len == 0) {
            /* Return on an empty line deals the next hand. It commits
             * the bet that is already set rather than choosing one, so
             * it cannot cost anything unintended -- the same reasoning
             * that gives roulette's Return the spin. */
            if (v->g->phase == BJ_PHASE_DONE ||
                v->g->phase == BJ_PHASE_BETTING) return BI_DEAL;
            return BI_NONE;
        }

        {
            char line[BJ_CMD_LEN];
            int i;
            for (i = 0; i < v->cmd_len && i < BJ_CMD_LEN - 1; i++)
                line[i] = v->cmd[i];
            line[i] = '\0';
            v->cmd_len = 0;
            v->cmd[0] = '\0';
            return bj_input_command(v, line);
        }
    }

    if (keysym == 0x08 || keysym == 0x7f) {
        if (v->cmd_len > 0) v->cmd[--v->cmd_len] = '\0';
        return BI_STATUS;
    }

    if (keysym == 0x1b) {
        v->cmd_len = 0;
        v->cmd[0] = '\0';
        return BI_STATUS;
    }

    if (v->cmd_len == 0 && (keysym == '+' || keysym == '=')) {
        if (v->chip_sel < BJ_NCHIPS - 1) v->chip_sel++;
        return BI_REDRAW;
    }
    if (v->cmd_len == 0 && keysym == '-') {
        if (v->chip_sel > 0) v->chip_sel--;
        return BI_REDRAW;
    }

    if (keysym >= 0x20 && keysym < 0x7f && v->cmd_len < BJ_CMD_LEN - 1) {
        v->cmd[v->cmd_len++] = (char)keysym;
        v->cmd[v->cmd_len] = '\0';
        return BI_STATUS;
    }

    return BI_NONE;
}

bi_action_t bj_input_click(bj_view_t *v, const bj_layout_t *L, int x, int y)
{
    int b = bj_board_chip_at(L, x, y);

    if (b >= 0) {
        /* Between rounds a chip sets the bet; during one it does
         * nothing, because the bet is already committed. */
        v->chip_sel = b;
        if (v->g->phase == BJ_PHASE_DONE || v->g->phase == BJ_PHASE_BETTING) {
            v->bet = bj_chip_values[b];
            say2(v, "betting ", v->bet, " a hand");
        }
        return BI_REDRAW;
    }

    b = bj_board_btn_at(L, x, y);
    if (b < 0) return BI_NONE;

    switch (b) {
    case BJ_BTN_HIT:       return act(v, BJ_HIT);
    case BJ_BTN_STAND:     return act(v, BJ_STAND);
    case BJ_BTN_DOUBLE:    return act(v, BJ_DOUBLE);
    case BJ_BTN_SPLIT:     return act(v, BJ_SPLIT);
    case BJ_BTN_SURRENDER: return act(v, BJ_SURRENDER);

    case BJ_BTN_DEAL:
        /* One button, two meanings, decided by the phase: while
         * insurance is offered it is the only decision available, and a
         * separate button that exists for one moment in twenty rounds
         * would be a control nobody recognises when it appears. */
        if (v->g->phase == BJ_PHASE_INSURANCE) {
            bj_insure(v->g, v->g->hand[0].bet / 2);
            return BI_ACTED;
        }
        return BI_DEAL;
    }

    return BI_NONE;
}
