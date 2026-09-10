/*
 * zcc -- lexer.
 *
 * Turns a source file into a linked list of tokens. Comments and line
 * continuations disappear here; #directives do not -- they come out as
 * ordinary tokens with `bol` set on the '#', and preprocess() (cpp.c)
 * decides what they mean.
 *
 * That split matters for one specific reason. A '#' is only a
 * directive at the start of a line, and only the lexer knows where
 * lines start once backslash-continuations have been folded away. So
 * the lexer records the fact and the preprocessor uses it, rather than
 * either one trying to do both jobs.
 */

#include <string.h>

#include "zcc.h"

static token_t *new_token(tk_kind_t kind, const char *file, int line, int col) {
    token_t *t = zalloc(sizeof(token_t));
    t->kind = kind;
    t->file = file;
    t->line = line;
    t->col = col;
    return t;
}

static const char *keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "int", "long", "register",
    "return", "short", "signed", "sizeof", "static", "struct", "switch",
    "typedef", "union", "unsigned", "void", "volatile", "while",
    "float", "double",
    "inline", "restrict", "_Bool",
    NULL
};

static int is_keyword(const char *s) {
    for (int i = 0; keywords[i]; i++)
        if (!strcmp(s, keywords[i])) return 1;
    return 0;
}

/*
 * Punctuators, longest first. The order is the algorithm: C's maximal
 * munch rule says `>>=` is one token and not `>>` followed by `=`, and
 * a longest-first linear scan is the whole of that rule.
 */
static const char *puncts[] = {
    "<<=", ">>=", "...",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "<", ">", "=",
    "(", ")", "[", "]", "{", "}", ";", ",", ".", "?", ":", "#",
    NULL
};

static int read_punct(const char *p) {
    for (int i = 0; puncts[i]; i++) {
        size_t n = strlen(puncts[i]);
        if (!strncmp(p, puncts[i], n)) return (int)n;
    }
    return 0;
}

/*
 * Escape sequences.
 *
 * \x and octal are here because real headers use them, and because
 * getting them wrong is invisible: a string with a mis-decoded escape
 * compiles, links and prints something slightly wrong. Octal stops at
 * three digits, hex does not -- that asymmetry is C's, not a bug here.
 */
static int read_escape(const char **pp) {
    const char *p = *pp;
    int c = *p++;

    switch (c) {
    case 'a': *pp = p; return '\a';
    case 'b': *pp = p; return '\b';
    case 'f': *pp = p; return '\f';
    case 'n': *pp = p; return '\n';
    case 'r': *pp = p; return '\r';
    case 't': *pp = p; return '\t';
    case 'v': *pp = p; return '\v';
    case 'e': *pp = p; return 27;       /* a GNU extension the tree uses */
    case 'x': {
        int v = 0;
        while (z_isxdigit(*p)) {
            int d = z_isdigit(*p) ? *p - '0'
                  : (z_tolower(*p) - 'a' + 10);
            v = v * 16 + d;
            p++;
        }
        *pp = p;
        return v & 0xff;
    }
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0';
            for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++)
                v = v * 8 + (*p++ - '0');
            *pp = p;
            return v & 0xff;
        }
        *pp = p;
        return c;                       /* \\ \' \" \? and anything else */
    }
}

/*
 * Reads a source file and folds away the two things that would
 * otherwise have to be handled everywhere downstream: backslash-newline
 * continuations, and a missing final newline.
 *
 * Continuations are folded by replacing the pair with nothing and
 * remembering that a line was consumed, so that line numbers in
 * diagnostics still point at the right place. A compiler that reports
 * the wrong line inside a multi-line macro is worse than one that does
 * not support them.
 */
static char *read_source(const char *path, int *out_len) {

    int n = 0;
    char *buf = zio_read_file(path, &n);
    if (!buf) return NULL;

    /* A file that does not end in a newline is made to. Every place
     * downstream that cares about line structure -- the preprocessor's
     * `bol`, the directive line-taker -- would otherwise need a special
     * case for the last line, in the file where a special case is
     * least likely to be tested. zio_read_file() leaves two spare
     * bytes past the end for exactly this. */
    if (n == 0 || buf[n - 1] != '\n') buf[n++] = '\n';
    buf[n] = 0;

    *out_len = n;
    return buf;
}

token_t *lex_string(const char *src, const char *file);

token_t *lex_file(const char *path) {
    int len;
    char *src = read_source(path, &len);
    if (!src) return NULL;
    return lex_string(src, zstrdup(path));
}

token_t *lex_string(const char *src, const char *file) {

    token_t head = {0};
    token_t *cur = &head;

    const char *p = src;
    const char *line_start = src;
    int line = 1;
    int bol = 1;
    int has_space = 0;

#define COL() ((int)(p - line_start) + 1)

    while (*p) {

        /* line continuation */
        if (p[0] == '\\' && p[1] == '\n') {
            p += 2;
            line++;
            line_start = p;
            has_space = 1;
            continue;
        }

        if (*p == '\n') {
            p++;
            line++;
            line_start = p;
            bol = 1;
            has_space = 0;
            continue;
        }

        if (z_isspace(*p)) {
            p++;
            has_space = 1;
            continue;
        }

        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            has_space = 1;
            continue;
        }

        if (p[0] == '/' && p[1] == '*') {
            const char *start = p;
            int sline = line;
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') { line++; line_start = p + 1; }
                p++;
            }
            if (!*p)
                zcc_error(file, sline, (int)(start - line_start) + 1,
                          "unterminated comment");
            p += 2;
            has_space = 1;
            continue;
        }

        int col = COL();

        /* number */
        if (z_isdigit(*p) ||
            (*p == '.' && z_isdigit(p[1]))) {

            token_t *t = new_token(TK_NUM, file, line, col);
            const char *start = p;
            uint32_t v = 0;
            int uns = 0;

            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                if (!z_isxdigit(*p))
                    zcc_error(file, line, col, "hex constant with no digits");
                while (z_isxdigit(*p)) {
                    int d = z_isdigit(*p) ? *p - '0'
                          : (z_tolower(*p) - 'a' + 10);
                    v = v * 16u + (uint32_t)d;
                    p++;
                }
            } else if (p[0] == '0' && z_isdigit(p[1])) {
                p++;
                while (*p >= '0' && *p <= '7') v = v * 8u + (uint32_t)(*p++ - '0');
            } else {
                while (z_isdigit(*p)) v = v * 10u + (uint32_t)(*p++ - '0');
            }

            if (*p == '.' || *p == 'e' || *p == 'E')
                zcc_error(file, line, col,
                          "floating point is not supported (see docs/zcc.md)");

            for (;;) {
                if (*p == 'u' || *p == 'U') { uns = 1; p++; }
                else if (*p == 'l' || *p == 'L') { p++; }
                else break;
            }

            if (z_isalpha(*p) || *p == '_')
                zcc_error(file, line, col, "malformed number '%.*s'",
                          (int)(p - start) + 1, start);

            t->val = v;
            t->is_unsigned = uns || (v > 0x7fffffffu);
            t->text = zstrndup(start, (size_t)(p - start));
            t->bol = bol; t->has_space = has_space;
            cur = cur->next = t;
            bol = 0; has_space = 0;
            continue;
        }

        /* identifier or keyword */
        if (z_isalpha(*p) || *p == '_') {
            const char *start = p;
            while (z_isalnum(*p) || *p == '_') p++;
            char *name = zstrndup(start, (size_t)(p - start));

            token_t *t = new_token(is_keyword(name) ? TK_KEYWORD : TK_IDENT,
                                   file, line, col);
            t->text = name;
            t->bol = bol; t->has_space = has_space;
            cur = cur->next = t;
            bol = 0; has_space = 0;
            continue;
        }

        /* string literal */
        if (*p == '"') {
            const char *q = ++p;
            char *out = zalloc(strlen(q) + 2);
            int n = 0;

            while (*p && *p != '"') {
                if (*p == '\n')
                    zcc_error(file, line, col, "unterminated string literal");
                if (*p == '\\') { p++; out[n++] = (char)read_escape(&p); }
                else out[n++] = *p++;
            }
            if (!*p) zcc_error(file, line, col, "unterminated string literal");
            p++;
            out[n] = 0;

            token_t *t = new_token(TK_STR, file, line, col);
            t->str = out;
            t->str_len = n + 1;         /* the NUL is part of the object */
            t->text = zstrdup("\"...\"");
            t->bol = bol; t->has_space = has_space;
            cur = cur->next = t;
            bol = 0; has_space = 0;
            continue;
        }

        /* character constant */
        if (*p == '\'') {
            p++;
            int c = 0;
            if (*p == '\\') { p++; c = read_escape(&p); }
            else if (*p == '\'' ) zcc_error(file, line, col, "empty character constant");
            else c = *p++;
            if (*p != '\'') zcc_error(file, line, col, "unterminated character constant");
            p++;

            token_t *t = new_token(TK_NUM, file, line, col);
            /* Plain char is UNSIGNED on RISC-V -- the psABI says so,
             * and riscv gcc follows it -- so '\xff' is 255, not -1.
             *
             * This was written the other way round first, on the
             * assumption that plain char is signed the way it is on
             * x86. The differential test against gcc caught it on the
             * first run. It is worth knowing how quiet the wrong
             * version is: nothing fails to compile, and the only
             * programs that notice are ones comparing a char against a
             * negative sentinel -- which is to say, every
             * getchar()-style loop. */
            t->val = (uint32_t)(uint8_t)c;
            t->text = zstrdup("'c'");
            t->bol = bol; t->has_space = has_space;
            cur = cur->next = t;
            bol = 0; has_space = 0;
            continue;
        }

        /* punctuator */
        {
            int n = read_punct(p);
            if (!n) zcc_error(file, line, col, "stray '%c' in program", *p);

            token_t *t = new_token(TK_PUNCT, file, line, col);
            t->text = zstrndup(p, (size_t)n);
            t->bol = bol; t->has_space = has_space;
            cur = cur->next = t;
            p += n;
            bol = 0; has_space = 0;
        }
    }

    token_t *eof = new_token(TK_EOF, file, line, COL());
    eof->text = zstrdup("<eof>");
    eof->bol = 1;
    cur->next = eof;

#undef COL

    return head.next;
}

int tok_is(token_t *t, const char *s) {
    return t && t->text && !strcmp(t->text, s);
}
