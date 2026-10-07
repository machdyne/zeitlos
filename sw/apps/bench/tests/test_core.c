/*
 * bench -- host tests for the core: netlists, nets, buses, and each part
 * against its datasheet. `make test` in sw/apps/bench.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core.h"
#include "../netlist.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static char err[128];

static int load(const char *text) {
    return bn_load(text, err, sizeof(err));
}

static int level(const char *ref) {
    char name[32];
    const char *dot = strchr(ref, '.');
    memcpy(name, ref, (size_t)(dot - ref));
    name[dot - ref] = 0;
    part_t *p = bn_part_find(name);
    for (int i = 0; i < p->type->npins; i++)
        if (!strcmp(p->type->pins[i], dot + 1)) return bn_pin_get(p, i);
    return -1;
}

static bool lit(const char *name) {
    part_t *p = bn_part_find(name);
    return p->type->lit(p);
}

static void click(const char *name, bool down) {
    part_t *p = bn_part_find(name);
    p->type->click(p, down);
}

/* write bytes to 0x20 on bus 0 */
static int wr(int n, const uint8_t *d) {
    return bn_xfer(0, 0x20, d, n, 0, 0, 0);
}

static int reg(uint8_t r, uint8_t *v, int n) {
    return bn_xfer(0, 0x20, &r, 1, v, n, 0);
}

static void errors(void) {
    struct { const char *net, *msg; } e[] = {
        { "led l0 x1.P00", "line 1: x1: no such part (declared later?)" },
        { "tca9535 x1 addr=0x20\nled l0 x1.P99", "line 2: x1.P99: tca9535 has no pin P99" },
        { "tca9535 x1", "line 1: x1: addr= is needed" },
        { "tca9535 x1 addr=0x30", "line 1: x1: addr= is 0x20-0x27 for a tca9535" },
        { "led l0\nled l0", "line 2: l0: declared twice" },
        { "led l0 actve=low", "line 1: l0: a led has no actve" },
        { "led l0 active=sideways", "line 1: l0: active= is high or low" },
        { "frob f1", "line 1: frob: no such part type or statement" },
        { "led 9x", "line 1: 9x: not a name (a letter, then letters, digits, _)" },
        { "tca9535 a addr=0x20\ntca9535 b addr=0x20\nbus main a b",
          "line 3: b: 0x20 is a's address on main" },
        { "led l0\nbus main l0", "line 2: l0: not an I2C part" },
        { "# a comment\n\nmodule m1 LS12", "line 3: LS12: not a module (LS10, LS11, LS99)" },
        { "led l0\nbus b master=l0", "line 2: l0: not a module (a master is a module's C/D)" },
        { "module m1 LS10\nbus a master=m1\nbus b master=m1", "line 3: m1: already the master of a bus" },
        { "module m1 LS10 addr=0x78", "line 1: m1: addr= is 0x08-0x77 for a ls10" },
        { "led l0 l0.PIN l0.PIN", "line 1: l0.PIN: more pins than the part has" },
    };
    for (unsigned i = 0; i < sizeof(e) / sizeof(e[0]); i++)
        CHECK(load(e[i].net) == -1 && !strcmp(err, e[i].msg), "error %u: [%s], not [%s]", i, err, e[i].msg);
}

static void nets(void) {
    CHECK(load("button b1\nled l1 b1.PIN\n") == 0, "%s", err);
    CHECK(level("b1.PIN") == BN_FLOAT && !lit("l1"), "nothing drives: floating, dark");
    CHECK(load("button b1 pullup\nled l1 b1.PIN active=low\n") == 0, "%s", err);
    CHECK(level("b1.PIN") == BN_L1 && !lit("l1"), "pulled up: high");
    click("b1", true);
    CHECK(level("b1.PIN") == BN_L0 && lit("l1"), "pressed: low, the active-low LED lit");
    click("b1", false);
    CHECK(level("b1.PIN") == BN_L1, "released: high again");

    /* a switch stays put; two drivers disagreeing: a conflict */
    CHECK(load("switch s1 pullup\nbutton b1 s1.PIN to=vcc\nled l1 s1.PIN\n") == 0, "%s", err);
    click("s1", true);
    click("s1", false);
    CHECK(level("s1.PIN") == BN_L0, "the switch stays on (low)");
    click("b1", true);
    CHECK(level("s1.PIN") == BN_CONFLICT && bn_conflicts == 1 && !lit("l1"), "a conflict");

    /* net statements merge; pullup / pulldown */
    CHECK(load("led a\nled b\nled c\nnet n1 a.PIN b.PIN\nnet n1 c.PIN\npulldown a.PIN") == 0, "%s", err);
    CHECK(bn_part_find("a")->net[0] == bn_part_find("c")->net[0] && level("c.PIN") == BN_L0,
          "one net, pulled down");
}

static void bus(void) {
    uint8_t v[4];
    CHECK(load("tca9535 x1 addr=0x20\nbus main x1") == 0, "%s", err);
    CHECK(bn_xfer(0, 0x21, (const uint8_t *)"\0", 1, 0, 0, 0) == BN_NACK_ADDR, "nobody at 0x21");
    CHECK(reg(6, v, 2) == BN_ACK && v[0] == 0xFF && v[1] == 0xFF, "config defaults 0xFF");
    int last = (bn_log_next + BN_LOG - 1) % BN_LOG;
    CHECK(bn_log[last].addr == 0x20 && bn_log[last].wn == 1 && bn_log[last].rn == 2 &&
          bn_log[last].r[0] == 0xFF, "logged");
    CHECK(bn_log[(last + BN_LOG - 1) % BN_LOG].status == BN_NACK_ADDR, "the NACK logged too");
}

/* the TCA9535, against its datasheet */
static void tca9535(void) {
    uint8_t v[4];
    CHECK(load("tca9535 x1 addr=0x20\nbus main x1\n"
               "led l0 x1.P00\nled l1 x1.P01 active=low\n"
               "button b0 x1.P10 pullup\nbutton b1 x1.P11\n"
               "pullup x1.INT") == 0, "%s", err);
    CHECK(reg(2, v, 2) == BN_ACK && v[0] == 0xFF && v[1] == 0xFF, "outputs default 0xFF");
    CHECK(reg(4, v, 2) == BN_ACK && v[0] == 0 && v[1] == 0, "polarity defaults 0");
    CHECK(level("x1.P00") == BN_FLOAT, "an input nobody drives floats (no pull-ups)");

    /* port 0 outputs, alternating */
    CHECK(wr(3, (const uint8_t []){ 6, 0x00, 0xFF }) == BN_ACK, "configure");
    CHECK(wr(2, (const uint8_t []){ 2, 0x55 }) == BN_ACK, "output");
    CHECK(level("x1.P00") == BN_L1 && level("x1.P01") == BN_L0, "P00 high, P01 low");
    CHECK(lit("l0") && lit("l1"), "both LEDs lit (one active-low)");

    /* the register pair: the next byte goes to the other register */
    CHECK(wr(3, (const uint8_t []){ 3, 0x0F, 0xAA }) == BN_ACK, "3 then 2");
    CHECK(reg(2, v, 4) == BN_ACK && v[0] == 0xAA && v[1] == 0x0F && v[2] == 0xAA && v[3] == 0x0F,
          "reads alternate within the pair: %02x %02x %02x %02x", v[0], v[1], v[2], v[3]);

    /* the input port shows outputs too; writes to it are ignored */
    CHECK(reg(0, v, 1) == BN_ACK && v[0] == 0xAA, "input port 0 = what it drives: %02x", v[0]);
    wr(2, (const uint8_t []){ 0, 0x12 });
    CHECK(reg(0, v, 1) == BN_ACK && v[0] == 0xAA, "writing the input port: no effect");

    /* port 1 inputs: b0 pulled up, b1 floating (reads 1 here) */
    CHECK(reg(1, v, 1) == BN_ACK && v[0] == 0xFF, "port 1 all ones: %02x", v[0]);
    click("b0", true);
    CHECK(level("x1.INT") == BN_L0, "a changed input: INT low");
    CHECK(reg(1, v, 1) == BN_ACK && v[0] == 0xFE, "b0 pressed reads 0: %02x", v[0]);
    CHECK(level("x1.INT") == BN_L1, "reading the port releases INT");
    click("b0", false);
    CHECK(level("x1.INT") == BN_L0, "and a change back sets it again");
    reg(1, v, 1);

    /* polarity inverts inputs only */
    wr(2, (const uint8_t []){ 5, 0x01 });
    CHECK(reg(1, v, 1) == BN_ACK && v[0] == 0xFE, "port 1 bit 0 inverted: %02x", v[0]);
    wr(2, (const uint8_t []){ 4, 0xFF });
    CHECK(reg(0, v, 1) == BN_ACK && v[0] == 0xAA, "outputs are not inverted: %02x", v[0]);

    /* back to inputs: the outputs let go */
    wr(2, (const uint8_t []){ 6, 0xFF });
    CHECK(level("x1.P00") == BN_FLOAT && !lit("l0"), "inputs again: floating, dark");
}

static void tca9555(void) {
    uint8_t v[2];
    CHECK(load("tca9555 x1 addr=0x27\nbus main x1\nbutton b0 x1.P00\n") == 0, "%s", err);
    CHECK(level("x1.P00") == BN_L1, "the 9555's pull-ups: an input is high");
    click("b0", true);
    CHECK(bn_xfer(0, 0x27, (const uint8_t []){ 0 }, 1, v, 1, 0) == BN_ACK && v[0] == 0xFE,
          "pressed: 0 (%02x)", v[0]);
    wr(1, (const uint8_t []){ 0 });
    CHECK(bn_xfer(0, 0x20, (const uint8_t []){ 0 }, 1, v, 1, 0) == BN_NACK_ADDR, "0x20: nobody");
}

/* the example on the card (examples/panel.net) loads as documented */
static void example(void) {
    static char text[4096];
    FILE *f = fopen("examples/panel.net", "r");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    uint8_t v;
    if (f) fclose(f);
    text[n] = 0;
    CHECK(n > 0 && load(text) == 0, "examples/panel.net: %s", err);
    CHECK(bn_nparts == 9 && bn_nbuses == 1 && bn_part_find("lamp") &&
          !strcmp(bn_part_find("lamp")->label, "Grow light"), "its parts");
    wr(2, (const uint8_t []){ 6, 0x00 });
    wr(2, (const uint8_t []){ 2, 0x55 });
    CHECK(lit("l0") && !lit("l1") && lit("lamp"), "its comments' commands work");
    CHECK(reg(1, &v, 1) == BN_ACK && v == 0xFF, "port 1, nothing pressed");
}

static void modules(void) {
    CHECK(load("module m1 LS10 program=/basic/GROW.BAS\nmodule m2 ls11 addr=0x0d\n"
               "tca9535 x1 addr=0x20\nbus local master=m1 x1\nbus main m1 m2") == 0, "%s", err);
    part_t *m1 = bn_part_find("m1"), *m2 = bn_part_find("m2");
    CHECK(m1 && !strcmp(m1->type->name, "ls10") && m1->type->npins == 5 && m1->addr == 0x0C,
          "an LS10, at 0x0c as a new module");
    CHECK(!strcmp(bn_module_program(m1), "/basic/GROW.BAS"), "its program");
    CHECK(m2->addr == 0x0D && m2->type->npins == 8, "an LS11 at 0x0d, pins A-G and LED");
    CHECK(bn_buses[0].master == 0 && bn_buses[1].master == -1, "m1 masters local");
    CHECK(bn_xfer(1, 0x0C, (const uint8_t []){ 0 }, 1, 0, 0, 0) == BN_NACK_ADDR,
          "nobody runs it: NACK");
}

int main(void) {
    modules();
    example();
    errors();
    nets();
    bus();
    tca9535();
    tca9555();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all bench core tests passed\n");
    return 0;
}
