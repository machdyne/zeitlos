/*
 * Zeitlos poker -- draw the table on the build machine and write it
 * out as a picture.
 *
 *   cd sw/apps/poker && make render
 *   make render WHAT=flop|showdown|stud|draw|allin|game|narrow
 *
 * See sw/common/tests/zrender.h for why this exists and what it can
 * and cannot catch. In short: tests/test_layout.c checks the
 * relationships somebody thought to write down, and this checks the
 * ones nobody did.
 *
 * For a card table that is most of them. Whether a rank is legible at
 * twenty pixels. Whether an overlapped fan of seven stud cards reads
 * as seven cards or as a smear. Whether a face-down card is instantly
 * distinguishable from a face-up one. Whether the hollow pips that
 * stand in for the red suits read as red or just as damage. None of
 * those is an assertion and all of them are obvious in one look.
 *
 * LOOK AT THE OUTPUT before changing the art in gen_cards.py.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "poker_shim.h"

#include "../table_ui.h"

static pk_game_t game;
static pt_view_t view;

static uint8_t C(const char *s)
{
    uint8_t c = pk_card_parse(s);
    if (c == PK_CARD_NONE) { fprintf(stderr, "bad card %s\n", s); exit(1); }
    return c;
}

static void names(int n)
{
    static const char *const who[] = {
        "you", "Anna", "Bo", "Cy", "Dee", "Eli", "Fay", "Gus" };
    int i;
    for (i = 0; i < n; i++) {
        int k = 0;
        while (who[i][k] && k < 7) { view.name[i][k] = who[i][k]; k++; }
        view.name[i][k] = '\0';
    }
}

static void say(const char *s)
{
    int i = 0;
    while (s[i] && i < PT_MSG_LEN - 1) { view.message[i] = s[i]; i++; }
    view.message[i] = '\0';
}

static void tag(int seat, const char *s)
{
    int i = 0;
    while (s[i] && i < PT_TAG_LEN - 1) { view.tag[seat][i] = s[i]; i++; }
    view.tag[seat][i] = '\0';
}

/* Deals a specific hold'em board and specific holdings, bypassing the
 * betting so a picture can show a moment that would otherwise take a
 * contrived sequence of actions to reach. */
static void set_hole(int seat, const char *a, const char *b)
{
    game.seat[seat].nhole = 2;
    game.seat[seat].hole[0] = C(a);
    game.seat[seat].hole[1] = C(b);
    game.seat[seat].up[0] = false;
    game.seat[seat].up[1] = false;
}

static void set_board(const char *const *c, int n)
{
    int i;
    game.nboard = n;
    for (i = 0; i < n; i++) game.board[i] = C(c[i]);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/poker.pbm";
    const char *what = argc > 2 ? argv[2] : "flop";
    z_win_t win;
    pt_layout_t L;
    int w = 320, h = 240;
    z_clip_t c;
    bool game_mode = !strcmp(what, "game");

    static const char *const flop[] = { "Ah", "Kd", "7c" };
    static const char *const river[] = { "Ah", "Kd", "7c", "2s", "Qh" };

    if (!strcmp(what, "narrow")) { w = 220; h = 200; }

    pk_rng_seed(20260915);

    if (!strcmp(what, "stud")) {
        pk_game_init(&game, &pk_variant_stud7, 6, 1000, 5, 10);
    } else if (!strcmp(what, "draw")) {
        pk_game_init(&game, &pk_variant_draw5, 4, 1000, 5, 10);
    } else {
        pk_game_init(&game, &pk_variant_holdem, 6, 1000, 5, 10);
    }

    pk_hand_begin(&game);

    view.g = &game;
    view.hero = 0;
    view.level = 5;
    view.hand_no = 12;
    view.bet_to = 60;
    names(game.nseats);
    say("your move -- f fold, c call, r raise");

    if (!strcmp(what, "flop") || game_mode) {
        game.street = 1;
        set_board(flop, 3);
        set_hole(0, "Qs", "Qc");
        set_hole(1, "9h", "9d");
        game.pot = 120;
        game.bet_to_match = 40;
        game.seat[1].bet = 40;
        game.seat[0].stack = 940;
        game.actor = 0;
        tag(2, "folds");
        game.seat[2].state = PK_SEAT_FOLDED;
        tag(3, "calls 40");
        game.seat[3].bet = 40;
        if (game_mode) say("full screen -- Esc to leave");

    } else if (!strcmp(what, "showdown")) {
        game.street = 3;
        set_board(river, 5);
        set_hole(0, "Qs", "Qc");
        set_hole(1, "Ac", "Ad");
        set_hole(3, "Kh", "Ks");
        game.seat[2].state = PK_SEAT_FOLDED;
        game.seat[4].state = PK_SEAT_FOLDED;
        game.seat[5].state = PK_SEAT_FOLDED;
        game.pot = 480;
        game.phase = PK_PHASE_COMPLETE;
        game.actor = -1;
        game.npots = 1;
        game.seat[1].won = 480;
        view.reveal = true;
        say("Anna wins 480 with three aces");

    } else if (!strcmp(what, "allin")) {
        game.street = 2;
        set_board(river, 4);
        set_hole(0, "Ah", "Kh");
        game.pot = 1450;
        game.npots = 3;
        game.seat[0].stack = 0;
        game.seat[0].state = PK_SEAT_ALLIN;
        game.seat[1].stack = 0;
        game.seat[1].state = PK_SEAT_ALLIN;
        game.seat[2].stack = 220;
        game.actor = 2;
        tag(1, "all in 300");
        say("three players all in -- running it out");

    } else if (!strcmp(what, "stud")) {
        int s;
        /* Fourth street: three up-cards each, three down. This is the
         * picture the mini-card fan exists for. */
        static const char *const up[6][4] = {
            { "As", "7d", "Th", "2c" },
            { "Kh", "Ks", "9c", "4d" },
            { "3c", "4h", "5s", "6d" },
            { "Qd", "Jh", "8s", "9h" },
            { "2h", "2d", "Tc", "Js" },
            { "Ac", "5c", "6c", "7h" }
        };
        game.street = 3;
        for (s = 0; s < 6; s++) {
            int i;
            game.seat[s].nhole = 6;
            game.seat[s].hole[0] = C("2s");   /* placeholder hole cards */
            game.seat[s].hole[1] = C("3d");
            game.seat[s].up[0] = false;
            game.seat[s].up[1] = false;
            for (i = 0; i < 4; i++) {
                game.seat[s].hole[2 + i] = C(up[s][i]);
                game.seat[s].up[2 + i] = true;
            }
        }
        game.seat[4].state = PK_SEAT_FOLDED;
        game.pot = 310;
        game.bet_to_match = 40;
        game.seat[1].bet = 40;
        game.actor = 0;
        say("sixth street -- kings showing are the danger");

    } else if (!strcmp(what, "draw")) {
        game.street = 0;
        game.seat[0].nhole = 5;
        game.seat[0].hole[0] = C("Ah");
        game.seat[0].hole[1] = C("Ad");
        game.seat[0].hole[2] = C("Kc");
        game.seat[0].hole[3] = C("7s");
        game.seat[0].hole[4] = C("2h");
        game.phase = PK_PHASE_DRAW;
        game.actor = 0;
        view.discard[3] = true;
        view.discard[4] = true;
        game.pot = 60;
        say("pick cards to throw away, then Return");
    }

    if (!z_render_open(&win, w, h)) {
        fprintf(stderr, "render: cannot map the framebuffer here "
            "(Linux/x86-64 only) -- skipping\n");
        return 77;
    }

    pt_init();

    if (game_mode) {
        /* A game-mode page has no window inset at all -- a different
         * rectangle from the windowed one, which is exactly what is
         * worth looking at. */
        pt_layout(&L, &view, 0, 0, 320, 240);
    } else {
        z_win_content_rect(&win, &c);
        pt_layout(&L, &view, c.x0, c.y0, c.x1 - c.x0 + 1, c.y1 - c.y0 + 1);
    }

    pt_draw_all(&L, &view);

    if (game_mode) {
        /* Write the whole page rather than the window's content rect,
         * since in game mode there is no window. */
        FILE *f = fopen(path, "wb");
        int x, y, sx, sy;
        if (!f) { perror(path); return 1; }
        fprintf(f, "P1\n%d %d\n", 320 * 2, 240 * 2);
        for (y = 0; y < 240; y++)
            for (sy = 0; sy < 2; sy++) {
                for (x = 0; x < 320; x++)
                    for (sx = 0; sx < 2; sx++)
                        fputc(shim_get(x, y) ? '1' : '0', f);
                fputc('\n', f);
            }
        fclose(f);
        printf("render: wrote %s -- game-mode page 320x240 at 2x\n", path);
    } else {
        z_render_write(path, &win, 2);
    }

    return 0;
}
