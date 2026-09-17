/*
 * Zeitlos blackjack -- draw the table on the build machine.
 *
 *   cd sw/apps/blackjack && make render WHAT=play|split|done
 *
 * See sw/common/tests/zrender.h for why. Here the questions a geometry
 * assertion cannot answer are: whether a hand of six cards is readable
 * overlapped, whether four split hands of small cards are telling
 * apart, and whether a face-down hole card is obviously face down.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "blackjack_shim.h"
#include "../bj_board.h"

static bj_game_t g;
static bj_view_t view;
static bj_layout_t L;
static z_win_t win;

static uint8_t C(const char *s) { return zcard_parse(s); }

static void set_hand(int i, const char *s, int n, int32_t bet)
{
    int k;
    g.hand[i].n = n;
    g.hand[i].bet = bet;
    for (k = 0; k < n; k++) g.hand[i].card[k] = C(s + 3 * k);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/blackjack.pbm";
    const char *what = argc > 2 ? argv[2] : "play";
    bj_rules_t r;
    int x, y, sx, sy;
    FILE *f;

    if (!z_render_open(&win, 320, 240)) {
        fprintf(stderr, "render: cannot map the framebuffer here\n");
        return 77;
    }
    bj_board_init();

    bj_rules_default(&r);
    bj_game_init(&g, &r);
    view.g = &g;
    view.chips = 1240;
    view.bet = 25;
    view.chip_sel = 1;
    view.trng = true;

    g.ndealer = 2;
    g.dealer[0] = C("Ts");
    g.dealer[1] = C("7h");
    g.shoe.pos = g.shoe.n / 3;

    if (!strcmp(what, "split")) {
        g.nhands = 4;
        g.active = 2;
        g.phase = BJ_PHASE_PLAYER;
        set_hand(0, "8h Ts 3d", 3, 25);
        set_hand(1, "8d 2c 4h 5s", 4, 25);
        set_hand(2, "8s Ah", 2, 25);
        set_hand(3, "8c Kd 9h 2s 5c 3d", 6, 25);
        strcpy(view.message, "third hand -- soft 19");
        strcpy(view.cmd, "s");
        view.cmd_len = 1;
    } else if (!strcmp(what, "done")) {
        g.nhands = 1;
        g.phase = BJ_PHASE_DONE;
        set_hand(0, "Ah Kd", 2, 50);
        g.hand[0].won = 125;
        g.staked = 50;
        g.returned = 125;
        strcpy(view.message, "blackjack -- you win 75");
    } else {
        g.nhands = 1;
        g.active = 0;
        g.phase = BJ_PHASE_PLAYER;
        set_hand(0, "9h 4s 3d 2c", 4, 25);
        strcpy(view.message, "eighteen -- your move");
        view.hints = true;
        strcpy(view.cmd, "h");
        view.cmd_len = 1;
    }

    bj_board_layout(&L, &view, 2, 13, 316, 225);
    bj_board_draw(&L, &view);

    f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fprintf(f, "P1\n%d %d\n", 320 * 2, 240 * 2);
    for (y = 0; y < 240; y++)
        for (sy = 0; sy < 2; sy++) {
            for (x = 0; x < 320; x++)
                for (sx = 0; sx < 2; sx++)
                    fputc(z_render_get(x, y) ? '1' : '0', f);
            fputc('\n', f);
        }
    fclose(f);
    printf("render: wrote %s (%s)\n", path, what);

    return 0;
}
