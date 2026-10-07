/*
 * bench parts -- gpio: a real PMOD port (docs/bench.md, "Real hardware").
 *
 *   gpio g1 port=1 in=3,4 out=7
 *
 * Its pins are the PMOD's, by the numbers printed on the connector: 1-4
 * on the top row, 7-10 below (GPIO bits 0-7, docs/gpio.md). in= pins
 * bring the real level onto their nets: low drives the net low, high is
 * only a pull-up, since the pin's own weak pull-up may be all that makes
 * it high. out= pins put their net's level on the real pin, open drain:
 * low pulls it down, anything else lets it go to the pull-up -- safe to
 * wire to a real pin that is itself an output. A bus may continue onto
 * pins 1 (SCL) and 2 (SDA): bus NAME segment=g1, and then those pins are
 * not in= or out=. bench_app.c does the reading and writing.
 */

#include <string.h>
#include <stdlib.h>
#include "../core.h"

static const char *const pmod_pins[] = { "1", "2", "3", "4", "7", "8", "9", "10" };

typedef struct {
    uint8_t port, in, out;      /* in=, out=: bit masks */
} gpio_t;

/* "3,4,10" to bits; -1 for a pin that is not a PMOD signal pin */
static int pins_of(const char *s) {
    int mask = 0;
    while (s && *s) {
        int n = atoi(s), bit = -1;
        for (int i = 0; i < 8; i++)
            if (atoi(pmod_pins[i]) == n && n) bit = i;
        if (bit < 0) return -1;
        mask |= 1 << bit;
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    return mask;
}

static const char *init(part_t *p, const param_t *pp) {
    gpio_t *g = BN_STATE_OF(p, gpio_t);
    long port = bn_param_num(pp, "port", -1);
    int in = pins_of(bn_param(pp, "in")), out = pins_of(bn_param(pp, "out"));
    memset(g, 0, sizeof(*g));
    if (port < 0 || port > 7) return ": port= is 0-7 (a PMOD port, docs/gpio.md)";
    if (in < 0 || out < 0) return ": in= and out= are PMOD pins, 1-4 and 7-10";
    if (in & out) return ": a pin is in= or out=, not both";
    g->port = (uint8_t)port;
    g->in = (uint8_t)in;
    g->out = (uint8_t)out;
    for (int i = 0; i < 8; i++) p->drive[i] = BN_NONE;
    return 0;
}

static char mark(part_t *p, int pin) {
    gpio_t *g = BN_STATE_OF(p, gpio_t);
    return g->in >> pin & 1 ? 'i' : g->out >> pin & 1 ? 'o' : '-';
}

int bn_gpio_port(part_t *p) { return BN_STATE_OF(p, gpio_t)->port; }
uint8_t bn_gpio_in_mask(part_t *p) { return BN_STATE_OF(p, gpio_t)->in; }
uint8_t bn_gpio_out_mask(part_t *p) { return BN_STATE_OF(p, gpio_t)->out; }

void bn_gpio_inputs(part_t *p, uint8_t bits) {
    gpio_t *g = BN_STATE_OF(p, gpio_t);
    for (int i = 0; i < 8; i++)
        if (g->in >> i & 1) bn_pin_drive(p, i, bits >> i & 1 ? BN_PULLUP : BN_LOW);
}

uint8_t bn_gpio_outputs(part_t *p) {
    gpio_t *g = BN_STATE_OF(p, gpio_t);
    uint8_t low = 0;
    for (int i = 0; i < 8; i++)
        if (g->out >> i & 1 && p->net[i] >= 0 && bn_nets[p->net[i]].level == BN_L0)
            low |= (uint8_t)(1 << i);
    return low;
}

bool bn_real(void) {
    extern const part_type_t pt_gpio;
    for (int i = 0; i < bn_nparts; i++)
        if (bn_parts[i].type == &pt_gpio) return true;
    return false;
}

const part_type_t pt_gpio = {
    "gpio", "a real PMOD port: its pins on bench nets", pmod_pins, 8, 0, 0,
    init, 0, 0, 0, 0, 0, 0, 0, 0, mark,
};
