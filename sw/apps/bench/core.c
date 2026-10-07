/*
 * bench -- the core (core.h).
 */

#include <string.h>
#include "core.h"

part_t bn_parts[BN_PARTS];
int bn_nparts;
net_t bn_nets[BN_NETS];
int bn_nnets;
bus_t bn_buses[BN_BUSES];
int bn_nbuses;
xfer_log_t bn_log[BN_LOG];
int bn_log_next;
uint32_t bn_now;
int bn_conflicts;
int (*bn_module_xfer)(part_t *p, const uint8_t *w, int wn, uint8_t *r, int rn);

static int depth;           /* nested resolves: a circuit that oscillates */
#define DEPTH_MAX 32

void bn_clear(void) {
    memset(bn_parts, 0, sizeof(bn_parts));
    memset(bn_nets, 0, sizeof(bn_nets));
    memset(bn_buses, 0, sizeof(bn_buses));
    memset(bn_log, 0, sizeof(bn_log));
    bn_nparts = bn_nnets = bn_nbuses = bn_log_next = bn_conflicts = 0;
    bn_now = 0;
    depth = 0;
}

/* ---- nets ---- */

int bn_net_find(const char *name) {
    for (int i = 0; i < bn_nnets; i++)
        if (!strcmp(bn_nets[i].name, name)) return i;
    return -1;
}

int bn_net_new(const char *name) {
    if (bn_nnets >= BN_NETS) return -1;
    net_t *n = &bn_nets[bn_nnets];
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, BN_NAME - 1);
    n->level = BN_FLOAT;
    return bn_nnets++;
}

void bn_net_merge(int keep, int gone) {
    if (keep == gone) return;
    for (int i = 0; i < bn_nparts; i++)
        for (int j = 0; j < BN_PINS; j++)
            if (bn_parts[i].net[j] == gone) bn_parts[i].net[j] = (int16_t)keep;
    if (!bn_nets[keep].pull) bn_nets[keep].pull = bn_nets[gone].pull;
    bn_nets[gone].name[0] = 0;      /* unused from now on */
}

static int level_of(int n) {
    int hi = 0, lo = 0, up = bn_nets[n].pull == BN_PULLUP, down = bn_nets[n].pull == BN_PULLDOWN;
    for (int i = 0; i < bn_nparts; i++) {
        part_t *p = &bn_parts[i];
        for (int j = 0; j < p->type->npins; j++) {
            if (p->net[j] != n) continue;
            switch (p->drive[j]) {
            case BN_HIGH: hi = 1; break;
            case BN_LOW: lo = 1; break;
            case BN_PULLUP: up = 1; break;
            case BN_PULLDOWN: down = 1; break;
            }
        }
    }
    if (hi && lo) return BN_CONFLICT;
    if (hi) return BN_L1;
    if (lo) return BN_L0;
    if (up && down) return BN_FLOAT;
    if (up) return BN_L1;
    if (down) return BN_L0;
    return BN_FLOAT;
}

void bn_net_resolve(int n) {
    if (n < 0 || !bn_nets[n].name[0]) return;
    int lv = level_of(n);
    if (lv == bn_nets[n].level) return;
    if (depth >= DEPTH_MAX) {           /* oscillating: call it a conflict */
        bn_nets[n].level = BN_CONFLICT;
        bn_conflicts++;
        return;
    }
    bn_nets[n].level = (uint8_t)lv;
    if (lv == BN_CONFLICT) bn_conflicts++;
    depth++;
    for (int i = 0; i < bn_nparts; i++) {
        part_t *p = &bn_parts[i];
        if (!p->type->inputs) continue;
        for (int j = 0; j < p->type->npins; j++)
            if (p->net[j] == n) {
                p->type->inputs(p);
                break;
            }
    }
    depth--;
}

void bn_resolve_all(void) {
    for (int n = 0; n < bn_nnets; n++) {
        bn_nets[n].level = 0xFF;        /* force: tell everyone once */
        bn_net_resolve(n);
    }
}

int bn_pin_get(part_t *p, int pin) {
    int n = p->net[pin];
    if (n < 0) {                        /* not connected: its own drive */
        int d = p->drive[pin];
        return d == BN_HIGH || d == BN_PULLUP ? BN_L1 : d == BN_LOW || d == BN_PULLDOWN ? BN_L0 : BN_FLOAT;
    }
    return bn_nets[n].level;
}

void bn_pin_drive(part_t *p, int pin, int drive) {
    if (p->drive[pin] == drive) return;
    p->drive[pin] = (uint8_t)drive;
    bn_net_resolve(p->net[pin]);
}

/* ---- parts and buses ---- */

part_t *bn_part_find(const char *name) {
    for (int i = 0; i < bn_nparts; i++)
        if (!strcmp(bn_parts[i].name, name)) return &bn_parts[i];
    return 0;
}

int bn_bus_find(const char *name) {
    for (int i = 0; i < bn_nbuses; i++)
        if (!strcmp(bn_buses[i].name, name)) return i;
    return -1;
}

static void log_xfer(int b, uint8_t addr, const uint8_t *w, int wn, const uint8_t *r,
                     int rn, int status) {
    xfer_log_t *l = &bn_log[bn_log_next];
    bn_log_next = (bn_log_next + 1) % BN_LOG;
    memset(l, 0, sizeof(*l));
    l->ms = bn_now;
    l->bus = (uint8_t)b;
    l->addr = addr;
    l->status = (uint8_t)status;
    l->wn = (uint8_t)(wn > 255 ? 255 : wn);
    l->rn = (uint8_t)(rn > 255 ? 255 : rn);
    if (w) memcpy(l->w, w, wn < 8 ? (size_t)wn : 8);
    if (r && status == BN_ACK) memcpy(l->r, r, rn < 8 ? (size_t)rn : 8);
}

int bn_xfer(int b, uint8_t addr, const uint8_t *w, int wn, uint8_t *r, int rn,
            int *written) {
    part_t *p = 0;
    int st = BN_ACK, n = 0;
    for (int i = 0; i < bn_nparts; i++)
        if (bn_parts[i].bus == b && bn_parts[i].addr == addr && bn_parts[i].type->addr_hi) {
            p = &bn_parts[i];
            break;
        }
    if (!p) {
        st = BN_NACK_ADDR;
    } else if (p->type->module) {
        st = bn_module_xfer ? bn_module_xfer(p, w, wn, r, rn) : BN_NACK_ADDR;
        n = st == BN_ACK ? wn : 0;
    } else {
        if (wn > 0) {
            n = p->type->i2c_write ? p->type->i2c_write(p, w, wn) : 0;
            if (n < wn) st = BN_NACK_DATA;
        }
        if (st == BN_ACK && rn > 0) {
            if (p->type->i2c_read) p->type->i2c_read(p, r, rn);
            else memset(r, 0xFF, (size_t)rn);   /* nothing drives: all ones */
        }
        if (p->type->i2c_stop) p->type->i2c_stop(p);
    }
    if (written) *written = n;
    log_xfer(b, addr, w, wn, r, rn, st);
    return st;
}
