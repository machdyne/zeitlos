/*
 * Zeitlos poker -- typed commands and mouse clicks.
 * See input.h for why there is a command line and not a keymap.
 */

#include "input.h"

/* -- tiny string helpers ---------------------------------------------
 *
 * Hand-rolled rather than <string.h> plus sscanf, for the reason
 * docs/app_runtime.md gives: one conversion specifier anywhere links
 * picolibc's formatter at a cost of around 100KB. Parsing a command
 * line is exactly the place that would creep in.
 */

static bool is_space(char c) { return c == ' ' || c == '\t'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive compare of `s` against a whole word. */
static bool word_is(const char *s, int n, const char *w)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!w[i]) return false;
        if (lower(s[i]) != w[i]) return false;
    }
    return w[n] == '\0';
}

/* Splits the next whitespace-delimited word out of *p. */
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
        if (v > 100000000) return false;
    }
    *out = v;
    return true;
}

static void say(pt_view_t *v, const char *s)
{
    int i = 0;
    while (s[i] && i < PT_MSG_LEN - 1) { v->message[i] = s[i]; i++; }
    v->message[i] = '\0';
}

static void say_num(pt_view_t *v, const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < PT_MSG_LEN - 1) { v->message[i] = a[i]; i++; }
    pt_num(n, t);
    for (k = 0; t[k] && i < PT_MSG_LEN - 1; k++) v->message[i++] = t[k];
    for (k = 0; b[k] && i < PT_MSG_LEN - 1; k++) v->message[i++] = b[k];
    v->message[i] = '\0';
}

/* -- the bet amount --------------------------------------------------- */

void input_bet_step(pt_view_t *v, int delta)
{
    pk_options_t o;
    int32_t step;

    pk_options(v->g, v->hero, &o);

    if (!o.can_bet && !o.can_raise) { v->bet_to = 0; return; }

    /* A step is half the pot, which is how people actually think about
     * bet sizing, with a floor of one big blind so the control still
     * works when the pot is tiny. Fixed limit has exactly one legal
     * size, so the clamp below collapses every step to it. */
    step = v->g->pot / 2;
    if (step < v->g->big_blind) step = v->g->big_blind;

    if (v->bet_to < o.min_to) v->bet_to = o.min_to;
    v->bet_to += (int32_t)delta * step;

    if (v->bet_to < o.min_to) v->bet_to = o.min_to;
    if (v->bet_to > o.max_to) v->bet_to = o.max_to;
}

/* -- acting ----------------------------------------------------------- */

static pi_action_t do_act(pt_view_t *v, int action, int32_t to)
{
    if (v->g->phase != PK_PHASE_BETTING) {
        say(v, "not your turn");
        return PI_REDRAW;
    }

    if (v->g->actor != v->hero) {
        say(v, "waiting for the others");
        return PI_REDRAW;
    }

    if (!pk_legal(v->g, v->hero, action, to)) {
        say(v, "you cannot do that here");
        return PI_REDRAW;
    }

    pk_act(v->g, action, to);
    return PI_ACTED;
}

static pi_action_t act_raise(pt_view_t *v, int32_t to, bool have_amount)
{
    pk_options_t o;
    int action;

    pk_options(v->g, v->hero, &o);

    if (!o.can_bet && !o.can_raise) {
        say(v, "there is nothing to bet or raise here");
        return PI_REDRAW;
    }

    action = o.can_bet ? PK_BET : PK_RAISE;

    /* No amount means the amount in the box, which is what the -/+
     * controls and the mouse are already looking at. Typing `r` and
     * clicking `raise` therefore do the same thing, which is the whole
     * point of them meeting here. */
    if (!have_amount) {
        to = v->bet_to;
        if (to < o.min_to || to > o.max_to) to = o.min_to;
    }

    if (to < o.min_to) {
        say_num(v, "the smallest raise here is ", o.min_to, "");
        return PI_REDRAW;
    }

    if (to > o.max_to) {
        /* Rounded down rather than refused. Somebody typing a number
         * larger than their stack means "all of it", and telling them
         * the exact figure they should have typed instead is a worse
         * answer than doing it. */
        to = o.max_to;
    }

    v->bet_to = to;
    return do_act(v, action, to);
}

/* -- the draw --------------------------------------------------------- */

static pi_action_t do_draw(pt_view_t *v)
{
    uint8_t idx[PK_MAX_HOLE];
    int n = 0, i;

    if (v->g->phase != PK_PHASE_DRAW || v->g->actor != v->hero) {
        say(v, "there is no draw right now");
        return PI_REDRAW;
    }

    for (i = 0; i < v->g->seat[v->hero].nhole; i++)
        if (v->discard[i]) idx[n++] = (uint8_t)i;

    if (!pk_draw(v->g, idx, n)) {
        say(v, "that draw was refused");
        return PI_REDRAW;
    }

    for (i = 0; i < PK_MAX_HOLE; i++) v->discard[i] = false;

    if (n == 0) say(v, "you stand pat");
    else say_num(v, "you draw ", n, "");

    return PI_ACTED;
}

/* -- commands ---------------------------------------------------------- */

const char *input_help_line(int i)
{
    static const char *const lines[] = {
        "f fold   c check/call   r [n] raise   a all in",
        "Return or Space checks when checking is free",
        "d 1 3 5 discard those cards   p stand pat",
        "new, level 1-8, variant holdem|draw5|stud5|stud7",
        "seats 2-8, limit nl|fl|pl, odds, game, quit",
        "click a button, or click cards to mark a discard",
        0
    };

    if (i < 0) return 0;
    return lines[i];
}

pi_action_t input_command(pt_view_t *v, const char *line)
{
    const char *p = line, *w;
    int n;
    int32_t val;

    n = next_word(&p, &w);
    if (n == 0) return PI_NONE;

    /* A bare number is a raise TO that amount. Nothing else a person
     * types at a poker table is a bare number, and it is what the
     * chips in front of them are measured in. */
    if (to_int(w, n, &val)) return act_raise(v, val, true);

    if (word_is(w, n, "f") || word_is(w, n, "fold"))
        return do_act(v, PK_FOLD, 0);

    if (word_is(w, n, "c") || word_is(w, n, "call") ||
        word_is(w, n, "k") || word_is(w, n, "check")) {
        pk_options_t o;
        pk_options(v->g, v->hero, &o);
        /* pk_act() already treats a call with nothing to call as a
         * check, so this does not have to choose. */
        return do_act(v, o.can_check ? PK_CHECK : PK_CALL, 0);
    }

    if (word_is(w, n, "r") || word_is(w, n, "raise") ||
        word_is(w, n, "b") || word_is(w, n, "bet")) {
        int m = next_word(&p, &w);
        if (m && to_int(w, m, &val)) return act_raise(v, val, true);
        return act_raise(v, 0, false);
    }

    if (word_is(w, n, "a") || word_is(w, n, "allin") ||
        word_is(w, n, "all")) {
        pk_options_t o;
        pk_options(v->g, v->hero, &o);
        if (!o.can_bet && !o.can_raise) {
            /* All in with nothing to raise means calling for
             * everything you have, which pk_act() already caps. */
            return do_act(v, PK_CALL, 0);
        }
        return act_raise(v, o.max_to, true);
    }

    if (word_is(w, n, "p") || word_is(w, n, "pat") ||
        word_is(w, n, "stand")) {
        int i;
        for (i = 0; i < PK_MAX_HOLE; i++) v->discard[i] = false;
        return do_draw(v);
    }

    if (word_is(w, n, "d") || word_is(w, n, "draw") ||
        word_is(w, n, "discard")) {
        int i, marked = 0;
        for (i = 0; i < PK_MAX_HOLE; i++) v->discard[i] = false;
        for (;;) {
            int m = next_word(&p, &w);
            if (!m) break;
            if (!to_int(w, m, &val) || val < 1 ||
                val > v->g->seat[v->hero].nhole) {
                say(v, "discard takes card numbers, 1 to 5");
                return PI_REDRAW;
            }
            v->discard[val - 1] = true;
            marked++;
        }
        /* `d` with no numbers marks nothing and therefore stands pat,
         * which is the same thing `p` does and is what somebody who
         * changed their mind mid-command expects. */
        (void)marked;
        return do_draw(v);
    }

    if (word_is(w, n, "level") || word_is(w, n, "lvl")) {
        int m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val < PK_MIN_LEVEL ||
            val > PK_MAX_LEVEL) {
            say(v, "level takes 1 to 8");
            return PI_REDRAW;
        }
        v->level = (int)val;
        say(v, pk_level_name(v->level));
        return PI_REDRAW;
    }

    if (word_is(w, n, "variant") || word_is(w, n, "v")) {
        int m = next_word(&p, &w);
        char name[12];
        int i;
        const pk_variant_t *nv;
        if (!m || m >= (int)sizeof name) {
            say(v, "variant: holdem draw5 stud5 stud7");
            return PI_REDRAW;
        }
        for (i = 0; i < m; i++) name[i] = lower(w[i]);
        name[m] = '\0';
        nv = pk_variant_find(name);
        if (!nv) {
            say(v, "variant: holdem draw5 stud5 stud7");
            return PI_REDRAW;
        }
        v->pending_variant = nv;
        say(v, nv->label);
        return PI_NEWGAME;
    }

    if (word_is(w, n, "seats")) {
        int m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val < 2 || val > PK_MAX_SEATS) {
            say(v, "seats takes 2 to 8");
            return PI_REDRAW;
        }
        v->pending_seats = (int)val;
        return PI_NEWGAME;
    }

    if (word_is(w, n, "limit")) {
        int m = next_word(&p, &w);
        if (m && word_is(w, m, "nl")) v->pending_limit = PK_LIMIT_NONE;
        else if (m && word_is(w, m, "fl")) v->pending_limit = PK_LIMIT_FIXED;
        else if (m && word_is(w, m, "pl")) v->pending_limit = PK_LIMIT_POT;
        else { say(v, "limit takes nl, fl or pl"); return PI_REDRAW; }
        pk_game_set_limit(v->g, v->pending_limit);
        say(v, "betting structure changed");
        return PI_REDRAW;
    }

    if (word_is(w, n, "new")) return PI_NEWGAME;
    if (word_is(w, n, "deal")) return PI_NEWHAND;
    if (word_is(w, n, "game") || word_is(w, n, "full")) return PI_GAME_MODE;
    if (word_is(w, n, "quit") || word_is(w, n, "exit") ||
        word_is(w, n, "q")) return PI_QUIT;

    if (word_is(w, n, "odds")) {
        /* The hero's own equity, on demand. The estimator is already
         * here and the number is the single most useful thing somebody
         * learning the game can be shown. */
        v->want_odds = true;
        return PI_REDRAW;
    }

    if (word_is(w, n, "help") || word_is(w, n, "?")) {
        say(v, input_help_line(0));
        return PI_REDRAW;
    }

    say(v, "unknown -- F1 for help");
    return PI_REDRAW;
}

/* -- keys -------------------------------------------------------------- */

pi_action_t input_key(pt_view_t *v, uint32_t keysym)
{
    if (keysym == '\r' || keysym == '\n') {

        if (v->cmd_len == 0) {
            pk_options_t o;

            /* THE ONE FREE SHORTCUT. See input.h: checking cannot be
             * regretted, so it gets the empty-Return path. Calling a
             * bet by accident is the mistake worth designing out, so
             * it does not. */
            pk_options(v->g, v->hero, &o);
            if (v->g->phase == PK_PHASE_BETTING &&
                v->g->actor == v->hero && o.can_check)
                return do_act(v, PK_CHECK, 0);

            /* Between hands, Return deals the next one. */
            if (v->g->phase == PK_PHASE_COMPLETE) return PI_NEWHAND;

            /* And during a draw it commits whatever is marked. */
            if (v->g->phase == PK_PHASE_DRAW && v->g->actor == v->hero)
                return do_draw(v);

            return PI_NONE;
        }

        {
            char line[PT_CMD_LEN];
            int i;
            for (i = 0; i < v->cmd_len && i < PT_CMD_LEN - 1; i++)
                line[i] = v->cmd[i];
            line[i] = '\0';
            v->cmd_len = 0;
            v->cmd[0] = '\0';
            return input_command(v, line);
        }
    }

    if (keysym == 0x08 || keysym == 0x7f) {
        if (v->cmd_len > 0) v->cmd[--v->cmd_len] = '\0';
        return PI_REDRAW;
    }

    if (keysym == 0x1b) {
        v->cmd_len = 0;
        v->cmd[0] = '\0';
        return PI_REDRAW;
    }

    if (keysym == ' ' && v->cmd_len == 0) {
        pk_options_t o;
        pk_options(v->g, v->hero, &o);
        if (v->g->phase == PK_PHASE_BETTING && v->g->actor == v->hero &&
            o.can_check)
            return do_act(v, PK_CHECK, 0);
        if (v->g->phase == PK_PHASE_COMPLETE) return PI_NEWHAND;
        return PI_NONE;
    }

    /* The bet amount, without going through the line. These two are
     * the only keys that adjust it, and they are the same operation
     * the -/+ buttons perform. */
    if (v->cmd_len == 0 && (keysym == '+' || keysym == '=')) {
        input_bet_step(v, 1);
        return PI_REDRAW;
    }
    if (v->cmd_len == 0 && keysym == '-') {
        input_bet_step(v, -1);
        return PI_REDRAW;
    }

    if (keysym >= 0x20 && keysym < 0x7f && v->cmd_len < PT_CMD_LEN - 1) {
        v->cmd[v->cmd_len++] = (char)keysym;
        v->cmd[v->cmd_len] = '\0';
        return PI_REDRAW;
    }

    return PI_NONE;
}

/* -- clicks ------------------------------------------------------------- */

pi_action_t input_click(pt_view_t *v, const pt_layout_t *L, int x, int y)
{
    int b, card;

    /* Between hands, a click anywhere deals the next one -- the same
     * thing Return does, and the thing somebody is most likely to want
     * when looking at a finished hand. */
    if (v->g->phase == PK_PHASE_COMPLETE) return PI_NEWHAND;

    card = pt_hero_card_at(L, v, x, y);
    if (card >= 0) {
        if (v->g->phase != PK_PHASE_DRAW || v->g->actor != v->hero)
            return PI_NONE;
        v->discard[card] = !v->discard[card];
        return PI_REDRAW;
    }

    b = pt_button_at(L, x, y);
    if (b < 0) return PI_NONE;

    if (v->g->phase == PK_PHASE_DRAW && v->g->actor == v->hero) {
        /* The action row is relabelled during a draw -- see
         * pt_draw_actions(). The same three rectangles mean different
         * things, so they are decoded differently here. */
        switch (b) {
        case PT_BTN_FOLD:  return do_act(v, PK_FOLD, 0);
        case PT_BTN_CALL:  return do_draw(v);
        case PT_BTN_RAISE: {
            int i;
            for (i = 0; i < PK_MAX_HOLE; i++) v->discard[i] = false;
            return do_draw(v);
        }
        default: return PI_NONE;
        }
    }

    switch (b) {

    case PT_BTN_FOLD:
        return do_act(v, PK_FOLD, 0);

    case PT_BTN_CALL: {
        pk_options_t o;
        pk_options(v->g, v->hero, &o);
        return do_act(v, o.can_check ? PK_CHECK : PK_CALL, 0);
    }

    case PT_BTN_RAISE:
        return act_raise(v, 0, false);

    case PT_BTN_ALLIN: {
        pk_options_t o;
        pk_options(v->g, v->hero, &o);
        if (!o.can_bet && !o.can_raise) return do_act(v, PK_CALL, 0);
        return act_raise(v, o.max_to, true);
    }

    case PT_BTN_LESS:
        input_bet_step(v, -1);
        return PI_REDRAW;

    case PT_BTN_MORE:
        input_bet_step(v, 1);
        return PI_REDRAW;
    }

    return PI_NONE;
}
