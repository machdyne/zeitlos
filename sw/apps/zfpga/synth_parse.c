/*
 * zfpga -- synth, the front end: a lexer and a recursive-descent parser
 * for the Verilog-2001 subset docs/zfpga.md sec. 19 describes.
 *
 * Refused by name, never approximated: module instantiation, generate,
 * for loops, functions and tasks, initial blocks, inout ports, signed,
 * integer, x and z in literals, `define and friends. docs/zcc.md's rule
 * holds more strongly here: a wrong netlist does not crash, it programs.
 */

#include "synth.h"

/* -- the lexer ------------------------------------------------------------ */

enum { T_EOF, T_ID, T_NUM, T_OP };

typedef struct {
    const char *src, *p, *file;
    int line;
    /* the current token */
    int t, tline;
    char text[128];
    int width;          /* T_NUM */
    uint64_t value;
} lex_t;

static lex_t L;
static module_t *M;

#define ERR(...) zf_fatal_at(L.file, L.tline, __VA_ARGS__)

static int isid0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int isid(int c) { return isid0(c) || (c >= '0' && c <= '9') || c == '$'; }

static int digit(int c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

static void skip_space(void) {
    for (;;) {
        char c = *L.p;
        if (c == '\n') { L.line++; L.p++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { L.p++; continue; }
        if (c == '/' && L.p[1] == '/') { while (*L.p && *L.p != '\n') L.p++; continue; }
        if (c == '/' && L.p[1] == '*') {
            L.p += 2;
            while (*L.p && !(L.p[0] == '*' && L.p[1] == '/')) { if (*L.p == '\n') L.line++; L.p++; }
            if (*L.p) L.p += 2;
            continue;
        }
        if (c == '(' && L.p[1] == '*' && L.p[2] != ')') {       /* (* attribute *) */
            L.p += 2;
            while (*L.p && !(L.p[0] == '*' && L.p[1] == ')')) { if (*L.p == '\n') L.line++; L.p++; }
            if (*L.p) L.p += 2;
            continue;
        }
        if (c == '`') {
            const char *q = L.p + 1;
            char d[24];
            int n = 0;
            while (isid(*q) && n < 23) d[n++] = *q++;
            d[n] = 0;
            if (zf_streq(d, "timescale") || zf_streq(d, "default_nettype") ||
                    zf_streq(d, "resetall") || zf_streq(d, "celldefine") || zf_streq(d, "endcelldefine")) {
                while (*L.p && *L.p != '\n') L.p++;
                continue;
            }
            L.tline = L.line;
            ERR("`%s is not supported (no preprocessor yet)", d);
        }
        break;
    }
}

static void next(void) {
    const char *p;
    int n = 0;
    skip_space();
    L.tline = L.line;
    p = L.p;
    if (!*p) { L.t = T_EOF; L.text[0] = 0; return; }
    if (isid0(*p)) {
        while (isid(*p)) { if (n < 127) L.text[n++] = *p; p++; }
        L.text[n] = 0;
        L.t = T_ID;
        L.p = p;
        return;
    }
    if ((*p >= '0' && *p <= '9') || *p == '\'') {
        uint64_t v = 0;
        int width = 0, base = 10, any = 0;
        if (*p != '\'') {
            while ((*p >= '0' && *p <= '9') || *p == '_') {
                if (*p != '_') v = v * 10 + (uint64_t)(*p - '0');
                p++;
            }
            if (*p != '\'') { L.t = T_NUM; L.width = 0; L.value = v; L.p = p; return; }
            if (!v || v > 64) { L.p = p; ERR("literal width %u: 1..64 only", (unsigned)v); }
            width = (int)v;
            v = 0;
        }
        p++;                                    /* ' */
        if (*p == 's' || *p == 'S') { L.p = p; ERR("signed literals are not supported"); }
        switch (*p) {
        case 'b': case 'B': base = 2; break;
        case 'o': case 'O': base = 8; break;
        case 'd': case 'D': base = 10; break;
        case 'h': case 'H': base = 16; break;
        default: L.p = p; ERR("bad literal base '%c'", *p);
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        for (;; p++) {
            int d;
            if (*p == '_') continue;
            if (*p == 'x' || *p == 'X' || *p == 'z' || *p == 'Z' || *p == '?') {
                L.p = p; ERR("x and z are not supported in literals");
            }
            d = digit(*p, base);
            if (d < 0) break;
            v = v * (uint64_t)base + (uint64_t)d;
            any = 1;
        }
        if (!any) { L.p = p; ERR("literal with no digits"); }
        if (width && width < 64) v &= ((uint64_t)1 << width) - 1;
        L.t = T_NUM;
        L.width = width ? width : 0;
        L.value = v;
        L.p = p;
        return;
    }
    {
        static const char *ops3[] = { "===", "!==", "<<<", ">>>", NULL };
        static const char *ops2[] = { "<=", ">=", "==", "!=", "&&", "||", "<<", ">>", "~&", "~|",
            "~^", "^~", "->", "**", NULL };
        int i;
        for (i = 0; ops3[i]; i++)
            if (p[0] == ops3[i][0] && p[1] == ops3[i][1] && p[2] == ops3[i][2]) {
                zf_memcpy(L.text, ops3[i], 4); L.p = p + 3; L.t = T_OP; return;
            }
        for (i = 0; ops2[i]; i++)
            if (p[0] == ops2[i][0] && p[1] == ops2[i][1]) {
                zf_memcpy(L.text, ops2[i], 3); L.p = p + 2; L.t = T_OP; return;
            }
        L.text[0] = *p;
        L.text[1] = 0;
        L.p = p + 1;
        L.t = T_OP;
    }
}

static int is(const char *s) { return (L.t == T_OP || L.t == T_ID) && zf_streq(L.text, s); }
static int accept(const char *s) { if (is(s)) { next(); return 1; } return 0; }
static void expect(const char *s) {
    if (!accept(s)) ERR("expected '%s', found '%s'", s, L.t == T_EOF ? "end of file" : L.text);
}
static const char *ident(void) {
    const char *s;
    if (L.t != T_ID) ERR("expected a name, found '%s'", L.text);
    s = zf_strdup(L.text);
    next();
    return s;
}

static const char *const reserved[] = {
    "initial", "generate", "genvar", "for", "while", "repeat", "forever", "function", "task",
    "integer", "real", "signed", "inout", "fork", "join", "casex", "casez", "tri", "supply0",
    "supply1", "specify", "primitive", NULL
};

static void refuse_reserved(void) {
    int i;
    for (i = 0; reserved[i]; i++)
        if (L.t == T_ID && zf_streq(L.text, reserved[i]))
            ERR("'%s' is not supported yet", L.text);
}

/* -- expressions ------------------------------------------------------------ */

static expr_t *mk(int kind, int op) {
    expr_t *e = zf_alloc(sizeof(*e));
    e->kind = kind;
    e->op = op;
    e->line = L.tline;
    return e;
}

static expr_t *expr(void);

static expr_t *primary(void) {
    expr_t *e;
    if (L.t == T_NUM) {
        e = mk(E_NUM, 0);
        e->width = L.width;
        e->value = L.value;
        next();
        return e;
    }
    if (accept("(")) {
        e = expr();
        expect(")");
        return e;
    }
    if (accept("{")) {
        expr_t *first = expr();
        if (accept("{")) {                      /* {n{...}} */
            expr_t **list = NULL;
            int n = 0, cap = 0;
            e = mk(E_REPL, 0);
            e->a = first;
            do {
                if (n == cap) {
                    expr_t **nl = zf_alloc(sizeof(expr_t *) * (size_t)(cap ? cap * 2 : 4));
                    if (n) zf_memcpy(nl, list, sizeof(expr_t *) * (size_t)n);
                    list = nl;
                    cap = cap ? cap * 2 : 4;
                }
                list[n++] = expr();
            } while (accept(","));
            expect("}");
            expect("}");
            e->list = list;
            e->n = n;
            return e;
        }
        {
            expr_t **list = zf_alloc(sizeof(expr_t *) * 4);
            int n = 1, cap = 4;
            e = mk(E_CONCAT, 0);
            list[0] = first;
            while (accept(",")) {
                if (n == cap) {
                    expr_t **nl = zf_alloc(sizeof(expr_t *) * (size_t)(cap * 2));
                    zf_memcpy(nl, list, sizeof(expr_t *) * (size_t)n);
                    list = nl;
                    cap *= 2;
                }
                list[n++] = expr();
            }
            expect("}");
            e->list = list;
            e->n = n;
            return e;
        }
    }
    if (L.t == T_ID) {
        refuse_reserved();
        e = mk(E_ID, 0);
        e->name = ident();
        if (accept("(")) ERR("function calls are not supported");
        if (accept("[")) {
            expr_t *i = expr();
            if (accept(":")) {
                expr_t *r = mk(E_RANGE, 0);
                r->a = e;
                r->b = i;
                r->c = expr();
                expect("]");
                return r;
            }
            if (is("+:") || is("-:")) ERR("indexed part-selects are not supported");
            expect("]");
            {
                expr_t *x = mk(E_INDEX, 0);
                x->a = e;
                x->b = i;
                return x;
            }
        }
        return e;
    }
    ERR("expected an expression, found '%s'", L.t == T_EOF ? "end of file" : L.text);
}

static expr_t *unary(void) {
    static const struct { const char *t; int op; } un[] = {
        { "!", OP_LNOT }, { "~&", OP_RNAND }, { "~|", OP_RNOR }, { "~^", OP_RXNOR },
        { "^~", OP_RXNOR }, { "~", OP_NOT }, { "-", OP_NEG }, { "+", OP_PLUS }, { "&", OP_RAND },
        { "|", OP_ROR }, { "^", OP_RXOR },
    };
    unsigned i;
    if (L.t == T_OP)
        for (i = 0; i < sizeof(un) / sizeof(un[0]); i++)
            if (zf_streq(L.text, un[i].t)) {
                expr_t *e = mk(E_UNARY, un[i].op);
                next();
                e->a = unary();
                return e;
            }
    return primary();
}

/* Verilog's binary precedence, lowest first */
static const struct { const char *t; int op, prec; } bins[] = {
    { "||", OP_LOR, 1 }, { "&&", OP_LAND, 2 }, { "|", OP_OR, 3 }, { "^", OP_XOR, 4 },
    { "~^", OP_XNOR, 4 }, { "^~", OP_XNOR, 4 }, { "&", OP_AND, 5 }, { "==", OP_EQ, 6 },
    { "!=", OP_NE, 6 }, { "===", OP_EQ, 6 }, { "!==", OP_NE, 6 }, { "<", OP_LT, 7 },
    { "<=", OP_LE, 7 }, { ">", OP_GT, 7 }, { ">=", OP_GE, 7 }, { "<<", OP_SHL, 8 },
    { ">>", OP_SHR, 8 }, { "<<<", OP_SHL, 8 }, { ">>>", OP_SHR, 8 }, { "+", OP_ADD, 9 },
    { "-", OP_SUB, 9 }, { "*", OP_MUL, 10 }, { "/", OP_DIV, 10 }, { "%", OP_MOD, 10 },
};

static int binop(int *op) {
    unsigned i;
    if (L.t != T_OP) return 0;
    for (i = 0; i < sizeof(bins) / sizeof(bins[0]); i++)
        if (zf_streq(L.text, bins[i].t)) { *op = bins[i].op; return bins[i].prec; }
    return 0;
}

static expr_t *binary(int minprec) {
    expr_t *lhs = unary();
    for (;;) {
        int op, prec = binop(&op);
        expr_t *e;
        if (!prec || prec < minprec) return lhs;
        if (is("**")) ERR("** is not supported");
        e = mk(E_BINARY, op);
        next();
        e->a = lhs;
        e->b = binary(prec + 1);
        lhs = e;
    }
}

static expr_t *expr(void) {
    expr_t *c = binary(1);
    if (accept("?")) {
        expr_t *e = mk(E_TERNARY, 0);
        e->a = c;
        e->b = expr();
        expect(":");
        e->c = expr();
        return e;
    }
    return c;
}

/* -- constants -------------------------------------------------------------- */

int param_find(const module_t *m, const char *name) {
    int i;
    for (i = 0; i < m->n_param; i++) if (zf_streq(m->params[i].name, name)) return i;
    return -1;
}

int sig_find(const module_t *m, const char *name) {
    int i;
    for (i = 0; i < m->n_sig; i++) if (zf_streq(m->sigs[i].name, name)) return i;
    return -1;
}

uint64_t const_eval(const module_t *m, const expr_t *e) {
    uint64_t a, b;
    switch (e->kind) {
    case E_NUM: return e->value;
    case E_ID: {
        int p = param_find(m, e->name);
        if (p < 0) zf_fatal_at(m->file, e->line, "'%s' is not a constant", e->name);
        return m->params[p].value;
    }
    case E_TERNARY: return const_eval(m, e->a) ? const_eval(m, e->b) : const_eval(m, e->c);
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
        case OP_DIV: if (!b) zf_fatal_at(m->file, e->line, "division by zero"); return a / b;
        case OP_MOD: if (!b) zf_fatal_at(m->file, e->line, "division by zero"); return a % b;
        case OP_SHL: return b >= 64 ? 0 : a << b;
        case OP_SHR: return b >= 64 ? 0 : a >> b;
        case OP_AND: return a & b;
        case OP_OR: return a | b;
        case OP_XOR: return a ^ b;
        case OP_LT: return a < b;
        case OP_LE: return a <= b;
        case OP_GT: return a > b;
        case OP_GE: return a >= b;
        case OP_EQ: return a == b;
        case OP_NE: return a != b;
        case OP_LAND: return a && b;
        case OP_LOR: return a || b;
        default: break;
        }
        break;
    default: break;
    }
    zf_fatal_at(m->file, e->line, "not a constant expression");
}

/* Verilog's self-determined width (IEEE 1364-2001 table 29), unsigned. */
int self_width(const module_t *m, const expr_t *e) {
    int i, w, a, b;
    switch (e->kind) {
    case E_NUM: return e->width ? e->width : 32;
    case E_ID: {
        int s = sig_find(m, e->name);
        if (s >= 0) return m->sigs[s].width;
        if (param_find(m, e->name) >= 0) {
            int p = param_find(m, e->name);
            return m->params[p].width ? m->params[p].width : 32;
        }
        zf_fatal_at(m->file, e->line, "'%s' is not declared", e->name);
    }
    case E_INDEX: return 1;
    case E_RANGE: {
        int64_t hi = (int64_t)const_eval(m, e->b), lo = (int64_t)const_eval(m, e->c);
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
        case OP_SHL: case OP_SHR:
            return self_width(m, e->a);
        default:
            a = self_width(m, e->a); b = self_width(m, e->b);
            return a > b ? a : b;
        }
    }
    return 1;
}

/* -- statements -------------------------------------------------------------- */

static stmt_t *mks(int kind) {
    stmt_t *s = zf_alloc(sizeof(*s));
    s->kind = kind;
    s->line = L.tline;
    return s;
}

static stmt_t *statement(void);

/* an assignment target: a name, a bit, a range, or a concatenation */
static expr_t *lvalue(void) {
    expr_t *e = primary();
    if (e->kind != E_ID && e->kind != E_INDEX && e->kind != E_RANGE && e->kind != E_CONCAT)
        zf_fatal_at(L.file, e->line, "not something that can be assigned");
    return e;
}

static stmt_t *statement(void) {
    stmt_t *s;
    refuse_reserved();
    if (accept("begin")) {
        stmt_t **list = NULL;
        int n = 0, cap = 0;
        s = mks(S_BLOCK);
        if (accept(":")) (void)ident();
        while (!accept("end")) {
            if (L.t == T_EOF) ERR("'begin' without 'end'");
            if (n == cap) {
                stmt_t **nl = zf_alloc(sizeof(stmt_t *) * (size_t)(cap ? cap * 2 : 8));
                if (n) zf_memcpy(nl, list, sizeof(stmt_t *) * (size_t)n);
                list = nl;
                cap = cap ? cap * 2 : 8;
            }
            list[n++] = statement();
        }
        s->list = list;
        s->n = n;
        return s;
    }
    if (accept(";")) return mks(S_BLOCK);
    if (accept("if")) {
        s = mks(S_IF);
        expect("(");
        s->cond = expr();
        expect(")");
        s->then = statement();
        if (accept("else")) s->els = statement();
        return s;
    }
    if (accept("case")) {
        caseitem_t *items = NULL;
        int n = 0, cap = 0;
        s = mks(S_CASE);
        expect("(");
        s->cond = expr();
        expect(")");
        while (!accept("endcase")) {
            caseitem_t *it;
            if (L.t == T_EOF) ERR("'case' without 'endcase'");
            if (n == cap) {
                caseitem_t *ni = zf_alloc(sizeof(caseitem_t) * (size_t)(cap ? cap * 2 : 8));
                if (n) zf_memcpy(ni, items, sizeof(caseitem_t) * (size_t)n);
                items = ni;
                cap = cap ? cap * 2 : 8;
            }
            it = &items[n++];
            if (accept("default")) {
                accept(":");
            } else {
                expr_t **vals = zf_alloc(sizeof(expr_t *) * 16);
                int nv = 0;
                do {
                    if (nv == 16) ERR("more than 16 values in one case item");
                    vals[nv++] = expr();
                } while (accept(","));
                expect(":");
                it->vals = vals;
                it->n = nv;
            }
            it->body = statement();
        }
        s->items = items;
        s->n_items = n;
        return s;
    }
    s = mks(S_NB);
    s->lhs = lvalue();
    if (accept("<=")) s->kind = S_NB;
    else if (accept("=")) s->kind = S_BA;
    else ERR("expected '<=' or '=', found '%s'", L.text);
    s->rhs = expr();
    expect(";");
    return s;
}

/* -- declarations and the module ------------------------------------------- */

/* kind -1: a name in a non-ANSI port list, direction to follow */
static void add_sig(const char *name, int kind, int msb, int lsb, int is_reg, int line) {
    int i = sig_find(M, name);
    sig_t *s;
    if (param_find(M, name) >= 0) zf_fatal_at(M->file, line, "'%s' is already a parameter", name);
    if (i >= 0) {
        s = &M->sigs[i];
        if (kind == SIG_IN || kind == SIG_OUT) {
            if (s->kind != -1) zf_fatal_at(M->file, line, "port '%s' declared twice", name);
            s->kind = kind;
            s->msb = msb; s->lsb = lsb;
            s->width = (msb >= lsb ? msb - lsb : lsb - msb) + 1;
            s->is_reg |= is_reg;
            return;
        }
        if (s->kind == SIG_IN || s->kind == SIG_OUT) {
            /* `reg [7:0] q;` after `output [7:0] q;` */
            if ((msb || lsb) && (msb != s->msb || lsb != s->lsb))
                zf_fatal_at(M->file, line, "'%s' declared with two widths", name);
            if (is_reg && s->kind == SIG_IN) zf_fatal_at(M->file, line, "input '%s' cannot be a reg", name);
            s->is_reg |= is_reg;
            return;
        }
        zf_fatal_at(M->file, line, "'%s' declared twice", name);
    }
    if (M->n_sig == M->cap_sig) {
        int cap = M->cap_sig ? M->cap_sig * 2 : 32;
        sig_t *ns = zf_alloc(sizeof(sig_t) * (size_t)cap);
        if (M->n_sig) zf_memcpy(ns, M->sigs, sizeof(sig_t) * (size_t)M->n_sig);
        M->sigs = ns;
        M->cap_sig = cap;
    }
    s = &M->sigs[M->n_sig++];
    zf_memset(s, 0, sizeof(*s));
    s->name = name;
    s->kind = kind;
    s->msb = msb; s->lsb = lsb;
    s->width = (msb >= lsb ? msb - lsb : lsb - msb) + 1;
    s->is_reg = is_reg;
    s->line = line;
}

static void range(int *msb, int *lsb) {
    *msb = *lsb = 0;
    if (accept("[")) {
        *msb = (int)const_eval(M, expr());
        expect(":");
        *lsb = (int)const_eval(M, expr());
        expect("]");
        if (*msb < 0 || *lsb < 0 || *msb > 4095 || *lsb > 4095) ERR("range out of bounds");
    }
}

static void add_param(const char *name, uint64_t v, int width) {
    param_t *p;
    if (param_find(M, name) >= 0) ERR("parameter '%s' declared twice", name);
    if (M->n_param == M->cap_param) {
        int cap = M->cap_param ? M->cap_param * 2 : 16;
        param_t *np = zf_alloc(sizeof(param_t) * (size_t)cap);
        if (M->n_param) zf_memcpy(np, M->params, sizeof(param_t) * (size_t)M->n_param);
        M->params = np;
        M->cap_param = cap;
    }
    p = &M->params[M->n_param++];
    p->name = name;
    p->value = v;
    p->width = width;
}

static void param_decl(void) {
    int msb, lsb, w;
    range(&msb, &lsb);
    w = (msb || lsb) ? (msb >= lsb ? msb - lsb : lsb - msb) + 1 : 0;
    do {
        const char *n = ident();
        expect("=");
        add_param(n, const_eval(M, expr()), w);
    } while (accept(",") && L.t == T_ID && !is("parameter") && !is("localparam"));
}

/* ANSI port declarations: input [wire|reg] [range] a, b, output ... */
static void port_list(void) {
    int dir = -1, is_reg = 0, msb = 0, lsb = 0;
    if (accept(")")) return;
    do {
        if (is("inout")) ERR("inout ports are not supported yet");
        if (accept("input")) { dir = SIG_IN; is_reg = 0; accept("wire"); range(&msb, &lsb); }
        else if (accept("output")) {
            dir = SIG_OUT;
            is_reg = accept("reg");
            if (!is_reg) accept("wire");
            range(&msb, &lsb);
        }
        {
            const char *n = ident();
            if (dir < 0) add_sig(n, -1, 0, 0, 0, L.tline);      /* non-ANSI: declared later */
            else add_sig(n, dir, msb, lsb, is_reg, L.tline);
        }
    } while (accept(","));
    expect(")");
}

static void add_assign(expr_t *lhs, expr_t *rhs, int line) {
    if (M->n_assign == M->cap_assign) {
        int cap = M->cap_assign ? M->cap_assign * 2 : 32;
        assign_t *na = zf_alloc(sizeof(assign_t) * (size_t)cap);
        if (M->n_assign) zf_memcpy(na, M->assigns, sizeof(assign_t) * (size_t)M->n_assign);
        M->assigns = na;
        M->cap_assign = cap;
    }
    M->assigns[M->n_assign].lhs = lhs;
    M->assigns[M->n_assign].rhs = rhs;
    M->assigns[M->n_assign].line = line;
    M->n_assign++;
}

static void always_block(int line) {
    always_t *a;
    if (M->n_always == M->cap_always) {
        int cap = M->cap_always ? M->cap_always * 2 : 8;
        always_t *na = zf_alloc(sizeof(always_t) * (size_t)cap);
        if (M->n_always) zf_memcpy(na, M->always, sizeof(always_t) * (size_t)M->n_always);
        M->always = na;
        M->cap_always = cap;
    }
    a = &M->always[M->n_always++];
    zf_memset(a, 0, sizeof(*a));
    a->line = line;
    expect("@");
    if (accept("*")) {
        a->comb = 1;
    } else {
        expect("(");
        if (accept("*")) {
            a->comb = 1;
        } else if (is("posedge") || is("negedge")) {
            int n = 0;
            do {
                int neg = is("negedge");
                const char *s;
                if (!accept("posedge") && !accept("negedge"))
                    ERR("mix edge and level sensitivity: not supported");
                s = ident();
                if (n == 0) {
                    if (neg) ERR("negedge clocks are not supported");
                    a->clk = s;
                } else if (n == 1) {
                    a->rst = s;
                    a->rst_neg = neg;
                } else {
                    ERR("more than a clock and one asynchronous reset");
                }
                n++;
            } while (accept("or") || accept(","));
        } else {
            ERR("a level-sensitive list: write always @(*)");
        }
        expect(")");
    }
    a->body = statement();
}

void synth_parse(const char *path, module_t *m) {
    uint32_t len;
    uint8_t *src = zf_read_all(path, &len);
    char *s;
    if (!src) zf_fatal("cannot open %s%s", path, zf_83_hint(path));
    s = zf_alloc(len + 1);
    zf_memcpy(s, src, len);
    s[len] = 0;
    zf_memset(&L, 0, sizeof(L));
    L.src = L.p = s;
    L.file = path;
    L.line = 1;
    M = m;
    zf_memset(m, 0, sizeof(*m));
    m->file = path;
    next();

    if (!accept("module")) ERR("expected 'module'");
    m->name = ident();
    if (accept("#")) {
        expect("(");
        do {
            accept("parameter");
            param_decl();
        } while (accept(",") || is("parameter"));
        expect(")");
    }
    if (accept("(")) port_list();
    expect(";");

    while (!accept("endmodule")) {
        int line = L.tline;
        if (L.t == T_EOF) ERR("missing 'endmodule'");
        refuse_reserved();
        if (is("input") || is("output")) {
            /* non-ANSI: input [range] a, b; */
            int dir = is("input") ? SIG_IN : SIG_OUT, is_reg = 0, msb, lsb;
            next();
            if (dir == SIG_OUT) is_reg = accept("reg");
            if (!is_reg) accept("wire");
            range(&msb, &lsb);
            do add_sig(ident(), dir, msb, lsb, is_reg, line); while (accept(","));
            expect(";");
        } else if (is("wire") || is("reg")) {
            int isreg = is("reg"), msb, lsb;
            next();
            range(&msb, &lsb);
            do {
                const char *n = ident();
                add_sig(n, isreg ? SIG_REG : SIG_WIRE, msb, lsb, isreg, line);
                if (accept("=")) {
                    expr_t *e = expr();
                    if (isreg) {
                        M->sigs[sig_find(M, n)].init = e;       /* an initial value */
                    } else {
                        expr_t *id = mk(E_ID, 0);               /* wire x = e; is an assign */
                        id->name = n;
                        add_assign(id, e, line);
                    }
                }
            } while (accept(","));
            expect(";");
        } else if (accept("parameter") || accept("localparam")) {
            param_decl();
            expect(";");
        } else if (accept("assign")) {
            do {
                expr_t *lhs = lvalue();
                expect("=");
                add_assign(lhs, expr(), line);
            } while (accept(","));
            expect(";");
        } else if (accept("always")) {
            always_block(line);
        } else if (L.t == T_ID) {
            ERR("'%s': module instances are not supported yet", L.text);
        } else {
            ERR("unexpected '%s'", L.text);
        }
    }
    {
        int i;
        for (i = 0; i < m->n_sig; i++)
            if (m->sigs[i].kind < 0)
                zf_fatal_at(path, m->sigs[i].line, "port '%s' has no direction", m->sigs[i].name);
    }
}
