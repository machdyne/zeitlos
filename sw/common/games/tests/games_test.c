/*
 * Zeitlos -- host tests for sw/common/games.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Native cc, no RISC-V toolchain, the shipped sources. The bank's
 * filesystem half is compiled out with -DZBANK_NO_FS: what is tested
 * here is the parser, the formatter and the arithmetic, which is where
 * anything can be wrong. The three I/O wrappers are a read, a write
 * and a call into the pure half.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../zrand.h"
#include "../zbank.h"
#include "../zdice.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) return;
    failures++;
    printf("FAIL: %s\n", what);
}

static void check_eq(long got, long want, const char *what)
{
    checks++;
    if (got == want) return;
    failures++;
    printf("FAIL: %s -- got %ld, want %ld\n", what, got, want);
}

/* -- the bounded draw -------------------------------------------------- */

static uint32_t feed_vals[8];
static int feed_n, feed_i, feed_used;

static uint32_t feed_rng(void *ctx)
{
    (void)ctx;
    feed_used++;

    /* A BROKEN ACCEPT BOUND DOES NOT RETURN, IT SPINS, so the source
     * has to stop it -- otherwise this file hangs instead of failing,
     * and a hung test looks like a slow machine and gets killed while
     * a failing one names the problem. */
    if (feed_used > 1000) {
        printf("FAIL: zg_rng_below() did not terminate -- %d draws and "
            "still rejecting\n", feed_used);
        exit(1);
    }

    if (feed_i >= feed_n) return 0;
    return feed_vals[feed_i++];
}

static void feed(uint32_t a, uint32_t b)
{
    feed_vals[0] = a;
    feed_vals[1] = b;
    feed_n = 2;
    feed_i = 0;
    feed_used = 0;
}

static void test_rand(void)
{
    uint32_t n;
    int worst = 0;
    long bad = 0;

    zg_rng_set(feed_rng, NULL);
    check(zg_rng_installed(), "an installed source is reported as installed");

    /* TERMINATION for every bound, which is the whole reason this
     * function exists rather than z_rng_below() being called. That one
     * computes its accept bound as 2^32 - (2^32 % n); for a power of
     * two that is 2^32, which truncates to 0, and `while (v >= 0)` on
     * an unsigned never ends. Every shuffle and every roulette wheel
     * with a power-of-two pocket count would hang. */
    for (n = 1; n <= 300; n++) {
        uint32_t v;
        feed(0xffffffffu, 0xfffffffeu);
        v = zg_rng_below(n);
        if (feed_used > worst) worst = feed_used;
        if (n > 0 && v >= n) bad++;
    }
    check_eq(bad, 0, "every draw lands inside its bound");
    check(worst <= 8, "and no bound needs more than a handful of draws");

    for (n = 2; n <= (1u << 20); n <<= 1) {
        feed(0xffffffffu, 0u);
        (void)zg_rng_below(n);
        check_eq(feed_used, 1,
            "a power-of-two bound rejects nothing and draws once");
    }

    /* THE BOUNDARY, exactly. 2^32 mod 3 is 1, so precisely one word --
     * 0xffffffff -- must be thrown away. One more or one fewer and the
     * accept region stops being a multiple of n, which is the
     * definition of the bias this is here to avoid. */
    feed(0xffffffffu, 0u);
    check_eq(zg_rng_below(3), 0, "the top word is rejected for n = 3");
    check_eq(feed_used, 2, "and exactly one redraw was needed");

    feed(0xfffffffeu, 0u);
    check_eq(zg_rng_below(3), 0xfffffffeu % 3, "the one below it is kept");
    check_eq(feed_used, 1, "with no redraw");

    /* 37 -- a roulette wheel. 2^32 mod 37 is 7, so the top seven words
     * go. */
    check_eq((int)(((0xffffffffu % 37u) + 1u) % 37u), 7,
        "2^32 mod 37 is seven");
    feed(0xffffffffu - 6u, 0u);
    check_eq(zg_rng_below(37), 0, "the seventh from the top is rejected");
    check_eq(feed_used, 2, "and redrawn");

    feed(0xffffffffu - 7u, 0u);
    check_eq(zg_rng_below(37), (0xffffffffu - 7u) % 37u,
        "the eighth from the top is kept");
    check_eq(feed_used, 1, "with no redraw");

    zg_rng_set(NULL, NULL);
    check(!zg_rng_installed(), "clearing the source is reported too");

    /* The built-in fallback still has to produce a usable spread --
     * it is what an app that forgot to install a source gets, and it
     * should be obviously repetitive across boots, not broken. */
    {
        long bins[37];
        long t;
        double chi2 = 0.0, expect;
        int i;

        for (i = 0; i < 37; i++) bins[i] = 0;
        zg_rng_seed(4242);
        for (t = 0; t < 370000; t++) bins[zg_rng_below(37)]++;

        expect = 370000.0 / 37.0;
        for (i = 0; i < 37; i++) {
            double d = (double)bins[i] - expect;
            chi2 += d * d / expect;
        }

        /* 36 degrees of freedom; the 0.999 critical value is about 68.
         * 110 leaves a wide margin, because this must not fail once a
         * month on a correct generator. A modulo bias scores in the
         * hundreds. */
        check(chi2 < 110.0, "the fallback spreads a wheel evenly");
        if (chi2 >= 110.0) printf("      chi2 = %.1f over 36 df\n", chi2);
    }
}

/* -- the bank ---------------------------------------------------------- */

static void test_bank_defaults(void)
{
    zbank_t b;

    zbank_defaults(&b);
    check_eq(b.chips, ZBANK_START, "a fresh bank starts with a stack");
    check_eq(b.peak, ZBANK_START, "and its peak is that stack");
    check_eq(b.ngames, 0, "and knows about no games yet");
    check_eq((long)b.buyins, 0, "and has never been topped up");
}

static void test_bank_roundtrip(void)
{
    zbank_t a, b;
    char buf[ZBANK_FILE_MAX];
    int n;

    zbank_defaults(&a);
    zbank_apply(&a, "roulette", -120);
    zbank_apply(&a, "poker", 340);
    zbank_apply(&a, "roulette", 40);

    n = zbank_format(&a, buf, sizeof buf);
    check(n > 0, "a bank formats");
    check(n < ZBANK_FILE_MAX, "and fits the file budget");

    check_eq(zbank_parse(buf, n, &b), ZBANK_OK, "and parses back");
    check_eq(b.chips, a.chips, "with the same balance");
    check_eq(b.peak, a.peak, "the same peak");
    check_eq(b.ngames, a.ngames, "and the same games");

    {
        zbank_game_t *g = zbank_game(&b, "roulette");
        check(g != NULL, "roulette survived the round trip");
        check_eq(g->net, -80, "with its net");
        check_eq((long)g->rounds, 2, "and its round count");
    }

    /* The worst case: every game slot full, so the file is as long as
     * it ever gets. */
    {
        static const char *const names[ZBANK_MAX_GAMES] = {
            "roulette", "blackjack", "poker", "baccarat",
            "craps", "keno", "slots", "sicbo" };
        int i;
        zbank_defaults(&a);
        for (i = 0; i < ZBANK_MAX_GAMES; i++)
            zbank_apply(&a, names[i], -999999);
        n = zbank_format(&a, buf, sizeof buf);
        check(n > 0, "a full bank still fits ZBANK_FILE_MAX");
        check_eq(zbank_parse(buf, n, &b), ZBANK_OK, "and parses back");
        check_eq(b.ngames, ZBANK_MAX_GAMES, "with every game");
    }
}

static void test_bank_apply(void)
{
    zbank_t b;

    zbank_defaults(&b);
    zbank_apply(&b, "roulette", 500);
    check_eq(b.chips, ZBANK_START + 500, "a win adds chips");
    check_eq(b.peak, ZBANK_START + 500, "and moves the peak");

    zbank_apply(&b, "roulette", -700);
    check_eq(b.chips, ZBANK_START - 200, "a loss takes them away");
    check_eq(b.peak, ZBANK_START + 500, "and leaves the peak alone");

    /* A balance must not go negative. A game that stakes more than the
     * bank holds is a bug in that game, and a negative balance would
     * quietly break every affordability check downstream. */
    zbank_apply(&b, "roulette", -999999);
    check_eq(b.chips, 0, "a balance floors at zero");

    {
        zbank_game_t *g = zbank_game(&b, "roulette");
        check_eq((long)g->rounds, 3, "every round is counted");
        check(g->net < 0, "and the net is negative after all that");
    }

    /* A second game keeps its own record. */
    zbank_apply(&b, "poker", 250);
    check_eq(b.chips, 250, "the second game spends the same pile");
    check_eq((long)zbank_game(&b, "poker")->rounds, 1, "with its own count");
    check_eq((long)zbank_game(&b, "roulette")->rounds, 3,
        "and the first game's count is untouched");

    /* Nine games into eight slots. The ninth must not corrupt anything
     * -- it simply goes unrecorded, and the chips still move. */
    {
        int i;
        char name[4];
        zbank_defaults(&b);
        for (i = 0; i < ZBANK_MAX_GAMES + 3; i++) {
            name[0] = 'g'; name[1] = (char)('a' + i); name[2] = '\0';
            zbank_apply(&b, name, -1);
        }
        check_eq(b.ngames, ZBANK_MAX_GAMES, "the game table does not overrun");
        check_eq(b.chips, ZBANK_START - (ZBANK_MAX_GAMES + 3),
            "and chips still move for a game with no slot");
    }
}

/* -- loans ---------------------------------------------------------------- */

static void test_bank_loans(void)
{
    zbank_t b;
    char buf[ZBANK_FILE_MAX];
    int n;

    /* THE VIG IS CHARGED ONCE, AT THE COUNTER, and rounded up. A rate
     * that rounds to nothing on small loans is not a rate. */
    check_eq(zbank_owed(1000), 1200, "borrowing 1000 owes 1200");
    check_eq(zbank_owed(5), 6, "borrowing 5 owes 6");
    check_eq(zbank_owed(1), 2, "and borrowing 1 owes 2, not 1");
    check_eq(zbank_owed(0), 0, "borrowing nothing owes nothing");
    check_eq(zbank_owed(-50), 0, "and a negative loan is not a loan");

    {
        int32_t a, bad = 0;
        for (a = 1; a <= 5000; a++)
            if (zbank_owed(a) <= a) bad++;
        check_eq(bad, 0, "every loan costs more than it hands over");
    }

    /* Net worth is what the debt is FOR. Without it a loan is free
     * money with extra steps. */
    zbank_defaults(&b);
    check_eq(zbank_net(&b), ZBANK_START, "a fresh bank owes nothing");
    b.chips = 1500;
    b.debt = 1200;
    check_eq(zbank_net(&b), 300, "net worth is chips minus debt");
    b.chips = 100;
    check_eq(zbank_net(&b), -1100, "and goes negative when it should");

    /* The debt survives a round trip, and an old file without one
     * still reads -- lenient parsing is what stops a new field from
     * bricking a bank written by an earlier build. */
    zbank_defaults(&b);
    b.chips = 640;
    b.debt = 1200;
    zbank_apply(&b, "slots", -60);
    n = zbank_format(&b, buf, sizeof buf);
    check(n > 0, "a bank with a debt formats");
    {
        zbank_t c;
        check_eq(zbank_parse(buf, n, &c), ZBANK_OK, "and parses back");
        check_eq(c.debt, 1200, "with the debt intact");
        check_eq(c.chips, b.chips, "and the chips");
    }

    {
        const char *old = "version: 1\nchips: 500\npeak: 900\n";
        zbank_t c;
        check_eq(zbank_parse(old, (int)strlen(old), &c), ZBANK_OK,
            "a file from before loans existed still reads");
        check_eq(c.debt, 0, "and owes nothing");
    }

    /* A negative debt is not a thing. */
    {
        const char *bad = "chips: 100\ndebt: -500\n";
        zbank_t c;
        zbank_parse(bad, (int)strlen(bad), &c);
        check_eq(c.debt, 0, "a negative debt in the file reads as none");
    }
}

static void test_bank_parse(void)
{
    zbank_t b;
    const char *text;

    /* An empty or absent file is a FIRST RUN, not damage. */
    check_eq(zbank_parse("", 0, &b), ZBANK_OK, "an empty file is fine");
    check_eq(b.chips, ZBANK_START, "and yields a fresh bank");

    check_eq(zbank_parse(NULL, 0, &b), ZBANK_OK, "so is nothing at all");

    /* Comments and blank lines. */
    text = "# a comment\n\n   \nchips: 250\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_OK,
        "comments and blanks are skipped");
    check_eq(b.chips, 250, "and the balance is read");

    /* Both separators, as /zeitlos.cfg allows. */
    text = "chips = 300\npeak:400\nbuyins   2\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_OK,
        "colon, equals and bare whitespace all separate");
    check_eq(b.chips, 300, "chips");
    check_eq(b.peak, 400, "peak");
    check_eq((long)b.buyins, 2, "buyins");

    /* Unknown keys and rubbish lines are IGNORED, not fatal. A future
     * game writing a key this build does not know must not brick the
     * bank for this one. */
    text = "chips: 77\nsomething.else: 5\nnot a setting at all\n"
        "chips_but_not_really: 9\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_OK,
        "unknown keys are ignored");
    check_eq(b.chips, 77, "and the balance still reads");

    /* A value that is not a number is skipped rather than read as
     * zero, which would silently empty the bank. */
    text = "chips: lots\npeak: 900\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_CORRUPT,
        "a non-numeric balance is not a balance");

    text = "chips: 500\npeak: plenty\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_OK,
        "a bad value elsewhere is survivable");
    check_eq(b.chips, 500, "and the balance still reads");

    /* THE CASE THAT MATTERS: a file with content but no balance is
     * CORRUPT, not fresh. Treating it as fresh would replace a real
     * bankroll with a default one, which is the single unrecoverable
     * outcome. */
    text = "version: 1\npeak: 4000\ngame.poker.net: -20\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_CORRUPT,
        "content with no balance is damage, not a first run");

    /* A file cut off by a power failure mid-write still yields
     * everything before the cut. */
    {
        zbank_t full;
        char buf[ZBANK_FILE_MAX];
        int n, cut;

        zbank_defaults(&full);
        zbank_apply(&full, "roulette", -120);
        zbank_apply(&full, "poker", 60);
        n = zbank_format(&full, buf, sizeof buf);

        for (cut = 40; cut < n; cut += 7) {
            int rv = zbank_parse(buf, cut, &b);
            check(rv == ZBANK_OK || rv == ZBANK_CORRUPT,
                "a truncated file parses or reports damage, never worse");
            if (rv == ZBANK_OK)
                check(b.chips >= 0, "and never yields a negative balance");
        }
    }

    /* Negative numbers, since a game's net is signed. */
    text = "chips: 10\ngame.roulette.net: -4500\ngame.roulette.rounds: 12\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_OK,
        "a negative net parses");
    check_eq(zbank_game(&b, "roulette")->net, -4500, "with its sign");
    check_eq((long)zbank_game(&b, "roulette")->rounds, 12, "and its rounds");

    /* A game name longer than the field is dropped rather than
     * overrunning it. */
    text = "chips: 10\ngame.averyverylongname.net: 5\n";
    check_eq(zbank_parse(text, (int)strlen(text), &b), ZBANK_OK,
        "an overlong game name is survivable");
    check_eq(b.chips, 10, "and the balance still reads");
}

static void test_bank_format_bounds(void)
{
    zbank_t b;
    char small[16];
    int i;

    zbank_defaults(&b);

    /* Every buffer size from nothing to plenty. The formatter must
     * refuse rather than write past the end, at every cut point --
     * this is the kind of thing that is right for the size somebody
     * tested and wrong one byte either side. */
    for (i = 0; i < (int)sizeof small; i++) {
        /* STATIC, not on the stack. An overrun of a stack array trips
         * the stack protector and the process aborts with no message
         * before the guard bytes below are ever examined -- a crash of
         * unknown origin instead of a named failure. Verified by
         * removing the formatter's bounds check and watching this
         * report it properly. */
        static char guard[sizeof small + 8];
        int k, rv;
        for (k = 0; k < (int)sizeof guard; k++) guard[k] = (char)0xAB;
        rv = zbank_format(&b, guard, i);
        check(rv < 0 || rv < i, "a short buffer refuses or fits");
        for (k = i > 0 ? i : 0; k < (int)sizeof guard; k++)
            if (guard[k] != (char)0xAB) {
                printf("FAIL: zbank_format wrote past %d bytes\n", i);
                failures++;
                break;
            }
        checks++;
    }
}

/* -- dice ------------------------------------------------------------------
 *
 * The pips are COUNTED OUT OF THE BITMAP rather than compared against a
 * second copy of the layout. Every pip is the same disc, so the dark
 * area inside a die's body is exactly the face value times one pip --
 * which catches a missing pip, a doubled one, or a face wired to the
 * wrong arrangement, without this file knowing where any of them go.
 */
static int dice_dark(const uint32_t *tile, int s)
{
    int x, y, n = 0;

    /* Inside the body only: the corners and the border are dark by
     * design and would swamp the count. */
    for (y = 3; y < s - 3; y++)
        for (x = 3; x < s - 3; x++)
            if (!((tile[y] >> x) & 1u)) n++;

    return n;
}

static void test_dice(void)
{
    static const int sizes[2] = { Z_DICE_BIG, Z_DICE_SMALL };
    int i, f, bad = 0;

    for (i = 0; i < 2; i++) {
        int s = sizes[i];
        int pip = dice_dark(z_dice_tile(s, 1), s);

        check(pip > 0, "a die's single pip has some ink in it");

        for (f = 1; f <= Z_DICE_FACES; f++) {
            int got = dice_dark(z_dice_tile(s, f), s);
            if (got != pip * f) {
                printf("      %d-pixel die, face %d: %d dark, expected %d\n",
                    s, f, got, pip * f);
                bad++;
            }
        }
    }
    check_eq(bad, 0, "every face has exactly its own number of pips");

    /* The body is LIT, which is what makes a die opaque -- rolled over
     * anything it covers what was there, so no clear is needed and
     * nothing flickers. */
    {
        const uint32_t *t = z_dice_tile(Z_DICE_BIG, 1);
        int mid = Z_DICE_BIG / 2;
        check((t[3] >> 3) & 1u, "the body is lit, not the pips");
        check(!((t[mid] >> mid) & 1u), "and the centre pip of a one is dark");
    }

    check(z_dice_tile(Z_DICE_BIG, 0) == NULL, "there is no face zero");
    check(z_dice_tile(Z_DICE_BIG, 7) == NULL, "and no face seven");
    check(z_dice_tile(Z_DICE_SMALL, 3) != z_dice_tile(Z_DICE_BIG, 3),
        "the two sizes are different tiles");
    check(z_dice_tile(1, 3) == z_dice_tile(Z_DICE_SMALL, 3),
        "an unoffered size falls back to the small one, not to nothing");
}

int main(void)
{
    printf("games: shared library tests\n");

    test_rand();
    test_dice();
    test_bank_defaults();
    test_bank_apply();
    test_bank_roundtrip();
    test_bank_loans();
    test_bank_parse();
    test_bank_format_bounds();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
