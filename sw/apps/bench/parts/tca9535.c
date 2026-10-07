/*
 * bench parts -- TCA9535 and TCA9555: 16-bit I2C I/O expanders (Texas
 * Instruments), at 0x20-0x27. The same chip but for the TCA9555's
 * pull-ups on its inputs; the TCA9535 has none, so an input nothing
 * drives floats (and the bench shows it).
 *
 * Four register pairs, port 0 then port 1:
 *   0, 1   input port: the pins, whatever their direction; inputs
 *          inverted where the polarity register says. Writes ignored.
 *   2, 3   output port: what the outputs drive. 0xFF at power-up.
 *   4, 5   polarity inversion, inputs only. 0.
 *   6, 7   configuration: 1 an input, 0 an output. 0xFF: all inputs.
 * The command byte (the first of a write) picks the register; every
 * further byte, written or read, moves to the other register of its pair.
 *
 * INT (open drain) is pulled low while an input differs from what it was
 * when its port was last read; reading the port releases it.
 *
 * A floating input reads 1 here; on the chip it is unpredictable.
 */

#include <string.h>
#include "../core.h"

#define INT_PIN 16

typedef struct {
    uint8_t out[2], pol[2], cfg[2];
    uint8_t ptr;
    uint8_t seen[2];        /* the inputs when each port was last read */
    bool pullups;           /* TCA9555 */
} tca_t;

static const char *const pin_names[] = {
    "P00", "P01", "P02", "P03", "P04", "P05", "P06", "P07",
    "P10", "P11", "P12", "P13", "P14", "P15", "P16", "P17", "INT",
};

/* the pins of a port as they are, before polarity */
static uint8_t pins(part_t *p, int port) {
    uint8_t v = 0;
    for (int b = 0; b < 8; b++)
        if (bn_pin_get(p, port * 8 + b) != BN_L0) v |= (uint8_t)(1 << b);
    return v;
}

static void interrupt(part_t *p) {
    tca_t *t = BN_STATE_OF(p, tca_t);
    bool on = false;
    for (int port = 0; port < 2; port++)
        if ((pins(p, port) ^ t->seen[port]) & t->cfg[port]) on = true;
    bn_pin_drive(p, INT_PIN, on ? BN_LOW : BN_NONE);
}

static void drives(part_t *p) {
    tca_t *t = BN_STATE_OF(p, tca_t);
    for (int i = 0; i < 16; i++) {
        int port = i / 8, m = 1 << (i % 8);
        if (t->cfg[port] & m) bn_pin_drive(p, i, t->pullups ? BN_PULLUP : BN_NONE);
        else bn_pin_drive(p, i, t->out[port] & m ? BN_HIGH : BN_LOW);
    }
    interrupt(p);
}

static const char *init_common(part_t *p, bool pullups) {
    tca_t *t = BN_STATE_OF(p, tca_t);
    memset(t, 0, sizeof(*t));
    t->out[0] = t->out[1] = 0xFF;
    t->cfg[0] = t->cfg[1] = 0xFF;
    t->pullups = pullups;
    for (int i = 0; i < 16; i++) p->drive[i] = pullups ? BN_PULLUP : BN_NONE;
    return 0;
}

static const char *init9535(part_t *p, const param_t *pp) {
    (void)pp;
    return init_common(p, false);
}

static const char *init9555(part_t *p, const param_t *pp) {
    (void)pp;
    return init_common(p, true);
}

static uint8_t reg_read(part_t *p, int r) {
    tca_t *t = BN_STATE_OF(p, tca_t);
    int port = r & 1;
    switch (r >> 1) {
    case 0: {
        uint8_t v = pins(p, port);
        t->seen[port] = v;                  /* reading releases INT */
        return v ^ (t->pol[port] & t->cfg[port]);
    }
    case 1: return t->out[port];
    case 2: return t->pol[port];
    default: return t->cfg[port];
    }
}

static int i2c_write(part_t *p, const uint8_t *d, int n) {
    tca_t *t = BN_STATE_OF(p, tca_t);
    t->ptr = d[0] & 7;
    for (int i = 1; i < n; i++) {
        int port = t->ptr & 1;
        switch (t->ptr >> 1) {
        case 1: t->out[port] = d[i]; break;
        case 2: t->pol[port] = d[i]; break;
        case 3: t->cfg[port] = d[i]; break;
        default: break;                     /* the input port: ignored */
        }
        t->ptr ^= 1;
    }
    drives(p);
    return n;
}

static void i2c_read(part_t *p, uint8_t *d, int n) {
    tca_t *t = BN_STATE_OF(p, tca_t);
    for (int i = 0; i < n; i++) {
        d[i] = reg_read(p, t->ptr);
        t->ptr ^= 1;
    }
    interrupt(p);
}

static void inputs(part_t *p) {
    interrupt(p);
}

const part_type_t pt_tca9535 = {
    "tca9535", "16-bit I2C I/O expander, no pull-ups (TI)", pin_names, 17, 0x20, 0x27,
    init9535, i2c_write, i2c_read, 0, inputs, 0, 0, 0, 0,
};

const part_type_t pt_tca9555 = {
    "tca9555", "16-bit I2C I/O expander, with pull-ups (TI)", pin_names, 17, 0x20, 0x27,
    init9555, i2c_write, i2c_read, 0, inputs, 0, 0, 0, 0,
};
