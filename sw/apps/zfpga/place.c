/*
 * zfpga -- place: a logical netlist (.zl) to a placed, physical one (.zn).
 *
 * Phase 5 of docs/zfpga.md, v1. Input is what synthesis produces (today
 * tools/ys2zl.py from yosys; later zfpga synth): LUT4s and flip-flops on
 * named nets, and IO with pins. Output is the .zn `zfpga pnr` routes:
 * cells at sites, nets by their pin wires.
 *
 *   zfpga place d.zl -o d.zn && zfpga pnr d.zn && zfpga pack d.config
 *
 * PACKING. A flip-flop whose data comes from a LUT shares that LUT's
 * logic cell: DI<k> is hard-wired from F<k> (PLC2's fixed connections),
 * so the pair needs no routing and uses SD=1. Any other flip-flop takes
 * its data through M<k> with SD=0. Two logic cells make a slice, and the
 * two flip-flops of a slice share its clock, clock enable and reset and
 * their mux settings, so cells are paired only when those match.
 *
 * CONSTANT LUT INPUTS are folded into the INIT. An input nothing drives
 * is tied to 1 by zfpga pnr (SLICEx.<pin>MUX 1), so the INIT is rewritten
 * to give the right function with that input at 1.
 *
 * PLACEMENT. IO is where the netlist says. Slices get an initial place
 * near what they connect to, then simulated annealing on half-perimeter
 * wirelength, with the tile rule that PLC2 has two clock wires and two
 * reset wires: at most two distinct clocks, and two distinct resets, per
 * tile. All integer, from a seeded generator, so the device places
 * exactly as the host does. Acceptance is exp(-delta/T) from a table,
 * because the CPU has no FPU.
 *
 * CARRY CHAINS (v2, docs/zfpga.md sec. 16). yosys's CCU2C cells are
 * linked carry-out to carry-in into chains, which run east through the
 * fixed FCOA=>FCIB=>...=>FCO=>HFIE=>FCI wiring. Every chain starts with
 * nextpnr's feed-in slice (ecp5/pack.cc, make_carry_feed_in): its first
 * half's LUT4 is 0 with the unused inputs tied to 1, which gates the
 * incoming hardware carry off, and its LUT2 makes A0 -- or, for a
 * constant carry in, the tied pins themselves -- the carry. So a chain
 * starts clean whatever sits to its west. A chain whose last carry out
 * feeds logic ends with nextpnr's feed-out slice, whose sum is its carry
 * in. A chain is a rigid block: it starts at slice A, owns whole tiles
 * in consecutive columns of one row, and moves only into free space.
 *
 * Not yet, refused by name: carry used by logic mid-chain, wide-LUT
 * muxes, distributed RAM, block RAM, DSP, tristate IO.
 */

#include "zfpga.h"

/* -- the logical netlist ------------------------------------------------- */

#define C0 (-2)     /* constant 0 */
#define C1 (-3)     /* constant 1 */
#define NONE (-1)

typedef struct { const char *name; int driver, n_sinks; } lnet_t;   /* driver: cell ref */

/* A placement constraint: loc=R<r>C<c>[.<slice A-D>[<half 0-1>]]; -1 for
 * the parts not given, r < 0 for none. */
typedef struct { int r, c, s, h; } loc_t;

typedef struct {
    const char *name;
    int in[4], out, line;
    uint32_t init;
    int lc, raw;
    loc_t at;
} llut_t;

typedef struct {
    const char *name;
    int d, q, clk, ce, lsr, line;
    const char *gsr, *regset, *srmode, *lsrmode, *cemux, *clkmux, *lsrmux;
    int lc, raw;
    loc_t at;
    int ck, lk;     /* clock and reset control classes, for tile_ok() */
} lff_t;

typedef struct {
    const char *name;
    int in[8], cin, cout, s[2], line;
    uint32_t init[2];
    const char *inj[2];
    int lc[2], next, prev, raw;
    loc_t at;
} lccu_t;

typedef struct {
    const char *name, *pin, *opts;
    int output, net, line;
    uint32_t pio;
} lio_t;

static zdb_t *db;
static const char *zl;
static lnet_t *nets; static int n_net, cap_net;
static llut_t *luts; static int n_lut, cap_lut;
static lff_t *ffs;   static int n_ff, cap_ff;
static lio_t *ios;   static int n_io, cap_io;
static lccu_t *ccus; static int n_ccu, cap_ccu;

/* The input, line by line, for `-l`: which lines are cells (1 lut, 2 ff,
 * 3 ccu2), so that their loc= can be rewritten and the rest kept. */
static char **raw; static int n_raw, cap_raw, cap_rawc;
static uint8_t *raw_cell;

#define GROW(arr, n, cap, T) do { if ((n) == (cap)) {                     \
    int nc_ = (cap) ? (cap) * 2 : 64;                                     \
    T *na_ = zf_alloc(sizeof(T) * (size_t)nc_);                           \
    if (n) zf_memcpy(na_, (arr), sizeof(T) * (size_t)(n));                \
    (arr) = na_; (cap) = nc_; } } while (0)

/* net names -> index */
static uint32_t *nh; static uint32_t nh_cap;

static uint32_t hs(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int net_of(const char *s, int line) {
    uint32_t i;
    if (zf_streq(s, "0")) return C0;
    if (zf_streq(s, "1")) return C1;
    if (zf_streq(s, "-")) return NONE;
    for (i = hs(s) & (nh_cap - 1); nh[i]; i = (i + 1) & (nh_cap - 1))
        if (zf_streq(nets[nh[i] - 1].name, s)) return (int)nh[i] - 1;
    if ((uint32_t)(n_net + 1) * 2 > nh_cap) {
        uint32_t cap = nh_cap * 2, k;
        uint32_t *nn = zf_alloc(sizeof(uint32_t) * cap);
        for (k = 0; k < (uint32_t)n_net; k++) {
            uint32_t j;
            for (j = hs(nets[k].name) & (cap - 1); nn[j]; j = (j + 1) & (cap - 1)) ;
            nn[j] = k + 1;
        }
        nh = nn;
        nh_cap = cap;
        return net_of(s, line);
    }
    GROW(nets, n_net, cap_net, lnet_t);
    nets[n_net].name = zf_strdup(s);
    nets[n_net].driver = NONE;
    nets[n_net].n_sinks = 0;
    nh[i] = (uint32_t)++n_net;
    (void)line;
    return n_net - 1;
}

/* A driver reference: 0x10000000 | lut, 0x20000000 | ff, 0x30000000 | io */
#define DRV_LUT 0x10000000
#define DRV_FF  0x20000000
#define DRV_IO  0x30000000
#define DRV_CCS 0x40000000      /* a CCU2 sum: idx << 1 | half */
#define DRV_CCC 0x50000000      /* a CCU2 carry out */
#define DRV_KIND(d) ((d) & 0x70000000)
#define DRV_IDX(d) ((d) & 0x0fffffff)

static void drive(int net, int ref, int line) {
    if (net < 0) return;
    if (nets[net].driver != NONE)
        zf_fatal_at(zl, line, "net %s has two drivers", nets[net].name);
    nets[net].driver = ref;
}

static void use(int net) {
    if (net >= 0) nets[net].n_sinks++;
}

/* key=value options: returns the value for key, or NULL */
static const char *opt(char **tok, int n, const char *key) {
    int i;
    size_t k = zf_strlen(key);
    for (i = 0; i < n; i++) {
        size_t j;
        for (j = 0; j < k && tok[i][j] == key[j]; j++) ;
        if (j == k && tok[i][k] == '=') return tok[i] + k + 1;
    }
    return NULL;
}

static loc_t no_loc(void) {
    loc_t l;
    l.r = l.c = l.s = l.h = -1;
    return l;
}

/* loc=R<r>C<c>[.<A-D>[<0-1>]] */
static loc_t parse_loc(const char *v, int line) {
    loc_t l = no_loc();
    const char *p = v;
    if (!v) return l;
    if (*p++ != 'R' || *p < '0' || *p > '9') goto bad;
    for (l.r = 0; *p >= '0' && *p <= '9'; p++) l.r = l.r * 10 + (*p - '0');
    if (*p++ != 'C' || *p < '0' || *p > '9') goto bad;
    for (l.c = 0; *p >= '0' && *p <= '9'; p++) l.c = l.c * 10 + (*p - '0');
    if (!*p) return l;
    if (*p++ != '.' || *p < 'A' || *p > 'D') goto bad;
    l.s = *p++ - 'A';
    if (!*p) return l;
    if (*p != '0' && *p != '1') goto bad;
    l.h = *p++ - '0';
    if (!*p) return l;
bad:
    zf_fatal_at(zl, line, "loc=%s: want R<row>C<col>, R<row>C<col>.<A-D> or R<row>C<col>.<A-D><0-1>", v);
}

/* func= expressions: a b c d 0 1, ~ or ! for not, & ^ | (that order of
 * binding), parentheses. No spaces -- a .zl line is split on them. */
static const char *fx_p, *fx_start;
static int fx_line, fx_used;

static int fx_or(int v);
static int fx_prim(int v) {
    int r;
    char c = *fx_p;
    if (c == '~' || c == '!') { fx_p++; return !fx_prim(v); }
    if (c == '(') {
        fx_p++;
        r = fx_or(v);
        if (*fx_p++ != ')') zf_fatal_at(zl, fx_line, "func=%s: missing )", fx_start);
        return r;
    }
    if (c >= 'a' && c <= 'd') { fx_p++; fx_used |= 1 << (c - 'a'); return (v >> (c - 'a')) & 1; }
    if (c == '0' || c == '1') { fx_p++; return c - '0'; }
    zf_fatal_at(zl, fx_line, "func=%s: unexpected '%c' (a b c d 0 1 ~ & ^ | ( ))", fx_start, c ? c : ' ');
}
static int fx_and(int v) { int r = fx_prim(v); while (*fx_p == '&') { fx_p++; r &= fx_prim(v); } return r; }
static int fx_xor(int v) { int r = fx_and(v); while (*fx_p == '^') { fx_p++; r ^= fx_and(v); } return r; }
static int fx_or(int v) { int r = fx_xor(v); while (*fx_p == '|') { fx_p++; r |= fx_xor(v); } return r; }

/* The INIT for an expression: bit i is the function at a = i&1, b =
 * i>>1&1, c, d -- the LUT4 order. *used gets the inputs it mentions. */
static uint32_t func_init(const char *f, int line, int *used) {
    uint32_t init = 0;
    int v;
    fx_start = f;
    fx_line = line;
    fx_used = 0;
    for (v = 0; v < 16; v++) {
        fx_p = f;
        if (fx_or(v)) init |= 1u << v;
        if (*fx_p) zf_fatal_at(zl, line, "func=%s: unexpected '%c'", f, *fx_p);
    }
    *used = fx_used;
    return init;
}

static void check_opts(char **tok, int n, const char *const *allowed, int line) {
    int i, k;
    for (i = 0; i < n; i++) {
        for (k = 0; allowed[k]; k++) {
            size_t l = zf_strlen(allowed[k]);
            size_t j;
            for (j = 0; j < l && tok[i][j] == allowed[k][j]; j++) ;
            if (j == l && tok[i][l] == '=') break;
        }
        if (!allowed[k]) zf_fatal_at(zl, line, "unknown option '%s'", tok[i]);
    }
}

/* -- packing ---------------------------------------------------------------- */

/* ccu: the CCU2 half this LC is, or NONE; FEED_* for a chain's feed-in
 * or feed-out slice */
#define FEED_IN   (-10)
#define FEED_OUT  (-11)
typedef struct { int lut, ff, sd, ccu, half; } lc_t;
typedef struct {
    int lc[2];
    int ctl;            /* an FF sharing the slice's controls, or NONE */
    int chain, pos;     /* its chain and place in it, or NONE */
    loc_t at;           /* locked by loc=, or at.r < 0 */
} pslice_t;

typedef struct {
    int first, n;       /* slices sl[first .. first+n-1], head first */
    int feed_net;       /* fabric carry in, routed to the feed-in's A0; NONE */
    int feed_const;     /* constant carry in: 0 or 1; NONE if fabric */
    int out_net;        /* the last carry out, to logic through a feed-out; NONE */
    loc_t at;           /* the feed-in's tile, from loc= on the first cell; or none */
} chain_t;

static chain_t *chains; static int n_chain, cap_chain;
#define RESERVED (-7)   /* a chain-owned site no slice of the chain fills */

static lc_t *lcs; static int n_lc, cap_lc;
static pslice_t *sl; static int n_sl, cap_sl;

static int same_ctl(const lff_t *a, const lff_t *b) {
    return a->clk == b->clk && a->ce == b->ce && a->lsr == b->lsr &&
        zf_streq(a->gsr, b->gsr) && zf_streq(a->clkmux, b->clkmux) &&
        zf_streq(a->cemux, b->cemux) && zf_streq(a->lsrmux, b->lsrmux) &&
        zf_streq(a->srmode, b->srmode);
}

static lc_t *new_lc(int lut, int ff, int sd, int ccu, int half) {
    lc_t *c;
    GROW(lcs, n_lc, cap_lc, lc_t);
    c = &lcs[n_lc++];
    c->lut = lut; c->ff = ff; c->sd = sd; c->ccu = ccu; c->half = half;
    return c;
}

static pslice_t *new_slice(int l0, int l1, int chain, int pos) {
    pslice_t *p;
    GROW(sl, n_sl, cap_sl, pslice_t);
    p = &sl[n_sl++];
    p->lc[0] = l0; p->lc[1] = l1;
    p->ctl = NONE;
    if (l0 != NONE && lcs[l0].ff != NONE) p->ctl = lcs[l0].ff;
    else if (l1 != NONE && lcs[l1].ff != NONE) p->ctl = lcs[l1].ff;
    p->chain = chain; p->pos = pos;
    p->at = no_loc();
    return p;
}

/* An LC's loc=: its LUT's or its flip-flop's, which must agree. */
static loc_t lc_at(int i) {
    loc_t a = no_loc(), b = no_loc();
    if (i == NONE) return a;
    if (lcs[i].lut != NONE) a = luts[lcs[i].lut].at;
    if (lcs[i].ff != NONE) b = ffs[lcs[i].ff].at;
    if (a.r >= 0 && b.r >= 0 && (a.r != b.r || a.c != b.c || a.s != b.s || a.h != b.h))
        zf_fatal_at(zl, ffs[lcs[i].ff].line, "flip-flop %s shares a cell with LUT %s, "
            "but their loc= differ", ffs[lcs[i].ff].name, luts[lcs[i].lut].name);
    return a.r >= 0 ? a : b;
}

static void pack(void) {
    int i, j;
    uint8_t *paired;
    int max_chain = 4 * 69 - 2;         /* nextpnr: (width - 4) * 4 - 2 */

    for (i = 0; i < n_lut; i++) luts[i].lc = NONE;

    /* -- chains: link carry out to carry in */
    for (i = 0; i < n_ccu; i++) { ccus[i].next = NONE; ccus[i].prev = NONE; }
    for (i = 0; i < n_ccu; i++) {
        int cn = ccus[i].cin;
        if (cn >= 0 && DRV_KIND(nets[cn].driver) == DRV_CCC) {
            int p = DRV_IDX(nets[cn].driver);
            if (nets[cn].n_sinks != 1)
                zf_fatal_at(zl, ccus[p].line, "carry out of %s also feeds logic mid-chain; not yet supported",
                    ccus[p].name);
            ccus[p].next = i;
            ccus[i].prev = p;
        }
    }
    for (i = 0; i < n_ccu; i++) {
        int k, len = 0, c;
        chain_t *ch;
        if (ccus[i].prev != NONE) continue;
        GROW(chains, n_chain, cap_chain, chain_t);
        ch = &chains[n_chain];
        ch->first = n_sl;
        ch->feed_net = ccus[i].cin >= 0 ? ccus[i].cin : NONE;
        ch->feed_const = ccus[i].cin == C1 ? 1 : ccus[i].cin >= 0 ? NONE : 0;
        ch->at = ccus[i].at;
        for (c = ccus[i].next; c != NONE; c = ccus[c].next)
            if (ccus[c].at.r >= 0)
                zf_fatal_at(zl, ccus[c].line, "ccu2 %s: loc= on a chain goes on its first cell, %s",
                    ccus[c].name, ccus[i].name);
        /* feed-in */
        {
            int l0 = n_lc, l1;
            new_lc(NONE, NONE, 0, FEED_IN, 0);
            l1 = n_lc;
            new_lc(NONE, NONE, 0, FEED_IN, 1);
            new_slice(l0, l1, n_chain, len++);
        }
        for (c = i; c != NONE; c = ccus[c].next) {
            for (k = 0; k < 2; k++) {
                ccus[c].lc[k] = n_lc;
                new_lc(NONE, NONE, 0, c, k);
            }
            new_slice(ccus[c].lc[0], ccus[c].lc[1], n_chain, len++);
            if (ccus[c].next == NONE) {
                int co = ccus[c].cout;
                ch->out_net = (co >= 0 && nets[co].n_sinks) ? co : NONE;
            }
        }
        if (ch->out_net != NONE) {
            int l0 = n_lc, l1;
            new_lc(NONE, NONE, 0, FEED_OUT, 0);
            l1 = n_lc;
            new_lc(NONE, NONE, 0, FEED_OUT, 1);
            new_slice(l0, l1, n_chain, len++);
        }
        ch->n = len;
        if (len > max_chain) zf_fatal_at(zl, ccus[i].line, "carry chain of %d slices; the longest is %d",
            len, max_chain);
        n_chain++;
    }

    /* -- flip-flops: with the LUT or carry half that drives them, if they can */
    for (i = 0; i < n_ff; i++) {
        lff_t *f = &ffs[i];
        int drv = f->d >= 0 ? nets[f->d].driver : NONE;
        if (f->d < 0) zf_fatal_at(zl, f->line, "flip-flop %s has a constant data input", f->name);
        f->lc = NONE;
        if (DRV_KIND(drv) == DRV_LUT && luts[DRV_IDX(drv)].lc == NONE) {
            int l = DRV_IDX(drv);
            luts[l].lc = n_lc;
            f->lc = n_lc;
            new_lc(l, i, 1, NONE, 0);
        } else if (DRV_KIND(drv) == DRV_CCS) {
            int c = DRV_IDX(drv) >> 1, h = DRV_IDX(drv) & 1;
            int lc = ccus[c].lc[h], other = ccus[c].lc[h ^ 1];
            if (lcs[lc].ff == NONE && (lcs[other].ff == NONE ||
                    same_ctl(&ffs[lcs[other].ff], f))) {
                lcs[lc].ff = i;
                lcs[lc].sd = 1;
                f->lc = lc;
                for (j = 0; j < n_sl; j++)
                    if (sl[j].lc[0] == lc || sl[j].lc[1] == lc) { if (sl[j].ctl == NONE) sl[j].ctl = i; break; }
            }
        }
        if (f->lc == NONE) {
            f->lc = n_lc;
            new_lc(NONE, i, 0, NONE, 0);
        }
    }
    for (i = 0; i < n_lut; i++) {
        if (luts[i].lc != NONE) continue;
        luts[i].lc = n_lc;
        new_lc(i, NONE, 0, NONE, 0);
    }

    paired = zf_alloc((size_t)n_lc + 1);
    for (i = 0; i < n_sl; i++)
        for (j = 0; j < 2; j++) if (sl[i].lc[j] != NONE) paired[sl[i].lc[j]] = 1;

    /* -- cells with loc=: those naming the same slice share it, in the
     * half they name if they name one */
    for (i = 0; i < n_lc; i++) {
        loc_t a = lc_at(i), b;
        pslice_t *ps;
        if (paired[i] || a.r < 0) continue;
        paired[i] = 1;
        ps = new_slice(i, NONE, NONE, 0);
        ps->at = a;
        if (a.s >= 0) {
            for (j = i + 1; j < n_lc; j++) {
                if (paired[j]) continue;
                b = lc_at(j);
                if (b.r != a.r || b.c != a.c || b.s != a.s) continue;
                if (ps->lc[1] != NONE) {
                    int ln = lcs[j].lut != NONE ? luts[lcs[j].lut].line : ffs[lcs[j].ff].line;
                    zf_fatal_at(zl, ln, "more than two cells have loc=R%dC%d.%c", a.r, a.c, 'A' + a.s);
                }
                if (lcs[i].ff != NONE && lcs[j].ff != NONE && !same_ctl(&ffs[lcs[i].ff], &ffs[lcs[j].ff]))
                    zf_fatal_at(zl, ffs[lcs[j].ff].line, "flip-flops %s and %s share slice R%dC%d.%c "
                        "but not clock, enable and reset", ffs[lcs[i].ff].name, ffs[lcs[j].ff].name,
                        a.r, a.c, 'A' + a.s);
                ps->lc[1] = j;
                if (ps->ctl == NONE) ps->ctl = lcs[j].ff;
                paired[j] = 1;
            }
        }
        /* halves as named */
        {
            int h0 = lc_at(ps->lc[0]).h, h1 = ps->lc[1] != NONE ? lc_at(ps->lc[1]).h : -1;
            if (h0 >= 0 && h0 == h1)
                zf_fatal("two cells have loc=R%dC%d.%c%d", a.r, a.c, 'A' + a.s, h0);
            if (h0 == 1 || h1 == 0) { int t = ps->lc[0]; ps->lc[0] = ps->lc[1]; ps->lc[1] = t; }
        }
        ps->at.h = -1;
    }

    /* -- the rest into slices: FF cells pair only with matching control */
    for (i = 0; i < n_lc; i++) {
        pslice_t *p;
        if (paired[i]) continue;
        paired[i] = 1;
        p = new_slice(i, NONE, NONE, 0);
        for (j = i + 1; j < n_lc; j++) {
            if (paired[j]) continue;
            if (lcs[i].ff != NONE && lcs[j].ff != NONE &&
                    !same_ctl(&ffs[lcs[i].ff], &ffs[lcs[j].ff]))
                continue;
            p->lc[1] = j;
            if (p->ctl == NONE) p->ctl = lcs[j].ff;
            paired[j] = 1;
            break;
        }
    }
}

/* -- placement --------------------------------------------------------------- */

static uint32_t *site_tile;     /* site -> tile index; 4 sites per PLC2 */
static int16_t *site_row, *site_col;
static int n_site;
static int *site_of;            /* slice -> site */
static int *loc_site, loc_rows, loc_cols;   /* (row, col) -> first site there, or -1 */
static int *occ;                /* site -> slice or NONE */

static uint32_t rng = 0x5eed1234u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* nets -> their movable slices, for incremental cost */
static int *net_sl0, *net_sl, *sl_net0, *sl_net;
static int *net_cost;
/* Each net's IO never moves, so the box around its IO is computed once
 * (r0, r1, c0, c1; r1 < 0 when the net has none). Rescanning every IO on
 * every evaluation was two thirds of placement time (sec. 15.3). */
static int16_t (*net_iobox)[4];

static int iabs(int x) { return x < 0 ? -x : x; }

static void io_loc(const lio_t *o, int *r, int *c) {
    *r = db->pio[o->pio].row;
    *c = db->pio[o->pio].col;
}

/* Half-perimeter of a net's bounding box: its driver's and sinks'
 * locations, slices where they are placed now, IO where it is. */
static int hpwl(int ni) {
    int r0 = 1 << 30, r1 = -1, c0 = 1 << 30, c1 = -1, k, i;
#define ADD(r, c) do {                       \
        if ((r) < r0) r0 = (r);                  \
        if ((r) > r1) r1 = (r);                  \
        if ((c) < c0) c0 = (c);                  \
        if ((c) > c1) c1 = (c);                  \
    } while (0)
    (void)i;
    if (net_iobox[ni][1] >= 0) {
        ADD(net_iobox[ni][0], net_iobox[ni][2]);
        ADD(net_iobox[ni][1], net_iobox[ni][3]);
    }
    for (k = net_sl0[ni]; k < net_sl0[ni + 1]; k++) {
        int s = site_of[net_sl[k]];
        ADD(site_row[s], site_col[s]);
    }
#undef ADD
    if (r1 < 0) return 0;
    return (r1 - r0) + (c1 - c0);
}

/* The PLC2 rule: at most two distinct clock controls and two distinct
 * reset controls per tile, since each tile has CLK0/CLK1 and LSR0/LSR1. */
/* Each flip-flop's clock control (net, CLKMUX) and reset control (net,
 * SRMODE, LSRMUX) as a small integer, worked out once: tile_ok() runs on
 * every move, and comparing the settings as strings there was 37M
 * instructions of a 214M build. */
static void control_classes(void) {
    int i, j;
    for (i = 0; i < n_ff; i++) {
        lff_t *f = &ffs[i];
        f->ck = i;
        f->lk = i;
        for (j = 0; j < i; j++)
            if (ffs[j].clk == f->clk && zf_streq(ffs[j].clkmux, f->clkmux)) { f->ck = ffs[j].ck; break; }
        for (j = 0; j < i; j++)
            if (ffs[j].lsr == f->lsr && zf_streq(ffs[j].srmode, f->srmode) &&
                    zf_streq(ffs[j].lsrmux, f->lsrmux)) { f->lk = ffs[j].lk; break; }
    }
}

/* The PLC2 rule: at most two distinct clock controls and two distinct
 * reset controls per tile, since each tile has CLK0/CLK1 and LSR0/LSR1. */
static int tile_ok(uint32_t tile_first_site) {
    int clk[4], lsr[4], nc = 0, nl = 0, s, k;
    for (s = 0; s < 4; s++) {
        int slc = occ[tile_first_site + (uint32_t)s];
        const lff_t *f;
        if (slc < 0 || sl[slc].ctl == NONE) continue;      /* empty, or reserved by a chain */
        f = &ffs[sl[slc].ctl];
        for (k = 0; k < nc; k++) if (clk[k] == f->ck) break;
        if (k == nc) clk[nc++] = f->ck;
        if (f->lsr >= 0) {
            for (k = 0; k < nl; k++) if (lsr[k] == f->lk) break;
            if (k == nl) lsr[nl++] = f->lk;
        }
    }
    return nc <= 2 && nl <= 2;
}

/* exp(-x/256) in 1/65536, x in [0, 4096) */
static uint16_t exptab[4096];

static void exptab_init(void) {
    uint32_t v = 65535;
    int i;
    for (i = 0; i < 4096; i++) {
        exptab[i] = (uint16_t)v;
        v = (v * 65281u) >> 16;         /* x exp(-1/256) */
    }
}

/* The nets a move touches, each once. A per-net stamp rather than a
 * search of the list so far: that search was 85M of a 214M-instruction
 * build, since every move of a carry chain touches dozens of nets. */
static uint32_t *net_seen, move_stamp;

static int sl_cost(int s, int *touched, int *nt) {
    int k, c = 0;
    for (k = sl_net0[s]; k < sl_net0[s + 1]; k++) {
        int ni = sl_net[k];
        if (net_seen[ni] == move_stamp) continue;
        net_seen[ni] = move_stamp;
        if (*nt == 4096) zf_fatal("placer: a move touches more than 4096 nets");
        touched[(*nt)++] = ni;
        c += net_cost[ni];
    }
    return c;
}

static void build_adjacency(void) {
    int i, k, n;
    int *cnt = zf_alloc(sizeof(int) * (size_t)(n_net + 1));
    int *scnt = zf_alloc(sizeof(int) * (size_t)(n_sl + 1));
    int *lc_sl = zf_alloc(sizeof(int) * (size_t)n_lc);
    for (i = 0; i < n_sl; i++)
        for (k = 0; k < 2; k++) if (sl[i].lc[k] != NONE) lc_sl[sl[i].lc[k]] = i;
    /* pass 1 counts, pass 2 fills: (net, slice) pairs, deduplicated per slice */
    for (n = 0; n < 2; n++) {
        for (i = 0; i < n_sl; i++) {
            int seen[16], ns = 0, j;
            for (k = 0; k < 2; k++) {
                int lc = sl[i].lc[k], p, list[8], nl = 0;
                if (lc == NONE) continue;
                if (lcs[lc].lut != NONE) {
                    const llut_t *l = &luts[lcs[lc].lut];
                    for (p = 0; p < 4; p++) if (l->in[p] >= 0) list[nl++] = l->in[p];
                    if (l->out >= 0) list[nl++] = l->out;
                }
                if (lcs[lc].ccu >= 0) {
                    const lccu_t *cc = &ccus[lcs[lc].ccu];
                    int h = lcs[lc].half;
                    for (p = 0; p < 4; p++) if (cc->in[4 * h + p] >= 0) list[nl++] = cc->in[4 * h + p];
                    if (cc->s[h] >= 0) list[nl++] = cc->s[h];
                }
                if (lcs[lc].ccu == FEED_IN && lcs[lc].half == 0 && chains[sl[i].chain].feed_net >= 0)
                    list[nl++] = chains[sl[i].chain].feed_net;
                if (lcs[lc].ccu == FEED_OUT && lcs[lc].half == 0 && chains[sl[i].chain].out_net >= 0)
                    list[nl++] = chains[sl[i].chain].out_net;
                if (lcs[lc].ff != NONE) {
                    const lff_t *f = &ffs[lcs[lc].ff];
                    list[nl++] = f->d; if (f->q >= 0) list[nl++] = f->q;
                    if (f->clk >= 0) list[nl++] = f->clk;
                    if (f->ce >= 0) list[nl++] = f->ce;
                    if (f->lsr >= 0) list[nl++] = f->lsr;
                }
                for (p = 0; p < nl; p++) {
                    for (j = 0; j < ns; j++) if (seen[j] == list[p]) break;
                    if (j < ns) continue;
                    seen[ns++] = list[p];
                    if (!n) { cnt[list[p]]++; scnt[i]++; }
                    else {
                        net_sl[net_sl0[list[p]] + cnt[list[p]]++] = i;
                        sl_net[sl_net0[i] + scnt[i]++] = list[p];
                    }
                }
            }
        }
        if (!n) {
            net_sl0 = zf_alloc(sizeof(int) * (size_t)(n_net + 1));
            sl_net0 = zf_alloc(sizeof(int) * (size_t)(n_sl + 1));
            for (i = 0; i < n_net; i++) net_sl0[i + 1] = net_sl0[i] + cnt[i];
            for (i = 0; i < n_sl; i++) sl_net0[i + 1] = sl_net0[i] + scnt[i];
            net_sl = zf_alloc(sizeof(int) * (size_t)(net_sl0[n_net] + 1));
            sl_net = zf_alloc(sizeof(int) * (size_t)(sl_net0[n_sl] + 1));
            zf_memset(cnt, 0, sizeof(int) * (size_t)(n_net + 1));
            zf_memset(scnt, 0, sizeof(int) * (size_t)(n_sl + 1));
        }
    }
}

#define RES(ch) (-100 - (ch))

/* The .zl line of a slice's first cell, for messages about loc=. */
static int sl_line(int i) {
    int k;
    for (k = 0; k < 2; k++) {
        int lc = sl[i].lc[k];
        if (lc == NONE) continue;
        if (lcs[lc].lut != NONE) return luts[lcs[lc].lut].line;
        if (lcs[lc].ff != NONE) return ffs[lcs[lc].ff].line;
    }
    return 0;
}

static int owned(int site, int ch) {
    int o = occ[site];
    return o == NONE || o == RES(ch) || (o >= 0 && sl[o].chain == ch);
}

static void chain_lift(int ch) {
    int k, t = (chains[ch].n + 3) / 4, base = site_of[chains[ch].first] & ~3;
    for (k = 0; k < 4 * t; k++) {
        int st = site_row[base] * loc_cols + site_col[base] + k / 4;
        occ[loc_site[st] + (k & 3)] = NONE;
    }
}

/* Puts chain ch with its head at slice A of the tile at (r, c). Returns
 * 0, changing nothing, if it does not fit there. */
static int chain_put(int ch, int r, int c) {
    const chain_t *h = &chains[ch];
    int t = (h->n + 3) / 4, j, k;
    if (r < 0 || r >= loc_rows || c < 0 || c + t > loc_cols) return 0;
    for (j = 0; j < t; j++) {
        int b = loc_site[r * loc_cols + c + j];
        if (b < 0) return 0;
        for (k = 0; k < 4; k++) if (!owned(b + k, ch)) return 0;
    }
    for (k = 0; k < 4 * t; k++) {
        int b = loc_site[r * loc_cols + c + k / 4] + (k & 3);
        if (k < h->n) { occ[b] = h->first + k; site_of[h->first + k] = b; }
        else occ[b] = RES(ch);
    }
    for (j = 0; j < t; j++)
        if (!tile_ok((uint32_t)loc_site[r * loc_cols + c + j])) {
            for (k = 0; k < 4 * t; k++) occ[loc_site[r * loc_cols + c + k / 4] + (k & 3)] = NONE;
            return 0;
        }
    return 1;
}

static void place(int effort, uint32_t *moves_out, int *cost0, int *cost1) {
    uint32_t t;
    int i, cost = 0;
    /* sites */
    for (t = 0; t < db->n_tile; t++)
        if (zf_streq(ZDB_STR(db, db->type[db->tile[t].type].name), "PLC2")) n_site += 4;
    site_tile = zf_alloc(sizeof(uint32_t) * (size_t)n_site);
    site_row = zf_alloc(sizeof(int16_t) * (size_t)n_site);
    site_col = zf_alloc(sizeof(int16_t) * (size_t)n_site);
    occ = zf_alloc(sizeof(int) * (size_t)n_site);
    n_site = 0;
    for (t = 0; t < db->n_tile; t++) {
        const char *n = ZDB_STR(db, db->tile[t].name);
        int r = 0, c = 0, s;
        if (!zf_streq(ZDB_STR(db, db->type[db->tile[t].type].name), "PLC2")) continue;
        for (n++; *n >= '0' && *n <= '9'; n++) r = r * 10 + (*n - '0');
        for (n++; *n >= '0' && *n <= '9'; n++) c = c * 10 + (*n - '0');
        for (s = 0; s < 4; s++) {
            site_tile[n_site] = t;
            site_row[n_site] = (int16_t)r;
            site_col[n_site] = (int16_t)c;
            occ[n_site] = NONE;
            n_site++;
        }
    }
    if (n_sl > n_site) zf_fatal("%d slices, but the device has %d", n_sl, n_site);
    control_classes();
    for (i = 0; i < n_site; i++) {
        if (site_row[i] + 1 > loc_rows) loc_rows = site_row[i] + 1;
        if (site_col[i] + 1 > loc_cols) loc_cols = site_col[i] + 1;
    }
    loc_site = zf_alloc(sizeof(int) * (size_t)(loc_rows * loc_cols));
    for (i = 0; i < loc_rows * loc_cols; i++) loc_site[i] = -1;
    for (i = 0; i < n_site; i += 4) loc_site[site_row[i] * loc_cols + site_col[i]] = i;
    site_of = zf_alloc(sizeof(int) * (size_t)n_sl);
    build_adjacency();

    /* Initial: chains first, as whole blocks; then each slice at the free
     * site nearest the centre of the IO it connects to, or of the die,
     * respecting the tile rule. */
    /* cells and chains with loc= first: exactly where they were put */
    for (i = 0; i < n_sl; i++) {
        loc_t a = sl[i].at;
        int base, s;
        if (a.r < 0) continue;
        if (a.r >= loc_rows || a.c >= loc_cols || loc_site[a.r * loc_cols + a.c] < 0)
            zf_fatal_at(zl, sl_line(i), "loc=R%dC%d is not a logic (PLC2) tile", a.r, a.c);
        base = loc_site[a.r * loc_cols + a.c];
        if (a.s >= 0) {
            if (occ[base + a.s] != NONE)
                zf_fatal_at(zl, sl_line(i), "loc=R%dC%d.%c is taken twice", a.r, a.c, 'A' + a.s);
            s = a.s;
        } else {
            for (s = 0; s < 4 && occ[base + s] != NONE; s++) ;
            if (s == 4) zf_fatal_at(zl, sl_line(i), "loc=R%dC%d: the tile is full", a.r, a.c);
        }
        occ[base + s] = i;
        site_of[i] = base + s;
        if (!tile_ok((uint32_t)base))
            zf_fatal_at(zl, sl_line(i), "loc= puts more than two clocks or two resets in tile R%dC%d",
                a.r, a.c);
    }
    for (i = 0; i < n_chain; i++)
        if (chains[i].at.r >= 0 && !chain_put(i, chains[i].at.r, chains[i].at.c))
            zf_fatal("loc=R%dC%d: a carry chain of %d slices does not fit there", chains[i].at.r,
                chains[i].at.c, chains[i].n);
    for (i = 0; i < n_chain; i++) {
        int k, j, r = 0, c = 0, n = 0, br = -1, bc = -1, bd = 1 << 30, rr, cc;
        if (chains[i].at.r >= 0) continue;
        for (j = chains[i].first; j < chains[i].first + chains[i].n; j++)
            for (k = sl_net0[j]; k < sl_net0[j + 1]; k++) {
                int q;
                for (q = 0; q < n_io; q++)
                    if (ios[q].net == sl_net[k]) { int a, b; io_loc(&ios[q], &a, &b); r += a; c += b; n++; }
            }
        if (n) { r /= n; c /= n; } else { r = loc_rows / 2; c = loc_cols / 2; }
        /* Outward from the target, ring by ring, stopping at the first
         * fit. Within a ring the order is the old whole-die scan's (row,
         * then column, ascending), so the position chosen is the same --
         * the nearest, ties to the first in scan order -- but found in a
         * handful of tries instead of every head position on the die,
         * which was 173M instructions for one 13-slice chain (sec. 16.3). */
        (void)rr;
        for (k = 0; br < 0 && k < loc_rows + loc_cols; k++) {
            int dr;
            for (dr = -k; dr <= k && br < 0; dr++) {
                int dc = k - iabs(dr), side;
                rr = r + dr;
                if (rr < 0 || rr >= loc_rows) continue;
                for (side = 0; side < (dc ? 2 : 1) && br < 0; side++) {
                    cc = side ? c + dc : c - dc;
                    if (cc < 0 || cc >= loc_cols) continue;
                    if (chain_put(i, rr, cc)) { chain_lift(i); br = rr; bc = cc; }
                }
            }
        }
        (void)bd;
        if (br < 0 || !chain_put(i, br, bc)) zf_fatal("no room for a carry chain of %d slices", chains[i].n);
    }
    for (i = 0; i < n_sl; i++) {
        int k, r = 0, c = 0, n = 0, best = -1, bd = 1 << 30, s;
        if (sl[i].chain != NONE || sl[i].at.r >= 0) continue;
        for (k = sl_net0[i]; k < sl_net0[i + 1]; k++) {
            int j;
            for (j = 0; j < n_io; j++)
                if (ios[j].net == sl_net[k]) { int rr, cc; io_loc(&ios[j], &rr, &cc); r += rr; c += cc; n++; }
        }
        if (n) { r /= n; c /= n; } else { r = site_row[n_site / 2]; c = site_col[n_site / 2]; }
        for (s = 0; s < n_site; s++) {
            int d;
            if (occ[s] != NONE) continue;
            d = iabs(site_row[s] - r) + iabs(site_col[s] - c);
            if (d >= bd) continue;
            occ[s] = i;
            if (tile_ok((uint32_t)(s & ~3))) { bd = d; best = s; }
            occ[s] = NONE;
        }
        if (best < 0) zf_fatal("no legal site for slice %d (too many clocks or resets per tile?)", i);
        occ[best] = i;
        site_of[i] = best;
    }

    net_iobox = zf_alloc(sizeof(*net_iobox) * (size_t)(n_net ? n_net : 1));
    for (i = 0; i < n_net; i++) {
        net_iobox[i][0] = 32767; net_iobox[i][1] = -1;
        net_iobox[i][2] = 32767; net_iobox[i][3] = -1;
    }
    for (i = 0; i < n_io; i++) {
        int r, c, ni = ios[i].net;
        io_loc(&ios[i], &r, &c);
        if (r < net_iobox[ni][0]) net_iobox[ni][0] = (int16_t)r;
        if (r > net_iobox[ni][1]) net_iobox[ni][1] = (int16_t)r;
        if (c < net_iobox[ni][2]) net_iobox[ni][2] = (int16_t)c;
        if (c > net_iobox[ni][3]) net_iobox[ni][3] = (int16_t)c;
    }
    net_cost = zf_alloc(sizeof(int) * (size_t)n_net);
    net_seen = zf_alloc(sizeof(uint32_t) * (size_t)(n_net ? n_net : 1));
    for (i = 0; i < n_net; i++) { net_cost[i] = hpwl(i); cost += net_cost[i]; }
    *cost0 = cost;
    *moves_out = 0;

    /* Annealing. T in 1/256; starts at a few units of wirelength, cools
     * geometrically; the move window shrinks with it. */
    exptab_init();
    if (n_sl > 1) {
        uint32_t T = 256 * 8, inner = (uint32_t)effort * (uint32_t)n_sl;
        int *touched = zf_alloc(sizeof(int) * 4096);
        int radius = 16;
        while (T > 8) {
            uint32_t m, acc = 0;
            for (m = 0; m < inner; m++) {
                int a = (int)(rnd() % (uint32_t)n_sl), sa = site_of[a], sb, b, nt = 0;
                int before, after, delta, k;
                int r = site_row[sa] + (int)(rnd() % (uint32_t)(2 * radius + 1)) - radius;
                int c = site_col[sa] + (int)(rnd() % (uint32_t)(2 * radius + 1)) - radius;
                move_stamp++;
                if (sl[a].at.r >= 0 || (sl[a].chain != NONE && chains[sl[a].chain].at.r >= 0))
                    continue;                           /* loc= holds it */
                if (sl[a].chain != NONE) {
                    /* the whole chain, head to slice A of (r, c) */
                    int ch = sl[a].chain, hs = site_of[chains[ch].first], j;
                    int orr = site_row[hs], oc = site_col[hs];
                    before = 0;
                    for (j = chains[ch].first; j < chains[ch].first + chains[ch].n; j++)
                        before += sl_cost(j, touched, &nt);
                    chain_lift(ch);
                    if (!chain_put(ch, r, c)) {
                        if (!chain_put(ch, orr, oc)) zf_fatal("placer: lost a carry chain");
                        continue;
                    }
                    after = 0;
                    for (k = 0; k < nt; k++) after += hpwl(touched[k]);
                    delta = after - before;
                    (*moves_out)++;
                    if (delta <= 0 || (((uint32_t)delta << 16) / T < 4096 &&
                            (rnd() & 0xffff) < exptab[((uint32_t)delta << 16) / T])) {
                        for (k = 0; k < nt; k++) net_cost[touched[k]] = hpwl(touched[k]);
                        cost += delta;
                        acc++;
                    } else {
                        chain_lift(ch);
                        if (!chain_put(ch, orr, oc)) zf_fatal("placer: lost a carry chain");
                    }
                    continue;
                }
                /* a random site in the window; not every location is a
                 * logic tile, and a miss is just a skipped move */
                if (r < 0 || r >= loc_rows || c < 0 || c >= loc_cols) continue;
                sb = loc_site[r * loc_cols + c];
                if (sb < 0) continue;
                sb |= (int)(rnd() & 3);
                if (sb == sa) continue;
                b = occ[sb];
                if (b != NONE && (b < 0 || sl[b].chain != NONE || sl[b].at.r >= 0))
                    continue;                           /* chains move whole; loc= holds */
                before = sl_cost(a, touched, &nt);
                if (b != NONE) before += sl_cost(b, touched, &nt);
                /* move */
                occ[sa] = b; occ[sb] = a; site_of[a] = sb;
                if (b != NONE) site_of[b] = sa;
                if (!tile_ok((uint32_t)(sa & ~3)) || !tile_ok((uint32_t)(sb & ~3))) {
                    occ[sa] = a; occ[sb] = b; site_of[a] = sa;
                    if (b != NONE) site_of[b] = sb;
                    continue;
                }
                after = 0;
                for (k = 0; k < nt; k++) after += hpwl(touched[k]);
                delta = after - before;
                (*moves_out)++;
                if (delta <= 0 || (((uint32_t)delta << 16) / T < 4096 &&
                        (rnd() & 0xffff) < exptab[((uint32_t)delta << 16) / T])) {
                    for (k = 0; k < nt; k++) net_cost[touched[k]] = hpwl(touched[k]);
                    cost += delta;
                    acc++;
                } else {
                    occ[sa] = a; occ[sb] = b; site_of[a] = sa;
                    if (b != NONE) site_of[b] = sb;
                }
            }
            T = T * 7 / 8;
            if (radius > 1 && acc * 4 < inner) radius--;
        }
    }
    *cost1 = cost;
}

/* -- output ------------------------------------------------------------------ */

typedef struct { zf_writer_t w; } out_t;
static out_t *O;

static void put(const char *s) {
    zf_writer_bytes(&O->w, (const uint8_t *)s, (uint32_t)zf_strlen(s));
}

static void putf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    zf_vfmt(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put(buf);
}

/* Rewrites an INIT so that input p held at 1 gives what input p at
 * value v gave. Only the entries with p = 1 are rewritten: they are the
 * only ones the LUT4 can now read, and the others must stay -- in carry
 * mode the LUT2 half reads INIT[3:0] whatever C and D are (sec. 16.2). */
static uint32_t fold(uint32_t init, int p, int v) {
    uint32_t r = init;
    int i;
    for (i = 0; i < 16; i++) {
        int src;
        if (!(i & (1 << p))) continue;
        src = v ? i : (i & ~(1 << p));
        r = (r & ~(1u << i)) | (((init >> src) & 1u) << i);
    }
    return r;
}

/* the pin wire of a slice's cell, for net terminals */
static void wire(char *buf, int cap, int s, const char *fmt, int n) {
    char name[24];
    zf_fmt(name, sizeof(name), fmt, n);
    zf_fmt(buf, cap, "R%dC%d/%s", site_row[site_of[s]], site_col[site_of[s]], name);
}

/* -- the command ---------------------------------------------------------- */

int cmd_place(int argc, char **argv, const char *dbdir_default) {
    const char *in = NULL, *out = NULL, *dbdir = dbdir_default, *device = NULL, *package = NULL;
    const char *locs_out = NULL;
    zf_reader_t *r = zf_alloc(sizeof(*r));
    char *line, *tok[40];
    int i, k, effort = 20, cost0, cost1;
    uint32_t moves;
    int *lc_sl, *lc_pos;
    char **tsink;
    int *tsink_n;

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (zf_streq(argv[i], "-D") && i + 1 < argc) dbdir = argv[++i];
        else if (zf_streq(argv[i], "-l") && i + 1 < argc) locs_out = argv[++i];
        else if (zf_streq(argv[i], "-e") && i + 1 < argc) {
            uint32_t e;
            if (!zf_parse_uint(argv[++i], &e) || !e || e > 1000) zf_fatal("-e: effort 1..1000");
            effort = (int)e;
        } else if (zf_streq(argv[i], "-s") && i + 1 < argc) {
            if (!zf_parse_uint(argv[++i], &rng) || !rng) zf_fatal("-s: a non-zero seed");
        } else if (argv[i][0] == '-') zf_fatal("place: unknown option %s", argv[i]);
        else if (!in) in = argv[i];
        else zf_fatal("place: more than one input");
    }
    if (!in) zf_fatal("usage: zfpga place IN.zl [-o OUT.zn] [-l PLACED.zl] [-e EFFORT] [-s SEED] [-D DBDIR]");
    if (!dbdir) zf_fatal("no database directory; give -D DIR");
    if (!out) {
        size_t n = zf_strlen(in), dot = n, j;
        char *o;
        for (j = n; j > 0; j--) { if (in[j - 1] == '/') break; if (in[j - 1] == '.') { dot = j - 1; break; } }
        o = zf_alloc(dot + 4);
        zf_memcpy(o, in, dot);
        zf_memcpy(o + dot, ".zn", 4);
        out = o;
    }
    zl = in;
    db = zf_alloc(sizeof(*db));
    nh_cap = 1024;
    nh = zf_alloc(sizeof(uint32_t) * nh_cap);

    zf_reader_open(r, in);
    while ((line = zf_reader_line(r)) != NULL) {
        int ln = r->lineno;
        int n;
        GROW(raw, n_raw, cap_raw, char *);
        raw[n_raw] = zf_strdup(line);
        if (n_raw >= cap_rawc) {
            uint8_t *nc = zf_alloc((size_t)cap_raw);
            if (n_raw) zf_memcpy(nc, raw_cell, (size_t)n_raw);
            raw_cell = nc;
            cap_rawc = cap_raw;
        }
        raw_cell[n_raw++] = 0;
        n = zf_tokens(line, tok, 40);
        if (n == 0) continue;
        if (n == 40) zf_fatal_at(in, ln, "too many words on one line");
        if (zf_streq(tok[0], "device") && n == 2) {
            if (device) zf_fatal_at(in, ln, "second device line");
            device = zf_strdup(tok[1]);
            zdb_load(db, dbdir, device);
            continue;
        }
        if (!device) zf_fatal_at(in, ln, "'device' must come first");
        if (zf_streq(tok[0], "package") && n == 2) {
            package = zf_strdup(tok[1]);
        } else if (zf_streq(tok[0], "input") || zf_streq(tok[0], "output")) {
            static const char *const ok[] = { "net", "io_type", "type", "pullmode",
                "hysteresis", "slewrate", "drive", "clamp", "opendrain", NULL };
            lio_t *o;
            uint32_t j;
            char buf[256];
            int m, len = 0;
            if (n < 4) zf_fatal_at(in, ln, "%s NAME PIN net=NET [options]", tok[0]);
            if (!package) zf_fatal_at(in, ln, "IO needs a 'package' line first");
            check_opts(tok + 3, n - 3, ok, ln);
            GROW(ios, n_io, cap_io, lio_t);
            o = &ios[n_io];
            o->name = zf_strdup(tok[1]);
            o->pin = zf_strdup(tok[2]);
            o->output = tok[0][0] == 'o';
            o->line = ln;
            if (!opt(tok + 3, n - 3, "net")) zf_fatal_at(in, ln, "IO %s has no net=", tok[1]);
            o->net = net_of(opt(tok + 3, n - 3, "net"), ln);
            if (o->net < 0) zf_fatal_at(in, ln, "IO %s on a constant is not supported", tok[1]);
            for (j = 0; j < db->n_pin; j++)
                if (zf_streq(ZDB_STR(db, db->pin[j].package), package) &&
                        zf_streq(ZDB_STR(db, db->pin[j].pin), o->pin)) break;
            if (j == db->n_pin) zf_fatal_at(in, ln, "package %s has no pin %s", package, o->pin);
            o->pio = db->pin[j].pio;
            /* the options pass to the .zn io line; io_type is its type= */
            buf[0] = 0;
            for (m = 3; m < n; m++) {
                if (tok[m][0] == 'n' && tok[m][1] == 'e' && tok[m][2] == 't' && tok[m][3] == '=') continue;
                len += zf_fmt(buf + len, (int)sizeof(buf) - len, " %s",
                    (tok[m][0] == 'i' && tok[m][1] == 'o' && tok[m][2] == '_') ? tok[m] + 3 : tok[m]);
            }
            o->opts = zf_strdup(buf);
            if (o->output) use(o->net); else drive(o->net, DRV_IO | n_io, ln);
            n_io++;
        } else if (zf_streq(tok[0], "lut")) {
            static const char *const ok[] = { "init", "func", "a", "b", "c", "d", "z", "loc", NULL };
            llut_t *l;
            const char *v;
            static const char *pins[4] = { "a", "b", "c", "d" };
            int p;
            if (n < 2) zf_fatal_at(in, ln, "lut NAME init=... a= b= c= d= z=");
            check_opts(tok + 2, n - 2, ok, ln);
            GROW(luts, n_lut, cap_lut, llut_t);
            l = &luts[n_lut];
            l->name = zf_strdup(tok[1]);
            l->line = ln;
            v = opt(tok + 2, n - 2, "init");
            if (v && opt(tok + 2, n - 2, "func"))
                zf_fatal_at(in, ln, "lut %s: give init= or func=, not both", tok[1]);
            if (v) {
                if (!zf_parse_uint(v, &l->init) || l->init > 0xffff)
                    zf_fatal_at(in, ln, "lut %s: init= is not a 16-bit value", tok[1]);
            } else if ((v = opt(tok + 2, n - 2, "func")) != NULL) {
                int used;
                l->init = func_init(v, ln, &used);
                for (p = 0; p < 4; p++)
                    if ((used >> p & 1) && !opt(tok + 2, n - 2, pins[p]))
                        zf_fatal_at(in, ln, "lut %s: func uses %s but %s= is not connected",
                            tok[1], pins[p], pins[p]);
            } else {
                zf_fatal_at(in, ln, "lut %s needs init= or func=", tok[1]);
            }
            for (p = 0; p < 4; p++) {
                v = opt(tok + 2, n - 2, pins[p]);
                l->in[p] = v ? net_of(v, ln) : C0;
                use(l->in[p]);
            }
            l->at = parse_loc(opt(tok + 2, n - 2, "loc"), ln);
            raw_cell[n_raw - 1] = 1;
            l->raw = n_raw - 1;
            v = opt(tok + 2, n - 2, "z");
            l->out = v ? net_of(v, ln) : NONE;
            drive(l->out, DRV_LUT | n_lut, ln);
            n_lut++;
        } else if (zf_streq(tok[0], "ff")) {
            static const char *const ok[] = { "d", "q", "clk", "ce", "lsr", "gsr", "regset",
                "srmode", "lsrmode", "cemux", "clkmux", "lsrmux", "loc", NULL };
            lff_t *f;
            const char *v;
            if (n < 2) zf_fatal_at(in, ln, "ff NAME d= q= clk= [ce= lsr=] [options]");
            check_opts(tok + 2, n - 2, ok, ln);
            GROW(ffs, n_ff, cap_ff, lff_t);
            f = &ffs[n_ff];
            f->name = zf_strdup(tok[1]);
            f->line = ln;
            v = opt(tok + 2, n - 2, "d");   f->d = v ? net_of(v, ln) : C0;
            v = opt(tok + 2, n - 2, "q");   f->q = v ? net_of(v, ln) : NONE;
            v = opt(tok + 2, n - 2, "clk"); f->clk = v ? net_of(v, ln) : NONE;
            v = opt(tok + 2, n - 2, "ce");  f->ce = v ? net_of(v, ln) : C1;
            v = opt(tok + 2, n - 2, "lsr"); f->lsr = v ? net_of(v, ln) : C0;
#define P(field, key, def) do { v = opt(tok + 2, n - 2, key); f->field = zf_strdup(v ? v : def); } while (0)
            P(gsr, "gsr", "ENABLED"); P(regset, "regset", "RESET"); P(srmode, "srmode", "LSR_OVER_CE");
            P(lsrmode, "lsrmode", "LSR"); P(cemux, "cemux", "1"); P(clkmux, "clkmux", "CLK");
            P(lsrmux, "lsrmux", "LSR");
#undef P
            if (f->clk < 0) zf_fatal_at(in, ln, "flip-flop %s has no clock", tok[1]);
            if (!zf_streq(f->clkmux, "CLK")) zf_fatal_at(in, ln, "clkmux=%s: inverted clocks are not in v1", f->clkmux);
            /* a constant enable or reset is a mux setting, not a net */
            if (f->ce == C1) f->cemux = "1";
            else if (f->ce == C0) zf_fatal_at(in, ln, "flip-flop %s is never enabled", tok[1]);
            else f->cemux = "CE";
            if (f->lsr == C1) zf_fatal_at(in, ln, "flip-flop %s is held in reset", tok[1]);
            if (f->lsr < 0) f->lsr = NONE;
            use(f->d); use(f->clk); if (f->ce >= 0) use(f->ce); if (f->lsr >= 0) use(f->lsr);
            f->at = parse_loc(opt(tok + 2, n - 2, "loc"), ln);
            raw_cell[n_raw - 1] = 2;
            f->raw = n_raw - 1;
            drive(f->q, DRV_FF | n_ff, ln);
            n_ff++;
        } else if (zf_streq(tok[0], "ccu2")) {
            static const char *const ok[] = { "init0", "init1", "inject0", "inject1", "a0", "b0",
                "c0", "d0", "a1", "b1", "c1", "d1", "cin", "cout", "s0", "s1", "loc", NULL };
            static const char *pins[8] = { "a0", "b0", "c0", "d0", "a1", "b1", "c1", "d1" };
            lccu_t *cc;
            const char *v;
            int p;
            if (n < 2) zf_fatal_at(in, ln, "ccu2 NAME init0= init1= ... cin= cout= s0= s1=");
            check_opts(tok + 2, n - 2, ok, ln);
            GROW(ccus, n_ccu, cap_ccu, lccu_t);
            cc = &ccus[n_ccu];
            cc->name = zf_strdup(tok[1]);
            cc->line = ln;
            for (p = 0; p < 2; p++) {
                v = opt(tok + 2, n - 2, p ? "init1" : "init0");
                if (!v || !zf_parse_uint(v, &cc->init[p]) || cc->init[p] > 0xffff)
                    zf_fatal_at(in, ln, "ccu2 %s needs 16-bit init0= and init1=", tok[1]);
                v = opt(tok + 2, n - 2, p ? "inject1" : "inject0");
                cc->inj[p] = zf_strdup(v ? v : "YES");
                if (!zf_streq(cc->inj[p], "YES") && !zf_streq(cc->inj[p], "NO"))
                    zf_fatal_at(in, ln, "inject must be YES or NO");
            }
            for (p = 0; p < 8; p++) {
                v = opt(tok + 2, n - 2, pins[p]);
                cc->in[p] = v ? net_of(v, ln) : C0;
                use(cc->in[p]);
            }
            v = opt(tok + 2, n - 2, "cin");
            cc->cin = v ? net_of(v, ln) : C0;
            use(cc->cin);
            for (p = 0; p < 2; p++) {
                v = opt(tok + 2, n - 2, p ? "s1" : "s0");
                cc->s[p] = v ? net_of(v, ln) : NONE;
                drive(cc->s[p], DRV_CCS | (n_ccu << 1 | p), ln);
            }
            v = opt(tok + 2, n - 2, "cout");
            cc->cout = v ? net_of(v, ln) : NONE;
            drive(cc->cout, DRV_CCC | n_ccu, ln);
            cc->at = parse_loc(opt(tok + 2, n - 2, "loc"), ln);
            if (cc->at.r >= 0 && (cc->at.s > 0 || cc->at.h >= 0))
                zf_fatal_at(in, ln, "ccu2 %s: a chain's loc= is a tile, R<row>C<col>; its feed-in "
                    "takes slice A", tok[1]);
            raw_cell[n_raw - 1] = 3;
            cc->raw = n_raw - 1;
            n_ccu++;
        } else {
            zf_fatal_at(in, ln, "unknown statement '%s' (input, output, lut, ff, ccu2)", tok[0]);
        }
    }
    zf_reader_close(r);
    if (!device) zf_fatal("%s: no device line", in);
    for (i = 0; i < n_net; i++)
        if (nets[i].driver == NONE && nets[i].n_sinks)
            zf_fatal("%s: net %s is used but nothing drives it", in, nets[i].name);

    pack();
    place(effort, &moves, &cost0, &cost1);
    zf_note("placed %d LUTs, %d carry cells in %d chains and %d flip-flops in %d slices; "
        "wirelength %d -> %d in %u moves", n_lut, n_ccu, n_chain, n_ff, n_sl, cost0, cost1, moves);

    /* where each LC ended up: slice, and k = 2 * (site & 3) + half */
    lc_sl = zf_alloc(sizeof(int) * (size_t)n_lc);
    lc_pos = zf_alloc(sizeof(int) * (size_t)n_lc);
    for (i = 0; i < n_sl; i++)
        for (k = 0; k < 2; k++)
            if (sl[i].lc[k] != NONE) { lc_sl[sl[i].lc[k]] = i; lc_pos[sl[i].lc[k]] = k; }

    /* -l: the input again, every cell with loc= where it went. Edit
     * and re-run for manual placement; a cell with loc= never moves. */
    if (locs_out) {
        int ln;
        O = zf_alloc(sizeof(*O));
        zf_writer_open(&O->w, locs_out);
        for (ln = 0; ln < n_raw; ln++) {
            char *t[40], *cp, where[32];
            int nt, j, lc = NONE;
            if (!raw_cell[ln]) { put(raw[ln]); put("\n"); continue; }
            cp = zf_strdup(raw[ln]);
            nt = zf_tokens(cp, t, 40);
            where[0] = 0;
            for (j = 0; j < n_lut && lc == NONE; j++) if (luts[j].raw == ln) lc = luts[j].lc;
            for (j = 0; j < n_ff && lc == NONE; j++) if (ffs[j].raw == ln) lc = ffs[j].lc;
            if (lc != NONE) {
                int st = site_of[lc_sl[lc]];
                zf_fmt(where, sizeof(where), "R%dC%d.%c%d", site_row[st], site_col[st],
                    'A' + (st & 3), lc_pos[lc]);
            }
            for (j = 0; j < n_ccu; j++)
                if (ccus[j].raw == ln && ccus[j].prev == NONE) {
                    int ch, st;
                    for (ch = 0; ch < n_chain; ch++)
                        if (sl[chains[ch].first + 1].lc[0] == ccus[j].lc[0]) break;
                    st = site_of[chains[ch].first];
                    zf_fmt(where, sizeof(where), "R%dC%d", site_row[st], site_col[st]);
                }
            for (j = 0; j < nt; j++) {
                if (t[j][0] == 'l' && t[j][1] == 'o' && t[j][2] == 'c' && t[j][3] == '=') continue;
                if (j) put(" ");
                put(t[j]);
            }
            if (where[0]) { put(" loc="); put(where); }
            put("\n");
        }
        zf_writer_close(&O->w);
    }

    O = zf_alloc(sizeof(*O));
    zf_writer_open(&O->w, out);
    putf("# placed by zfpga place from %s\ndevice %s\n", in, device);
    if (package) putf("package %s\n", package);
    put("\n");
    for (i = 0; i < n_io; i++) {
        const zdb_pio_t *p = &db->pio[ios[i].pio];
        putf("io R%uC%u.%c dir=%s%s\n", p->row, p->col, p->letter,
            ios[i].output ? "OUTPUT" : "INPUT", ios[i].opts);
    }
    for (i = 0; i < n_lc; i++) {
        int s = lc_sl[i], site = site_of[s], half = lc_pos[i], kk = 2 * (site & 3) + half;
        char slc[8];
        zf_fmt(slc, sizeof(slc), "SLICE%c", "ABCD"[site & 3]);
        if (lcs[i].lut != NONE) {
            const llut_t *l = &luts[lcs[i].lut];
            uint32_t init = l->init;
            char pins[5];
            int p, np = 0;
            for (p = 0; p < 4; p++) {
                if (l->in[p] >= 0) pins[np++] = (char)('A' + p);
                else init = fold(init, p, l->in[p] == C1);
            }
            pins[np] = 0;
            putf("comb R%dC%d %s %d init=0x%04X pins=%s\n", site_row[site], site_col[site], slc,
                half, init, np ? pins : "-");
        }
        if (lcs[i].ccu >= 0) {
            const lccu_t *cc = &ccus[lcs[i].ccu];
            int h = lcs[i].half, p, np = 0;
            uint32_t init = cc->init[h];
            char pins[5];
            for (p = 0; p < 4; p++) {
                if (cc->in[4 * h + p] >= 0) pins[np++] = (char)('A' + p);
                else init = fold(init, p, cc->in[4 * h + p] == C1);
            }
            pins[np] = 0;
            putf("comb R%dC%d %s %d mode=CCU2 init=0x%04X pins=%s inject=%s\n", site_row[site],
                site_col[site], slc, half, init, np ? pins : "-", cc->inj[h]);
        } else if (lcs[i].ccu == FEED_IN) {
            /* nextpnr's feed-in: half 0's LUT4 is 0 (unused pins at 1),
             * gating the hardware carry in off; its LUT2 on A makes the
             * carry. Half 1 passes it through. */
            const chain_t *ch = &chains[sl[s].chain];
            if (half == 0) {
                if (ch->feed_net >= 0)
                    putf("comb R%dC%d %s 0 mode=CCU2 init=0x000A pins=A inject=NO\n",
                        site_row[site], site_col[site], slc);
                else
                    putf("comb R%dC%d %s 0 mode=CCU2 init=0x%04X pins=- inject=NO\n",
                        site_row[site], site_col[site], slc, ch->feed_const ? 0x0008 : 0x0000);
            } else {
                putf("comb R%dC%d %s 1 mode=CCU2 init=0xFFFF pins=- inject=YES\n",
                    site_row[site], site_col[site], slc);
            }
        } else if (lcs[i].ccu == FEED_OUT) {
            /* nextpnr's feed-out: half 0's sum is its carry in */
            putf("comb R%dC%d %s %d mode=CCU2 init=0x0000 pins=- inject=NO\n",
                site_row[site], site_col[site], slc, half);
        }
        if (lcs[i].ff != NONE) {
            const lff_t *f = &ffs[lcs[i].ff];
            putf("ff R%dC%d %s %d clk=?@%s lsr=%s%s sd=%d gsr=%s regset=%s lsrmode=%s cemux=%s "
                "clkmux=%s srmode=%s lsrmux=%s\n", site_row[site], site_col[site], slc, half,
                nets[f->clk].name, f->lsr >= 0 ? "?@" : "-", f->lsr >= 0 ? nets[f->lsr].name : "",
                lcs[i].sd, f->gsr, f->regset, f->lsrmode, f->cemux, f->clkmux, f->srmode, f->lsrmux);
        }
        (void)kk;
    }
    put("\n");

    /* nets by their pin wires */
    tsink = zf_alloc(sizeof(char *) * 512);
    tsink_n = zf_alloc(sizeof(int));
    for (i = 0; i < n_net; i++) {
        int drv = nets[i].driver, ns = 0, j;
        char src[48], w[48];
        if (drv == NONE || !nets[i].n_sinks) continue;
        if (DRV_KIND(drv) == DRV_CCC) {
            /* a carry out: fixed wiring to the next carry in, or, at a
             * chain's end, to logic through the feed-out's sum */
            int ch, fo = NONE;
            for (ch = 0; ch < n_chain; ch++)
                if (chains[ch].out_net == i) fo = chains[ch].first + chains[ch].n - 1;
            if (fo == NONE) continue;
            wire(src, sizeof(src), fo, "F%d_SLICE", 2 * (site_of[fo] & 3));
        } else if (DRV_KIND(drv) == DRV_CCS) {
            int lc = ccus[DRV_IDX(drv) >> 1].lc[DRV_IDX(drv) & 1], s = lc_sl[lc];
            wire(src, sizeof(src), s, "F%d_SLICE", 2 * (site_of[s] & 3) + lc_pos[lc]);
        } else if (DRV_KIND(drv) == DRV_IO) {
            const zdb_pio_t *p = &db->pio[ios[DRV_IDX(drv)].pio];
            zf_fmt(src, sizeof(src), "R%uC%u/JPADDI%c_PIO", p->row, p->col, p->letter);
        } else {
            int lc = DRV_KIND(drv) == DRV_LUT ? luts[DRV_IDX(drv)].lc : ffs[DRV_IDX(drv)].lc;
            int s = lc_sl[lc];
            wire(src, sizeof(src), s, DRV_KIND(drv) == DRV_LUT ? "F%d_SLICE" : "Q%d_SLICE",
                2 * (site_of[s] & 3) + lc_pos[lc]);
        }
#define SINK(buf) do { for (j = 0; j < ns; j++) if (zf_streq(tsink[j], buf)) break; \
    if (j == ns) { if (ns == 512) zf_fatal("net %s: more than 512 sinks", nets[i].name); \
    tsink[ns++] = zf_strdup(buf); } } while (0)
        for (k = 0; k < n_lut; k++) {
            int p, lc = luts[k].lc, s = lc_sl[lc], kk = 2 * (site_of[s] & 3) + lc_pos[lc];
            for (p = 0; p < 4; p++)
                if (luts[k].in[p] == i) {
                    char nm[8];
                    zf_fmt(nm, sizeof(nm), "%c%%d", 'A' + p);
                    wire(w, sizeof(w), s, nm, kk);
                    SINK(w);
                }
        }
        for (k = 0; k < n_ccu; k++) {
            int h, p;
            for (h = 0; h < 2; h++) {
                int lc = ccus[k].lc[h], s = lc_sl[lc], kk = 2 * (site_of[s] & 3) + lc_pos[lc];
                for (p = 0; p < 4; p++)
                    if (ccus[k].in[4 * h + p] == i) {
                        char nm[8];
                        zf_fmt(nm, sizeof(nm), "%c%%d", 'A' + p);
                        wire(w, sizeof(w), s, nm, kk);
                        SINK(w);
                    }
            }
        }
        for (k = 0; k < n_chain; k++)
            if (chains[k].feed_net == i) {
                wire(w, sizeof(w), chains[k].first, "A%d", 2 * (site_of[chains[k].first] & 3));
                SINK(w);
            }
        for (k = 0; k < n_ff; k++) {
            const lff_t *f = &ffs[k];
            int lc = f->lc, s = lc_sl[lc], kk = 2 * (site_of[s] & 3) + lc_pos[lc], sx = site_of[s] & 3;
            if (f->d == i) { wire(w, sizeof(w), s, lcs[lc].sd ? "DI%d_SLICE" : "M%d_SLICE", kk); SINK(w); }
            if (f->clk == i) { wire(w, sizeof(w), s, "CLK%d_SLICE", sx); SINK(w); }
            if (f->ce == i) { wire(w, sizeof(w), s, "CE%d_SLICE", sx); SINK(w); }
            if (f->lsr == i) { wire(w, sizeof(w), s, "LSR%d_SLICE", sx); SINK(w); }
        }
        for (k = 0; k < n_io; k++)
            if (ios[k].output && ios[k].net == i) {
                const zdb_pio_t *p = &db->pio[ios[k].pio];
                zf_fmt(w, sizeof(w), "R%uC%u/PADDO%c_PIO", p->row, p->col, p->letter);
                SINK(w);
            }
#undef SINK
        if (!ns) continue;
        putf("net %s %s", nets[i].name, src);
        for (j = 0; j < ns; j++) {
            if (j && j % 16 == 0) putf("\nsinks %s", nets[i].name);
            putf(" %s", tsink[j]);
        }
        put("\n");
    }
    (void)tsink_n;
    zf_writer_close(&O->w);
    return 0;
}
