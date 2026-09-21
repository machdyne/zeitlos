/*
 * zfpga -- synth, flattening: module definitions to one flat module.
 *
 * From the top down, each instance is elaborated in its own scope:
 *
 *   1. its parameters, in order, each the override its instance gives
 *      or else its default, evaluated in this scope;
 *   2. its widths and memory depths, evaluated now -- so a child's
 *      `reg [W-1:0] q` is as wide as THIS instance's W;
 *   3. its statements, cloned with every name prefixed by the instance
 *      path, `u_fifo.count`, so that two instances never share a wire;
 *   4. its ports, which become assigns: parent -> child for an input,
 *      child -> parent for an output.
 *
 * The flat module is what synth_elab.c consumed before instances existed,
 * so everything downstream -- gates, mapping, the .zl -- is unchanged.
 * Names in the .zl keep the paths: `u_uart.rx_state[1]` is findable.
 */

#include "synth.h"

static const lib_t *LIB;
static module_t *M;

#define GROW(arr, n, cap, T) do { if ((n) == (cap)) {                     \
    int nc_ = (cap) ? (cap) * 2 : 16;                                     \
    T *na_ = zf_alloc(sizeof(T) * (size_t)nc_);                           \
    if (n) zf_memcpy(na_, (arr), sizeof(T) * (size_t)(n));                \
    (arr) = na_; (cap) = nc_; } } while (0)

/* A memory bigger than this is refused rather than built from
 * flip-flops: block RAM inference is its own job (sec. 24.6). */
#define MEM_BITS_MAX 4096

/* -- lookups --------------------------------------------------------------- */

int sig_find(const module_t *m, const char *name) {
    int i;
    for (i = 0; i < m->n_sig; i++) if (zf_streq(m->sigs[i].name, name)) return i;
    return -1;
}

int param_find(const module_t *m, const char *name) {
    int i;
    for (i = 0; i < m->n_param; i++) if (zf_streq(m->params[i].name, name)) return i;
    return -1;
}

int mem_find(const module_t *m, const char *name) {
    int i;
    for (i = 0; i < m->n_mem; i++) if (zf_streq(m->mems[i].name, name)) return i;
    return -1;
}

int func_find(const module_t *m, const char *name) {
    int i;
    for (i = 0; i < m->n_func; i++) if (zf_streq(m->funcs[i].name, name)) return i;
    return -1;
}

#define EFAIL(e, ...) zf_fatal_at((e)->file ? (e)->file : M->file, (e)->line, __VA_ARGS__)

/* -- constants --------------------------------------------------------------- */

static int clog2(uint64_t v) {
    int r = 0;
    uint64_t x = 1;
    while (x < v && r < 64) { x <<= 1; r++; }
    return r;
}

/* the bits [hi:lo] of a constant, as Verilog indexes it */
static uint64_t bits_of(uint64_t v, int hi, int lo) {
    int w = hi - lo + 1;
    v >>= lo;
    return w >= 64 ? v : (v & (((uint64_t)1 << w) - 1));
}

/* The indices a constant range names: [h:l] as written; [s +: w] is
 * [s+w-1 : s]; [s -: w] is [s : s-w+1]. (For an ascending declaration
 * the first index is still the one nearer the msb: offset() sorts it.) */
void range_bounds(const module_t *m, const expr_t *e, int *hi, int *lo) {
    int64_t s = (int64_t)const_eval(m, e->b), w;
    if (e->op == RANGE_FIXED) {
        *hi = (int)s;
        *lo = (int)(int64_t)const_eval(m, e->c);
        return;
    }
    w = (int64_t)const_eval(m, e->c);
    if (w < 1) EFAIL(e, "a part-select must be at least one bit wide");
    if (e->op == RANGE_UP) { *lo = (int)s; *hi = (int)(s + w - 1); }
    else { *hi = (int)s; *lo = (int)(s - w + 1); }
}

/* A select of a parameter outside the width it has: Verilog makes those
 * bits undefined, so rather than pick a value, refuse. */
static void param_bounds(const module_t *m, const expr_t *base, int hi, int lo, const expr_t *where) {
    int p, w;
    if (base->kind != E_ID || (p = param_find(m, base->name)) < 0) return;
    w = m->params[p].width ? m->params[p].width : 32;
    if (lo < 0 || hi >= w)
        EFAIL(where, "[%d:%d] is outside parameter %s, which is %d bits", hi, lo, base->name, w);
}

int const_is(const module_t *m, const expr_t *e) {
    int i;
    uint64_t lv;
    switch (e->kind) {
    case E_NUM: return 1;
    case E_ID: return param_find(m, e->name) >= 0 || loopvar_find(e->name, &lv);
    case E_CALL:
        if (zf_streq(e->name, "$clog2") || zf_streq(e->name, "$signed") || zf_streq(e->name, "$unsigned"))
            return e->n == 1 && const_is(m, e->list[0]);
        return 0;
    case E_UNARY: return const_is(m, e->a);
    case E_BINARY: return const_is(m, e->a) && const_is(m, e->b);
    case E_TERNARY: return const_is(m, e->a) && const_is(m, e->b) && const_is(m, e->c);
    case E_INDEX: return const_is(m, e->a) && const_is(m, e->b);
    case E_RANGE: return const_is(m, e->a) && const_is(m, e->b) && const_is(m, e->c);
    case E_CONCAT:
        for (i = 0; i < e->n; i++) if (!const_is(m, e->list[i])) return 0;
        return 1;
    case E_REPL:
        if (!const_is(m, e->a)) return 0;
        for (i = 0; i < e->n; i++) if (!const_is(m, e->list[i])) return 0;
        return 1;
    }
    return 0;
}

uint64_t const_eval(const module_t *m, const expr_t *e) {
    uint64_t a, b;
    int i;
    switch (e->kind) {
    case E_NUM: return e->value;
    case E_ID: {
        int p;
        uint64_t lv;
        if (loopvar_find(e->name, &lv)) return lv;
        p = param_find(m, e->name);
        if (p < 0) EFAIL(e, "'%s' is not a constant", e->name);
        return m->params[p].value;
    }
    case E_CALL:
        if (e->n != 1) EFAIL(e, "%s takes one argument", e->name);
        if (zf_streq(e->name, "$clog2")) return (uint64_t)clog2(const_eval(m, e->list[0]));
        if (zf_streq(e->name, "$signed") || zf_streq(e->name, "$unsigned")) return const_eval(m, e->list[0]);
        EFAIL(e, "%s(...) is not a constant", e->name);
    case E_TERNARY: return const_eval(m, e->a) ? const_eval(m, e->b) : const_eval(m, e->c);
    case E_INDEX: {
        /* a bit of a parameter */
        uint64_t v = const_eval(m, e->a), k = const_eval(m, e->b);
        param_bounds(m, e->a, (int)k, (int)k, e);
        return k >= 64 ? 0 : (v >> k) & 1;
    }
    case E_RANGE: {
        uint64_t v = const_eval(m, e->a);
        int hi, lo;
        range_bounds(m, e, &hi, &lo);
        if (hi < lo) { int t = hi; hi = lo; lo = t; }
        param_bounds(m, e->a, hi, lo, e);
        return bits_of(v, hi, lo);
    }
    case E_CONCAT:
        for (a = 0, i = 0; i < e->n; i++) {
            int w = self_width(m, e->list[i]);
            a = (w >= 64 ? 0 : a << w) | bits_of(const_eval(m, e->list[i]), w - 1, 0);
        }
        return a;
    case E_REPL: {
        int k, n = (int)const_eval(m, e->a);
        expr_t c = *e;
        c.kind = E_CONCAT;
        b = const_eval(m, &c);
        {
            int w = 0;
            for (i = 0; i < e->n; i++) w += self_width(m, e->list[i]);
            for (a = 0, k = 0; k < n; k++) a = (w >= 64 ? 0 : a << w) | b;
        }
        return a;
    }
    case E_UNARY:
        a = const_eval(m, e->a);
        switch (e->op) {
        case OP_NOT: return ~a;
        case OP_LNOT: return !a;
        case OP_NEG: return (uint64_t)0 - a;
        case OP_PLUS: return a;
        default: break;
        }
        break;
    case E_BINARY:
        a = const_eval(m, e->a);
        b = const_eval(m, e->b);
        switch (e->op) {
        case OP_ADD: return a + b;
        case OP_SUB: return a - b;
        case OP_MUL: return a * b;
        case OP_DIV: if (!b) EFAIL(e, "division by zero"); return a / b;
        case OP_MOD: if (!b) EFAIL(e, "division by zero"); return a % b;
        case OP_SHL: return b >= 64 ? 0 : a << b;
        case OP_SHR: case OP_ASHR: return b >= 64 ? 0 : a >> b;
        case OP_AND: return a & b;
        case OP_OR: return a | b;
        case OP_XOR: return a ^ b;
        case OP_XNOR: return ~(a ^ b);
        case OP_LT: return (int64_t)a < (int64_t)b;
        case OP_LE: return (int64_t)a <= (int64_t)b;
        case OP_GT: return (int64_t)a > (int64_t)b;
        case OP_GE: return (int64_t)a >= (int64_t)b;
        case OP_EQ: return a == b;
        case OP_NE: return a != b;
        case OP_LAND: return a && b;
        case OP_LOR: return a || b;
        default: break;
        }
        break;
    default: break;
    }
    EFAIL(e, "not a constant expression");
}

/* Verilog's self-determined width (IEEE 1364-2001 table 29). */
int self_width(const module_t *m, const expr_t *e) {
    int i, w, a, b;
    uint64_t lv;
    switch (e->kind) {
    case E_NUM: return e->width ? e->width : 32;
    case E_ID: {
        int s = sig_find(m, e->name), p;
        if (loopvar_find(e->name, &lv)) return 32;
        if (s >= 0) return m->sigs[s].width;
        p = param_find(m, e->name);
        if (p >= 0) return m->params[p].width ? m->params[p].width : 32;
        if (mem_find(m, e->name) >= 0) EFAIL(e, "memory '%s' used without an index", e->name);
        EFAIL(e, "'%s' is not declared", e->name);
    }
    case E_CALL: {
        int f;
        if (zf_streq(e->name, "$clog2")) return 32;
        if (zf_streq(e->name, "$signed") || zf_streq(e->name, "$unsigned")) {
            if (e->n != 1) EFAIL(e, "%s takes one argument", e->name);
            return self_width(m, e->list[0]);
        }
        f = func_find(m, e->name);
        if (f < 0) EFAIL(e, "'%s' is not a function%s", e->name, e->name[0] == '$' ? " zfpga knows" : "");
        return m->sigs[m->funcs[f].result].width;
    }
    case E_INDEX:
        if (e->a->kind == E_ID) {
            int k = mem_find(m, e->a->name);
            if (k >= 0) return m->sigs[m->mems[k].sig0].width;      /* a memory word */
        }
        return 1;
    case E_RANGE: {
        int64_t hi, lo;
        if (e->op != RANGE_FIXED) return (int)const_eval(m, e->c);     /* the width, given */
        hi = (int64_t)const_eval(m, e->b); lo = (int64_t)const_eval(m, e->c);
        return (int)(hi >= lo ? hi - lo + 1 : lo - hi + 1);
    }
    case E_CONCAT:
        for (w = 0, i = 0; i < e->n; i++) w += self_width(m, e->list[i]);
        return w;
    case E_REPL:
        for (w = 0, i = 0; i < e->n; i++) w += self_width(m, e->list[i]);
        return w * (int)const_eval(m, e->a);
    case E_UNARY:
        if (e->op == OP_NOT || e->op == OP_NEG || e->op == OP_PLUS) return self_width(m, e->a);
        return 1;
    case E_TERNARY:
        a = self_width(m, e->b); b = self_width(m, e->c);
        return a > b ? a : b;
    case E_BINARY:
        switch (e->op) {
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE:
        case OP_LAND: case OP_LOR:
            return 1;
        case OP_SHL: case OP_SHR: case OP_ASHR:
            return self_width(m, e->a);
        default:
            a = self_width(m, e->a); b = self_width(m, e->b);
            return a > b ? a : b;
        }
    }
    return 1;
}

/* Verilog: an expression is signed only if every operand is. Selects,
 * concatenations, comparisons and reductions are unsigned. */
int expr_signed(const module_t *m, const expr_t *e) {
    uint64_t lv;
    switch (e->kind) {
    case E_NUM: return e->is_signed;
    case E_ID: {
        int s, p;
        if (loopvar_find(e->name, &lv)) return 1;
        s = sig_find(m, e->name);
        if (s >= 0) return m->sigs[s].is_signed;
        p = param_find(m, e->name);
        return p >= 0 ? m->params[p].is_signed : 0;
    }
    case E_CALL: {
        int f;
        if (zf_streq(e->name, "$signed") || zf_streq(e->name, "$clog2")) return 1;
        if (zf_streq(e->name, "$unsigned")) return 0;
        f = func_find(m, e->name);
        return f >= 0 ? m->sigs[m->funcs[f].result].is_signed : 0;
    }
    case E_INDEX:
        if (e->a->kind == E_ID) {
            int k = mem_find(m, e->a->name);
            if (k >= 0) return m->sigs[m->mems[k].sig0].is_signed;
        }
        return 0;
    case E_UNARY:
        return (e->op == OP_NOT || e->op == OP_NEG || e->op == OP_PLUS) ? expr_signed(m, e->a) : 0;
    case E_TERNARY: return expr_signed(m, e->b) && expr_signed(m, e->c);
    case E_BINARY:
        switch (e->op) {
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE:
        case OP_LAND: case OP_LOR:
            return 0;
        case OP_SHL: case OP_SHR: case OP_ASHR:
            return expr_signed(m, e->a);
        default:
            return expr_signed(m, e->a) && expr_signed(m, e->b);
        }
    default:
        return 0;
    }
}

/* -- cloning, with every name renamed ----------------------------------------- */

/* Renaming: a module scope prefixes every name; a function scope maps its
 * own inputs, locals and result into the function's space first. */
typedef struct {
    const char *prefix;             /* module scope */
    const funcdef_t *fn;            /* or NULL */
    const char *fname;              /* the function's flat name */
} scope_t;

static const char *cat(const char *a, const char *b) {
    size_t na = zf_strlen(a), nb = zf_strlen(b);
    char *r = zf_alloc(na + nb + 1);
    zf_memcpy(r, a, na);
    zf_memcpy(r + na, b, nb + 1);
    return r;
}

static const char *rename_id(const scope_t *sc, const char *name) {
    if (name[0] == '$') return name;
    if (sc->fn) {
        int i;
        if (zf_streq(name, sc->fn->name)) return sc->fname;
        for (i = 0; i < sc->fn->n_decl; i++)
            if (zf_streq(sc->fn->decls[i].name, name)) return cat(cat(sc->fname, "."), name);
    }
    return cat(sc->prefix, name);
}

static expr_t *clone_e(const scope_t *sc, const expr_t *e) {
    expr_t *c;
    int i;
    if (!e) return NULL;
    c = zf_alloc(sizeof(*c));
    *c = *e;
    if (e->name && (e->kind == E_ID || e->kind == E_CALL)) c->name = rename_id(sc, e->name);
    c->a = clone_e(sc, e->a);
    c->b = clone_e(sc, e->b);
    c->c = clone_e(sc, e->c);
    if (e->list) {
        c->list = zf_alloc(sizeof(expr_t *) * (size_t)(e->n ? e->n : 1));
        for (i = 0; i < e->n; i++) c->list[i] = clone_e(sc, e->list[i]);
    }
    return c;
}

static stmt_t *clone_s(const scope_t *sc, const stmt_t *s) {
    stmt_t *c;
    int i, k;
    if (!s) return NULL;
    c = zf_alloc(sizeof(*c));
    *c = *s;
    c->lhs = clone_e(sc, s->lhs);
    c->rhs = clone_e(sc, s->rhs);
    c->cond = clone_e(sc, s->cond);
    c->init = clone_e(sc, s->init);
    c->step = clone_e(sc, s->step);
    if (s->var) c->var = rename_id(sc, s->var);
    c->then = clone_s(sc, s->then);
    c->els = clone_s(sc, s->els);
    if (s->list) {
        c->list = zf_alloc(sizeof(stmt_t *) * (size_t)(s->n ? s->n : 1));
        for (i = 0; i < s->n; i++) c->list[i] = clone_s(sc, s->list[i]);
    }
    if (s->items) {
        c->items = zf_alloc(sizeof(caseitem_t) * (size_t)(s->n_items ? s->n_items : 1));
        for (i = 0; i < s->n_items; i++) {
            c->items[i] = s->items[i];
            if (s->items[i].vals) {
                c->items[i].vals = zf_alloc(sizeof(expr_t *) * (size_t)s->items[i].n);
                for (k = 0; k < s->items[i].n; k++) c->items[i].vals[k] = clone_e(sc, s->items[i].vals[k]);
            }
            c->items[i].body = clone_s(sc, s->items[i].body);
        }
    }
    return c;
}

/* -- building the flat module ---------------------------------------------------- */

static int add_sig(const char *name, int kind, int msb, int lsb, int is_reg, int is_signed,
        const char *file, int line) {
    sig_t *s;
    if (sig_find(M, name) >= 0) zf_fatal_at(file, line, "'%s' declared twice", name);
    GROW(M->sigs, M->n_sig, M->cap_sig, sig_t);
    s = &M->sigs[M->n_sig];
    zf_memset(s, 0, sizeof(*s));
    s->name = name;
    s->kind = kind;
    s->msb = msb;
    s->lsb = lsb;
    s->width = (msb >= lsb ? msb - lsb : lsb - msb) + 1;
    s->is_reg = is_reg;
    s->is_signed = is_signed;
    s->file = file;
    s->line = line;
    return M->n_sig++;
}

static int eval_int(const scope_t *sc, const expr_t *e, int dflt) {
    int64_t v;
    if (!e) return dflt;
    v = (int64_t)const_eval(M, clone_e(sc, e));
    if (v < 0 || v > 1000000) EFAIL(e, "range bound %d is out of bounds", (int)v);
    return (int)v;
}

static void add_assign(expr_t *lhs, expr_t *rhs, int line) {
    GROW(M->assigns, M->n_assign, M->cap_assign, assign_t);
    M->assigns[M->n_assign].lhs = lhs;
    M->assigns[M->n_assign].rhs = rhs;
    M->assigns[M->n_assign].line = line;
    M->assigns[M->n_assign].file = lhs->file;
    M->n_assign++;
}

static expr_t *id_expr(const char *name, const char *file, int line) {
    expr_t *e = zf_alloc(sizeof(*e));
    e->kind = E_ID;
    e->name = name;
    e->file = file;
    e->line = line;
    return e;
}

typedef struct { const char *name; uint64_t value; int given; } ovr_t;

static void instantiate(const moddef_t *d, const char *prefix, const ovr_t *ov, int n_ov, int depth,
        int top) {
    scope_t sc;
    int i, k;
    if (depth > 64) zf_fatal_at(d->file, d->line, "instances nested more than 64 deep (a module instantiating itself?)");
    sc.prefix = prefix;
    sc.fn = NULL;
    sc.fname = NULL;

    /* 1. parameters */
    for (i = 0; i < d->n_pdecl; i++) {
        const pdecl_t *pd = &d->pdecls[i];
        param_t *p;
        expr_t *v = clone_e(&sc, pd->value);
        GROW(M->params, M->n_param, M->cap_param, param_t);
        p = &M->params[M->n_param];
        p->name = cat(prefix, pd->name);
        if (pd->msb) {
            /* parameter [7:0] P: as wide as it says, and a select outside
             * that is refused rather than guessed at */
            int hi = eval_int(&sc, pd->msb, 0), lo = eval_int(&sc, pd->lsb, 0);
            p->width = (hi >= lo ? hi - lo : lo - hi) + 1;
            p->ranged = 1;
            p->is_signed = pd->is_signed;
        } else {
            p->width = v->kind == E_NUM && v->width ? v->width : 0;
            p->ranged = 0;
            p->is_signed = expr_signed(M, v);
        }
        p->value = const_eval(M, v);
        if (p->width && p->width < 64) p->value &= ((uint64_t)1 << p->width) - 1;
        for (k = 0; k < n_ov; k++)
            if (ov[k].name && zf_streq(ov[k].name, pd->name)) {
                if (pd->local) zf_fatal_at(d->file, pd->line, "localparam %s cannot be overridden", pd->name);
                p->value = ov[k].value;
            }
        M->n_param++;
    }

    /* 2. declarations: widths and depths in this scope */
    for (i = 0; i < d->n_decl; i++) {
        const decl_t *dc = &d->decls[i];
        const char *name = cat(prefix, dc->name);
        int msb = eval_int(&sc, dc->msb, 0), lsb = eval_int(&sc, dc->lsb, 0), kind = dc->kind;
        if (!top && (kind == SIG_IN || kind == SIG_OUT)) kind = dc->is_reg ? SIG_REG : SIG_WIRE;
        if (dc->alo) {
            int lo = eval_int(&sc, dc->alo, 0), hi = eval_int(&sc, dc->ahi, 0), a, depth, w;
            mem_t *mm;
            if (lo > hi) { int t = lo; lo = hi; hi = t; }
            depth = hi - lo + 1;
            w = (msb >= lsb ? msb - lsb : lsb - msb) + 1;
            if ((long)depth * w > MEM_BITS_MAX)
                zf_fatal_at(d->file, dc->line, "memory %s is %d x %d = %u bits: memories are built "
                    "from flip-flops, up to %d bits; block RAM inference is not supported yet",
                    dc->name, depth, w, (unsigned)((long)depth * w), MEM_BITS_MAX);
            if (dc->init) zf_fatal_at(d->file, dc->line, "memory %s: an initial value is not supported", dc->name);
            GROW(M->mems, M->n_mem, M->cap_mem, mem_t);
            mm = &M->mems[M->n_mem++];
            mm->name = name;
            mm->lo = lo;
            mm->hi = hi;
            mm->depth = depth;
            mm->sig0 = M->n_sig;
            for (a = lo; a <= hi; a++) {
                char buf[24];
                zf_fmt(buf, sizeof(buf), "[%d]", a);
                add_sig(cat(name, buf), SIG_REG, msb, lsb, 1, dc->is_signed, d->file, dc->line);
            }
            continue;
        }
        k = add_sig(name, kind, msb, lsb, dc->is_reg, dc->is_signed, d->file, dc->line);
        M->sigs[k].init = clone_e(&sc, dc->init);
    }

    /* 3. functions, each in a scope of its own */
    for (i = 0; i < d->n_func; i++) {
        const funcdef_t *fd = &d->funcs[i];
        func_t *f;
        scope_t fs;
        int msb = eval_int(&sc, fd->msb, 0), lsb = eval_int(&sc, fd->lsb, 0);
        GROW(M->funcs, M->n_func, M->cap_func, func_t);
        f = &M->funcs[M->n_func];
        zf_memset(f, 0, sizeof(*f));
        f->name = cat(prefix, fd->name);
        f->line = fd->line;
        f->result = add_sig(f->name, SIG_FUNC, msb, lsb, 1, fd->is_signed, d->file, fd->line);
        f->inputs = zf_alloc(sizeof(int) * (size_t)(fd->n_decl + 1));
        f->locals = zf_alloc(sizeof(int) * (size_t)(fd->n_decl + 1));
        for (k = 0; k < fd->n_decl; k++) {
            const decl_t *dc = &fd->decls[k];
            int m2 = eval_int(&sc, dc->msb, 0), l2 = eval_int(&sc, dc->lsb, 0);
            int s = add_sig(cat(cat(f->name, "."), dc->name), SIG_FUNC, m2, l2, 1, dc->is_signed,
                d->file, dc->line);
            if (dc->alo) zf_fatal_at(d->file, dc->line, "a memory inside a function is not supported");
            if (dc->kind == SIG_IN) f->inputs[f->n_inputs++] = s;
            else if (dc->kind == SIG_INT) M->sigs[s].kind = SIG_INT;
            else f->locals[f->n_locals++] = s;
        }
        fs.prefix = prefix;
        fs.fn = fd;
        fs.fname = f->name;
        f->body = clone_s(&fs, fd->body);
        M->n_func++;
    }

    /* 4. assigns and always blocks, renamed */
    for (i = 0; i < d->n_assign; i++)
        add_assign(clone_e(&sc, d->assigns[i].lhs), clone_e(&sc, d->assigns[i].rhs), d->assigns[i].line);
    for (i = 0; i < d->n_always; i++) {
        always_t *a;
        GROW(M->always, M->n_always, M->cap_always, always_t);
        a = &M->always[M->n_always++];
        *a = d->always[i];
        if (a->clk) a->clk = cat(prefix, a->clk);
        if (a->rst) a->rst = cat(prefix, a->rst);
        a->body = clone_s(&sc, d->always[i].body);
    }

    /* 5. instances: parameters from this scope, then ports as assigns */
    for (i = 0; i < d->n_inst; i++) {
        const inst_t *in = &d->insts[i];
        const moddef_t *cd = lib_find(LIB, in->mod);
        const char *cprefix = cat(cat(prefix, in->name), ".");
        ovr_t *cov;
        int np = 0;
        if (!cd) zf_fatal_at(d->file, in->line, "module %s is not defined (give its file too)", in->mod);
        M->n_inst++;
        cov = zf_alloc(sizeof(ovr_t) * (size_t)(in->n_params + 1));
        for (k = 0; k < in->n_params; k++) {
            const conn_t *c = &in->params[k];
            if (c->name) {
                int j;
                for (j = 0; j < cd->n_pdecl; j++) if (zf_streq(cd->pdecls[j].name, c->name)) break;
                if (j == cd->n_pdecl)
                    zf_fatal_at(d->file, in->line, "%s has no parameter %s", in->mod, c->name);
                cov[np].name = c->name;
            } else {
                /* positional: the child's overridable parameters, in order */
                int j, seen = 0;
                cov[np].name = NULL;
                for (j = 0; j < cd->n_pdecl; j++) {
                    if (cd->pdecls[j].local) continue;
                    if (seen++ == k) { cov[np].name = cd->pdecls[j].name; break; }
                }
                if (!cov[np].name) zf_fatal_at(d->file, in->line, "%s: too many parameters", in->mod);
            }
            cov[np].value = const_eval(M, clone_e(&sc, c->e));
            cov[np].given = 1;
            np++;
        }
        instantiate(cd, cprefix, cov, np, depth + 1, 0);

        for (k = 0; k < in->n_ports; k++) {
            const conn_t *c = &in->ports[k];
            const char *port;
            int j;
            if (c->name) {
                port = c->name;
            } else {
                if (k >= cd->n_ports) zf_fatal_at(d->file, in->line, "%s: too many port connections", in->mod);
                port = cd->ports[k];
            }
            for (j = 0; j < cd->n_decl; j++) if (zf_streq(cd->decls[j].name, port)) break;
            if (j == cd->n_decl || (cd->decls[j].kind != SIG_IN && cd->decls[j].kind != SIG_OUT))
                zf_fatal_at(d->file, in->line, "%s has no port %s", in->mod, port);
            if (!c->e) continue;                /* .port(): unconnected */
            if (cd->decls[j].kind == SIG_IN) {
                add_assign(id_expr(cat(cprefix, port), d->file, in->line), clone_e(&sc, c->e), in->line);
            } else {
                expr_t *lhs = clone_e(&sc, c->e);
                if (lhs->kind != E_ID && lhs->kind != E_INDEX && lhs->kind != E_RANGE && lhs->kind != E_CONCAT)
                    zf_fatal_at(d->file, in->line, "output %s of %s must connect to a wire, not an expression",
                        port, in->name);
                add_assign(lhs, id_expr(cat(cprefix, port), d->file, in->line), in->line);
            }
        }
        /* an input nothing connects is tied to 0, and said so */
        for (k = 0; k < cd->n_decl; k++) {
            int j, found = 0;
            if (cd->decls[k].kind != SIG_IN) continue;
            for (j = 0; j < in->n_ports; j++) {
                const char *port = in->ports[j].name ? in->ports[j].name
                    : (j < cd->n_ports ? cd->ports[j] : "");
                if (zf_streq(port, cd->decls[k].name)) { found = 1; break; }
            }
            if (!found || 0) {
                expr_t *zero = zf_alloc(sizeof(*zero));
                zero->kind = E_NUM;
                zero->file = d->file;
                zero->line = in->line;
                zf_note("%s:%d: input %s of %s is not connected: tied to 0", d->file, in->line,
                    cd->decls[k].name, in->name);
                add_assign(id_expr(cat(cprefix, cd->decls[k].name), d->file, in->line), zero, in->line);
            }
        }
    }
}

void synth_flatten(const lib_t *lib, const char *top, module_t *m) {
    const moddef_t *d = NULL;
    int i, j, k;
    LIB = lib;
    M = m;
    zf_memset(m, 0, sizeof(*m));
    if (top) {
        d = lib_find(lib, top);
        if (!d) zf_fatal("no module %s", top);
    } else {
        /* the top: the one module nothing instantiates */
        int n_top = 0;
        char names[256];
        int len = 0;
        names[0] = 0;
        for (i = 0; i < lib->n_mod; i++) {
            int used = 0;
            for (j = 0; j < lib->n_mod && !used; j++)
                for (k = 0; k < lib->mods[j]->n_inst; k++)
                    if (zf_streq(lib->mods[j]->insts[k].mod, lib->mods[i]->name)) { used = 1; break; }
            if (!used) {
                d = lib->mods[i];
                n_top++;
                if (len < 200) len += zf_fmt(names + len, (int)sizeof(names) - len, " %s", d->name);
            }
        }
        if (n_top != 1) zf_fatal("%d candidate top modules (%s ): name one with -t", n_top, names);
    }
    m->name = d->name;
    m->file = d->file;
    instantiate(d, "", NULL, 0, 0, 1);
}
