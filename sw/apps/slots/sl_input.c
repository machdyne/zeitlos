/*
 * Zeitlos slots -- typed commands and mouse clicks.
 */

#include "sl_input.h"

#define SL_MAX_BET 25

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

static void say(sl_view_t *v, const char *s)
{
    int i = 0;
    while (s[i] && i < SL_MSG_LEN - 1) { v->message[i] = s[i]; i++; }
    v->message[i] = '\0';
}

static void say2(sl_view_t *v, const char *a, int32_t n, const char *b)
{
    char t[12];
    int i = 0, k;
    while (a[i] && i < SL_MSG_LEN - 1) { v->message[i] = a[i]; i++; }
    sl_num(n, t);
    for (k = 0; t[k] && i < SL_MSG_LEN - 1; k++) v->message[i++] = t[k];
    for (k = 0; b[k] && i < SL_MSG_LEN - 1; k++) v->message[i++] = b[k];
    v->message[i] = '\0';
}

int32_t sl_stake(const sl_view_t *v)
{
    return v->bet * (int32_t)v->lines;
}

const char *sl_input_help_line(int i)
{
    static const char *const lines[] = {
        "Return or SPIN to play; bet 5; lines 1-5; max",
        "cherries pay from the left -- reel 1, then reels 1 and 2",
        "any three bars pay, whatever kind they are",
        "bank, buyin, game, quit -- F1 cycles this help",
        0
    };
    if (i < 0) return 0;
    return lines[i];
}

static si_action_t try_spin(sl_view_t *v)
{
    if (v->spin->spinning) return SI_NONE;

    if (sl_stake(v) > v->chips) {
        /* The stake, not the balance, is what has to be covered -- and
         * saying which is the useful half, because the fix is to lower
         * the bet or the lines rather than to find more chips. */
        say2(v, "that costs ", sl_stake(v), " and you cannot cover it");
        return SI_REDRAW;
    }

    return SI_SPIN;
}

si_action_t sl_input_command(sl_view_t *v, const char *line)
{
    const char *p = line, *w;
    int n, m;
    int32_t val;

    n = next_word(&p, &w);
    if (n == 0) return SI_NONE;

    if (word_is(w, n, "spin") || word_is(w, n, "s")) return try_spin(v);

    if (word_is(w, n, "bet") || word_is(w, n, "b")) {
        if (v->spin->spinning) { say(v, "the reels are turning"); return SI_REDRAW; }
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val < 1 || val > SL_MAX_BET) {
            say2(v, "bet takes 1 to ", SL_MAX_BET, " coins a line");
            return SI_REDRAW;
        }
        v->bet = val;
        say2(v, "a spin now costs ", sl_stake(v), "");
        return SI_REDRAW;
    }

    if (word_is(w, n, "lines") || word_is(w, n, "l")) {
        if (v->spin->spinning) { say(v, "the reels are turning"); return SI_REDRAW; }
        m = next_word(&p, &w);
        if (!m || !to_int(w, m, &val) || val < 1 || val > SL_LINES) {
            say2(v, "lines takes 1 to ", SL_LINES, "");
            return SI_REDRAW;
        }
        v->lines = (int)val;
        say2(v, "a spin now costs ", sl_stake(v), "");
        return SI_REDRAW;
    }

    if (word_is(w, n, "max")) {
        if (v->spin->spinning) return SI_NONE;
        v->lines = SL_LINES;
        v->bet = SL_MAX_BET;
        /* Backed off until it is affordable rather than refused. "Max
         * bet" on a machine you cannot afford to max out should give
         * you the biggest bet you CAN make, which is what the button
         * means everywhere else. */
        while (v->bet > 1 && sl_stake(v) > v->chips) v->bet--;
        say2(v, "max bet -- ", sl_stake(v), " a spin");
        return SI_REDRAW;
    }

    if (word_is(w, n, "bank")) { say2(v, "you have ", v->chips, " chips"); return SI_REDRAW; }
    if (word_is(w, n, "buyin")) return SI_BUYIN;
    if (word_is(w, n, "game") || word_is(w, n, "full")) return SI_GAME_MODE;
    if (word_is(w, n, "quit") || word_is(w, n, "q")) return SI_QUIT;

    if (word_is(w, n, "help") || word_is(w, n, "?")) {
        say(v, sl_input_help_line(0));
        return SI_REDRAW;
    }

    say(v, "unknown -- F1 for help");
    return SI_REDRAW;
}

si_action_t sl_input_key(sl_view_t *v, uint32_t keysym)
{
    if (keysym == '\r' || keysym == '\n') {

        if (v->cmd_len == 0) {
            /* Return on an empty line spins. It commits the stake that
             * is already set rather than choosing one, so it cannot
             * cost anything unintended -- and on a slot machine it is
             * the only verb there is. */
            return try_spin(v);
        }

        {
            char l[SL_CMD_LEN];
            int i;
            for (i = 0; i < v->cmd_len && i < SL_CMD_LEN - 1; i++) l[i] = v->cmd[i];
            l[i] = '\0';
            v->cmd_len = 0;
            v->cmd[0] = '\0';
            return sl_input_command(v, l);
        }
    }

    if (keysym == 0x08 || keysym == 0x7f) {
        if (v->cmd_len > 0) v->cmd[--v->cmd_len] = '\0';
        return SI_STATUS;
    }

    if (keysym == 0x1b) {
        v->cmd_len = 0;
        v->cmd[0] = '\0';
        return SI_STATUS;
    }

    if (keysym >= 0x20 && keysym < 0x7f && v->cmd_len < SL_CMD_LEN - 1) {
        v->cmd[v->cmd_len++] = (char)keysym;
        v->cmd[v->cmd_len] = '\0';
        /* Only the two text rows changed. Repainting the whole machine
         * per keystroke is what made roulette flash while typing. */
        return SI_STATUS;
    }

    return SI_NONE;
}

si_action_t sl_input_click(sl_view_t *v, const sl_layout_t *L, int x, int y)
{
    int b;

    if (v->spin->spinning) return SI_NONE;

    /* THE HANDLE SPINS. It is the most obvious thing on the machine to
     * click and it did nothing, which is worse than not drawing one. */
    if (sl_board_lever_at(L, x, y)) return try_spin(v);

    b = sl_board_btn_at(L, x, y);
    if (b < 0) return SI_NONE;

    switch (b) {
    case SL_BTN_SPIN:  return try_spin(v);
    case SL_BTN_MAX:   return sl_input_command(v, "max");

    case SL_BTN_BET:
        /* The buttons CYCLE, because a slot machine has no keyboard and
         * a person clicking `bet` wants the next value, not a dialog. */
        v->bet = (v->bet >= SL_MAX_BET) ? 1 : v->bet + 1;
        say2(v, "a spin now costs ", sl_stake(v), "");
        return SI_REDRAW;

    case SL_BTN_LINES:
        v->lines = (v->lines >= SL_LINES) ? 1 : v->lines + 1;
        say2(v, "a spin now costs ", sl_stake(v), "");
        return SI_REDRAW;
    }

    return SI_NONE;
}
