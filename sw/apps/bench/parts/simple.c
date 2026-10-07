/*
 * bench parts -- the simple ones, one pin each:
 *
 *   led     lit when its pin is high (active=low: when low)
 *   load    the same, with a label: "Grow light"
 *   button  pulls its pin to ground while pressed (to=vcc: drives it
 *           high); a pull-up keeps it high otherwise
 *   switch  the same, staying put: a click turns it on or off
 *
 * A pin that floats or is in conflict lights nothing.
 */

#include <string.h>
#include "../core.h"

static const char *const one_pin[] = { "PIN" };

typedef struct {
    bool active_low;
    bool to_vcc;
    bool on;                /* button: pressed; switch: on */
} simple_t;

static const char *init_lamp(part_t *p, const param_t *pp) {
    simple_t *s = BN_STATE_OF(p, simple_t);
    const char *a = bn_param(pp, "active");
    if (a && strcmp(a, "high") && strcmp(a, "low")) return ": active= is high or low";
    s->active_low = a && !strcmp(a, "low");
    return 0;
}

static bool lit(part_t *p) {
    int lv = bn_pin_get(p, 0);
    return BN_STATE_OF(p, simple_t)->active_low ? lv == BN_L0 : lv == BN_L1;
}

static const char *init_button(part_t *p, const param_t *pp) {
    simple_t *s = BN_STATE_OF(p, simple_t);
    const char *to = bn_param(pp, "to");
    if (to && strcmp(to, "gnd") && strcmp(to, "vcc")) return ": to= is gnd or vcc";
    s->to_vcc = to && !strcmp(to, "vcc");
    return 0;
}

static void set(part_t *p, bool on) {
    simple_t *s = BN_STATE_OF(p, simple_t);
    s->on = on;
    bn_pin_drive(p, 0, !on ? BN_NONE : s->to_vcc ? BN_HIGH : BN_LOW);
}

static void click_button(part_t *p, bool down) {
    set(p, down);
}

static void click_switch(part_t *p, bool down) {
    if (down) set(p, !BN_STATE_OF(p, simple_t)->on);
}

static bool pressed(part_t *p) {
    return BN_STATE_OF(p, simple_t)->on;
}

const part_type_t pt_led = {
    "led", "an LED", one_pin, 1, 0, 0, init_lamp, 0, 0, 0, 0, 0, lit, 0, 0,
};

const part_type_t pt_load = {
    "load", "something switched on and off, with a label", one_pin, 1, 0, 0,
    init_lamp, 0, 0, 0, 0, 0, lit, 0, 0,
};

const part_type_t pt_button = {
    "button", "a push button", one_pin, 1, 0, 0, init_button, 0, 0, 0, 0,
    click_button, pressed, 0, 0,
};

const part_type_t pt_switch = {
    "switch", "an on/off switch", one_pin, 1, 0, 0, init_button, 0, 0, 0, 0,
    click_switch, pressed, 0, 0,
};
