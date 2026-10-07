/*
 * ls99 -- end to end, on a host: a virtual module (ls99.c, with the real
 * interpreter and Sechs core) on bench's own core (netlists, nets, parts,
 * buses), in one process, with exact virtual time. Real BASIC programs:
 * the grow light over 50 virtual hours, a panel on a TCA9535, and the
 * module's Sechs side as a master sees it. `make test` in sw/apps/ls99.
 */

#include <stdio.h>
#include <string.h>
#include "../ls99.h"
#include "../../../ext/basic/basic.h"
#include "../../../ext/basic/sechs/sechs.h"
#include "../../bench/core.h"
#include "../../bench/netlist.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ---- a board made of bench's core ---- */

static part_t *mp;                  /* the module's part */
static char console[8192];
static int console_len;
static uint32_t stop_at;            /* virtual ms: HALT the program then */
static struct { uint32_t ms; int level; } changes[64];
static int nchanges, watch = -1;    /* the bench pin whose drives are logged */

static int pin_of(int i) {          /* ls99's A-G, LED to the part's pins */
    if (i == LS99_LED) return mp->type->npins - 1;
    return i < mp->type->npins - 1 ? i : -1;
}

static void b_drive(int i, int d) {
    int p = pin_of(i);
    if (p < 0) return;
    if (p == watch && mp->drive[p] != d && nchanges < 64) {
        changes[nchanges].ms = bn_now;
        changes[nchanges].level = d;
        nchanges++;
    }
    bn_pin_drive(mp, p, d);
}

static int b_level(int i) {
    int p = pin_of(i);
    return p < 0 ? LS99_FLOAT : bn_pin_get(mp, p);
}

static int b_i2c(uint8_t addr, const uint8_t *w, int wn, uint8_t *r, int rn) {
    for (int b = 0; b < bn_nbuses; b++)
        if (bn_buses[b].master == mp - bn_parts)
            return bn_xfer(b, addr, w, wn, r, rn, 0) == BN_ACK ? 0 : -1;
    return -1;
}

static void b_sleep(uint32_t ms) {
    bn_now += ms;
}

static void b_service(void) {
    if (stop_at && bn_now >= stop_at) sechs.cmd = CMD_HALT;    /* a master's HALT */
}

static void b_console(char c) {
    if (c != '\r' && console_len < (int)sizeof(console) - 1) {
        console[console_len++] = c;
        console[console_len] = 0;
    }
}

static void b_set_addr(uint8_t a) {
    mp->addr = a;
}

static const ls99_board_t board = { b_drive, b_level, b_i2c, b_sleep, b_service, b_console, b_set_addr };

/* bench sends the module's transactions here: in-process */
static int module_xfer(part_t *p, const uint8_t *w, int wn, uint8_t *r, int rn) {
    (void)p;
    ls99_slave(w, wn, r, rn);
    return BN_ACK;
}

/* files: not in these tests */
int hw_fopen(const char *n, uint8_t m) { (void)n; (void)m; return HW_ERR_UNSUPPORTED; }
int hw_fread(uint8_t *b, uint16_t n) { (void)b; (void)n; return HW_ERR_UNSUPPORTED; }
int hw_fwrite(const uint8_t *b, uint16_t n) { (void)b; (void)n; return HW_ERR_UNSUPPORTED; }
int hw_fclose(void) { return HW_ERR_UNSUPPORTED; }
void hw_fabort(void) { }
int hw_fdelete(const char *n) { (void)n; return HW_ERR_UNSUPPORTED; }
int hw_fdir(fs_dir_cb cb) { (void)cb; return HW_ERR_UNSUPPORTED; }
int hw_fformat(void) { return HW_ERR_UNSUPPORTED; }

static void start(const char *net, int profile) {
    char err[128];
    if (bn_load(net, err, sizeof(err))) printf("netlist: %s\n", err);
    mp = bn_part_find("m1");
    bn_module_xfer = module_xfer;
    console_len = 0;
    console[0] = 0;
    nchanges = 0;
    stop_at = 0;
    ls99_init(&board, profile, mp->addr);
    ls99_line("NEW");
    console_len = 0;
    console[0] = 0;
}

static void type(const char *lines) {
    char l[256];
    while (*lines) {
        int n = 0;
        while (*lines && *lines != '\n') l[n++] = *lines++;
        l[n] = 0;
        if (*lines) lines++;
        ls99_line(l);
    }
}

/* a master's register read on bus b */
static int sechs_read(int b, uint8_t reg, uint8_t *d, int n) {
    return bn_xfer(b, mp->addr, &reg, 1, d, n, 0);
}

/* ---- the tests ---- */

static void identity(void) {
    uint8_t d[64];
    start("module m1 LS10\nbus main m1", LS99_LS10);
    CHECK(sechs_read(0, SR_SIG0, d, 2) == BN_ACK && d[0] == 'S' && d[1] == '6', "S6");
    CHECK(sechs_read(0, SR_INFO, d, 64) == BN_ACK &&
          strstr((char *)d, "mod=LS99\nprofile=LS10\n"), "INFO: [%.40s]", d);
    CHECK(bn_xfer(0, 0x0D, d, 1, 0, 0, 0) == BN_NACK_ADDR, "nobody at 0x0d");
}

/* what a real LS10 says, the virtual one says */
static void too_big(void) {
    start("module m1 LS10\nbus main m1", LS99_LS10);
    ls99_line("10 SLEEP 57600");
    CHECK(strstr(console, "TOO BIG") != 0, "SLEEP 57600: TOO BIG, as on a module");
}

static void profile(void) {
    char line[80];
    start("module m1 LS10\nbus main m1", LS99_LS10);
    for (int i = 1; i <= 40; i++) {
        snprintf(line, sizeof(line), "%d PRINT \"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\"", i * 10);
        ls99_line(line);
    }
    CHECK(strstr(console, "NO MEMORY") != 0, "an LS10 holds 1KB: NO MEMORY");
    start("module m1 LS11\nbus main m1", LS99_LS11);
    for (int i = 1; i <= 40; i++) {
        snprintf(line, sizeof(line), "%d PRINT \"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\"", i * 10);
        ls99_line(line);
    }
    CHECK(strstr(console, "NO MEMORY") == 0, "an LS11 holds it");
}

static void grow_light(void) {
    start("module m1 LS10\nload lamp \"Grow light\" m1.C\n", LS99_LS10);
    watch = 2;                                      /* pin C */
    /* numbers are 16-bit in BASIC 1: SLEEP 57600 is TOO BIG on a module,
     * and here too -- so the hours are counted */
    type("10 PINS NET, NET, PP, -\n"
         "20 OUT 3, 1: FOR H = 1 TO 16: SLEEP 3600: NEXT\n"     /* 16 hours on */
         "30 OUT 3, 0: FOR H = 1 TO 8: SLEEP 3600: NEXT\n"      /* 8 hours off */
         "40 GOTO 20");
    stop_at = 50UL * 3600 * 1000;                   /* 50 virtual hours */
    ls99_line("RUN");
    watch = -1;
    static const uint32_t want_ms[] = { 0, 57600000, 86400000, 144000000, 172800000 };
    static const int want_lv[] = { BN_HIGH, BN_LOW, BN_HIGH, BN_LOW, BN_HIGH };
    int k = 0;
    for (int i = 0; i < nchanges; i++) {
        if (changes[i].level == BN_NONE) continue;  /* RUN: pins let go */
        if (changes[i].ms == 0 && changes[i].level == BN_LOW && k == 0) continue;  /* PINS: low */
        if (k < 5) {
            CHECK(changes[i].ms == want_ms[k] && changes[i].level == want_lv[k],
                  "switch %d at %lu ms (%s), not %lu (%s)", k, (unsigned long)changes[i].ms,
                  changes[i].level == BN_HIGH ? "on" : "off", (unsigned long)want_ms[k],
                  want_lv[k] == BN_HIGH ? "on" : "off");
        }
        k++;
    }
    CHECK(k == 5, "five switches in 50 hours: %d; console [%s]", k, console);
    CHECK(ls99_halted() && strstr(console, "BREAK"), "HALT stopped it");
    part_t *lamp = bn_part_find("lamp");
    CHECK(lamp->type->lit(lamp), "on at 50 hours (on from 48)");
}

static void panel(void) {
    uint8_t v;
    start("module m1 LS10\n"
          "tca9535 x1 addr=0x20\n"
          "bus local master=m1 x1\n"
          "bus main m1\n"
          "led l0 x1.P00\nled l1 x1.P01\n"
          "button b0 x1.P10 pullup\n", LS99_LS10);
    type("10 PINS NET, NET, I2C, I2C\n"
         "20 I2C 32, 6, 0\n"                        /* port 0: outputs */
         "30 I2C 32, 2, 85\n"                       /* 0x55 */
         "40 REG 0, I2CR(32, 1)");                  /* port 1, for the master */
    bn_part_find("b0")->type->click(bn_part_find("b0"), true);
    ls99_line("RUN");
    part_t *l0 = bn_part_find("l0"), *l1 = bn_part_find("l1");
    CHECK(l0->type->lit(l0) && !l1->type->lit(l1), "the program lit P00, not P01");
    CHECK(sechs_read(1, SR_REG + 0, &v, 1) == BN_ACK && v == 0xFE,
          "REG 0, read over Sechs: the pressed button (%02x)", v);
    CHECK(strstr(console, "ERROR") == 0, "no errors: [%s]", console);

    /* the same program with nothing at 0x21: I2C ERROR, as on the module */
    type("20 I2C 33, 6, 0");
    ls99_line("RUN");
    CHECK(strstr(console, "I2C ERROR IN 20") != 0, "I2C ERROR IN 20: [%s]", console);
}

static void control(void) {
    uint8_t v = CMD_HALT;
    start("module m1 LS10\nbus main m1", LS99_LS10);
    uint8_t w[2] = { SR_CONTROL, CMD_HALT };
    bn_xfer(0, 0x0C, w, 2, 0, 0, 0);
    ls99_poll();
    CHECK(ls99_halted(), "CONTROL HALT: halted");
    ls99_poll();                    /* STATUS follows on the next turn, as on a module */
    CHECK(sechs_read(0, SR_STATUS, &v, 1) == BN_ACK && (v & ST_HALTED), "STATUS says so");
    w[1] = CMD_RUN;
    bn_xfer(0, 0x0C, w, 2, 0, 0, 0);
    ls99_poll();
    CHECK(!ls99_halted(), "CONTROL RUN: running again");
    uint8_t a[3] = { SR_ADDR, 0x22, (uint8_t)~0x22 };
    bn_xfer(0, 0x0C, a, 3, 0, 0, 0);
    ls99_poll();
    CHECK(mp->addr == 0x22 && sechs_read(0, SR_SIG0, &v, 1) == BN_ACK, "ADDR: now at 0x22");
}

static char *slurp(const char *path) {
    static char buf[2][4096];
    static int which;
    FILE *f = fopen(path, "r");
    char *b = buf[which ^= 1];
    size_t n = f ? fread(b, 1, sizeof(buf[0]) - 1, f) : 0;
    if (f) fclose(f);
    b[n] = 0;
    return b;
}

/* the examples on the card (sw/apps/bench/examples): their netlists and
 * their programs, as typed in by ls99_app.c */
static void examples(void) {
    uint8_t v;
    /* the grow light, the override off: 0h on, 16h off, 24h on, 40h off, 48h on */
    start(slurp("../bench/examples/growlight.net"), LS99_LS10);
    type(slurp("../bench/examples/GROW.BAS"));
    CHECK(strstr(console, "ERROR") == 0 && strstr(console, "NO MEMORY") == 0,
          "GROW.BAS fits an LS10: [%s]", console);
    watch = 2;
    stop_at = 50UL * 3600 * 1000;
    ls99_line("RUN");
    watch = -1;
    int on = 0, off = 0, bad = 0;
    for (int i = 0; i < nchanges; i++) {
        uint32_t h = changes[i].ms / 3600000, rest = changes[i].ms % 3600000;
        if (changes[i].level == BN_HIGH) { on++; if (rest || (h != 0 && h != 24 && h != 48)) bad++; }
        if (changes[i].level == BN_LOW && changes[i].ms) { off++; if (rest || (h != 16 && h != 40)) bad++; }
    }
    CHECK(on == 3 && off == 2 && !bad, "GROW.BAS: on %d, off %d, wrong %d", on, off, bad);

    /* the override on: never off */
    start(slurp("../bench/examples/growlight.net"), LS99_LS10);
    type(slurp("../bench/examples/GROW.BAS"));
    bn_part_find("keep")->type->click(bn_part_find("keep"), true);
    watch = 2;
    stop_at = 50UL * 3600 * 1000;
    ls99_line("RUN");
    watch = -1;
    off = 0;
    for (int i = 0; i < nchanges; i++) off += changes[i].level == BN_LOW && changes[i].ms;
    CHECK(off == 0, "Keep on: never off (%d)", off);

    /* the panel: b1 held lights l1, and the master sees it in REG 0 */
    start(slurp("../bench/examples/modpanel.net"), LS99_LS10);
    type(slurp("../bench/examples/PANEL.BAS"));
    bn_part_find("b1")->type->click(bn_part_find("b1"), true);
    stop_at = 1000;
    ls99_line("RUN");
    part_t *l0 = bn_part_find("l0"), *l1 = bn_part_find("l1");
    CHECK(l1->type->lit(l1) && !l0->type->lit(l0), "PANEL.BAS: b1 lights l1 only");
    CHECK(sechs_read(0, SR_REG + 0, &v, 1) == BN_ACK && v == 0xFD, "REG 0 = %02x", v);
}

/* BLINK.BAS, for a real module or a virtual one: C toggles every 250ms,
 * and stays on while D is low */
static void blink(void) {
    start("module m1 LS10\nload lamp \"C\" m1.C\nswitch sw \"D\" m1.D pullup\n", LS99_LS10);
    type(slurp("../bench/examples/BLINK.BAS"));
    watch = 2;
    stop_at = 2000;
    ls99_line("RUN");
    watch = -1;
    int highs = 0, lows = 0;
    for (int i = 0; i < nchanges; i++) {
        if (changes[i].level == BN_HIGH) highs++;
        if (changes[i].level == BN_LOW && changes[i].ms) lows++;
    }
    CHECK(highs == 4 && lows == 4, "two seconds: on 4 times, off 4 (%d, %d)", highs, lows);
    start("module m1 LS10\nload lamp \"C\" m1.C\nswitch sw \"D\" m1.D pullup\n", LS99_LS10);
    type(slurp("../bench/examples/BLINK.BAS"));
    bn_part_find("sw")->type->click(bn_part_find("sw"), true);
    watch = 2;
    stop_at = 2000;
    ls99_line("RUN");
    watch = -1;
    lows = 0;
    for (int i = 0; i < nchanges; i++) lows += changes[i].level == BN_LOW && changes[i].ms;
    CHECK(lows == 0, "D low: C stays on (%d offs)", lows);
}

int main(void) {
    blink();
    examples();
    identity();
    too_big();
    profile();
    grow_light();
    panel();
    control();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all ls99 tests passed\n");
    return 0;
}
