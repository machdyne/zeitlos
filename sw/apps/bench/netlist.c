/*
 * bench -- netlists (/bench/NAME.net, docs/bench.md, "Netlists"). One
 * statement per line, # to the end of a line is a comment:
 *
 *   TYPE NAME ["label"] [PART.PIN ...] [key=value ...] [word ...]
 *                                  a part; its pins joined, in order, to
 *                                  the pins listed. pullup / pulldown as
 *                                  words put a resistor on its pins' nets
 *   net NAME PART.PIN ...          those pins on one net, named
 *   pullup PART.PIN                a resistor on that pin's net
 *   pulldown PART.PIN
 *   bus NAME PART ...              an I2C bus and the parts on it
 *   place NAME COL ROW             where a part's card goes
 *
 * A part must be declared before another statement names it. Errors say
 * the line: "line 4: x2: no such part".
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "core.h"
#include "netlist.h"

#define NTOK 32

struct param {
    int n;
    char key[NTOK][BN_NAME];
    char val[NTOK][BN_LABEL];
    bool used[NTOK];
};

static char *err;
static int errlen, line_no;

static int fail(const char *a, const char *b) {
    snprintf(err, (size_t)errlen, "line %d: %s%s", line_no, a, b ? b : "");
    return -1;
}

/* ---- parameters (core.h) ---- */

const char *bn_param(const param_t *pp, const char *key) {
    param_t *p = (param_t *)pp;
    for (int i = 0; i < p->n; i++)
        if (p->val[i][0] != '\1' && !strcmp(p->key[i], key)) {
            p->used[i] = true;
            return p->val[i];
        }
    return 0;
}

bool bn_flag(const param_t *pp, const char *word) {
    param_t *p = (param_t *)pp;
    for (int i = 0; i < p->n; i++)
        if (p->val[i][0] == '\1' && !strcmp(p->key[i], word)) {
            p->used[i] = true;
            return true;
        }
    return false;
}

long bn_param_num(const param_t *pp, const char *key, long dflt) {
    const char *s = bn_param(pp, key);
    long v = 0;
    int base = 10;
    if (!s) return dflt;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s) return -1;
    for (; *s; s++) {
        int d = isdigit((unsigned char)*s) ? *s - '0' :
            base == 16 && isxdigit((unsigned char)*s) ? (tolower((unsigned char)*s) - 'a' + 10) : -1;
        if (d < 0 || v > 0xFFFFFF) return -1;
        v = v * base + d;
    }
    return v;
}

/* ---- tokens ---- */

static int split(char *s, char **tok) {
    int n = 0;
    while (*s && n < NTOK) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (*s == '"') {
            tok[n++] = s;               /* kept with its quote: a label */
            s++;
            while (*s && *s != '"') s++;
            if (*s) *s++ = 0;
            continue;
        }
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '#') s++;
        if (*s == '#') {
            *s = 0;
            break;
        }
        if (*s) *s++ = 0;
    }
    return n;
}

static bool good_name(const char *s) {
    if (!isalpha((unsigned char)*s) || strlen(s) >= BN_NAME) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return false;
    return true;
}

/* PART.PIN: the part and its pin's number; -1 and an error */
static int pin_ref(const char *ref, part_t **pp) {
    char name[BN_NAME * 2];
    const char *dot = strchr(ref, '.');
    if (!dot || dot == ref || (size_t)(dot - ref) >= BN_NAME)
        return fail(ref, ": not a pin (PART.PIN)");
    memcpy(name, ref, (size_t)(dot - ref));
    name[dot - ref] = 0;
    part_t *p = bn_part_find(name);
    if (!p) return fail(name, ": no such part (declared later?)");
    for (int i = 0; i < p->type->npins; i++)
        if (!strcasecmp(p->type->pins[i], dot + 1)) {
            *pp = p;
            return i;
        }
    snprintf(name, sizeof(name), "%s: %s has no pin ", ref, p->type->name);
    return fail(name, dot + 1);
}

/* the net of a pin, made if it has none */
static int net_of(const char *ref) {
    part_t *p;
    int pin = pin_ref(ref, &p);
    if (pin < 0) return -1;
    if (p->net[pin] < 0) {
        int n = bn_net_new(ref);
        if (n < 0) return fail("too many nets", 0);
        p->net[pin] = (int16_t)n;
    }
    return p->net[pin];
}

/* ---- statements ---- */

static int st_part(const part_type_t *t, char **tok, int n) {
    static param_t pp;
    part_t *p;
    int pin = 0;
    if (n < 2) return fail(t->name, ": a name is needed");
    if (!good_name(tok[1])) return fail(tok[1], ": not a name (a letter, then letters, digits, _)");
    if (bn_part_find(tok[1])) return fail(tok[1], ": declared twice");
    if (bn_nparts >= BN_PARTS) return fail("too many parts", 0);
    p = &bn_parts[bn_nparts];
    memset(p, 0, sizeof(*p));
    p->type = t;
    strcpy(p->name, tok[1]);
    p->bus = p->col = p->row = -1;
    for (int i = 0; i < BN_PINS; i++) p->net[i] = -1;
    bn_nparts++;                        /* now it can be named */

    memset(&pp, 0, sizeof(pp));
    for (int i = 2; i < n; i++) {
        char *eq = strchr(tok[i], '=');
        if (tok[i][0] == '"') {
            strncpy(p->label, tok[i] + 1, BN_LABEL - 1);
        } else if (eq) {
            *eq = 0;
            if (pp.n < NTOK) {
                strncpy(pp.key[pp.n], tok[i], BN_NAME - 1);
                strncpy(pp.val[pp.n], eq + 1, BN_LABEL - 1);
                pp.n++;
            }
        } else if (strchr(tok[i], '.')) {
            if (pin >= t->npins) return fail(tok[i], ": more pins than the part has");
            int other = net_of(tok[i]);
            if (other < 0) return -1;
            p->net[pin++] = (int16_t)other;
        } else if (pp.n < NTOK) {
            strncpy(pp.key[pp.n], tok[i], BN_NAME - 1);
            pp.val[pp.n][0] = '\1';     /* a word, not key=value */
            pp.n++;
        }
    }
    int pull = bn_flag(&pp, "pullup") ? BN_PULLUP : bn_flag(&pp, "pulldown") ? BN_PULLDOWN : 0;
    if (pull)
        for (int i = 0; i < t->npins; i++) {
            if (p->net[i] < 0) {
                char nm[BN_NAME * 2];
                snprintf(nm, sizeof(nm), "%s.%s", p->name, t->pins[i]);
                int nn = bn_net_new(nm);
                if (nn < 0) return fail("too many nets", 0);
                p->net[i] = (int16_t)nn;
            }
            bn_nets[p->net[i]].pull = (uint8_t)pull;
        }
    if (t->addr_hi) {
        long a = bn_param_num(&pp, "addr", -2);
        if (a == -2 && t->addr_dflt) a = t->addr_dflt;
        if (a == -2) return fail(p->name, ": addr= is needed");
        if (a < t->addr_lo || a > t->addr_hi) {
            char m[64];
            snprintf(m, sizeof(m), ": addr= is 0x%02x-0x%02x for a %s",
                     t->addr_lo, t->addr_hi, t->name);
            return fail(p->name, m);
        }
        p->addr = (uint8_t)a;
    }
    const char *e = t->init ? t->init(p, &pp) : 0;
    if (e) return fail(p->name, e);
    for (int i = 0; i < pp.n; i++)
        if (!pp.used[i]) {
            char m[64];
            snprintf(m, sizeof(m), ": a %s has no %s", t->name, pp.key[i]);
            return fail(p->name, m);
        }
    return 0;
}

static int st_net(char **tok, int n) {
    if (n < 3) return fail("net NAME PART.PIN ...", 0);
    if (!good_name(tok[1])) return fail(tok[1], ": not a name");
    int net = bn_net_find(tok[1]);
    if (net < 0 && (net = bn_net_new(tok[1])) < 0) return fail("too many nets", 0);
    for (int i = 2; i < n; i++) {
        part_t *p;
        int pin = pin_ref(tok[i], &p);
        if (pin < 0) return -1;
        if (p->net[pin] < 0) p->net[pin] = (int16_t)net;
        else bn_net_merge(net, p->net[pin]);
    }
    return 0;
}

static int st_pull(char **tok, int n, int pull) {
    if (n != 2) return fail(tok[0], " PART.PIN");
    int net = net_of(tok[1]);
    if (net < 0) return -1;
    bn_nets[net].pull = (uint8_t)pull;
    return 0;
}

static int st_bus(char **tok, int n) {
    if (n < 2) return fail("bus NAME PART ...", 0);
    if (!good_name(tok[1])) return fail(tok[1], ": not a name");
    if (bn_bus_find(tok[1]) >= 0) return fail(tok[1], ": declared twice");
    if (bn_nbuses >= BN_BUSES) return fail("too many buses", 0);
    int b = bn_nbuses++;
    strcpy(bn_buses[b].name, tok[1]);
    bn_buses[b].master = -1;
    for (int i = 2; i < n; i++) {
        if (!strncmp(tok[i], "master=", 7)) {
            part_t *m = bn_part_find(tok[i] + 7);
            if (!m) return fail(tok[i] + 7, ": no such part (declared later?)");
            if (!m->type->module) return fail(tok[i] + 7, ": not a module (a master is a module's C/D)");
            for (int j = 0; j < bn_nbuses; j++)
                if (bn_buses[j].master == m - bn_parts)
                    return fail(tok[i] + 7, ": already the master of a bus");
            bn_buses[b].master = (int8_t)(m - bn_parts);
            continue;
        }
        part_t *p = bn_part_find(tok[i]);
        if (!p) return fail(tok[i], ": no such part (declared later?)");
        if (!p->type->addr_hi) return fail(tok[i], ": not an I2C part");
        if (p->bus >= 0) return fail(tok[i], ": already on a bus");
        for (int j = 0; j < bn_nparts; j++)
            if (bn_parts[j].bus == b && bn_parts[j].addr == p->addr) {
                char m[64];
                snprintf(m, sizeof(m), ": 0x%02x is %s's address on %s",
                         p->addr, bn_parts[j].name, tok[1]);
                return fail(tok[i], m);
            }
        p->bus = (int8_t)b;
    }
    return 0;
}

static int st_place(char **tok, int n) {
    if (n != 4) return fail("place NAME COL ROW", 0);
    part_t *p = bn_part_find(tok[1]);
    if (!p) return fail(tok[1], ": no such part");
    p->col = (int8_t)atoi(tok[2]);
    p->row = (int8_t)atoi(tok[3]);
    return 0;
}

int bn_load(const char *text, char *e, int elen) {
    char buf[256], *tok[NTOK];
    err = e;
    errlen = elen;
    e[0] = 0;
    line_no = 0;
    bn_clear();
    while (*text) {
        int len = (int)strcspn(text, "\r\n");
        line_no++;
        if (len >= (int)sizeof(buf)) return fail("line too long", 0);
        memcpy(buf, text, (size_t)len);
        buf[len] = 0;
        text += len;
        if (*text == '\r') text++;
        if (*text == '\n') text++;
        int n = split(buf, tok), r;
        if (!n) continue;
        const part_type_t *t;
        if (!strcmp(tok[0], "net")) r = st_net(tok, n);
        else if (!strcmp(tok[0], "pullup")) r = st_pull(tok, n, BN_PULLUP);
        else if (!strcmp(tok[0], "pulldown")) r = st_pull(tok, n, BN_PULLDOWN);
        else if (!strcmp(tok[0], "bus")) r = st_bus(tok, n);
        else if (!strcmp(tok[0], "place")) r = st_place(tok, n);
        else if (!strcmp(tok[0], "module")) {
            /* module NAME LS10 ...: the part type is the module's model */
            char model[BN_NAME];
            if (n < 3) r = fail("module NAME LS10|LS11|LS99 ...", 0);
            else {
                int k = 0;
                for (; tok[2][k] && k < BN_NAME - 1; k++) model[k] = (char)tolower((unsigned char)tok[2][k]);
                model[k] = 0;
                t = bn_type_find(model);
                if (!t || !t->module) r = fail(tok[2], ": not a module (LS10, LS11, LS99)");
                else {
                    char *nt[NTOK];
                    nt[0] = model;
                    nt[1] = tok[1];
                    for (int i = 3; i < n; i++) nt[i - 1] = tok[i];
                    r = st_part(t, nt, n - 1);
                }
            }
        }
        else if (!strcmp(tok[0], "gpio")) r = fail("gpio: real ports arrive in phase 4", 0);
        else if ((t = bn_type_find(tok[0]))) r = st_part(t, tok, n);
        else r = fail(tok[0], ": no such part type or statement");
        if (r) return -1;
    }
    bn_resolve_all();
    return 0;
}
