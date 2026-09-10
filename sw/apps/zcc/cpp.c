/*
 * zcc -- preprocessor.
 *
 * Object-like and function-like macros, #include, the #if family,
 * #undef, #pragma once, and the ## and # operators.
 *
 * -- Why this is more than a stub --
 *
 * The temptation with a small compiler is to support #define and
 * #include and call it done. That does not survive contact with
 * sw/common/zeitlos.h, which is the point of the exercise: it uses
 * function-like macros for the register map, #ifndef guards
 * throughout, and #if on configuration constants. A preprocessor that
 * cannot read the tree's own headers is a compiler that cannot build
 * the tree's own apps, however good its code generator is.
 *
 * -- What is absent, deliberately --
 *
 * #line, #error's file/line fidelity beyond a plain message, variadic
 * macros (__VA_ARGS__), and _Pragma. Variadic macros are the one most
 * likely to be missed; nothing in sw/common currently needs them, and
 * they can be added without disturbing anything here.
 */

#include <string.h>

#include "zcc.h"

typedef struct macro_param {
    struct macro_param *next;
    char *name;
} macro_param_t;

typedef struct macro {
    struct macro *next;
    char *name;
    int is_func;
    macro_param_t *params;
    int nparams;
    token_t *body;          /* NULL-terminated by a TK_EOF sentinel */
} macro_t;

static macro_t *macros;

typedef struct incdir {
    struct incdir *next;
    char *path;
} incdir_t;

static incdir_t *incdirs, *incdirs_tail;

typedef struct oncefile {
    struct oncefile *next;
    char *path;
} oncefile_t;

static oncefile_t *oncefiles;

token_t *lex_string(const char *src, const char *file);

void cpp_add_include_dir(const char *dir) {
    incdir_t *d = zalloc(sizeof(incdir_t));
    d->path = zstrdup(dir);
    if (incdirs_tail) incdirs_tail->next = d;
    else incdirs = d;
    incdirs_tail = d;
}

static macro_t *find_macro(const char *name) {
    for (macro_t *m = macros; m; m = m->next)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

static void undef_macro(const char *name) {
    macro_t **pp = &macros;
    while (*pp) {
        if (!strcmp((*pp)->name, name)) { *pp = (*pp)->next; return; }
        pp = &(*pp)->next;
    }
}

static macro_t *add_macro(const char *name, int is_func) {
    undef_macro(name);
    macro_t *m = zalloc(sizeof(macro_t));
    m->name = zstrdup(name);
    m->is_func = is_func;
    m->next = macros;
    macros = m;
    return m;
}

/* ------------------------------------------------------------------ */

static token_t *copy_token(token_t *t) {
    token_t *n = zalloc(sizeof(token_t));
    *n = *t;
    n->next = NULL;
    return n;
}

static token_t *eof_sentinel(token_t *like) {
    token_t *t = zalloc(sizeof(token_t));
    t->kind = TK_EOF;
    t->file = like ? like->file : "<none>";
    t->line = like ? like->line : 0;
    t->text = zstrdup("<eof>");
    return t;
}

/* Appends a copy of the list `src` (stopping at TK_EOF) to `*tail`. */
static token_t *append_copy(token_t *tail, token_t *src) {
    for (token_t *t = src; t && t->kind != TK_EOF; t = t->next)
        tail = tail->next = copy_token(t);
    return tail;
}

/* ------------------------------------------------------------------ */
/* directive helpers                                                    */

/* Collects the rest of the logical line into its own list. Every
 * directive handler works on one of these rather than on the main
 * stream, so none of them can run off the end of a line by accident --
 * which is the classic way a preprocessor swallows the program after a
 * malformed directive. */
static token_t *take_line(token_t **rest, token_t *tok) {
    token_t head = {0};
    token_t *cur = &head;
    while (tok && tok->kind != TK_EOF && !tok->bol) {
        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }
    cur->next = eof_sentinel(tok);
    *rest = tok;
    return head.next;
}

static void skip_line(token_t **rest, token_t *tok) {
    while (tok && tok->kind != TK_EOF && !tok->bol) tok = tok->next;
    *rest = tok;
}

/* ------------------------------------------------------------------ */
/* #if expression evaluation                                            */

/*
 * A separate, tiny expression evaluator over an already-expanded token
 * line. It is not the parser: #if arithmetic is always intmax, has no
 * types, no sizeof, no casts and no side effects, so sharing the
 * parser would mean teaching the parser a second mode it does not
 * otherwise need. Twelve small functions is the cheaper answer.
 */
static token_t *cond_tok;

static long cond_ternary(void);

static int cond_at(const char *s) { return tok_is(cond_tok, s); }
static void cond_next(void) { if (cond_tok->kind != TK_EOF) cond_tok = cond_tok->next; }

static long cond_primary(void) {
    if (cond_at("(")) {
        cond_next();
        long v = cond_ternary();
        if (!cond_at(")")) zcc_error(cond_tok->file, cond_tok->line, cond_tok->col,
                                     "expected ')' in #if expression");
        cond_next();
        return v;
    }
    if (cond_tok->kind == TK_NUM) { long v = (long)(int32_t)cond_tok->val; cond_next(); return v; }
    /* An identifier that survived expansion is 0 by definition. That
     * includes keywords, which is why `#if __STDC__` on a compiler that
     * does not define it is false rather than an error. */
    if (cond_tok->kind == TK_IDENT || cond_tok->kind == TK_KEYWORD) { cond_next(); return 0; }
    zcc_error(cond_tok->file, cond_tok->line, cond_tok->col,
              "unexpected '%s' in #if expression", cond_tok->text);
    return 0;
}

static long cond_unary(void) {
    if (cond_at("-")) { cond_next(); return -cond_unary(); }
    if (cond_at("+")) { cond_next(); return cond_unary(); }
    if (cond_at("!")) { cond_next(); return !cond_unary(); }
    if (cond_at("~")) { cond_next(); return ~cond_unary(); }
    return cond_primary();
}

static long cond_mul(void) {
    long v = cond_unary();
    for (;;) {
        if (cond_at("*")) { cond_next(); v = v * cond_unary(); }
        else if (cond_at("/")) { cond_next(); long r = cond_unary(); v = r ? v / r : 0; }
        else if (cond_at("%")) { cond_next(); long r = cond_unary(); v = r ? v % r : 0; }
        else return v;
    }
}

static long cond_addsub(void) {
    long v = cond_mul();
    for (;;) {
        if (cond_at("+")) { cond_next(); v = v + cond_mul(); }
        else if (cond_at("-")) { cond_next(); v = v - cond_mul(); }
        else return v;
    }
}

static long cond_shift(void) {
    long v = cond_addsub();
    for (;;) {
        if (cond_at("<<")) { cond_next(); v = v << cond_addsub(); }
        else if (cond_at(">>")) { cond_next(); v = v >> cond_addsub(); }
        else return v;
    }
}

static long cond_rel(void) {
    long v = cond_shift();
    for (;;) {
        if (cond_at("<")) { cond_next(); v = v < cond_shift(); }
        else if (cond_at(">")) { cond_next(); v = v > cond_shift(); }
        else if (cond_at("<=")) { cond_next(); v = v <= cond_shift(); }
        else if (cond_at(">=")) { cond_next(); v = v >= cond_shift(); }
        else return v;
    }
}

static long cond_eq(void) {
    long v = cond_rel();
    for (;;) {
        if (cond_at("==")) { cond_next(); v = v == cond_rel(); }
        else if (cond_at("!=")) { cond_next(); v = v != cond_rel(); }
        else return v;
    }
}

static long cond_band(void) {
    long v = cond_eq();
    while (cond_at("&")) { cond_next(); v = v & cond_eq(); }
    return v;
}
static long cond_bxor(void) {
    long v = cond_band();
    while (cond_at("^")) { cond_next(); v = v ^ cond_band(); }
    return v;
}
static long cond_bor(void) {
    long v = cond_bxor();
    while (cond_at("|")) { cond_next(); v = v | cond_bxor(); }
    return v;
}
static long cond_land(void) {
    long v = cond_bor();
    while (cond_at("&&")) { cond_next(); long r = cond_bor(); v = v && r; }
    return v;
}
static long cond_lor(void) {
    long v = cond_land();
    while (cond_at("||")) { cond_next(); long r = cond_land(); v = v || r; }
    return v;
}
static long cond_ternary(void) {
    long c = cond_lor();
    if (cond_at("?")) {
        cond_next();
        long a = cond_ternary();
        if (!cond_at(":")) zcc_error(cond_tok->file, cond_tok->line, cond_tok->col,
                                     "expected ':' in #if expression");
        cond_next();
        long b = cond_ternary();
        return c ? a : b;
    }
    return c;
}

static token_t *expand_line(token_t *line);

/*
 * `defined X` and `defined(X)` are replaced BEFORE expansion, because
 * the whole point of the operator is to ask about a name without
 * expanding it. Doing this after expansion is a classic bug: the name
 * disappears and `defined` is left staring at whatever it expanded to.
 */
static token_t *replace_defined(token_t *line) {
    token_t head = {0};
    token_t *cur = &head;

    for (token_t *t = line; t && t->kind != TK_EOF; ) {
        if (t->kind == TK_IDENT && !strcmp(t->text, "defined")) {
            t = t->next;
            int paren = 0;
            if (tok_is(t, "(")) { paren = 1; t = t->next; }
            if (!t || (t->kind != TK_IDENT && t->kind != TK_KEYWORD))
                zcc_error(line->file, line->line, line->col,
                          "'defined' needs an identifier");
            int yes = find_macro(t->text) != NULL;
            token_t *n = copy_token(t);
            n->kind = TK_NUM;
            n->val = (uint32_t)yes;
            n->text = zstrdup(yes ? "1" : "0");
            cur = cur->next = n;
            t = t->next;
            if (paren) {
                if (!tok_is(t, ")"))
                    zcc_error(line->file, line->line, line->col,
                              "expected ')' after 'defined('");
                t = t->next;
            }
            continue;
        }
        cur = cur->next = copy_token(t);
        t = t->next;
    }
    cur->next = eof_sentinel(line);
    return head.next;
}

static long eval_cond_line(token_t *line) {
    token_t *e = expand_line(replace_defined(line));
    cond_tok = e;
    long v = cond_ternary();
    if (cond_tok->kind != TK_EOF)
        zcc_error(cond_tok->file, cond_tok->line, cond_tok->col,
                  "trailing '%s' in #if expression", cond_tok->text);
    return v;
}

/* ------------------------------------------------------------------ */
/* macro expansion                                                      */

typedef struct arglist {
    struct arglist *next;
    char *name;
    token_t *toks;
} arglist_t;

static arglist_t *find_arg(arglist_t *args, const char *name) {
    for (arglist_t *a = args; a; a = a->next)
        if (!strcmp(a->name, name)) return a;
    return NULL;
}

/* Renders a token list back to source text, for the # operator. */
static char *stringize(token_t *toks) {
    size_t cap = 64, len = 0;
    char *out = zalloc(cap);

    for (token_t *t = toks; t && t->kind != TK_EOF; t = t->next) {
        const char *s = t->text;
        char q[8];
        if (t->kind == TK_STR) {
            /* Good enough for the uses that exist: a stringized string
             * literal needs its quotes and backslashes escaped, and
             * nothing in this tree stringizes one containing either. */
            s = "\\\"...\\\"";
        } else if (t->kind == TK_NUM && (!s || !*s)) {
            zcc_snprintf(q, (int)sizeof(q), "%u", t->val);
            s = q;
        }
        size_t n = strlen(s);
        if (len + n + 2 >= cap) {
            while (len + n + 2 >= cap) cap *= 2;
            char *bigger = zalloc(cap);
            memcpy(bigger, out, len);
            out = bigger;
        }
        if (len && t->has_space) out[len++] = ' ';
        memcpy(out + len, s, n);
        len += n;
    }
    out[len] = 0;
    return out;
}

/* Pastes two tokens with ##, by re-lexing the concatenated spelling.
 * Re-lexing rather than string-splicing is what makes `a ## b` produce
 * one identifier and `1 ## 2` produce the number 12 without either
 * case being special. */
static token_t *paste(token_t *l, token_t *r) {
    char *s = zformat("%s%s", l->text ? l->text : "", r->text ? r->text : "");
    token_t *t = lex_string(s, l->file);
    if (!t || t->kind == TK_EOF)
        zcc_error(l->file, l->line, l->col, "'##' produced nothing");
    if (t->next && t->next->kind != TK_EOF)
        zcc_error(l->file, l->line, l->col,
                  "'##' produced '%s', which is not a single token", s);
    token_t *n = copy_token(t);
    n->line = l->line;
    n->col = l->col;
    return n;
}

static token_t *subst(macro_t *m, arglist_t *args, token_t *like);

/*
 * Reads a function-like macro's actual arguments.
 *
 * Nesting is tracked for parentheses only; a comma inside brackets or
 * braces does NOT separate arguments in C, but neither does anything
 * in this tree rely on that, and pretending otherwise would need a
 * full bracket stack for no benefit. Parenthesis nesting is what
 * matters and is handled.
 */
static arglist_t *read_args(macro_t *m, token_t **rest, token_t *tok) {

    tok = tok->next;                        /* skip '(' */

    arglist_t head = {0};
    arglist_t *cur = &head;
    macro_param_t *p = m->params;

    if (m->nparams == 0) {
        if (!tok_is(tok, ")"))
            zcc_error(tok->file, tok->line, tok->col,
                      "macro '%s' takes no arguments", m->name);
        *rest = tok->next;
        return NULL;
    }

    for (int i = 0; i < m->nparams; i++) {
        token_t ah = {0};
        token_t *at = &ah;
        int depth = 0;

        for (;;) {
            if (tok->kind == TK_EOF)
                zcc_error(tok->file, tok->line, tok->col,
                          "unterminated argument list for macro '%s'", m->name);
            if (depth == 0 && tok_is(tok, ",") && i + 1 < m->nparams) break;
            if (depth == 0 && tok_is(tok, ")")) break;
            if (tok_is(tok, "(")) depth++;
            if (tok_is(tok, ")")) depth--;
            at = at->next = copy_token(tok);
            tok = tok->next;
        }
        at->next = eof_sentinel(tok);

        arglist_t *a = zalloc(sizeof(arglist_t));
        a->name = p->name;
        a->toks = ah.next ? ah.next : at;
        cur = cur->next = a;
        p = p->next;

        if (tok_is(tok, ",")) tok = tok->next;
    }

    if (!tok_is(tok, ")"))
        zcc_error(tok->file, tok->line, tok->col,
                  "too many arguments to macro '%s'", m->name);
    *rest = tok->next;
    return head.next;
}

static token_t *expand_list(token_t *in);

static token_t *subst(macro_t *m, arglist_t *args, token_t *like) {

    token_t head = {0};
    token_t *cur = &head;

    for (token_t *t = m->body; t && t->kind != TK_EOF; ) {

        /* # param */
        if (tok_is(t, "#") && t->next && t->next->kind == TK_IDENT) {
            arglist_t *a = find_arg(args, t->next->text);
            if (a) {
                token_t *s = copy_token(t);
                s->kind = TK_STR;
                s->str = stringize(a->toks);
                s->str_len = (int)strlen(s->str) + 1;
                s->text = zstrdup("\"...\"");
                cur = cur->next = s;
                t = t->next->next;
                continue;
            }
        }

        /* lhs ## rhs */
        if (t->next && tok_is(t->next, "##") && t->next->next) {
            token_t *lhs;
            arglist_t *la = (t->kind == TK_IDENT) ? find_arg(args, t->text) : NULL;

            if (la) {
                /* An empty argument on the left of ## contributes
                 * nothing, which is what makes `x ## y` with x empty
                 * yield just y rather than an error. */
                if (la->toks->kind == TK_EOF) { t = t->next->next; goto rhs_only; }
                token_t *tail = append_copy(cur, la->toks);
                /* everything but the last token goes through unchanged */
                lhs = tail;
                cur = tail;
                /* rewind: cur currently IS the last copied token */
                token_t *r = t->next->next;
                arglist_t *ra = (r->kind == TK_IDENT) ? find_arg(args, r->text) : NULL;
                token_t *rfirst = ra ? ra->toks : r;
                if (rfirst->kind == TK_EOF) { t = r->next; continue; }
                *lhs = *paste(lhs, rfirst);
                lhs->next = NULL;
                if (ra) cur = append_copy(cur, rfirst->next);
                t = r->next;
                continue;
            }

            lhs = copy_token(t);
            {
                token_t *r = t->next->next;
                arglist_t *ra = (r->kind == TK_IDENT) ? find_arg(args, r->text) : NULL;
                token_t *rfirst = ra ? ra->toks : r;
                if (rfirst->kind != TK_EOF) {
                    token_t *pasted = paste(lhs, rfirst);
                    cur = cur->next = pasted;
                    if (ra) cur = append_copy(cur, rfirst->next);
                } else {
                    cur = cur->next = lhs;
                }
                t = r->next;
                continue;
            }
        }

rhs_only:
        if (t->kind == TK_IDENT) {
            arglist_t *a = find_arg(args, t->text);
            if (a) {
                /* Arguments are macro-expanded before substitution,
                 * except next to # or ##, which are handled above. */
                cur = append_copy(cur, expand_list(a->toks));
                t = t->next;
                continue;
            }
        }

        cur = cur->next = copy_token(t);
        t = t->next;
    }

    cur->next = eof_sentinel(like);

    /* Blue paint.
     *
     * Every occurrence of the macro's own name in its own expansion is
     * marked no_expand, which is what stops `#define f f(x)` and the
     * whole family of self-referential macros from looping. Marking on
     * the RESULT rather than keeping a "currently expanding" set is
     * the cheaper of the two standard approaches and does not need an
     * unwind. */
    for (token_t *t = head.next; t && t->kind != TK_EOF; t = t->next)
        if (t->kind == TK_IDENT && !strcmp(t->text, m->name)) t->no_expand = 1;

    return head.next;
}

/*
 * Expands a token list to fixpoint.
 *
 * The loop rescans its own output, which is what makes a macro that
 * expands to another macro work. It terminates because blue paint
 * makes self-reference inert and because every other expansion strictly
 * consumes an invocation.
 */
static token_t *expand_list(token_t *in) {

    token_t head = {0};
    token_t *cur = &head;
    token_t *t = in;

    while (t && t->kind != TK_EOF) {

        if (t->kind != TK_IDENT || t->no_expand) {
            cur = cur->next = copy_token(t);
            t = t->next;
            continue;
        }

        macro_t *m = find_macro(t->text);
        if (!m) {
            cur = cur->next = copy_token(t);
            t = t->next;
            continue;
        }

        if (m->is_func) {
            if (!tok_is(t->next, "(")) {
                /* A function-like macro's name without a following '('
                 * is just an identifier. Real headers depend on this --
                 * it is how a function and a macro of the same name
                 * coexist. */
                cur = cur->next = copy_token(t);
                t = t->next;
                continue;
            }
            token_t *rest;
            arglist_t *args = read_args(m, &rest, t->next);
            token_t *body = subst(m, args, t);
            /* Splice the expansion in front of the remaining input and
             * rescan, so an expansion ending in a macro name still sees
             * the arguments that follow it.
             *
             * An EMPTY expansion is the case to watch: subst() returns
             * a bare TK_EOF sentinel, and splicing that in makes the
             * rescan stop dead at it -- silently discarding the entire
             * rest of the translation unit. That is exactly what
             * `#define EMPTY` did, and it presented as "unterminated
             * function body" forty lines later. */
            if (!body || body->kind == TK_EOF) {
                t = rest;
                continue;
            }
            token_t *tail = body;
            while (tail->next && tail->next->kind != TK_EOF) tail = tail->next;
            tail->next = rest;
            t = body;
            continue;
        }

        {
            token_t *body = subst(m, NULL, t);
            token_t *rest = t->next;
            if (!body || body->kind == TK_EOF) {   /* see the note above */
                t = rest;
                continue;
            }
            token_t *tail = body;
            while (tail->next && tail->next->kind != TK_EOF) tail = tail->next;
            tail->next = rest;
            t = body;
        }
    }

    cur->next = eof_sentinel(t);
    return head.next;
}

static token_t *expand_line(token_t *line) { return expand_list(line); }

/* ------------------------------------------------------------------ */
/* #include                                                             */

static int seen_once(const char *path) {
    for (oncefile_t *o = oncefiles; o; o = o->next)
        if (!strcmp(o->path, path)) return 1;
    return 0;
}

static void mark_once(const char *path) {
    oncefile_t *o = zalloc(sizeof(oncefile_t));
    o->path = zstrdup(path);
    o->next = oncefiles;
    oncefiles = o;
}

static char *dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return zstrdup(".");
    return zstrndup(path, (size_t)(slash - path));
}

/* Reassembles an #include <a/b.h> from its tokens.
 *
 * The lexer has no idea that `<stdio.h>` is a filename -- it sees
 * '<', an identifier, '.', an identifier, '>' -- so the pieces are put
 * back together here from their spellings. That is why has_space is
 * tracked: `<sys/ types.h>` and `<sys/types.h>` must not become the
 * same string. */
static char *include_name(token_t *tok, int *is_angle) {
    if (tok->kind == TK_STR) { *is_angle = 0; return zstrdup(tok->str); }

    if (tok_is(tok, "<")) {
        *is_angle = 1;
        size_t cap = 64, len = 0;
        char *out = zalloc(cap);
        for (tok = tok->next; tok && tok->kind != TK_EOF && !tok_is(tok, ">");
             tok = tok->next) {
            const char *s = tok->text;
            size_t n = strlen(s);
            if (len + n + 1 >= cap) {
                while (len + n + 1 >= cap) cap *= 2;
                char *b = zalloc(cap);
                memcpy(b, out, len);
                out = b;
            }
            memcpy(out + len, s, n);
            len += n;
        }
        out[len] = 0;
        return out;
    }

    zcc_error(tok->file, tok->line, tok->col, "malformed #include");
    return NULL;
}

/*
 * Existence is probed by READING, because the device build has no
 * stat() and no fopen() -- fs_size() returning 0 is the only "is it
 * there" this system offers, and it is ambiguous with an empty file.
 *
 * So the candidate is read, and the result is kept: a header found on
 * the second search directory would otherwise be read twice, once to
 * prove it exists and once for its contents. On an SD card at the
 * throughput docs/sdcard.md is still trying to explain, reading a
 * header twice is not a rounding error.
 */
static char *include_cache_path;
static char *include_cache_data;

static char *try_include(const char *cand) {
    int n = 0;
    char *data = zio_read_file(cand, &n);
    if (!data) return NULL;
    include_cache_path = (char *)cand;
    include_cache_data = data;
    (void)n;
    return (char *)cand;
}

static char *search_include(const char *name, const char *from, int is_angle) {
    char *cand;

    if (!is_angle) {
        char *dir = dirname_of(from);
        cand = zformat("%s/%s", dir, name);
        if (try_include(cand)) return cand;
    }
    for (incdir_t *d = incdirs; d; d = d->next) {
        cand = zformat("%s/%s", d->path, name);
        if (try_include(cand)) return cand;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */

static token_t *cpp(token_t *tok);

static token_t *do_include(token_t *tok, token_t *directive) {

    int is_angle = 0;
    char *name = include_name(tok, &is_angle);
    char *path = search_include(name, directive->file, is_angle);

    if (!path)
        zcc_error(directive->file, directive->line, directive->col,
                  "cannot find include file '%s'", name);

    if (seen_once(path)) return NULL;

    token_t *sub;
    if (include_cache_path && !strcmp(include_cache_path, path)) {
        sub = lex_string(include_cache_data, zstrdup(path));
        include_cache_path = NULL;
        include_cache_data = NULL;
    } else {
        sub = lex_file(path);
    }
    if (!sub)
        zcc_error(directive->file, directive->line, directive->col,
                  "cannot read include file '%s'", path);

    return cpp(sub);
}

static void do_define(token_t *line, token_t *directive) {

    if (!line || (line->kind != TK_IDENT && line->kind != TK_KEYWORD))
        zcc_error(directive->file, directive->line, directive->col,
                  "#define needs a name");

    char *name = line->text;
    token_t *t = line->next;

    /* The space is load-bearing: `#define F(x) ...` is function-like,
     * `#define F (x)` is object-like expanding to "(x)". Nothing but
     * has_space distinguishes them. */
    if (tok_is(t, "(") && !t->has_space) {
        macro_t *m = add_macro(name, 1);
        macro_param_t phead = {0};
        macro_param_t *pcur = &phead;

        t = t->next;
        while (!tok_is(t, ")")) {
            if (m->nparams) {
                if (!tok_is(t, ","))
                    zcc_error(t->file, t->line, t->col,
                              "expected ',' in macro parameter list");
                t = t->next;
            }
            if (t->kind != TK_IDENT)
                zcc_error(t->file, t->line, t->col,
                          "expected a parameter name, got '%s'", t->text);
            macro_param_t *p = zalloc(sizeof(macro_param_t));
            p->name = t->text;
            pcur = pcur->next = p;
            m->nparams++;
            t = t->next;
        }
        t = t->next;
        m->params = phead.next;
        m->body = t;
        return;
    }

    macro_t *m = add_macro(name, 0);
    m->body = t;
}

/*
 * The conditional stack.
 *
 * `state` is what an #else would flip to, which is subtler than a
 * plain taken/not-taken flag: in an #if/#elif chain, once any arm has
 * been taken, every later arm is skipped even if its condition is
 * true. `taken_any` is that memory, and leaving it out is the standard
 * way to end up with two arms of one #if both compiled.
 */
typedef struct cond_state {
    struct cond_state *next;
    int active;         /* is this arm being compiled */
    int taken_any;      /* has any arm in this chain been taken */
    int parent_active;
    token_t *at;
} cond_state_t;

/*
 * Directives and macro expansion, interleaved.
 *
 * These were two passes to begin with -- run every directive, then
 * expand every macro -- which is simpler and WRONG, in a way that took
 * a real header to expose. sw/common/zeitlos.h builds its syscall enum
 * like this:
 *
 *     #define Z_MKSYSCALL(name, handler) Z_SYS_##name,
 *     #include "syscalls.def"
 *     #undef Z_MKSYSCALL
 *
 * With expansion deferred to a second pass, the #undef has already run
 * by the time anything is expanded, so every Z_MKSYSCALL in the
 * included file is left as a bare identifier and the enum will not
 * parse. The X-macro idiom depends on a macro's definition being the
 * one that was in force AT THAT POINT IN THE TEXT.
 *
 * So expansion happens here, interleaved: consecutive non-directive
 * tokens accumulate into a run, and the run is expanded and flushed
 * before any directive that follows it is processed. Tokens coming
 * back from an #include are already expanded and are appended
 * directly, never re-scanned.
 */
static token_t *cpp(token_t *tok) {

    token_t head = {0};
    token_t *cur = &head;
    cond_state_t *conds = NULL;

    token_t run_head = {0};
    token_t *run_cur = &run_head;

#define FLUSH_RUN() do {                                            \
        if (run_head.next) {                                        \
            run_cur->next = eof_sentinel(tok);                      \
            token_t *ex = expand_list(run_head.next);               \
            for (token_t *e = ex; e && e->kind != TK_EOF; e = e->next) \
                cur = cur->next = e;                                \
            run_head.next = NULL;                                   \
            run_cur = &run_head;                                    \
        }                                                           \
    } while (0)

    while (tok && tok->kind != TK_EOF) {

        int active = !conds || conds->active;

        if (!(tok->bol && tok_is(tok, "#"))) {
            if (active) run_cur = run_cur->next = copy_token(tok);
            tok = tok->next;
            continue;
        }

        FLUSH_RUN();

        token_t *directive = tok;
        tok = tok->next;

        /* A '#' alone on a line is a null directive and is legal. */
        if (tok->bol || tok->kind == TK_EOF) continue;

        char *d = tok->text;

        if (!strcmp(d, "if") || !strcmp(d, "ifdef") || !strcmp(d, "ifndef")) {
            token_t *line = take_line(&tok, tok->next);
            int val = 0;

            /* A nested #if inside a skipped region is not evaluated at
             * all -- it may well be nonsense, which is exactly why it
             * was guarded. Only its nesting is tracked. */
            if (active) {
                if (!strcmp(d, "if")) {
                    val = eval_cond_line(line) != 0;
                } else {
                    if (!line || (line->kind != TK_IDENT && line->kind != TK_KEYWORD))
                        zcc_error(directive->file, directive->line, directive->col,
                                  "#%s needs a name", d);
                    val = find_macro(line->text) != NULL;
                    if (!strcmp(d, "ifndef")) val = !val;
                }
            }

            cond_state_t *c = zalloc(sizeof(cond_state_t));
            c->next = conds;
            c->parent_active = active;
            c->active = active && val;
            c->taken_any = c->active;
            c->at = directive;
            conds = c;
            continue;
        }

        if (!strcmp(d, "elif")) {
            if (!conds)
                zcc_error(directive->file, directive->line, directive->col,
                          "#elif without #if");
            token_t *line = take_line(&tok, tok->next);
            if (conds->parent_active && !conds->taken_any) {
                int val = eval_cond_line(line) != 0;
                conds->active = val;
                conds->taken_any = val;
            } else {
                conds->active = 0;
            }
            continue;
        }

        if (!strcmp(d, "else")) {
            if (!conds)
                zcc_error(directive->file, directive->line, directive->col,
                          "#else without #if");
            skip_line(&tok, tok->next);
            conds->active = conds->parent_active && !conds->taken_any;
            conds->taken_any = 1;
            continue;
        }

        if (!strcmp(d, "endif")) {
            if (!conds)
                zcc_error(directive->file, directive->line, directive->col,
                          "#endif without #if");
            skip_line(&tok, tok->next);
            conds = conds->next;
            continue;
        }

        if (!active) { skip_line(&tok, tok); continue; }

        if (!strcmp(d, "include")) {
            token_t *line = take_line(&tok, tok->next);
            line = (line->kind == TK_STR || tok_is(line, "<"))
                 ? line : expand_line(line);
            token_t *sub = do_include(line, directive);
            if (sub) {
                token_t *tail = sub;
                if (tail->kind == TK_EOF) { continue; }
                while (tail->next && tail->next->kind != TK_EOF) tail = tail->next;
                cur->next = sub;
                cur = tail;
            }
            continue;
        }

        if (!strcmp(d, "define")) {
            token_t *line = take_line(&tok, tok->next);
            do_define(line, directive);
            continue;
        }

        if (!strcmp(d, "undef")) {
            token_t *line = take_line(&tok, tok->next);
            if (!line || (line->kind != TK_IDENT && line->kind != TK_KEYWORD))
                zcc_error(directive->file, directive->line, directive->col,
                          "#undef needs a name");
            undef_macro(line->text);
            continue;
        }

        if (!strcmp(d, "pragma")) {
            token_t *line = take_line(&tok, tok->next);
            if (line && line->kind == TK_IDENT && !strcmp(line->text, "once"))
                mark_once(directive->file);
            /* Any other pragma is ignored rather than refused: a
             * pragma is by definition implementation-defined, and
             * failing on one this compiler has never heard of would
             * make it unable to read headers written for others. */
            continue;
        }

        if (!strcmp(d, "error")) {
            token_t *line = take_line(&tok, tok->next);
            zcc_error(directive->file, directive->line, directive->col,
                      "#error %s", stringize(line));
        }

        if (!strcmp(d, "warning")) {
            token_t *line = take_line(&tok, tok->next);
            zcc_printf("%s:%d: warning: #warning %s\n",
                       directive->file, directive->line, stringize(line));
            continue;
        }

        if (!strcmp(d, "line")) { skip_line(&tok, tok); continue; }

        zcc_error(directive->file, directive->line, directive->col,
                  "unknown directive '#%s'", d);
    }

    FLUSH_RUN();

#undef FLUSH_RUN

    if (conds)
        zcc_error(conds->at->file, conds->at->line, conds->at->col,
                  "unterminated #if");

    cur->next = eof_sentinel(tok);
    return head.next;
}

void cpp_define_cli(const char *arg) {
    const char *eq = strchr(arg, '=');
    char *src;
    if (eq) src = zformat("%.*s %s\n", (int)(eq - arg), arg, eq + 1);
    else src = zformat("%s 1\n", arg);
    token_t *line = lex_string(src, "<command line>");
    do_define(line, line);
}

void cpp_undef_cli(const char *name) {
    undef_macro(name);
}

token_t *preprocess(token_t *tok) {
    /* cpp() expands as it goes -- see its own header comment for the
     * X-macro case that made a separate expansion pass untenable. */
    return cpp(tok);
}
