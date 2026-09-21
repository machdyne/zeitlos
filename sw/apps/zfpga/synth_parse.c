/*
 * zfpga -- synth, the front end: a preprocessing lexer and a recursive-
 * descent parser for the Verilog-2001 subset of docs/zfpga.md sec. 24.
 *
 * It produces module DEFINITIONS with their declarations unevaluated --
 * `reg [W-1:0] q` keeps `W-1` as an expression -- because a width can
 * depend on a parameter an instance overrides. synth_flat.c evaluates
 * them, once per instance.
 *
 * THE PREPROCESSOR lives in the lexer, as a stack of sources: `include
 * pushes a file, a macro use pushes the macro's text, and every token
 * keeps its own file and line, so an error in an included file points
 * into that file. `ifdef regions are skipped by the lexer's whitespace
 * skipper. Object-like macros only: a macro with arguments is refused.
 *
 * Refused by name, never approximated: generate, tasks, initial blocks,
 * inout ports, x and z in literals, function-like macros, casex/casez.
 * docs/zcc.md's rule holds more strongly here: a wrong netlist does not
 * crash, it programs.
 */

#include "synth.h"

/* -- sources and macros --------------------------------------------------- */

typedef struct src {
    const char *p, *file, *dir;
    int line;
    int macro;                      /* the text of a macro: ends mid-line */
    struct src *up;
} src_t;

typedef struct { const char *name, *body; } macro_t;
static macro_t *macros; static int n_macros, cap_macros;

/* `ifdef nesting: active, and whether a branch was taken */
typedef struct { uint8_t active, taken, outer; } cond_t;
static cond_t conds[32];
static int n_conds;

static int cond_active(void) { return !n_conds || conds[n_conds - 1].active; }

static const char *macro_get(const char *name) {
    int i;
    for (i = n_macros - 1; i >= 0; i--)
        if (macros[i].name && zf_streq(macros[i].name, name)) return macros[i].body;
    return NULL;
}

static void macro_set(const char *name, const char *body) {
    int i;
    for (i = 0; i < n_macros; i++)
        if (macros[i].name && zf_streq(macros[i].name, name)) { macros[i].body = body; return; }
    if (n_macros == cap_macros) {
        int cap = cap_macros ? cap_macros * 2 : 32;
        macro_t *nm = zf_alloc(sizeof(macro_t) * (size_t)cap);
        if (n_macros) zf_memcpy(nm, macros, sizeof(macro_t) * (size_t)n_macros);
        macros = nm;
        cap_macros = cap;
    }
    macros[n_macros].name = name;
    macros[n_macros].body = body;
    n_macros++;
}

/* -- the lexer ------------------------------------------------------------ */

enum { T_EOF, T_ID, T_NUM, T_OP };

typedef struct {
    src_t *s;
    /* the current token */
    int t, tline;
    const char *tfile;
    char text[128];
    int width, is_signed;
    uint64_t value;
} lex_t;

static lex_t L;
static moddef_t *D;
static lib_t *LIB;

#define ERR(...) zf_fatal_at(L.tfile, L.tline, __VA_ARGS__)

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

static char *read_source(const char *path) {
    uint32_t len;
    uint8_t *raw = zf_read_all(path, &len);
    char *s;
    if (!raw) return NULL;
    s = zf_alloc(len + 1);
    zf_memcpy(s, raw, len);
    s[len] = 0;
    return s;
}

static const char *dir_of(const char *path) {
    size_t n = zf_strlen(path);
    char *d;
    while (n && path[n - 1] != '/') n--;
    d = zf_alloc(n + 1);
    zf_memcpy(d, path, n);
    d[n] = 0;
    return d;
}

static void push_file(const char *path) {
    src_t *s = zf_alloc(sizeof(*s));
    char *text = read_source(path);
    if (!text) {
        if (L.s) zf_fatal_at(L.s->file, L.s->line, "cannot open %s%s", path, zf_83_hint(path));
        zf_fatal("cannot open %s%s", path, zf_83_hint(path));
    }
    s->p = text;
    s->file = zf_strdup(path);
    s->dir = dir_of(path);
    s->line = 1;
    s->up = L.s;
    L.s = s;
}

/* the rest of the current line, for a directive's argument */
static char *rest_of_line(void) {
    const char *p = L.s->p, *start;
    char *r, *w;
    while (*p == ' ' || *p == '\t') p++;
    start = p;
    while (*p && *p != '\n') {
        if (p[0] == '\\' && p[1] == '\n') { p += 2; L.s->line++; continue; }
        if (p[0] == '/' && p[1] == '/') break;
        p++;
    }
    r = zf_alloc((size_t)(p - start) + 1);
    for (w = r; start < p; start++) {
        if (start[0] == '\\' && start[1] == '\n') { start++; continue; }
        *w++ = *start;
    }
    while (w > r && (w[-1] == ' ' || w[-1] == '\t' || w[-1] == '\r')) w--;
    *w = 0;
    while (*p && *p != '\n') p++;
    L.s->p = p;
    return r;
}

static const char *word_at(const char **pp) {
    const char *p = *pp;
    char buf[128];
    int n = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (isid(*p) && n < 127) buf[n++] = *p++;
    buf[n] = 0;
    *pp = p;
    return zf_strdup(buf);
}

/* A directive, the backquote already consumed. Returns 1 if the lexer
 * should carry on skipping space. */
static void directive(void) {
    const char *p = L.s->p;
    const char *d = word_at(&p);
    L.s->p = p;
    L.tline = L.s->line;
    L.tfile = L.s->file;
    if (zf_streq(d, "ifdef") || zf_streq(d, "ifndef")) {
        const char *n = word_at(&p);
        int def = macro_get(n) != NULL;
        cond_t c;
        L.s->p = p;
        if (n_conds == 32) ERR("`ifdef nested too deeply");
        c.outer = (uint8_t)cond_active();
        c.active = (uint8_t)(c.outer && (zf_streq(d, "ifdef") ? def : !def));
        c.taken = c.active;
        conds[n_conds++] = c;
        return;
    }
    if (zf_streq(d, "elsif")) {
        const char *n = word_at(&p);
        cond_t *c;
        L.s->p = p;
        if (!n_conds) ERR("`elsif without `ifdef");
        c = &conds[n_conds - 1];
        c->active = (uint8_t)(c->outer && !c->taken && macro_get(n) != NULL);
        if (c->active) c->taken = 1;
        return;
    }
    if (zf_streq(d, "else")) {
        cond_t *c;
        if (!n_conds) ERR("`else without `ifdef");
        c = &conds[n_conds - 1];
        c->active = (uint8_t)(c->outer && !c->taken);
        c->taken = 1;
        return;
    }
    if (zf_streq(d, "endif")) {
        if (!n_conds) ERR("`endif without `ifdef");
        n_conds--;
        return;
    }
    if (!cond_active()) return;             /* anything else, skipped */
    if (zf_streq(d, "define")) {
        const char *n = word_at(&p);
        L.s->p = p;
        if (!*n) ERR("`define needs a name");
        if (*p == '(') ERR("`define %s(...): macros with arguments are not supported", n);
        macro_set(n, rest_of_line());
        return;
    }
    if (zf_streq(d, "undef")) {
        const char *n = word_at(&p);
        int i;
        L.s->p = p;
        for (i = 0; i < n_macros; i++) if (macros[i].name && zf_streq(macros[i].name, n)) macros[i].name = NULL;
        return;
    }
    if (zf_streq(d, "include")) {
        const char *q = L.s->p;
        char path[256];
        int n = 0;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') ERR("`include wants a \"file\"");
        q++;
        while (*q && *q != '"' && *q != '\n' && n < 200) path[n++] = *q++;
        path[n] = 0;
        if (*q == '"') q++;
        L.s->p = q;
        {
            char full[512];
            if (path[0] == '/') zf_fmt(full, sizeof(full), "%s", path);
            else zf_fmt(full, sizeof(full), "%s%s", L.s->dir, path);
            push_file(full);
        }
        return;
    }
    if (zf_streq(d, "timescale") || zf_streq(d, "default_nettype") || zf_streq(d, "resetall") ||
            zf_streq(d, "celldefine") || zf_streq(d, "endcelldefine") || zf_streq(d, "nounconnected_drive") ||
            zf_streq(d, "unconnected_drive")) {
        (void)rest_of_line();
        return;
    }
    {
        /* a macro's use: its text becomes the next source */
        const char *body = macro_get(d);
        src_t *s;
        if (!body) ERR("`%s is not defined", d);
        s = zf_alloc(sizeof(*s));
        s->p = body;
        s->file = L.s->file;
        s->dir = L.s->dir;
        s->line = L.s->line;
        s->macro = 1;
        s->up = L.s;
        L.s = s;
    }
}

static void skip_space(void) {
    for (;;) {
        char c;
        if (!L.s) return;
        c = *L.s->p;
        if (!c) {
            src_t *done = L.s;
            if (!done->up) return;
            L.s = done->up;
            continue;
        }
        if (c == '\n') { if (!L.s->macro) L.s->line++; L.s->p++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { L.s->p++; continue; }
        if (c == '/' && L.s->p[1] == '/') { while (*L.s->p && *L.s->p != '\n') L.s->p++; continue; }
        if (c == '/' && L.s->p[1] == '*') {
            L.s->p += 2;
            while (*L.s->p && !(L.s->p[0] == '*' && L.s->p[1] == '/')) {
                if (*L.s->p == '\n') L.s->line++;
                L.s->p++;
            }
            if (*L.s->p) L.s->p += 2;
            continue;
        }
        if (c == '(' && L.s->p[1] == '*' && L.s->p[2] != ')') {      /* (* attribute *) */
            L.s->p += 2;
            while (*L.s->p && !(L.s->p[0] == '*' && L.s->p[1] == ')')) {
                if (*L.s->p == '\n') L.s->line++;
                L.s->p++;
            }
            if (*L.s->p) L.s->p += 2;
            continue;
        }
        if (c == '`') { L.s->p++; directive(); continue; }
        if (!cond_active()) {
            if (c == '"') {
                L.s->p++;
                while (*L.s->p && *L.s->p != '"' && *L.s->p != '\n') L.s->p++;
                if (*L.s->p == '"') L.s->p++;
            } else {
                L.s->p++;
            }
            continue;
        }
        break;
    }
}

static void next(void) {
    const char *p;
    int n = 0;
    skip_space();
    if (!L.s || !*L.s->p) {
        if (n_conds) zf_fatal_at(L.tfile, L.tline, "`ifdef without `endif");
        L.t = T_EOF;
        L.text[0] = 0;
        return;
    }
    L.tline = L.s->line;
    L.tfile = L.s->file;
    p = L.s->p;
    if (isid0(*p) || *p == '$') {
        while (isid(*p) || (n == 0 && *p == '$')) { if (n < 127) L.text[n++] = *p; p++; }
        L.text[n] = 0;
        L.t = T_ID;
        L.s->p = p;
        return;
    }
    if ((*p >= '0' && *p <= '9') || *p == '\'') {
        uint64_t v = 0;
        int width = 0, base = 10, any = 0, sgn = 0;
        if (*p != '\'') {
            while ((*p >= '0' && *p <= '9') || *p == '_') {
                if (*p != '_') v = v * 10 + (uint64_t)(*p - '0');
                p++;
            }
            while (*p == ' ' || *p == '\t') {
                const char *q = p;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '\'') p = q; else break;
            }
            if (*p != '\'') {
                /* an unsized decimal is a signed 32-bit integer */
                L.t = T_NUM; L.width = 0; L.value = v; L.is_signed = 1; L.s->p = p;
                return;
            }
            if (!v || v > 64) { L.s->p = p; ERR("literal width %u: 1..64 only", (unsigned)v); }
            width = (int)v;
            v = 0;
        }
        p++;                                    /* ' */
        if (*p == 's' || *p == 'S') { sgn = 1; p++; }
        switch (*p) {
        case 'b': case 'B': base = 2; break;
        case 'o': case 'O': base = 8; break;
        case 'd': case 'D': base = 10; break;
        case 'h': case 'H': base = 16; break;
        default: L.s->p = p; ERR("bad literal base '%c'", *p);
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        for (;; p++) {
            int d;
            if (*p == '_') continue;
            if (*p == 'z' || *p == 'Z' || *p == '?') {
                L.s->p = p; ERR("z in a literal: tristate is not supported yet");
            }
            if (*p == 'x' || *p == 'X') {
                L.s->p = p; ERR("x in a literal is not supported (write the value it should take)");
            }
            d = digit(*p, base);
            if (d < 0) break;
            v = v * (uint64_t)base + (uint64_t)d;
            any = 1;
        }
        if (!any) { L.s->p = p; ERR("literal with no digits"); }
        if (width && width < 64) v &= ((uint64_t)1 << width) - 1;
        L.t = T_NUM;
        L.width = width;
        L.is_signed = sgn;
        L.value = v;
        L.s->p = p;
        return;
    }
    if (*p == '"') ERR("strings are not supported");
    {
        static const char *ops3[] = { "===", "!==", "<<<", ">>>", NULL };
        static const char *ops2[] = { "<=", ">=", "==", "!=", "&&", "||", "<<", ">>", "~&", "~|",
            "~^", "^~", "->", "**", "+:", "-:", NULL };
        int i;
        for (i = 0; ops3[i]; i++)
            if (p[0] == ops3[i][0] && p[1] == ops3[i][1] && p[2] == ops3[i][2]) {
                zf_memcpy(L.text, ops3[i], 4); L.s->p = p + 3; L.t = T_OP; return;
            }
        for (i = 0; ops2[i]; i++)
            if (p[0] == ops2[i][0] && p[1] == ops2[i][1]) {
                zf_memcpy(L.text, ops2[i], 3); L.s->p = p + 2; L.t = T_OP; return;
            }
        L.text[0] = *p;
        L.text[1] = 0;
        L.s->p = p + 1;
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
    if (L.t != T_ID) ERR("expected a name, found '%s'", L.t == T_EOF ? "end of file" : L.text);
    s = zf_strdup(L.text);
    next();
    return s;
}

static const char *const reserved[] = {
    "initial", "generate", "genvar", "while", "repeat", "forever", "task", "real",
    "inout", "fork", "join", "casex", "casez", "tri", "supply0", "supply1", "specify",
    "primitive", "wait", "disable", NULL
};

static void refuse_reserved(void) {
    int i;
    for (i = 0; reserved[i]; i++)
        if (L.t == T_ID && zf_streq(L.text, reserved[i])) {
            if (zf_streq(L.text, "inout")) ERR("inout ports are not supported yet");
            ERR("'%s' is not supported yet", L.text);
        }
}

/* -- expressions ------------------------------------------------------------ */

static expr_t *mk(int kind, int op) {
    expr_t *e = zf_alloc(sizeof(*e));
    e->kind = kind;
    e->op = op;
    e->line = L.tline;
    e->file = L.tfile;
    return e;
}

static expr_t *expr(void);

static expr_t **expr_list(const char *close, int *n) {
    expr_t **list = NULL;
    int cap = 0;
    *n = 0;
    if (is(close)) return list;
    do {
        if (*n == cap) {
            expr_t **nl = zf_alloc(sizeof(expr_t *) * (size_t)(cap ? cap * 2 : 4));
            if (*n) zf_memcpy(nl, list, sizeof(expr_t *) * (size_t)*n);
            list = nl;
            cap = cap ? cap * 2 : 4;
        }
        list[(*n)++] = expr();
    } while (accept(","));
    return list;
}

/* a[i], a[i][j] (a memory word's bit), a[h:l] */
static expr_t *selects(expr_t *e) {
    while (accept("[")) {
        expr_t *i = expr();
        if (is("+:") || is("-:")) {
            /* a[start +: width], a[start -: width]: width constant */
            expr_t *r = mk(E_RANGE, is("+:") ? RANGE_UP : RANGE_DOWN);
            next();
            r->a = e;
            r->b = i;
            r->c = expr();
            expect("]");
            e = r;
            continue;
        }
        if (accept(":")) {
            expr_t *r = mk(E_RANGE, 0);
            r->a = e;
            r->b = i;
            r->c = expr();
            expect("]");
            e = r;
        } else {
            expr_t *x = mk(E_INDEX, 0);
            expect("]");
            x->a = e;
            x->b = i;
            e = x;
        }
    }
    return e;
}

static expr_t *primary(void) {
    expr_t *e;
    if (L.t == T_NUM) {
        e = mk(E_NUM, 0);
        e->width = L.width;
        e->is_signed = L.is_signed;
        e->value = L.value;
        next();
        return e;
    }
    if (accept("(")) {
        e = expr();
        expect(")");
        return selects(e);
    }
    if (accept("{")) {
        expr_t *first = expr();
        if (accept("{")) {                      /* {n{...}} */
            e = mk(E_REPL, 0);
            e->a = first;
            e->list = expr_list("}", &e->n);
            expect("}");
            expect("}");
            return e;
        }
        {
            int n, i;
            expr_t **rest;
            e = mk(E_CONCAT, 0);
            if (accept(",")) {
                rest = expr_list("}", &n);
            } else {
                rest = NULL;
                n = 0;
            }
            e->list = zf_alloc(sizeof(expr_t *) * (size_t)(n + 1));
            e->list[0] = first;
            for (i = 0; i < n; i++) e->list[i + 1] = rest[i];
            e->n = n + 1;
            expect("}");
            return e;
        }
    }
    if (L.t == T_ID) {
        refuse_reserved();
        e = mk(E_ID, 0);
        e->name = ident();
        if (accept("(")) {
            e->kind = E_CALL;
            e->list = expr_list(")", &e->n);
            expect(")");
            return e;
        }
        if (e->name[0] == '$') ERR("system identifier %s is not supported", e->name);
        return selects(e);
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

static const struct { const char *t; int op, prec; } bins[] = {
    { "||", OP_LOR, 1 }, { "&&", OP_LAND, 2 }, { "|", OP_OR, 3 }, { "^", OP_XOR, 4 },
    { "~^", OP_XNOR, 4 }, { "^~", OP_XNOR, 4 }, { "&", OP_AND, 5 }, { "==", OP_EQ, 6 },
    { "!=", OP_NE, 6 }, { "===", OP_EQ, 6 }, { "!==", OP_NE, 6 }, { "<", OP_LT, 7 },
    { "<=", OP_LE, 7 }, { ">", OP_GT, 7 }, { ">=", OP_GE, 7 }, { "<<", OP_SHL, 8 },
    { ">>", OP_SHR, 8 }, { "<<<", OP_SHL, 8 }, { ">>>", OP_ASHR, 8 }, { "+", OP_ADD, 9 },
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

/* -- statements -------------------------------------------------------------- */

static stmt_t *mks(int kind) {
    stmt_t *s = zf_alloc(sizeof(*s));
    s->kind = kind;
    s->line = L.tline;
    s->file = L.tfile;
    return s;
}

static stmt_t *statement(void);

static expr_t *lvalue(void) {
    expr_t *e = primary();
    if (e->kind != E_ID && e->kind != E_INDEX && e->kind != E_RANGE && e->kind != E_CONCAT)
        zf_fatal_at(e->file, e->line, "not something that can be assigned");
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
    if (accept("for")) {
        s = mks(S_FOR);
        expect("(");
        s->var = ident();
        expect("=");
        s->init = expr();
        expect(";");
        s->cond = expr();
        expect(";");
        if (!zf_streq(ident(), s->var)) ERR("a for loop must step its own variable, %s", s->var);
        expect("=");
        s->step = expr();
        expect(")");
        s->then = statement();
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
                it->vals = expr_list(":", &it->n);
                expect(":");
            }
            it->body = statement();
        }
        s->items = items;
        s->n_items = n;
        return s;
    }
    if (L.t == T_ID && L.text[0] == '$') {
        /* $display and friends: simulation only, ignored as yosys does */
        (void)primary();
        expect(";");
        return mks(S_BLOCK);
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

/* -- declarations -------------------------------------------------------------- */

#define GROW(arr, n, cap, T) do { if ((n) == (cap)) {                     \
    int nc_ = (cap) ? (cap) * 2 : 16;                                     \
    T *na_ = zf_alloc(sizeof(T) * (size_t)nc_);                           \
    if (n) zf_memcpy(na_, (arr), sizeof(T) * (size_t)(n));                \
    (arr) = na_; (cap) = nc_; } } while (0)

static int find_decl(const char *name) {
    int i;
    for (i = 0; i < D->n_decl; i++) if (zf_streq(D->decls[i].name, name)) return i;
    return -1;
}

static int find_pdecl(const char *name) {
    int i;
    for (i = 0; i < D->n_pdecl; i++) if (zf_streq(D->pdecls[i].name, name)) return i;
    return -1;
}

/* kind -1: a name in a non-ANSI port list, direction to follow */
static decl_t *add_decl(const char *name, int kind, expr_t *msb, expr_t *lsb, int is_reg,
        int is_signed, int line) {
    int i = find_decl(name);
    decl_t *d;
    if (find_pdecl(name) >= 0) zf_fatal_at(L.tfile, line, "'%s' is already a parameter", name);
    if (i >= 0) {
        d = &D->decls[i];
        if (kind == SIG_IN || kind == SIG_OUT) {
            if (d->kind != -1) zf_fatal_at(L.tfile, line, "port '%s' declared twice", name);
            d->kind = kind;
            d->msb = msb; d->lsb = lsb;
            d->is_reg |= is_reg;
            d->is_signed |= is_signed;
            return d;
        }
        if (d->kind == SIG_IN || d->kind == SIG_OUT || d->kind == -1) {
            /* `reg [7:0] q;` after `output [7:0] q;` */
            if (is_reg && d->kind == SIG_IN) zf_fatal_at(L.tfile, line, "input '%s' cannot be a reg", name);
            if (!d->msb && msb) { d->msb = msb; d->lsb = lsb; }
            d->is_reg |= is_reg;
            d->is_signed |= is_signed;
            return d;
        }
        zf_fatal_at(L.tfile, line, "'%s' declared twice", name);
    }
    GROW(D->decls, D->n_decl, D->cap_decl, decl_t);
    d = &D->decls[D->n_decl++];
    zf_memset(d, 0, sizeof(*d));
    d->name = name;
    d->kind = kind;
    d->msb = msb; d->lsb = lsb;
    d->is_reg = is_reg;
    d->is_signed = is_signed;
    d->line = line;
    return d;
}

static void range(expr_t **msb, expr_t **lsb) {
    *msb = *lsb = NULL;
    if (accept("[")) {
        *msb = expr();
        expect(":");
        *lsb = expr();
        expect("]");
    }
}

static void add_pdecl(const char *name, expr_t *v, expr_t *m, expr_t *l, int sg, int local, int line) {
    pdecl_t *p;
    if (find_pdecl(name) >= 0) zf_fatal_at(L.tfile, line, "parameter '%s' declared twice", name);
    GROW(D->pdecls, D->n_pdecl, D->cap_pdecl, pdecl_t);
    p = &D->pdecls[D->n_pdecl++];
    p->name = name;
    p->value = v;
    p->msb = m;
    p->lsb = l;
    p->is_signed = sg;
    p->local = local;
    p->line = line;
}

/* after `parameter`/`localparam`: [integer|signed] [range] NAME = expr {, NAME = expr} */
static void param_decl(int local) {
    expr_t *m, *l;
    int sg = 0;
    if (accept("integer")) {
        /* an integer parameter: 32 bits, signed */
        sg = 1;
        m = mk(E_NUM, 0); m->value = 31;
        l = mk(E_NUM, 0);
    } else {
        sg = accept("signed");
        range(&m, &l);
    }
    do {
        const char *n;
        int line = L.tline;
        if (is("parameter") || is("localparam")) break;
        n = ident();
        expect("=");
        add_pdecl(n, expr(), m, l, sg, local, line);
    } while (accept(",") && !is("parameter") && !is("localparam"));
}

/* ANSI: input [wire|reg] [signed] [range] a, b, output ...; or non-ANSI names */
static void port_list(void) {
    int dir = -1, is_reg = 0, is_signed = 0, cap = 0;
    expr_t *m = NULL, *l = NULL;
    if (accept(")")) return;
    do {
        const char *n;
        refuse_reserved();
        if (accept("input")) {
            dir = SIG_IN; is_reg = 0;
            accept("wire");
            is_signed = accept("signed");
            range(&m, &l);
        } else if (accept("output")) {
            dir = SIG_OUT;
            is_reg = accept("reg");
            if (!is_reg) accept("wire");
            is_signed = accept("signed");
            range(&m, &l);
        }
        n = ident();
        if (D->n_ports == cap) {
            const char **np = zf_alloc(sizeof(char *) * (size_t)(cap ? cap * 2 : 16));
            if (D->n_ports) zf_memcpy(np, D->ports, sizeof(char *) * (size_t)D->n_ports);
            D->ports = np;
            cap = cap ? cap * 2 : 16;
        }
        D->ports[D->n_ports++] = n;
        add_decl(n, dir, m, l, is_reg, is_signed, L.tline);
        /* `input b,\n);` -- a trailing comma, which yosys accepts and
         * this tree's RTL uses */
    } while (accept(",") && !is(")"));
    expect(")");
}

static void add_assign(expr_t *lhs, expr_t *rhs, int line) {
    GROW(D->assigns, D->n_assign, D->cap_assign, assign_t);
    D->assigns[D->n_assign].lhs = lhs;
    D->assigns[D->n_assign].rhs = rhs;
    D->assigns[D->n_assign].line = line;
    D->assigns[D->n_assign].file = L.tfile;
    D->n_assign++;
}

static void always_block(int line) {
    always_t *a;
    GROW(D->always, D->n_always, D->cap_always, always_t);
    a = &D->always[D->n_always++];
    zf_memset(a, 0, sizeof(*a));
    a->line = line;
    a->file = L.tfile;
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

/* wire/reg/integer [signed] [range] a [= e] [\[lo:hi\]], ...; */
static void net_decl(int kind, int line) {
    int is_signed = accept("signed");
    expr_t *m = NULL, *l = NULL;
    if (kind != SIG_INT) range(&m, &l);
    do {
        const char *n = ident();
        decl_t *d = add_decl(n, kind, m, l, kind == SIG_REG, is_signed || kind == SIG_INT, line);
        if (kind == SIG_INT) {
            d->msb = mk(E_NUM, 0); d->msb->value = 31;
            d->lsb = mk(E_NUM, 0);
        }
        if (is("[")) {
            next();
            d->alo = expr();
            expect(":");
            d->ahi = expr();
            expect("]");
            if (kind != SIG_REG) ERR("only a reg can be a memory");
        }
        if (accept("=")) {
            expr_t *e = expr();
            if (kind == SIG_REG) {
                d->init = e;
            } else {
                expr_t *id = mk(E_ID, 0);
                id->name = n;
                add_assign(id, e, line);
            }
        }
    } while (accept(","));
    expect(";");
}

/* function [signed] [range] name; input ...; reg ...; begin ... end endfunction,
 * or function ... name(input [range] a, ...); */
static void function_decl(int line) {
    funcdef_t *f;
    moddef_t *saved = D, scope;
    GROW(D->funcs, D->n_func, D->cap_func, funcdef_t);
    f = &D->funcs[D->n_func++];
    zf_memset(f, 0, sizeof(*f));
    f->line = line;
    accept("automatic");
    f->is_signed = accept("signed");
    accept("integer");
    range(&f->msb, &f->lsb);
    f->name = ident();
    /* the function's own declarations, in a scope of their own */
    zf_memset(&scope, 0, sizeof(scope));
    scope.name = f->name;
    D = &scope;
    if (accept("(")) {
        int sg = 0;
        expr_t *m = NULL, *l = NULL;
        do {
            if (accept("input")) { accept("wire"); sg = accept("signed"); range(&m, &l); }
            add_decl(ident(), SIG_IN, m, l, 0, sg, L.tline);
        } while (accept(","));
        expect(")");
    }
    expect(";");
    for (;;) {
        int ln = L.tline;
        if (accept("input")) {
            int sg;
            expr_t *m, *l;
            accept("wire");
            sg = accept("signed");
            range(&m, &l);
            do add_decl(ident(), SIG_IN, m, l, 0, sg, ln); while (accept(","));
            expect(";");
        } else if (accept("reg")) {
            net_decl(SIG_REG, ln);
        } else if (accept("integer")) {
            net_decl(SIG_INT, ln);
        } else if (is("parameter") || is("localparam")) {
            ERR("parameters inside a function are not supported");
        } else {
            break;
        }
    }
    f->body = statement();
    expect("endfunction");
    f->decls = scope.decls;
    f->n_decl = scope.n_decl;
    if (scope.n_assign) zf_fatal_at(L.tfile, line, "function %s: an initialised declaration", f->name);
    D = saved;
}

/* NAME [#(params)] INST (ports); -- after NAME has been read */
static void instance(const char *mod, int line) {
    inst_t *in;
    GROW(D->insts, D->n_inst, D->cap_inst, inst_t);
    in = &D->insts[D->n_inst++];
    zf_memset(in, 0, sizeof(*in));
    in->mod = mod;
    in->line = line;
    if (accept("#")) {
        int cap = 0;
        expect("(");
        if (!is(")")) do {
            conn_t *c;
            if (in->n_params == cap) {
                conn_t *nc = zf_alloc(sizeof(conn_t) * (size_t)(cap ? cap * 2 : 8));
                if (in->n_params) zf_memcpy(nc, in->params, sizeof(conn_t) * (size_t)in->n_params);
                in->params = nc;
                cap = cap ? cap * 2 : 8;
            }
            c = &in->params[in->n_params++];
            if (accept(".")) {
                c->name = ident();
                expect("(");
                c->e = expr();
                expect(")");
            } else {
                c->name = NULL;
                c->e = expr();
            }
        } while (accept(","));
        expect(")");
    }
    in->name = ident();
    if (is("[")) ERR("arrays of instances are not supported");
    expect("(");
    {
        int cap = 0;
        if (!is(")")) do {
            conn_t *c;
            if (in->n_ports == cap) {
                conn_t *nc = zf_alloc(sizeof(conn_t) * (size_t)(cap ? cap * 2 : 16));
                if (in->n_ports) zf_memcpy(nc, in->ports, sizeof(conn_t) * (size_t)in->n_ports);
                in->ports = nc;
                cap = cap ? cap * 2 : 16;
            }
            c = &in->ports[in->n_ports++];
            if (accept(".")) {
                c->name = ident();
                expect("(");
                c->e = is(")") ? NULL : expr();
                expect(")");
            } else {
                /* an empty slot, `(a, , c)`, is an unconnected port */
                c->name = NULL;
                c->e = (is(",") || is(")")) ? NULL : expr();
            }
        } while (accept(","));
    }
    expect(")");
    if (is(",")) ERR("several instances in one statement are not supported");
    expect(";");
}

static void module(void) {
    moddef_t *m = zf_alloc(sizeof(*m));
    m->line = L.tline;
    m->file = L.tfile;
    D = m;
    m->name = ident();
    if (lib_find(LIB, m->name)) ERR("module %s is defined twice", m->name);
    if (accept("#")) {
        expect("(");
        if (!is(")")) do {
            if (!accept("parameter")) accept("localparam");
            param_decl(0);
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
            /* non-ANSI: input [wire|reg] [signed] [range] a, b; */
            int dir = is("input") ? SIG_IN : SIG_OUT, is_reg = 0, sg;
            expr_t *m2, *l2;
            next();
            if (dir == SIG_OUT) is_reg = accept("reg");
            if (!is_reg) accept("wire");
            sg = accept("signed");
            range(&m2, &l2);
            do add_decl(ident(), dir, m2, l2, is_reg, sg, line); while (accept(","));
            expect(";");
        } else if (accept("wire")) {
            net_decl(SIG_WIRE, line);
        } else if (accept("reg")) {
            net_decl(SIG_REG, line);
        } else if (accept("integer")) {
            net_decl(SIG_INT, line);
        } else if (accept("parameter")) {
            param_decl(0);
            expect(";");
        } else if (accept("localparam")) {
            param_decl(1);
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
        } else if (accept("function")) {
            function_decl(line);
        } else if (L.t == T_ID) {
            const char *mod = ident();
            instance(mod, line);
        } else {
            ERR("unexpected '%s'", L.text);
        }
    }
    {
        int i;
        for (i = 0; i < m->n_decl; i++)
            if (m->decls[i].kind < 0)
                zf_fatal_at(m->file, m->decls[i].line, "port '%s' has no direction", m->decls[i].name);
    }
    GROW(LIB->mods, LIB->n_mod, LIB->cap_mod, moddef_t *);
    LIB->mods[LIB->n_mod++] = m;
}

moddef_t *lib_find(const lib_t *lib, const char *name) {
    int i;
    for (i = 0; i < lib->n_mod; i++) if (zf_streq(lib->mods[i]->name, name)) return lib->mods[i];
    return NULL;
}

void synth_parse(const char *path, lib_t *lib) {
    int before = lib->n_mod;
    zf_memset(&L, 0, sizeof(L));
    L.tfile = path;
    LIB = lib;
    push_file(path);
    next();
    while (L.t != T_EOF) {
        if (!accept("module") && !accept("macromodule"))
            ERR("expected 'module', found '%s'", L.text);
        module();
    }
    if (lib->n_mod == before) zf_fatal("%s: no module", path);
}
