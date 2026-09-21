/*
 * zfpga -- synth, elaboration: the syntax tree to a bit-level gate graph.
 *
 * Every Verilog bit becomes a gate. Gates are hash-consed with their
 * constants folded, so equal logic is one node and x & 0 never exists.
 *
 * Widths follow Verilog's context rule, done the way it can be done
 * exactly: an expression is evaluated AT the width it is assigned to.
 * For + - & | ^ ~ and <<, the low bits of a result depend only on the low
 * bits of the operands, so computing at the target width is the same as
 * computing wide and truncating -- and it is what makes `{co, s} = a + b`
 * catch the carry and `ctr + 1` a 24-bit counter, not a 32-bit one.
 * Comparisons, >> and concatenation are self-determined, as the standard
 * says.
 *
 * Wires are resolved lazily and in any order; a combinational loop is
 * refused. always @(*) runs symbolically with blocking semantics, and a
 * variable not assigned on every path is refused as a latch. A clocked
 * block runs with non-blocking semantics: reads see the old value.
 */

#include "synth.h"

static module_t *M;
static graph_t *G;

#define EFAIL(e, ...) zf_fatal_at((e)->file ? (e)->file : M->file, (e)->line, __VA_ARGS__)
#define SFAIL(s, ...) zf_fatal_at((s)->file ? (s)->file : M->file, (s)->line, __VA_ARGS__)

/* -- gates ---------------------------------------------------------------- */

static uint32_t hg(int op, int a, int b, int c) {
    return ((uint32_t)op * 2654435761u) ^ ((uint32_t)a * 40503u) ^ ((uint32_t)b * 97787u) ^
        ((uint32_t)c * 7919u);
}

static void grow_hash(void) {
    uint32_t cap = G->h_cap ? G->h_cap * 2 : 4096, k;
    uint32_t *nh = zf_alloc(sizeof(uint32_t) * cap);
    for (k = 2; k < (uint32_t)G->n_g; k++) {
        gate_t *q = &G->g[k];
        uint32_t i;
        for (i = hg(q->op, q->a, q->b, q->c) & (cap - 1); nh[i]; i = (i + 1) & (cap - 1)) ;
        nh[i] = k + 1;
    }
    G->h = nh;
    G->h_cap = cap;
}

int g_make(graph_t *g, int op, int a, int b, int c) {
    uint32_t i;
    gate_t *q;
    if ((uint32_t)(g->n_g + 1) * 2 > g->h_cap) grow_hash();
    for (i = hg(op, a, b, c) & (g->h_cap - 1); g->h[i]; i = (i + 1) & (g->h_cap - 1)) {
        q = &g->g[g->h[i] - 1];
        if (q->op == op && q->a == a && q->b == b && q->c == c) return (int)g->h[i] - 1;
    }
    if (g->n_g == g->cap_g) {
        int cap = g->cap_g * 2;
        gate_t *ng = zf_alloc(sizeof(gate_t) * (size_t)cap);
        zf_memcpy(ng, g->g, sizeof(gate_t) * (size_t)g->n_g);
        g->g = ng;
        g->cap_g = cap;
    }
    q = &g->g[g->n_g];
    q->op = (uint8_t)op; q->a = a; q->b = b; q->c = c; q->d = 0;
    g->h[i] = (uint32_t)++g->n_g;
    return g->n_g - 1;
}

#define C0 0
#define C1 1

int g_not(graph_t *g, int a) {
    if (a == C0) return C1;
    if (a == C1) return C0;
    if (g->g[a].op == G_NOT) return g->g[a].a;
    return g_make(g, G_NOT, a, 0, 0);
}

static int is_not_of(graph_t *g, int a, int b) {
    return (g->g[a].op == G_NOT && g->g[a].a == b) || (g->g[b].op == G_NOT && g->g[b].a == a);
}

int g_and(graph_t *g, int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a == C0) return C0;
    if (a == C1) return b;
    if (a == b) return a;
    if (is_not_of(g, a, b)) return C0;
    return g_make(g, G_AND, a, b, 0);
}

int g_or(graph_t *g, int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a == C1) return C1;
    if (a == C0) return b;
    if (a == b) return a;
    if (is_not_of(g, a, b)) return C1;
    return g_make(g, G_OR, a, b, 0);
}

int g_xor(graph_t *g, int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a == C0) return b;
    if (a == C1) return g_not(g, b);
    if (a == b) return C0;
    if (is_not_of(g, a, b)) return C1;
    return g_make(g, G_XOR, a, b, 0);
}

int g_mux(graph_t *g, int s, int a, int b) {       /* s ? a : b */
    if (s == C1 || a == b) return a;
    if (s == C0) return b;
    if (a == C1 && b == C0) return s;
    if (a == C0 && b == C1) return g_not(g, s);
    if (a == C0) return g_and(g, g_not(g, s), b);
    if (b == C0) return g_and(g, s, a);
    if (a == C1) return g_or(g, s, b);
    if (b == C1) return g_or(g, g_not(g, s), a);
    return g_make(g, G_MUX, a, b, s);
}

/* -- vectors -------------------------------------------------------------- */

typedef struct { int *b; int n; } vec_t;

static vec_t vnew(int n) {
    vec_t v;
    v.n = n;
    v.b = zf_alloc(sizeof(int) * (size_t)(n ? n : 1));
    return v;                      /* all C0 (0) */
}

static vec_t vconst(uint64_t val, int w) {
    vec_t r = vnew(w);
    int i;
    for (i = 0; i < w && i < 64; i++) r.b[i] = (int)((val >> i) & 1);
    return r;
}

static int vbool(vec_t v) {
    int r = C0, i;
    for (i = 0; i < v.n; i++) r = g_or(G, r, v.b[i]);
    return r;
}

/* -- carry chains --------------------------------------------------------- */

static int all_const(vec_t v) {
    int i;
    for (i = 0; i < v.n; i++) if (v.b[i] > C1) return 0;
    return 1;
}

static uint64_t const_of(vec_t v) {
    uint64_t r = 0;
    int i;
    for (i = 0; i < v.n && i < 64; i++) if (v.b[i] == C1) r |= (uint64_t)1 << i;
    return r;
}

/* x + y or x - y at width n; *cout gets the carry out (for - it is 1
 * when x >= y). */
static vec_t chain(vec_t x, vec_t y, int sub, int *cout) {
    chain_t2 *c;
    vec_t r = vnew(x.n);
    int i, ci;
    if (all_const(x) && all_const(y) && x.n <= 63) {
        uint64_t a = const_of(x), b = const_of(y), m = ((uint64_t)1 << x.n) - 1, s;
        s = sub ? a + (~b & m) + 1 : a + b;
        if (cout) *cout = (int)((s >> x.n) & 1);
        return vconst(s & m, x.n);
    }
    if (!sub && all_const(y) && const_of(y) == 0 && !cout) return x;
    if (G->n_ch == G->cap_ch) {
        int cap = G->cap_ch ? G->cap_ch * 2 : 8;
        chain_t2 *nc = zf_alloc(sizeof(chain_t2) * (size_t)cap);
        if (G->n_ch) zf_memcpy(nc, G->ch, sizeof(chain_t2) * (size_t)G->n_ch);
        G->ch = nc;
        G->cap_ch = cap;
    }
    ci = G->n_ch++;
    c = &G->ch[ci];
    c->n = x.n;
    c->sub = sub;
    c->x = x.b;
    c->y = y.b;
    c->cin = sub ? C1 : C0;
    c->cout_used = cout != NULL;
    for (i = 0; i < x.n; i++) {
        r.b[i] = g_make(G, G_SUM, ci, i, 0);
    }
    if (cout) *cout = g_make(G, G_COUT, ci, 0, 0);
    return r;
}

/* -- signals ------------------------------------------------------------------
 * For each signal bit, what drives it: an assign, a combinational always,
 * a flip-flop, an input, or nothing yet. */

enum { D_NONE, D_ASSIGN, D_COMB, D_FF, D_IN };

typedef struct {
    int *val;           /* gate per bit, -1 until known */
    uint8_t *drv;       /* D_* per bit */
    int *drv_idx;       /* the assign or always */
} sstate_t;

static sstate_t *S;
static uint8_t *assign_state, *always_state;    /* 0 new, 1 running, 2 done */

/* The block (or function call) being executed, for reads of the
 * variables it assigns. */
typedef struct {
    int n;
    int *sig, *bit;     /* its targets */
    int *val;           /* current value, -1 unassigned */
    int clocked;
    /* Clocked blocks: whether this path has written the target with a
     * BLOCKING assignment -- then a read sees the new value; after a
     * non-blocking one, or none, it sees the old. Always allocated with
     * val, one int per target, so env_copy() can copy both. */
    int *blk;
} env_t;

static env_t *cur_env;

/* -- loop variables ---------------------------------------------------------
 * A for loop unrolls: its variable is a constant for each pass, visible
 * to const_eval() -- so `a[i]` is a constant select -- and to eval(). */

typedef struct { const char *name; uint64_t value; } lvar_t;
static lvar_t lvars[32];
static int n_lvars;

int loopvar_find(const char *name, uint64_t *value) {
    int i;
    for (i = n_lvars - 1; i >= 0; i--)
        if (zf_streq(lvars[i].name, name)) { *value = lvars[i].value; return 1; }
    return 0;
}

#define LOOP_MAX 65536

static vec_t ev(const expr_t *e, int w, int sgn);
static vec_t eval(const expr_t *e, int w) { return ev(e, w, expr_signed(M, e)); }
static void exec(const stmt_t *st, env_t *E);
static int sigbit(int s, int i, const expr_t *where);

/* v at width w: truncated, or extended with zeros or, signed, its top bit */
static vec_t vext(vec_t v, int w, int sgn) {
    vec_t r = vnew(w);
    int i, fill = (sgn && v.n) ? v.b[v.n - 1] : C0;
    for (i = 0; i < w; i++) r.b[i] = i < v.n ? v.b[i] : fill;
    return r;
}

static int env_find(const env_t *E, int s, int i) {
    int k;
    for (k = 0; k < E->n; k++) if (E->sig[k] == s && E->bit[k] == i) return k;
    return -1;
}

/* offset in our LSB-first vector of Verilog index idx */
static int offset(const sig_t *s, int64_t idx, const expr_t *where) {
    int64_t o = s->msb >= s->lsb ? idx - s->lsb : s->lsb - idx;
    if (o < 0 || o >= s->width)
        EFAIL(where, "index %d is outside %s[%d:%d]", (int)idx, s->name, s->msb, s->lsb);
    return (int)o;
}

static void exec_assign(int a);
static void exec_comb(int b);

static int sigbit(int s, int i, const expr_t *where) {
    const sig_t *sg = &M->sigs[s];
    if (cur_env) {
        int k = env_find(cur_env, s, i);
        if (k >= 0) {
            if (cur_env->clocked) {
                /* the new value after `=`, the old after `<=` or nothing */
                if (cur_env->blk && cur_env->blk[k]) return cur_env->val[k];
                return S[s].val[i];
            }
            if (cur_env->val[k] < 0)
                EFAIL(where, "'%s' is read before it is assigned", sg->name);
            return cur_env->val[k];
        }
    }
    if (sg->kind == SIG_FUNC) EFAIL(where, "'%s' belongs to a function and is read outside it", sg->name);
    if (sg->kind == SIG_INT) EFAIL(where, "integer '%s' is only supported as a for-loop variable", sg->name);
    if (S[s].val[i] >= 0) return S[s].val[i];
    switch (S[s].drv[i]) {
    case D_ASSIGN: exec_assign(S[s].drv_idx[i]); break;
    case D_COMB: exec_comb(S[s].drv_idx[i]); break;
    default: {
        /* Nothing anywhere drives it: legal Verilog -- a stub's unused
         * output floats -- so 0, once per signal, said so. (yosys gives
         * x.) A bit assigned on some paths only is still a latch. */
        int k, any = 0;
        for (k = 0; k < sg->width; k++) {
            if (S[s].drv[k] != D_NONE || S[s].val[k] >= 0) continue;
            S[s].val[k] = C0;
            any = 1;
        }
        if (any) zf_note("%s:%d: '%s' is never assigned (not all of it): taken as 0",
            sg->file ? sg->file : M->file, sg->line, sg->name);
        return S[s].val[i];
    }
    }
    if (S[s].val[i] < 0) EFAIL(where, "'%s' bit %d is never assigned", sg->name, i);
    return S[s].val[i];
}

static vec_t sigvec(int s, const expr_t *where) {
    vec_t v = vnew(M->sigs[s].width);
    int i;
    for (i = 0; i < v.n; i++) v.b[i] = sigbit(s, i, where);
    return v;
}

/* -- assignment targets ------------------------------------------------------- */

typedef struct { int *sig, *bit, n; } lv_t;

static void lv_add(lv_t *l, int s, int b) {
    int *ns = zf_alloc(sizeof(int) * (size_t)(l->n + 1)), *nb = zf_alloc(sizeof(int) * (size_t)(l->n + 1));
    if (l->n) { zf_memcpy(ns, l->sig, sizeof(int) * (size_t)l->n); zf_memcpy(nb, l->bit, sizeof(int) * (size_t)l->n); }
    ns[l->n] = s; nb[l->n] = b;
    l->sig = ns; l->bit = nb;
    l->n++;
}

/* A memory word by constant index: its signal. */
static int mem_word(int mi, const expr_t *idx, const expr_t *where) {
    const mem_t *mm = &M->mems[mi];
    int64_t k = (int64_t)const_eval(M, idx);
    if (k < mm->lo || k > mm->hi)
        EFAIL(where, "index %d is outside memory %s[%d:%d]", (int)k, mm->name, mm->lo, mm->hi);
    return mm->sig0 + (int)(k - mm->lo);
}

/* The signal an lvalue's base names: a signal, or a memory word by
 * constant index. *mem_var gets the memory for a variable index. */
static int lv_base(const expr_t *e, int *mem_var) {
    int s, mi;
    *mem_var = -1;
    if (e->kind == E_ID) {
        s = sig_find(M, e->name);
        if (s >= 0) return s;
        if (mem_find(M, e->name) >= 0) EFAIL(e, "memory '%s' assigned without an index", e->name);
        EFAIL(e, "'%s' is not declared", e->name);
    }
    if (e->kind == E_INDEX && e->a->kind == E_ID && (mi = mem_find(M, e->a->name)) >= 0) {
        if (const_is(M, e->b)) return mem_word(mi, e->b, e);
        *mem_var = mi;
        return -1;
    }
    EFAIL(e, "not something that can be assigned");
}

/* The bits an assignment target names, LSB first. A memory write with a
 * variable index names every word (all_words); exec() muxes it. */
static void lvalue_bits(const expr_t *e, lv_t *l) {
    int s, i, mv;
    if (e->kind == E_CONCAT) {
        for (i = e->n - 1; i >= 0; i--) lvalue_bits(e->list[i], l);     /* {a, b}: b low */
        return;
    }
    if (e->kind == E_ID) {
        s = lv_base(e, &mv);
        for (i = 0; i < M->sigs[s].width; i++) lv_add(l, s, i);
        return;
    }
    if (e->kind == E_INDEX) {
        int wm = e->a->kind == E_ID ? mem_find(M, e->a->name) : -1;
        if (wm >= 0) {
            s = lv_base(e, &mv);
            if (s < 0) {
                const mem_t *mm = &M->mems[mv];
                int k;
                for (k = 0; k < mm->depth; k++)
                    for (i = 0; i < M->sigs[mm->sig0 + k].width; i++) lv_add(l, mm->sig0 + k, i);
                return;
            }
            for (i = 0; i < M->sigs[s].width; i++) lv_add(l, s, i);
            return;
        }
        s = lv_base(e->a, &mv);
        if (s < 0) EFAIL(e, "a bit of a memory word by variable index cannot be assigned");
        if (!const_is(M, e->b)) {
            /* x[i] = v by variable i names every bit of x; exec() muxes */
            for (i = 0; i < M->sigs[s].width; i++) lv_add(l, s, i);
            return;
        }
        lv_add(l, s, offset(&M->sigs[s], (int64_t)const_eval(M, e->b), e));
        return;
    }
    if (e->kind == E_RANGE) {
        int a, b, lo, hi, hh, ll;
        s = lv_base(e->a, &mv);
        if (s < 0) EFAIL(e, "a part of a memory word by variable index cannot be assigned");
        if (e->op != RANGE_FIXED && !const_is(M, e->b)) {
            /* x[s +: n] = v by variable s names every bit; exec() muxes */
            for (i = 0; i < M->sigs[s].width; i++) lv_add(l, s, i);
            return;
        }
        range_bounds(M, e, &hh, &ll);
        a = offset(&M->sigs[s], hh, e);
        b = offset(&M->sigs[s], ll, e);
        lo = a < b ? a : b; hi = a < b ? b : a;
        for (i = lo; i <= hi; i++) lv_add(l, s, i);
        return;
    }
    EFAIL(e, "not something that can be assigned");
}

static void exec_assign(int a) {
    const assign_t *as = &M->assigns[a];
    lv_t l;
    vec_t v;
    int i;
    env_t *saved = cur_env;
    if (assign_state[a] == 1) zf_fatal_at(as->lhs->file ? as->lhs->file : M->file, as->line,
        "combinational loop through this assign");
    if (assign_state[a] == 2) return;
    assign_state[a] = 1;
    zf_memset(&l, 0, sizeof(l));
    lvalue_bits(as->lhs, &l);
    cur_env = NULL;
    v = eval(as->rhs, l.n);
    cur_env = saved;
    for (i = 0; i < l.n; i++) S[l.sig[i]].val[l.bit[i]] = v.b[i];
    assign_state[a] = 2;
}

/* -- expressions ----------------------------------------------------------- */

/* mux tree on the bits of sel: v[sel] (0 past the end).
 *
 * The two halves are built in a fixed order, into locals: C leaves the
 * order of function arguments unspecified, GCC on x86 and on RV32 take
 * them in opposite orders, and the device then numbered gates, and so
 * wrote its .zl, differently from the host (sec. 19.4). Every call below
 * that builds gates in two arguments is sequenced the same way. */
static int select_tree(vec_t v, vec_t sel, int base, int level) {
    int lo, hi;
    if (level < 0) return base < v.n ? v.b[base] : C0;
    if (base >= v.n) return C0;
    lo = select_tree(v, sel, base, level - 1);
    hi = select_tree(v, sel, base + (1 << level), level - 1);
    return g_mux(G, sel.b[level], hi, lo);
}

/* Only the low bits that can address v take part in the tree; any
 * higher bit set means the index is past the end, and the result 0. A
 * 32-bit index used to take the tree 32 levels deep and overflow
 * `base + (1 << level)` (found by tests/rtlcheck.sh on audio_spdif.v). */
static int select_bit(vec_t v, vec_t sel, int base, int level) {
    int need = 0, over = C0, i, r;
    (void)base; (void)level;
    while (need < 30 && (1 << need) < v.n) need++;
    if (sel.n <= need) return select_tree(v, sel, 0, sel.n - 1);
    for (i = need; i < sel.n; i++) over = g_or(G, over, sel.b[i]);
    r = select_tree(v, sel, 0, need - 1);
    return g_and(G, g_not(G, over), r);
}

/* sel == k, for a constant k */
static int equals_const(vec_t sel, int64_t k) {
    int eq = C1, i;
    if (k < 0) return C0;
    for (i = 0; i < sel.n; i++) {
        int bit = i < 63 ? (int)((k >> i) & 1) : 0;
        eq = g_and(G, eq, bit ? sel.b[i] : g_not(G, sel.b[i]));
    }
    if (sel.n < 63 && (k >> sel.n)) return C0;
    return eq;
}

static vec_t shift_vec(vec_t a, vec_t s, int left, int w, int fill);

/* a shifted by `amount` (constant or not), filling with `fill` */
static vec_t shift(vec_t a, const expr_t *amount, int left, int w, int fill) {
    vec_t r = vnew(w);
    int i;
    if (!const_is(M, amount)) return shift_vec(a, eval(amount, self_width(M, amount)), left, w, fill);
    {
        uint64_t k = const_eval(M, amount);
        for (i = 0; i < w; i++) {
            int64_t src = left ? (int64_t)i - (int64_t)k : (int64_t)i + (int64_t)k;
            if (k > 4096) src = -1;
            r.b[i] = (src >= 0 && src < a.n) ? a.b[src] : (left ? C0 : fill);
        }
        return r;
    }
}

/* a barrel shifter: one mux layer per bit of the amount */
static vec_t shift_vec(vec_t a, vec_t s, int left, int w, int fill) {
    {
        vec_t cur = vext(a, a.n > w ? a.n : w, 0);
        int k, i;
        if (!left && a.n < cur.n) for (i = a.n; i < cur.n; i++) cur.b[i] = fill;
        for (k = 0; k < s.n; k++) {
            vec_t nx = vnew(cur.n);
            int64_t d = k < 20 ? ((int64_t)1 << k) : cur.n + 1;
            for (i = 0; i < cur.n; i++) {
                int64_t src = left ? i - d : i + d;
                int moved = (src >= 0 && src < cur.n) ? cur.b[src] : (left ? C0 : fill);
                nx.b[i] = g_mux(G, s.b[k], moved, cur.b[i]);
            }
            cur = nx;
        }
        return vext(cur, w, 0);
    }
}

/* x * y at width w, shift and add on carry chains: the low w bits of a
 * product are the same signed or unsigned once both are extended to w. */
static vec_t mult(vec_t x, vec_t y, int w) {
    vec_t acc = vconst(0, w);
    int i, j, zx = 0, zy = 0;
    for (i = 0; i < w; i++) { zx += x.b[i] == C0; zy += y.b[i] == C0; }
    if (zx > zy) { vec_t t = x; x = y; y = t; }         /* the sparser one drives the rows */
    for (i = 0; i < w; i++) {
        vec_t pp;
        if (y.b[i] == C0) continue;
        pp = vnew(w);
        for (j = 0; j < w; j++) {
            int src = j - i;
            pp.b[j] = src >= 0 ? g_and(G, x.b[src], y.b[i]) : C0;
        }
        acc = chain(acc, pp, 0, NULL);
    }
    return acc;
}

static int call_depth;

/* A function call: its body run like a small always @(*), inputs bound. */
static vec_t call_function(int fi, const expr_t *call) {
    func_t *f = &M->funcs[fi];
    env_t FE, *saved = cur_env;
    vec_t r;
    int k, i, n = 0;
    if (f->active) EFAIL(call, "function %s calls itself: recursion is not supported", f->name);
    if (call->n != f->n_inputs)
        EFAIL(call, "function %s takes %d arguments, not %d", f->name, f->n_inputs, call->n);
    zf_memset(&FE, 0, sizeof(FE));
    {
        int total = M->sigs[f->result].width;
        for (k = 0; k < f->n_inputs; k++) total += M->sigs[f->inputs[k]].width;
        for (k = 0; k < f->n_locals; k++) total += M->sigs[f->locals[k]].width;
        FE.sig = zf_alloc(sizeof(int) * (size_t)(total + 1));
        FE.bit = zf_alloc(sizeof(int) * (size_t)(total + 1));
        FE.val = zf_alloc(sizeof(int) * (size_t)(total + 1));
    }
    for (k = 0; k < f->n_inputs; k++) {
        int s = f->inputs[k];
        /* an argument is assigned to its input: evaluated at the input's width */
        vec_t a = eval(call->list[k], M->sigs[s].width);
        for (i = 0; i < M->sigs[s].width; i++) { FE.sig[n] = s; FE.bit[n] = i; FE.val[n] = a.b[i]; n++; }
    }
    for (i = 0; i < M->sigs[f->result].width; i++) { FE.sig[n] = f->result; FE.bit[n] = i; FE.val[n] = -1; n++; }
    for (k = 0; k < f->n_locals; k++)
        for (i = 0; i < M->sigs[f->locals[k]].width; i++) {
            FE.sig[n] = f->locals[k]; FE.bit[n] = i; FE.val[n] = -1; n++;
        }
    FE.n = n;
    if (++call_depth > 64) EFAIL(call, "functions nested more than 64 deep");
    f->active = 1;
    cur_env = &FE;
    exec(f->body, &FE);
    cur_env = saved;
    f->active = 0;
    call_depth--;
    r = vnew(M->sigs[f->result].width);
    for (i = 0; i < r.n; i++) {
        int k2 = env_find(&FE, f->result, i);
        if (FE.val[k2] < 0) EFAIL(call, "function %s does not assign its result on every path", f->name);
        r.b[i] = FE.val[k2];
    }
    return r;
}

/* The value of a select's base as a vector with its own msb/lsb: a
 * signal, a memory word, or a constant. */
static vec_t base_vec(const expr_t *a, int *msb, int *lsb, int *ok) {
    int s, mi;
    *ok = 1;
    if (a->kind == E_ID && (s = sig_find(M, a->name)) >= 0) {
        *msb = M->sigs[s].msb; *lsb = M->sigs[s].lsb;
        return sigvec(s, a);
    }
    if (a->kind == E_INDEX && a->a->kind == E_ID && (mi = mem_find(M, a->a->name)) >= 0) {
        const sig_t *w = &M->sigs[M->mems[mi].sig0];
        *msb = w->msb; *lsb = w->lsb;
        return ev(a, w->width, 0);
    }
    *ok = 0;
    return vnew(0);
}

static vec_t ev(const expr_t *e, int w, int sgn) {
    int i;
    switch (e->kind) {
    case E_NUM:
        return vext(vconst(e->value, e->width ? e->width : 32), w, sgn);
    case E_ID: {
        int s, p;
        uint64_t lv;
        if (loopvar_find(e->name, &lv)) return vext(vconst(lv, 32), w, sgn);
        s = sig_find(M, e->name);
        if (s >= 0) return vext(sigvec(s, e), w, sgn);
        p = param_find(M, e->name);
        if (p >= 0) return vext(vconst(M->params[p].value, M->params[p].width ? M->params[p].width : 32), w, sgn);
        if (mem_find(M, e->name) >= 0) EFAIL(e, "memory '%s' used without an index", e->name);
        EFAIL(e, "'%s' is not declared", e->name);
    }
    case E_INDEX: {
        int mi = e->a->kind == E_ID ? mem_find(M, e->a->name) : -1;
        if (mi >= 0) {
            /* a memory word: by constant index, a signal; otherwise a mux
             * across the words, bit by bit */
            const mem_t *mm = &M->mems[mi];
            const sig_t *w0 = &M->sigs[mm->sig0];
            vec_t sel, r;
            int b, k;
            if (const_is(M, e->b)) return vext(sigvec(mem_word(mi, e->b, e), e), w, sgn);
            sel = eval(e->b, self_width(M, e->b));
            r = vnew(w0->width);
            for (b = 0; b < w0->width; b++) {
                vec_t col = vnew(mm->hi + 1);
                for (k = mm->lo; k <= mm->hi; k++) col.b[k] = sigbit(mm->sig0 + (k - mm->lo), b, e);
                r.b[b] = select_bit(col, sel, 0, sel.n - 1);
            }
            return vext(r, w, sgn);
        }
        if (const_is(M, e)) return vext(vconst(const_eval(M, e), 1), w, 0);
        {
            int msb, lsb, ok;
            vec_t bv = base_vec(e->a, &msb, &lsb, &ok), r = vnew(w);
            if (!ok) EFAIL(e, "only a signal, a memory word or a parameter can be indexed");
            if (const_is(M, e->b)) {
                int64_t k = (int64_t)const_eval(M, e->b), o = msb >= lsb ? k - lsb : lsb - k;
                if (o < 0 || o >= bv.n) EFAIL(e, "index %d is outside [%d:%d]", (int)k, msb, lsb);
                if (w) r.b[0] = bv.b[o];
            } else {
                vec_t sel = eval(e->b, self_width(M, e->b)), col;
                int k;
                if (msb < lsb) EFAIL(e, "a variable index needs a [high:low] vector");
                col = vnew(msb + 1);
                for (k = lsb; k <= msb; k++) col.b[k] = bv.b[k - lsb];
                if (w) r.b[0] = select_bit(col, sel, 0, sel.n - 1);
            }
            return r;
        }
    }
    case E_RANGE: {
        int msb, lsb, ok, a, b, lo, hi;
        vec_t bv, r;
        if (const_is(M, e)) return vext(vconst(const_eval(M, e), self_width(M, e)), w, 0);
        bv = base_vec(e->a, &msb, &lsb, &ok);
        if (!ok) EFAIL(e, "only a signal, a memory word or a parameter can be part-selected");
        if (e->op != RANGE_FIXED && !const_is(M, e->b)) {
            /* [s +: n] by variable s. The vector gets P zeros below its
             * bit 0 -- P = lsb for +:, lsb + n - 1 for -: -- so that the
             * window's lowest bit is at position s of the padded vector
             * whatever s is; then shift right by s and keep n bits. No
             * subtraction: s - (n - 1) used to wrap for s < n - 1 and
             * zero even the bits that were in range. Bits outside the
             * vector read as 0, a choice Verilog allows (they are x). */
            int n = (int)const_eval(M, e->c), pad, k2;
            vec_t s = eval(e->b, self_width(M, e->b)), pv;
            if (msb < lsb) EFAIL(e, "a variable part-select needs a [high:low] vector");
            pad = lsb + (e->op == RANGE_DOWN ? n - 1 : 0);
            pv = vnew(bv.n + pad);
            for (k2 = 0; k2 < bv.n; k2++) pv.b[pad + k2] = bv.b[k2];
            return vext(shift_vec(pv, s, 0, n, C0), w, 0);
        }
        {
            int hh, ll;
            int64_t h, l;
            range_bounds(M, e, &hh, &ll);
            h = hh; l = ll;
            int64_t oh = msb >= lsb ? h - lsb : lsb - h, ol = msb >= lsb ? l - lsb : lsb - l;
            if (oh < 0 || oh >= bv.n || ol < 0 || ol >= bv.n)
                EFAIL(e, "[%d:%d] is outside [%d:%d]", (int)h, (int)l, msb, lsb);
            a = (int)oh; b = (int)ol;
        }
        lo = a < b ? a : b; hi = a < b ? b : a;
        r = vnew(hi - lo + 1);
        for (i = lo; i <= hi; i++) r.b[i - lo] = bv.b[i];
        return vext(r, w, 0);
    }
    case E_CONCAT:
    case E_REPL: {
        int total = self_width(M, e), pos = 0, rep = 1, k;
        vec_t r = vnew(total);
        if (e->kind == E_REPL) rep = (int)const_eval(M, e->a);
        for (k = 0; k < rep; k++)
            for (i = e->n - 1; i >= 0; i--) {
                int sw = self_width(M, e->list[i]), j;
                vec_t v = eval(e->list[i], sw);
                for (j = 0; j < sw; j++) r.b[pos++] = v.b[j];
            }
        return vext(r, w, 0);
    }
    case E_CALL: {
        int f;
        if (zf_streq(e->name, "$clog2")) return vext(vconst(const_eval(M, e), 32), w, sgn);
        if (zf_streq(e->name, "$signed") || zf_streq(e->name, "$unsigned")) {
            const expr_t *x;
            if (e->n != 1) EFAIL(e, "%s takes one argument", e->name);
            x = e->list[0];
            return vext(ev(x, self_width(M, x), expr_signed(M, x)), w, sgn);
        }
        f = func_find(M, e->name);
        if (f < 0) EFAIL(e, "'%s' is not a function%s", e->name, e->name[0] == '$' ? " zfpga knows" : "");
        return vext(call_function(f, e), w, sgn);
    }
    case E_UNARY: {
        vec_t r;
        switch (e->op) {
        case OP_NOT:
            r = ev(e->a, w, sgn);
            for (i = 0; i < w; i++) r.b[i] = g_not(G, r.b[i]);
            return r;
        case OP_PLUS:
            return ev(e->a, w, sgn);
        case OP_NEG: {
            vec_t y = ev(e->a, w, sgn);
            return chain(vconst(0, w), y, 1, NULL);
        }
        case OP_LNOT:
            r = vnew(w);
            if (w) r.b[0] = g_not(G, vbool(eval(e->a, self_width(M, e->a))));
            return r;
        default: {
            vec_t a = eval(e->a, self_width(M, e->a));
            int acc = (e->op == OP_RAND || e->op == OP_RNAND) ? C1 : C0;
            r = vnew(w);
            for (i = 0; i < a.n; i++) {
                if (e->op == OP_RAND || e->op == OP_RNAND) acc = g_and(G, acc, a.b[i]);
                else if (e->op == OP_ROR || e->op == OP_RNOR) acc = g_or(G, acc, a.b[i]);
                else acc = g_xor(G, acc, a.b[i]);
            }
            if (e->op == OP_RNAND || e->op == OP_RNOR || e->op == OP_RXNOR) acc = g_not(G, acc);
            if (w) r.b[0] = acc;
            return r;
        }
        }
    }
    case E_TERNARY: {
        int c = vbool(eval(e->a, self_width(M, e->a)));
        vec_t a = ev(e->b, w, sgn);
        vec_t b = ev(e->c, w, sgn);
        vec_t r = vnew(w);
        for (i = 0; i < w; i++) r.b[i] = g_mux(G, c, a.b[i], b.b[i]);
        return r;
    }
    case E_BINARY:
        switch (e->op) {
        case OP_AND: case OP_OR: case OP_XOR: case OP_XNOR: {
            vec_t a = ev(e->a, w, sgn);
            vec_t b = ev(e->b, w, sgn);
            vec_t r = vnew(w);
            for (i = 0; i < w; i++) {
                switch (e->op) {
                case OP_AND: r.b[i] = g_and(G, a.b[i], b.b[i]); break;
                case OP_OR: r.b[i] = g_or(G, a.b[i], b.b[i]); break;
                case OP_XOR: r.b[i] = g_xor(G, a.b[i], b.b[i]); break;
                default: r.b[i] = g_not(G, g_xor(G, a.b[i], b.b[i])); break;
                }
            }
            return r;
        }
        case OP_ADD:
        case OP_SUB: {
            vec_t x = ev(e->a, w, sgn);         /* sequenced: see select_bit() */
            vec_t y = ev(e->b, w, sgn);
            return chain(x, y, e->op == OP_SUB, NULL);
        }
        case OP_MUL: {
            vec_t x, y;
            if (const_is(M, e)) return vext(vconst(const_eval(M, e), self_width(M, e)), w, sgn);
            x = ev(e->a, w, sgn);
            y = ev(e->b, w, sgn);
            return mult(x, y, w);
        }
        case OP_DIV: case OP_MOD:
            if (const_is(M, e)) return vext(vconst(const_eval(M, e), self_width(M, e)), w, sgn);
            EFAIL(e, "/ and %% are not supported yet (except between constants)");
        case OP_SHL:
            return shift(ev(e->a, w, sgn), e->b, 1, w, C0);
        case OP_SHR:
        case OP_ASHR: {
            int sw = self_width(M, e->a), cw = sw > w ? sw : w;
            vec_t a = ev(e->a, cw, sgn);
            int fill = (e->op == OP_ASHR && sgn && a.n) ? a.b[a.n - 1] : C0;
            return shift(a, e->b, 0, w, fill);
        }
        case OP_LAND: case OP_LOR: {
            int a = vbool(eval(e->a, self_width(M, e->a)));
            int b = vbool(eval(e->b, self_width(M, e->b)));
            vec_t r = vnew(w);
            if (w) r.b[0] = e->op == OP_LAND ? g_and(G, a, b) : g_or(G, a, b);
            return r;
        }
        default: {
            /* comparisons, at the wider operand's width; signed only if
             * both are, and then by flipping the top bits and comparing
             * unsigned */
            int sa = self_width(M, e->a), sb = self_width(M, e->b), cw = sa > sb ? sa : sb, bit;
            int cs = expr_signed(M, e->a) && expr_signed(M, e->b);
            vec_t a = ev(e->a, cw, cs);
            vec_t b = ev(e->b, cw, cs);
            vec_t r = vnew(w);
            if (e->op == OP_EQ || e->op == OP_NE) {
                bit = C1;
                for (i = 0; i < cw; i++) bit = g_and(G, bit, g_not(G, g_xor(G, a.b[i], b.b[i])));
                if (e->op == OP_NE) bit = g_not(G, bit);
            } else {
                int co;
                if (cs && cw) {
                    a.b[cw - 1] = g_not(G, a.b[cw - 1]);
                    b.b[cw - 1] = g_not(G, b.b[cw - 1]);
                }
                /* x - y carries out when x >= y */
                if (e->op == OP_LT || e->op == OP_GE) (void)chain(a, b, 1, &co);
                else (void)chain(b, a, 1, &co);
                bit = (e->op == OP_GE || e->op == OP_LE) ? co : g_not(G, co);
            }
            if (w) r.b[0] = bit;
            return r;
        }
        }
    }
    EFAIL(e, "unsupported expression");
}

/* -- statements, symbolically ---------------------------------------------- */

static void targets(const stmt_t *st, env_t *E);

/* for (v = init; cond; v = step): calls body(arg) with v bound, each pass */
static void unroll(const stmt_t *st, void (*body)(const stmt_t *, env_t *), env_t *E) {
    uint64_t v;
    int n = 0, idx;
    int s = sig_find(M, st->var);
    if (s < 0 || M->sigs[s].kind != SIG_INT)
        SFAIL(st, "for loop variable '%s' must be declared integer", st->var);
    if (n_lvars == 32) SFAIL(st, "for loops nested more than 32 deep");
    v = const_eval(M, st->init);
    idx = n_lvars++;
    lvars[idx].name = st->var;
    for (;;) {
        lvars[idx].value = (uint64_t)(int64_t)(int32_t)v;   /* an integer: 32-bit signed */
        if (!const_eval(M, st->cond)) break;
        if (++n > LOOP_MAX) SFAIL(st, "for loop runs more than %d times", LOOP_MAX);
        body(st->then, E);
        v = const_eval(M, st->step);
    }
    n_lvars--;
}

static void targets(const stmt_t *st, env_t *E) {
    int i;
    if (!st) return;
    switch (st->kind) {
    case S_BLOCK: for (i = 0; i < st->n; i++) targets(st->list[i], E); break;
    case S_IF: targets(st->then, E); targets(st->els, E); break;
    case S_CASE: for (i = 0; i < st->n_items; i++) targets(st->items[i].body, E); break;
    case S_FOR: unroll(st, targets, E); break;
    default: {
        lv_t l;
        zf_memset(&l, 0, sizeof(l));
        lvalue_bits(st->lhs, &l);
        for (i = 0; i < l.n; i++) {
            if (env_find(E, l.sig[i], l.bit[i]) >= 0) continue;
            {
                int *ns = zf_alloc(sizeof(int) * (size_t)(E->n + 1)), *nb = zf_alloc(sizeof(int) * (size_t)(E->n + 1));
                if (E->n) { zf_memcpy(ns, E->sig, sizeof(int) * (size_t)E->n); zf_memcpy(nb, E->bit, sizeof(int) * (size_t)E->n); }
                ns[E->n] = l.sig[i]; nb[E->n] = l.bit[i];
                E->sig = ns; E->bit = nb;
                E->n++;
            }
        }
    }
    }
}

/* A snapshot of an environment: values, then blocking flags. */
static int *env_copy(const env_t *E, const int *v) {
    int *r = zf_alloc(sizeof(int) * (size_t)(2 * E->n + 1));
    zf_memcpy(r, v, sizeof(int) * (size_t)E->n);
    if (E->blk) zf_memcpy(r + E->n, E->blk, sizeof(int) * (size_t)E->n);
    return r;
}

static void env_restore(env_t *E, const int *snap) {
    zf_memcpy(E->val, snap, sizeof(int) * (size_t)E->n);
    if (E->blk) zf_memcpy(E->blk, snap + E->n, sizeof(int) * (size_t)E->n);
}

static void merge(env_t *E, int c, const int *t, const int *f, const stmt_t *where) {
    int k;
    if (E->blk) for (k = 0; k < E->n; k++) E->blk[k] = t[E->n + k] | f[E->n + k];
    for (k = 0; k < E->n; k++) {
        if (t[k] == f[k]) { E->val[k] = t[k]; continue; }
        if (t[k] < 0 || f[k] < 0)
            SFAIL(where, "'%s' is not assigned on every path through always @(*): that is a latch",
                M->sigs[E->sig[k]].name);
        E->val[k] = g_mux(G, c, t[k], f[k]);
    }
}

static void exec(const stmt_t *st, env_t *E) {
    int i, k;
    if (!st) return;
    switch (st->kind) {
    case S_BLOCK:
        for (i = 0; i < st->n; i++) exec(st->list[i], E);
        return;
    case S_FOR:
        unroll(st, exec, E);
        return;
    case S_NB:
    case S_BA: {
        lv_t l;
        vec_t v;
        if (!E->clocked && st->kind == S_NB) SFAIL(st, "use = in always @(*) and in functions");
        if (st->lhs->kind == E_INDEX && st->lhs->a->kind == E_ID && !const_is(M, st->lhs->b)) {
            int mi = mem_find(M, st->lhs->a->name);
            if (mi >= 0) {
                /* a memory write by variable index: every word, each
                 * muxed on index == its own */
                const mem_t *mm = &M->mems[mi];
                int ww = M->sigs[mm->sig0].width;
                vec_t sel = eval(st->lhs->b, self_width(M, st->lhs->b));
                v = eval(st->rhs, ww);
                for (k = 0; k < mm->depth; k++) {
                    int eq = equals_const(sel, mm->lo + k), s = mm->sig0 + k;
                    for (i = 0; i < ww; i++) {
                        int pos = env_find(E, s, i);
                        if (E->val[pos] < 0)
                            SFAIL(st, "memory %s written by variable index in always @(*): that is a latch",
                                mm->name);
                        E->val[pos] = g_mux(G, eq, v.b[i], E->val[pos]);
                        if (E->blk && st->kind == S_BA) E->blk[pos] = 1;
                    }
                }
                return;
            }
        }
        if (st->lhs->kind == E_RANGE && st->lhs->op != RANGE_FIXED && st->lhs->a->kind == E_ID &&
                !const_is(M, st->lhs->b) && sig_find(M, st->lhs->a->name) >= 0) {
            /* x[s +: n] = v by variable s: bit j takes v[k] where the
             * part's k-th bit lands on j, i.e. where s == j - k (+:) or
             * s == j + k (-:) */
            int s = sig_find(M, st->lhs->a->name), n = (int)const_eval(M, st->lhs->c);
            const sig_t *sg = &M->sigs[s];
            vec_t sel = eval(st->lhs->b, self_width(M, st->lhs->b));
            if (sg->msb < sg->lsb) SFAIL(st, "a variable part-select needs a [high:low] vector");
            v = eval(st->rhs, n);
            for (i = 0; i < sg->width; i++) {
                int pos = env_find(E, s, i), j = sg->lsb + i;
                if (E->val[pos] < 0)
                    SFAIL(st, "%s[... +: ...] assigned in always @(*) before the whole of %s is: "
                        "that is a latch", sg->name, sg->name);
                for (k = 0; k < n; k++) {
                    int at = st->lhs->op == RANGE_UP ? j - k : j + k;
                    E->val[pos] = g_mux(G, equals_const(sel, at), v.b[k], E->val[pos]);
                }
                if (E->blk && st->kind == S_BA) E->blk[pos] = 1;
            }
            return;
        }
        if (st->lhs->kind == E_INDEX && st->lhs->a->kind == E_ID && !const_is(M, st->lhs->b) &&
                sig_find(M, st->lhs->a->name) >= 0) {
            /* x[i] = v by variable i: every bit, muxed on i == its index */
            int s = sig_find(M, st->lhs->a->name);
            const sig_t *sg = &M->sigs[s];
            vec_t sel = eval(st->lhs->b, self_width(M, st->lhs->b));
            v = eval(st->rhs, 1);
            for (i = 0; i < sg->width; i++) {
                int idx = sg->msb >= sg->lsb ? sg->lsb + i : sg->lsb - i;
                int pos = env_find(E, s, i);
                if (E->val[pos] < 0)
                    SFAIL(st, "%s[...] assigned by variable index in always @(*) before the whole of "
                        "%s is: that is a latch", sg->name, sg->name);
                E->val[pos] = g_mux(G, equals_const(sel, idx), v.b[0], E->val[pos]);
                if (E->blk && st->kind == S_BA) E->blk[pos] = 1;
            }
            return;
        }
        zf_memset(&l, 0, sizeof(l));
        lvalue_bits(st->lhs, &l);
        v = eval(st->rhs, l.n);
        for (i = 0; i < l.n; i++) {
            k = env_find(E, l.sig[i], l.bit[i]);
            E->val[k] = v.b[i];
            if (E->blk && st->kind == S_BA) E->blk[k] = 1;
        }
        return;
    }
    case S_IF: {
        int c = vbool(eval(st->cond, self_width(M, st->cond)));
        int *before = env_copy(E, E->val), *t;
        exec(st->then, E);
        t = env_copy(E, E->val);
        env_restore(E, before);
        exec(st->els, E);
        merge(E, c, t, env_copy(E, E->val), st);
        return;
    }
    case S_CASE: {
        /* an if-chain: the first matching item wins, as Verilog says */
        int sw = self_width(M, st->cond), n = st->n_items;
        int *before = env_copy(E, E->val), **res = zf_alloc(sizeof(int *) * (size_t)(n + 1));
        int *conds = zf_alloc(sizeof(int) * (size_t)(n + 1)), def = -1;
        for (i = 0; i < n; i++) {
            const caseitem_t *it = &st->items[i];
            int c = C0, j;
            if (!it->vals) { def = i; conds[i] = C0; }
            for (j = 0; it->vals && j < it->n; j++) {
                int w2 = self_width(M, it->vals[j]), cw = sw > w2 ? sw : w2, eq = C1, b;
                int cs = expr_signed(M, st->cond) && expr_signed(M, it->vals[j]);
                vec_t a = ev(st->cond, cw, cs);
                vec_t bv = ev(it->vals[j], cw, cs);
                for (b = 0; b < cw; b++) eq = g_and(G, eq, g_not(G, g_xor(G, a.b[b], bv.b[b])));
                c = g_or(G, c, eq);
            }
            if (it->vals) conds[i] = c;
            env_restore(E, before);
            exec(it->body, E);
            res[i] = env_copy(E, E->val);
        }
        {
            int *acc = def >= 0 ? res[def] : before;
            for (i = n - 1; i >= 0; i--) {
                if (i == def) continue;
                env_restore(E, acc);
                merge(E, conds[i], res[i], acc, st);
                acc = env_copy(E, E->val);
            }
            env_restore(E, acc);
        }
        return;
    }
    }
}

static void exec_comb(int b) {
    const always_t *al = &M->always[b];
    env_t E, *saved = cur_env;
    int k;
    if (always_state[b] == 1) zf_fatal_at(al->file ? al->file : M->file, al->line,
        "combinational loop through this always @(*)");
    if (always_state[b] == 2) return;
    always_state[b] = 1;
    zf_memset(&E, 0, sizeof(E));
    targets(al->body, &E);
    E.val = zf_alloc(sizeof(int) * (size_t)(E.n ? E.n : 1));
    for (k = 0; k < E.n; k++) E.val[k] = -1;
    cur_env = &E;
    exec(al->body, &E);
    cur_env = saved;
    for (k = 0; k < E.n; k++) {
        if (E.val[k] < 0)
            zf_fatal_at(al->file ? al->file : M->file, al->line,
                "'%s' is not assigned on every path: that is a latch", M->sigs[E.sig[k]].name);
        S[E.sig[k]].val[E.bit[k]] = E.val[k];
    }
    always_state[b] = 2;
}

/* -- the module ----------------------------------------------------------------- */

static const char *bitname(int s, int i) {
    const sig_t *sg = &M->sigs[s];
    char buf[160];
    if (sg->width == 1) return sg->name;
    zf_fmt(buf, sizeof(buf), "%s[%d]", sg->name, sg->msb >= sg->lsb ? sg->lsb + i : sg->lsb - i);
    return zf_strdup(buf);
}

static void claim(int s, int i, int kind, int idx, int line) {
    if (S[s].drv[i] != D_NONE)
        zf_fatal_at(M->file, line, "'%s' has more than one driver", bitname(s, i));
    S[s].drv[i] = (uint8_t)kind;
    S[s].drv_idx[i] = idx;
}

void synth_elab(module_t *m, graph_t *g) {
    int s, i, a, k;
    M = m;
    G = g;
    zf_memset(g, 0, sizeof(*g));
    g->cap_g = 1024;
    g->g = zf_alloc(sizeof(gate_t) * (size_t)g->cap_g);
    grow_hash();
    g->g[0].op = G_C0;
    g->g[1].op = G_C1;
    g->n_g = 2;

    S = zf_alloc(sizeof(sstate_t) * (size_t)(m->n_sig ? m->n_sig : 1));
    for (s = 0; s < m->n_sig; s++) {
        int w = m->sigs[s].width;
        S[s].val = zf_alloc(sizeof(int) * (size_t)w);
        S[s].drv = zf_alloc((size_t)w);
        S[s].drv_idx = zf_alloc(sizeof(int) * (size_t)w);
        for (i = 0; i < w; i++) S[s].val[i] = -1;
    }
    assign_state = zf_alloc((size_t)m->n_assign + 1);
    always_state = zf_alloc((size_t)m->n_always + 1);

    /* who drives what */
    for (a = 0; a < m->n_assign; a++) {
        lv_t l;
        zf_memset(&l, 0, sizeof(l));
        lvalue_bits(m->assigns[a].lhs, &l);
        for (i = 0; i < l.n; i++) {
            if (m->sigs[l.sig[i]].kind == SIG_IN)
                zf_fatal_at(m->file, m->assigns[a].line, "input '%s' is assigned", m->sigs[l.sig[i]].name);
            claim(l.sig[i], l.bit[i], D_ASSIGN, a, m->assigns[a].line);
        }
    }
    for (a = 0; a < m->n_always; a++) {
        env_t E;
        zf_memset(&E, 0, sizeof(E));
        targets(m->always[a].body, &E);
        for (k = 0; k < E.n; k++) {
            const sig_t *sg = &m->sigs[E.sig[k]];
            if (sg->kind == SIG_IN) zf_fatal_at(m->file, m->always[a].line, "input '%s' is assigned", sg->name);
            if (sg->kind == SIG_INT)
                zf_fatal_at(m->always[a].file ? m->always[a].file : m->file, m->always[a].line,
                    "integer '%s' is assigned: integers are only supported as for-loop variables", sg->name);
            if (!sg->is_reg && sg->kind != SIG_REG)
                zf_fatal_at(m->file, m->always[a].line, "'%s' is assigned in an always block but is not a reg", sg->name);
            claim(E.sig[k], E.bit[k], m->always[a].comb ? D_COMB : D_FF, a, m->always[a].line);
        }
    }

    /* inputs */
    for (s = 0; s < m->n_sig; s++) {
        if (m->sigs[s].kind != SIG_IN) continue;
        for (i = 0; i < m->sigs[s].width; i++) {
            sio_t *o;
            if (g->n_io == g->cap_io) {
                int cap = g->cap_io ? g->cap_io * 2 : 32;
                sio_t *no = zf_alloc(sizeof(sio_t) * (size_t)cap);
                if (g->n_io) zf_memcpy(no, g->io, sizeof(sio_t) * (size_t)g->n_io);
                g->io = no;
                g->cap_io = cap;
            }
            o = &g->io[g->n_io];
            o->name = bitname(s, i);
            o->output = 0;
            o->gate = g_make(g, G_IN, g->n_io, 0, 0);
            g->n_io++;
            S[s].val[i] = o->gate;
            S[s].drv[i] = D_IN;
        }
    }

    /* flip-flops: every bit a clocked block assigns; Q first, so logic
     * can read it before its next state is known */
    for (a = 0; a < m->n_always; a++) {
        env_t E;
        if (m->always[a].comb) continue;
        zf_memset(&E, 0, sizeof(E));
        targets(m->always[a].body, &E);
        for (k = 0; k < E.n; k++) {
            sff_t *f;
            if (g->n_ff == g->cap_ff) {
                int cap = g->cap_ff ? g->cap_ff * 2 : 64;
                sff_t *nf = zf_alloc(sizeof(sff_t) * (size_t)cap);
                if (g->n_ff) zf_memcpy(nf, g->ff, sizeof(sff_t) * (size_t)g->n_ff);
                g->ff = nf;
                g->cap_ff = cap;
            }
            f = &g->ff[g->n_ff];
            zf_memset(f, 0, sizeof(*f));
            f->name = bitname(E.sig[k], E.bit[k]);
            f->ce = f->lsr = -1;
            S[E.sig[k]].val[E.bit[k]] = g_make(g, G_Q, g->n_ff, 0, 0);
            g->n_ff++;
        }
    }

    /* next state, block by block */
    for (a = 0; a < m->n_always; a++) {
        const always_t *al = &m->always[a];
        env_t E;
        const stmt_t *body = al->body, *rst_part = NULL;
        int *reset = NULL, clk, rst = -1;
        if (al->comb) { exec_comb(a); continue; }
        zf_memset(&E, 0, sizeof(E));
        targets(body, &E);
        E.clocked = 1;
        E.val = zf_alloc(sizeof(int) * (size_t)(E.n ? E.n : 1));
        E.blk = zf_alloc(sizeof(int) * (size_t)(E.n ? E.n : 1));
        {
            int cs = sig_find(m, al->clk);
            expr_t where;
            zf_memset(&where, 0, sizeof(where));
            where.line = al->line;
            if (cs < 0) zf_fatal_at(m->file, al->line, "clock '%s' is not declared", al->clk);
            clk = sigbit(cs, 0, &where);
            if (al->rst) {
                int rs = sig_find(m, al->rst);
                if (rs < 0) zf_fatal_at(m->file, al->line, "reset '%s' is not declared", al->rst);
                rst = sigbit(rs, 0, &where);
            }
        }
        if (al->rst) {
            /* the asynchronous reset: `if (rst) <constants> else <logic>`,
             * or `if (!rst)` for negedge */
            const stmt_t *b = body;
            const expr_t *c;
            while (b && b->kind == S_BLOCK && b->n == 1) b = b->list[0];
            if (!b || b->kind != S_IF)
                zf_fatal_at(m->file, al->line, "with an asynchronous reset the block must be if (%s) ... else ...", al->rst);
            c = b->cond;
            if (al->rst_neg) {
                if (!(c->kind == E_UNARY && (c->op == OP_LNOT || c->op == OP_NOT) &&
                        c->a->kind == E_ID && zf_streq(c->a->name, al->rst)))
                    zf_fatal_at(m->file, b->line, "negedge %s: test it as if (!%s)", al->rst, al->rst);
            } else if (!(c->kind == E_ID && zf_streq(c->name, al->rst))) {
                zf_fatal_at(m->file, b->line, "posedge %s: test it as if (%s)", al->rst, al->rst);
            }
            rst_part = b->then;
            body = b->els;
            for (k = 0; k < E.n; k++) { E.val[k] = S[E.sig[k]].val[E.bit[k]]; E.blk[k] = 0; }
            cur_env = &E;
            exec(rst_part, &E);
            cur_env = NULL;
            reset = env_copy(&E, E.val);
        }
        for (k = 0; k < E.n; k++) { E.val[k] = S[E.sig[k]].val[E.bit[k]]; E.blk[k] = 0; }    /* hold */
        cur_env = &E;
        exec(body, &E);
        cur_env = NULL;
        for (k = 0; k < E.n; k++) {
            int q = S[E.sig[k]].val[E.bit[k]], d = E.val[k];
            sff_t *f = &g->ff[g->g[q].a];
            const sig_t *sg = &m->sigs[E.sig[k]];
            f->clk = clk;
            /* a clock enable: next = en ? x : q */
            if (g->g[d].op == G_MUX && g->g[d].b == q) { f->ce = g->g[d].c; d = g->g[d].a; }
            else if (g->g[d].op == G_MUX && g->g[d].a == q) { f->ce = g_not(g, g->g[d].c); d = g->g[d].b; }
            else if (g->g[d].op == G_AND && (g->g[d].a == q || g->g[d].b == q)) { /* q & x: keep as logic */ }
            f->d = d;
            if (reset && reset[k] != q) {
                if (reset[k] > C1)
                    zf_fatal_at(m->file, al->line, "'%s' is reset to something that is not a constant", sg->name);
                f->lsr = rst;
                f->lsr_inv = al->rst_neg;
                f->async = 1;
                f->regset = reset[k] == C1;
            }
            if (sg->init) {
                uint64_t iv = const_eval(m, sg->init);
                int bitv = (int)((iv >> E.bit[k]) & 1);
                if (f->lsr >= 0 && bitv != f->regset)
                    zf_fatal_at(m->file, sg->line, "'%s' has an initial value and a different reset value; "
                        "the hardware has one", sg->name);
                f->regset = bitv;
            }
        }
    }

    /* outputs */
    for (s = 0; s < m->n_sig; s++) {
        expr_t where;
        if (m->sigs[s].kind != SIG_OUT) continue;
        zf_memset(&where, 0, sizeof(where));
        where.line = m->sigs[s].line;
        for (i = 0; i < m->sigs[s].width; i++) {
            sio_t *o;
            int gt = sigbit(s, i, &where);
            if (g->n_io == g->cap_io) {
                int cap = g->cap_io ? g->cap_io * 2 : 32;
                sio_t *no = zf_alloc(sizeof(sio_t) * (size_t)cap);
                if (g->n_io) zf_memcpy(no, g->io, sizeof(sio_t) * (size_t)g->n_io);
                g->io = no;
                g->cap_io = cap;
            }
            o = &g->io[g->n_io++];
            o->name = bitname(s, i);
            o->output = 1;
            o->gate = gt;
        }
    }

    /* readable names: a gate is named after the first signal bit that is it */
    g->gname = zf_alloc(sizeof(char *) * (size_t)g->n_g);
    {
        /* internal signals first, so a register keeps its own name even
         * when an output port is the same bit */
        int pass;
        for (pass = 0; pass < 2; pass++)
            for (s = 0; s < m->n_sig; s++) {
                int port = m->sigs[s].kind == SIG_IN || m->sigs[s].kind == SIG_OUT;
                if (port != pass) continue;
                for (i = 0; i < m->sigs[s].width; i++) {
                    int gt = S[s].val[i];
                    if (gt > C1 && !g->gname[gt] && g->g[gt].op != G_IN) g->gname[gt] = bitname(s, i);
                }
            }
    }
    for (i = 0; i < g->n_io; i++)
        if (!g->io[i].output) g->gname[g->io[i].gate] = g->io[i].name;
}
