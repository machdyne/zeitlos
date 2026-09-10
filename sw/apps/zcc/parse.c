/*
 * zcc -- parser and code generator.
 *
 * One pass. The recursive-descent parser emits RV32IM as it goes;
 * there is no AST and no IR. See zcc.h's header for why the target
 * makes that the natural shape rather than a compromise.
 *
 * -- The evaluation model --
 *
 * Every expression leaves its result in a0. An lvalue leaves its
 * ADDRESS in a0 and sets is_lval, so `x = y` is "evaluate y to a0,
 * push, evaluate &x to a0, pop the value, store" and `x` on its own is
 * that address followed by a load. Binary operators push the left
 * operand to the stack, evaluate the right into a0, and pop the left
 * into t0.
 *
 * This is an accumulator machine with a memory stack, and it is
 * roughly three times the size of what GCC produces. It is also
 * correct by construction with no register allocator, no liveness and
 * no spilling logic -- three things that are individually harder than
 * everything else in this file combined, and whose bugs appear as
 * wrong answers rather than as crashes.
 *
 * Improving it is a contained later job: a peephole pass over the
 * emitted stream, or a small register stack that only falls back to
 * memory at depth. Neither touches the front end. See docs/zcc.md.
 *
 * -- The stack --
 *
 * Pushes move sp by 16, not 4. The RISC-V ABI wants sp 16-byte aligned
 * at a call, and keeping every push aligned is cheaper to be sure of
 * than tracking the depth and correcting before each call. It wastes
 * stack on deep expressions, which on a 4MB heap allowance is not a
 * cost worth reasoning about.
 *
 * -- The frame --
 *
 *   entry:  sp -= 16; sw ra,12(sp); sw s0,8(sp); mv s0,sp
 *           sp -= locals_size            (patched, 3 instruction slots)
 *
 * s0 anchors the frame and never moves, so locals sit at NEGATIVE
 * offsets from it and expression pushes happen below sp, which does
 * move. Saving ra and s0 above s0 rather than below means the
 * epilogue needs no patching at all.
 */

#include <string.h>
#include <stdarg.h>

#include "zcc.h"

/* ------------------------------------------------------------------ */
/* types                                                                */

static type_t ty_void_  = { .kind = TY_VOID,  .size = 1, .align = 1 };
/* Plain `char` is UNSIGNED, per the RISC-V psABI. See the note in
 * lex.c on character constants -- same rule, and the same reason it
 * matters more than it looks. */
static type_t ty_char_  = { .kind = TY_CHAR,  .size = 1, .align = 1, .is_unsigned = 1 };
static type_t ty_uchar_ = { .kind = TY_CHAR,  .size = 1, .align = 1, .is_unsigned = 1 };
static type_t ty_short_ = { .kind = TY_SHORT, .size = 2, .align = 2 };
static type_t ty_ushort_= { .kind = TY_SHORT, .size = 2, .align = 2, .is_unsigned = 1 };
static type_t ty_int_   = { .kind = TY_INT,   .size = 4, .align = 4 };
static type_t ty_uint_  = { .kind = TY_INT,   .size = 4, .align = 4, .is_unsigned = 1 };
static type_t ty_float_ = { .kind = TY_FLOAT, .size = 4, .align = 4 };
static type_t ty_double_= { .kind = TY_FLOAT, .size = 8, .align = 8 };

type_t *ty_void = &ty_void_, *ty_char = &ty_char_, *ty_uchar = &ty_uchar_;
type_t *ty_short = &ty_short_, *ty_ushort = &ty_ushort_;
type_t *ty_int = &ty_int_, *ty_uint = &ty_uint_;
type_t *ty_float = &ty_float_, *ty_double = &ty_double_;

type_t *ty_pointer_to(type_t *base) {
    type_t *t = zalloc(sizeof(type_t));
    t->kind = TY_PTR;
    t->size = 4;
    t->align = 4;
    t->base = base;
    t->is_unsigned = 1;      /* pointer comparisons are unsigned */
    return t;
}

type_t *ty_array_of(type_t *base, int len) {
    type_t *t = zalloc(sizeof(type_t));
    t->kind = TY_ARRAY;
    t->base = base;
    t->array_len = len;
    t->align = base->align;
    t->size = (len < 0) ? 0 : base->size * len;
    return t;
}

type_t *ty_func(type_t *ret) {
    type_t *t = zalloc(sizeof(type_t));
    t->kind = TY_FUNC;
    t->base = ret;
    t->size = 4;
    t->align = 4;
    return t;
}

int ty_is_integer(type_t *t) {
    return t->kind == TY_CHAR || t->kind == TY_SHORT ||
           t->kind == TY_INT  || t->kind == TY_ENUM;
}

int ty_is_scalar(type_t *t) {
    /* TY_FLOAT counts, so that a float member can be assigned as a
     * 4- or 8-byte copy. Arithmetic on one is refused separately, in
     * gen_binop() -- see ty_float's own note in zcc.h. */
    return ty_is_integer(t) || t->kind == TY_PTR || t->kind == TY_FLOAT;
}

type_t *ty_decay(type_t *t) {
    if (t->kind == TY_ARRAY) return ty_pointer_to(t->base);
    if (t->kind == TY_FUNC)  return ty_pointer_to(t);
    return t;
}

static int ty_same(type_t *a, type_t *b) {
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    if (a->kind == TY_PTR || a->kind == TY_ARRAY) return ty_same(a->base, b->base);
    return a->size == b->size && a->is_unsigned == b->is_unsigned;
}

/* ------------------------------------------------------------------ */
/* scopes                                                              */

typedef struct scope {
    struct scope *parent;
    symbol_t *syms;
    type_t *tags;           /* struct/union/enum tags, chained by ->next */
} scope_t;

static scope_t *scope;
static scope_t global_scope;

static void enter_scope(void) {
    scope_t *s = zalloc(sizeof(scope_t));
    s->parent = scope;
    scope = s;
}

static void leave_scope(void) { scope = scope->parent; }

static symbol_t *find_sym(const char *name) {
    for (scope_t *s = scope; s; s = s->parent)
        for (symbol_t *v = s->syms; v; v = v->next)
            if (!strcmp(v->name, name)) return v;
    return NULL;
}

static symbol_t *find_sym_local(const char *name) {
    for (symbol_t *v = scope->syms; v; v = v->next)
        if (!strcmp(v->name, name)) return v;
    return NULL;
}

static symbol_t *push_sym(const char *name, type_t *ty, sym_kind_t kind) {
    symbol_t *s = zalloc(sizeof(symbol_t));
    s->name = zstrdup(name);
    s->ty = ty;
    s->kind = kind;
    s->next = scope->syms;
    scope->syms = s;
    return s;
}

static type_t *find_tag(const char *name) {
    for (scope_t *s = scope; s; s = s->parent)
        for (type_t *t = s->tags; t; t = t->next)
            if (t->tag && !strcmp(t->tag, name)) return t;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* parser state                                                        */

static token_t *tk;             /* the cursor */
static int cur_locals;          /* bytes of frame allocated so far */
static int cur_frame_patch;     /* .text offset of the 3-slot frame adjust */
static int cur_ret_label;
static type_t *cur_ret_type;
static int main_label = -1;
static int entry_stub_at;

/* Names the runtime blob provides, each already pinned to its
 * jump-table slot. Empty in freestanding mode. */
static libz_sym_t *libz_syms;
static int libz_nsyms;

void parse_set_libz(libz_sym_t *syms, int n) {
    libz_syms = syms;
    libz_nsyms = n;
}

/*
 * A declaration of a runtime function resolves to its table slot.
 *
 * The declaration itself arrives normally, from libz.h -- there is no
 * special syntax and no builtin. All this does is make the symbol's
 * label the pinned one instead of a fresh unplaced label, so that a
 * call becomes a single `jal` into the table rather than a reference
 * to a function that will never be defined.
 *
 * Returns -1 when the name is not one of the runtime's, which is the
 * normal case for everything in the program being compiled.
 */
static int libz_label_for(const char *name) {
    for (int i = 0; i < libz_nsyms; i++)
        if (!strcmp(libz_syms[i].name, name)) return libz_syms[i].label;
    return -1;
}

typedef struct loop_ctx {
    struct loop_ctx *next;
    int brk, cont;              /* -1 where the construct does not provide one */
} loop_ctx_t;

static loop_ctx_t *loops;

typedef struct goto_ref {
    struct goto_ref *next;
    char *name;
    int label;
    token_t *at;
    int defined;
} goto_ref_t;

static goto_ref_t *gotos;

/* Both of these format into ZCC_MSG_MAX and go through zio_out(), so
 * that the compiler has no FILE anywhere and the device build needs no
 * special case. See util.c. */
static char tok_msg[ZCC_MSG_MAX];

void zcc_error_at_tok(const char *fmt, ...) {
    va_list ap;
    zcc_printf("%s:%d:%d: error: ", tk->file, tk->line, tk->col);
    va_start(ap, fmt);
    zcc_vfmt(tok_msg, (int)sizeof(tok_msg), fmt, ap);
    va_end(ap);
    zio_out(tok_msg);
    zio_out("\n");
    zio_exit(1);
}

void zcc_warn_at_tok(const char *fmt, ...) {
    va_list ap;
    zcc_printf("%s:%d:%d: warning: ", tk->file, tk->line, tk->col);
    va_start(ap, fmt);
    zcc_vfmt(tok_msg, (int)sizeof(tok_msg), fmt, ap);
    va_end(ap);
    zio_out(tok_msg);
    zio_out("\n");
}

static int at(const char *s) { return tok_is(tk, s); }

static void expect(const char *s) {
    if (!at(s)) zcc_error_at_tok("expected '%s', found '%s'", s, tk->text);
    tk = tk->next;
}

static int accept(const char *s) {
    if (!at(s)) return 0;
    tk = tk->next;
    return 1;
}

/* ------------------------------------------------------------------ */
/* code generation helpers                                             */

static void gen_push(void) {
    ei_addi(REG_SP, REG_SP, -16);
    ei_store(REG_A0, REG_SP, 0, 4);
}

static void gen_pop(int reg) {
    ei_load(reg, REG_SP, 0, 4, 0);
    ei_addi(REG_SP, REG_SP, 16);
}

/* rd = s0 + off, for any off. The two-instruction path exists because
 * a function with more than 2KB of locals is unusual but not
 * impossible, and silently truncating the offset would corrupt a
 * neighbouring variable rather than fail. */
static void gen_local_addr(int rd, int off) {
    if (off >= -2048 && off < 2048) {
        ei_addi(rd, REG_S0, off);
    } else {
        ei_li(rd, (uint32_t)off);
        ei_add(rd, REG_S0, rd);
    }
}

static int align_to(int n, int a) { return (n + a - 1) / a * a; }

static int alloc_local(int size, int align) {
    cur_locals = align_to(cur_locals + size, align < 4 ? 4 : align);
    return -cur_locals;
}

/* Loads through a0, which holds an address, replacing it with the
 * value. Structs, unions and arrays are NOT loaded: there is no
 * register wide enough, and every context that can use one wants its
 * address anyway. */
static void gen_load(type_t *ty) {
    if (ty->kind == TY_ARRAY || ty->kind == TY_STRUCT ||
        ty->kind == TY_UNION || ty->kind == TY_FUNC)
        return;
    if (ty->kind == TY_FLOAT && ty->size != 4)
        zcc_error_at_tok("'double' values cannot be loaded into a register "
                         "(see docs/zcc.md)");
    ei_load(REG_A0, REG_A0, 0, ty->size, ty->is_unsigned);
}

/* Stores a0 through the address in t0. */
static void gen_store(type_t *ty) {
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        /* Struct assignment as an inline byte loop.
         *
         * A word-at-a-time copy would be three times faster and is
         * wrong here: a struct's size need not be a multiple of four,
         * and its address need not be aligned once it is a member of
         * something else. Bytes are always safe, and struct assignment
         * is rare enough in this tree that the difference has never
         * been measurable. */
        int loop = emit_new_label(), done = emit_new_label();
        ei_li(REG_T1, (uint32_t)ty->size);          /* count */
        ei_mv(REG_T2, REG_ZERO);                    /* index */
        emit_place_label(loop);
        ei_branch_label("bge", REG_T2, REG_T1, done);
        ei_add(REG_A1, REG_A0, REG_T2);
        ei_load(REG_A1, REG_A1, 0, 1, 1);
        ei_add(REG_A2, REG_T0, REG_T2);
        ei_store(REG_A1, REG_A2, 0, 1);
        ei_addi(REG_T2, REG_T2, 1);
        ei_jal_label(REG_ZERO, loop);
        emit_place_label(done);
        return;
    }
    ei_store(REG_A0, REG_T0, 0, ty->size);
}

/* Narrows a0 to `ty` after arithmetic done at 32 bits. Only needed for
 * casts; assignment narrows for free via the store width. */
static void gen_cast(type_t *from, type_t *to) {
    (void)from;
    if (to->kind == TY_VOID) return;
    if (to->kind == TY_CHAR) {
        if (to->is_unsigned) ei_andi(REG_A0, REG_A0, 0xff);
        else { ei_slli(REG_A0, REG_A0, 24); ei_srai(REG_A0, REG_A0, 24); }
    } else if (to->kind == TY_SHORT) {
        ei_slli(REG_A0, REG_A0, 16);
        if (to->is_unsigned) ei_srli(REG_A0, REG_A0, 16);
        else ei_srai(REG_A0, REG_A0, 16);
    }
}

/* ------------------------------------------------------------------ */

typedef struct {
    type_t *ty;
    int is_lval;        /* a0 holds the object's ADDRESS */
} val_t;

static val_t expr(void);
static val_t assign_expr(void);
static val_t unary(void);
static void statement(void);
static void declaration_or_statement(void);
static int is_type_start(token_t *t);
static type_t *declspec(int *is_typedef, int *is_static, int *is_extern);
static type_t *declarator(type_t *base, char **name);
static type_t *abstract_declarator(type_t *base);
static long const_expr(void);

static val_t rvalue(val_t v) {
    if (v.is_lval) {
        gen_load(v.ty);
        v.is_lval = 0;
    }
    v.ty = ty_decay(v.ty);
    return v;
}

/* ------------------------------------------------------------------ */
/* struct / union / enum                                               */

static type_t *struct_union_decl(int is_union) {

    char *tag = NULL;
    if (tk->kind == TK_IDENT) { tag = tk->text; tk = tk->next; }

    if (tag && !at("{")) {
        type_t *t = find_tag(tag);
        if (t) return t;
        /* An incomplete type. Legal as long as nothing asks for its
         * size before it is completed -- which is exactly what makes
         * `struct node { struct node *next; };` work. */
        t = zalloc(sizeof(type_t));
        t->kind = is_union ? TY_UNION : TY_STRUCT;
        t->tag = tag;
        t->align = 1;
        t->next = scope->tags;
        scope->tags = t;
        return t;
    }

    type_t *ty = tag ? find_tag(tag) : NULL;
    if (!ty || ty->is_complete) {
        ty = zalloc(sizeof(type_t));
        ty->kind = is_union ? TY_UNION : TY_STRUCT;
        ty->tag = tag;
        ty->align = 1;
        if (tag) { ty->next = scope->tags; scope->tags = ty; }
    }

    expect("{");

    member_t head = {0};
    member_t *cur = &head;
    int offset = 0, maxalign = 1;

    while (!at("}")) {
        int dummy_t = 0, dummy_s = 0, dummy_e = 0;
        type_t *base = declspec(&dummy_t, &dummy_s, &dummy_e);

        int first = 1;
        while (!accept(";")) {
            if (!first) expect(",");
            first = 0;
            char *name = NULL;
            type_t *mty = declarator(base, &name);
            if (!name) zcc_error_at_tok("a struct member needs a name");
            if (mty->size == 0 && mty->kind != TY_ARRAY)
                zcc_error_at_tok("member '%s' has incomplete type", name);

            member_t *m = zalloc(sizeof(member_t));
            m->ty = mty;
            m->name = name;
            if (is_union) {
                m->offset = 0;
                if (mty->size > offset) offset = mty->size;
            } else {
                offset = align_to(offset, mty->align);
                m->offset = offset;
                offset += mty->size;
            }
            if (mty->align > maxalign) maxalign = mty->align;
            cur = cur->next = m;
        }
    }
    expect("}");

    ty->members = head.next;
    ty->align = maxalign;
    ty->size = align_to(offset, maxalign);
    ty->is_complete = 1;
    return ty;
}

static type_t *enum_decl(void) {

    char *tag = NULL;
    if (tk->kind == TK_IDENT) { tag = tk->text; tk = tk->next; }

    if (tag && !at("{")) {
        type_t *t = find_tag(tag);
        if (!t) zcc_error_at_tok("unknown enum '%s'", tag);
        return t;
    }

    type_t *ty = zalloc(sizeof(type_t));
    /* An enum IS an int here, not a distinct type. C permits either
     * and this tree's headers use enums as plain integer constants, so
     * the distinction would buy warnings nobody asked for. */
    ty->kind = TY_ENUM;
    ty->size = 4;
    ty->align = 4;
    ty->tag = tag;
    ty->is_complete = 1;
    if (tag) { ty->next = scope->tags; scope->tags = ty; }

    expect("{");
    uint32_t next = 0;
    while (!at("}")) {
        if (tk->kind != TK_IDENT)
            zcc_error_at_tok("expected an enumerator name, found '%s'", tk->text);
        char *name = tk->text;
        tk = tk->next;
        if (accept("=")) next = (uint32_t)const_expr();
        symbol_t *s = push_sym(name, ty, SYM_ENUMCONST);
        s->enum_val = next++;
        if (!accept(",")) break;
    }
    expect("}");
    return ty;
}

/* ------------------------------------------------------------------ */
/* declaration specifiers                                              */

static int is_type_start(token_t *t) {
    static const char *kw[] = {
        "void", "char", "short", "int", "long", "signed", "unsigned",
        "struct", "union", "enum", "const", "volatile", "static",
        "extern", "typedef", "register", "auto", "inline", "restrict",
        "_Bool", "float", "double", NULL
    };
    if (t->kind == TK_KEYWORD)
        for (int i = 0; kw[i]; i++) if (!strcmp(t->text, kw[i])) return 1;
    if (t->kind == TK_IDENT) {
        symbol_t *s = find_sym(t->text);
        return s && s->kind == SYM_TYPEDEF;
    }
    return 0;
}

/*
 * Declaration specifiers.
 *
 * Counters rather than a state machine, because C lets the parts
 * appear in any order -- `unsigned long int` and `int long unsigned`
 * are the same declaration -- and a state machine for that is a table
 * nobody can read. Counting what was seen and deciding at the end is
 * both shorter and obviously correct.
 *
 * `long` is 32 bits here, same as int. That is the LP32-ish choice
 * forced by the target: rv32 with ilp32, where long and int are both
 * 4 bytes. `long long` is refused rather than silently truncated.
 */
static type_t *declspec(int *is_typedef, int *is_static, int *is_extern) {

    int is_void = 0, is_char = 0, is_short = 0, is_int = 0, is_long = 0;
    int is_signed = 0, is_unsigned = 0, is_bool = 0;
    int is_float = 0, is_double = 0;
    type_t *user = NULL;

    *is_typedef = *is_static = *is_extern = 0;

    for (;;) {
        if (at("typedef")) { *is_typedef = 1; tk = tk->next; continue; }
        if (at("static"))  { *is_static = 1; tk = tk->next; continue; }
        if (at("extern"))  { *is_extern = 1; tk = tk->next; continue; }
        if (at("const") || at("volatile") || at("register") ||
            at("auto") || at("inline") || at("restrict")) {
            tk = tk->next;
            continue;
        }

        if (at("struct") || at("union")) {
            if (user) break;
            int u = at("union");
            tk = tk->next;
            user = struct_union_decl(u);
            continue;
        }
        if (at("enum")) {
            if (user) break;
            tk = tk->next;
            user = enum_decl();
            continue;
        }

        if (tk->kind == TK_IDENT && !is_void && !is_char && !is_short &&
            !is_int && !is_long && !is_signed && !is_unsigned && !user) {
            symbol_t *s = find_sym(tk->text);
            if (s && s->kind == SYM_TYPEDEF) { user = s->ty; tk = tk->next; continue; }
            break;
        }

        if (at("void"))     { is_void++;     tk = tk->next; continue; }
        if (at("_Bool"))    { is_bool++;     tk = tk->next; continue; }
        if (at("char"))     { is_char++;     tk = tk->next; continue; }
        if (at("short"))    { is_short++;    tk = tk->next; continue; }
        if (at("int"))      { is_int++;      tk = tk->next; continue; }
        if (at("long"))     { is_long++;     tk = tk->next; continue; }
        if (at("signed"))   { is_signed++;   tk = tk->next; continue; }
        if (at("unsigned")) { is_unsigned++; tk = tk->next; continue; }
        if (at("float"))    { is_float++;    tk = tk->next; continue; }
        if (at("double"))   { is_double++;   tk = tk->next; continue; }
        break;
    }

    if (is_long >= 2)
        zcc_error_at_tok("'long long' is not supported (see docs/zcc.md)");

    if (user) {
        if (is_void || is_char || is_short || is_int || is_long ||
            is_signed || is_unsigned)
            zcc_error_at_tok("conflicting type specifiers");
        return user;
    }

    if (is_float) return ty_float;
    if (is_double) return ty_double;
    if (is_void) return ty_void;
    if (is_bool) return ty_uchar;
    if (is_char) return is_unsigned ? ty_uchar : ty_char;
    if (is_short) return is_unsigned ? ty_ushort : ty_short;
    if (is_int || is_long || is_signed || is_unsigned)
        return is_unsigned ? ty_uint : ty_int;

    zcc_error_at_tok("expected a type, found '%s'", tk->text);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* declarators                                                         */

static type_t *func_params(type_t *ret) {

    type_t *fn = ty_func(ret);
    type_t phead = {0};
    type_t *pcur = &phead;

    if (at("void") && tok_is(tk->next, ")")) { tk = tk->next; tk = tk->next; return fn; }
    if (accept(")")) return fn;

    for (;;) {
        if (at("...")) {
            tk = tk->next;
            fn->is_variadic = 1;
            break;
        }
        int a, b, c;
        type_t *base = declspec(&a, &b, &c);
        char *name = NULL;
        type_t *pt = declarator(base, &name);
        pt = ty_decay(pt);

        type_t *copy = zalloc(sizeof(type_t));
        *copy = *pt;
        copy->next = NULL;
        copy->tag = name;           /* the parameter name rides in `tag` */
        pcur = pcur->next = copy;

        if (!accept(",")) break;
    }
    expect(")");
    fn->params = phead.next;
    return fn;
}

static type_t *type_suffix(type_t *ty) {

    if (at("(")) { tk = tk->next; return func_params(ty); }

    if (at("[")) {
        tk = tk->next;
        int len = -1;
        if (!at("]")) len = (int)const_expr();
        expect("]");
        ty = type_suffix(ty);
        return ty_array_of(ty, len);
    }
    return ty;
}

static type_t *declarator(type_t *base, char **name) {

    while (accept("*")) {
        base = ty_pointer_to(base);
        while (at("const") || at("volatile") || at("restrict")) tk = tk->next;
    }

    /* A parenthesised declarator, which is how function pointers are
     * spelled: `int (*f)(void)`. The inner declarator has to be parsed
     * before the outer suffix is known, so it is parsed twice -- once
     * over a placeholder to learn the shape, then again once the real
     * base type exists. Re-scanning is what avoids building a
     * declarator tree for the sake of one construct. */
    if (at("(")) {
        token_t *start = tk;
        tk = tk->next;

        /* Scanned twice, on purpose.
         *
         * `int (*f)(void)` cannot be built in one pass: the inner
         * declarator's meaning depends on the OUTER suffix, which is
         * only reachable by skipping the inner one first. So the first
         * pass runs the inner declarator over a throwaway type purely
         * to find the ')' , the outer suffix is then parsed against
         * the real base, and the second pass re-runs the inner
         * declarator against that. Two scans of a few tokens, against
         * building and walking a declarator tree for the sake of one
         * construct. */
        type_t placeholder = {0};
        char *ignored = NULL;
        declarator(&placeholder, &ignored);
        expect(")");
        type_t *real = type_suffix(base);
        token_t *after = tk;

        tk = start->next;
        type_t *r = declarator(real, name);
        tk = after;
        return r;
    }

    if (tk->kind == TK_IDENT) {
        *name = tk->text;
        tk = tk->next;
    }
    return type_suffix(base);
}

static type_t *abstract_declarator(type_t *base) {
    char *ignored = NULL;
    return declarator(base, &ignored);
}

static type_t *typename_(void) {
    int a, b, c;
    type_t *base = declspec(&a, &b, &c);
    return abstract_declarator(base);
}

/* ------------------------------------------------------------------ */
/* constant expressions                                                */

/*
 * A second, non-emitting expression evaluator.
 *
 * Array bounds, enum values, case labels, global initialisers and #if
 * all need a VALUE, not code, and the main expression parser produces
 * code by construction. Rolling back emitted code (emit_mark) works
 * for `sizeof`, where only the type is wanted, but not here, where the
 * answer has to be computed at compile time and there may be no code
 * that computes it.
 *
 * So this duplicates precedence, and the duplication is the honest
 * cost. It is bounded: constant expressions have no assignment, no
 * function calls, no lvalues and no side effects, so it is a third of
 * the grammar and cannot drift in any way that matters -- a
 * disagreement between the two shows up immediately as a constant that
 * will not compile.
 */
static long ce_ternary(void);

static long ce_primary(void) {
    if (accept("(")) {
        long v = ce_ternary();
        expect(")");
        return v;
    }
    if (tk->kind == TK_NUM) { long v = (long)(int32_t)tk->val; tk = tk->next; return v; }

    if (at("sizeof")) {
        tk = tk->next;
        if (at("(") && is_type_start(tk->next)) {
            tk = tk->next;
            type_t *t = typename_();
            expect(")");
            return t->size;
        }
        emit_mark_t m = emit_mark();
        val_t v = unary();
        emit_rollback(m);
        return v.ty->size;
    }

    if (tk->kind == TK_IDENT) {
        symbol_t *s = find_sym(tk->text);
        if (s && s->kind == SYM_ENUMCONST) { tk = tk->next; return (long)s->enum_val; }
        zcc_error_at_tok("'%s' is not a constant", tk->text);
    }
    zcc_error_at_tok("expected a constant expression, found '%s'", tk->text);
    return 0;
}

static long ce_unary(void) {
    if (accept("-")) return -ce_unary();
    if (accept("+")) return ce_unary();
    if (accept("!")) return !ce_unary();
    if (accept("~")) return ~ce_unary();
    if (at("(") && is_type_start(tk->next)) {
        tk = tk->next;
        typename_();
        expect(")");
        return ce_unary();          /* casts in constants: value unchanged */
    }
    return ce_primary();
}

static long ce_mul(void) {
    long v = ce_unary();
    for (;;) {
        if (accept("*")) v = v * ce_unary();
        else if (accept("/")) { long r = ce_unary(); if (!r) zcc_error_at_tok("division by zero in constant"); v /= r; }
        else if (accept("%")) { long r = ce_unary(); if (!r) zcc_error_at_tok("division by zero in constant"); v %= r; }
        else return v;
    }
}
static long ce_add(void) {
    long v = ce_mul();
    for (;;) {
        if (accept("+")) v = v + ce_mul();
        else if (accept("-")) v = v - ce_mul();
        else return v;
    }
}
static long ce_shift(void) {
    long v = ce_add();
    for (;;) {
        if (accept("<<")) v = v << ce_add();
        else if (accept(">>")) v = v >> ce_add();
        else return v;
    }
}
static long ce_rel(void) {
    long v = ce_shift();
    for (;;) {
        if (accept("<")) v = v < ce_shift();
        else if (accept(">")) v = v > ce_shift();
        else if (accept("<=")) v = v <= ce_shift();
        else if (accept(">=")) v = v >= ce_shift();
        else return v;
    }
}
static long ce_eq(void) {
    long v = ce_rel();
    for (;;) {
        if (accept("==")) v = (v == ce_rel());
        else if (accept("!=")) v = (v != ce_rel());
        else return v;
    }
}
static long ce_band(void) { long v = ce_eq(); while (at("&") && !at("&&")) { tk = tk->next; v &= ce_eq(); } return v; }
static long ce_bxor(void) { long v = ce_band(); while (accept("^")) v ^= ce_band(); return v; }
static long ce_bor(void)  { long v = ce_bxor(); while (at("|") && !at("||")) { tk = tk->next; v |= ce_bxor(); } return v; }
static long ce_land(void) { long v = ce_bor(); while (accept("&&")) { long r = ce_bor(); v = v && r; } return v; }
static long ce_lor(void)  { long v = ce_land(); while (accept("||")) { long r = ce_land(); v = v || r; } return v; }

static long ce_ternary(void) {
    long c = ce_lor();
    if (accept("?")) {
        long a = ce_ternary();
        expect(":");
        long b = ce_ternary();
        return c ? a : b;
    }
    return c;
}

static long const_expr(void) { return ce_ternary(); }

/* ------------------------------------------------------------------ */
/* expressions                                                         */

static member_t *find_member(type_t *ty, const char *name) {
    for (member_t *m = ty->members; m; m = m->next)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

/* Emits the address of a symbol into a0. */
static void gen_sym_addr(symbol_t *s) {
    switch (s->kind) {
    case SYM_LOCAL: gen_local_addr(REG_A0, s->offset); break;
    case SYM_FUNC:
        if (!s->referenced) { s->referenced = 1; s->first_use = tk; }
        ei_la_label(REG_A0, s->label, 0);
        break;
    case SYM_GLOBAL: ei_la_label(REG_A0, s->label, 0); break;
    default:
        zcc_error_at_tok("'%s' has no address", s->name);
    }
}

static val_t funcall(symbol_t *fsym, val_t callee) {

    /* Direct calls go through jal and a fixup; indirect ones evaluate
     * the callee first and stash it on the stack, because a0..a7 are
     * about to be overwritten with arguments. */
    if (!fsym) gen_push();

    int nargs = 0;
    if (!at(")")) {
        for (;;) {
            val_t a = rvalue(assign_expr());
            (void)a;
            gen_push();
            if (++nargs > 8)
                zcc_error_at_tok("more than 8 arguments is not supported "
                                 "(see docs/zcc.md)");
            if (!accept(",")) break;
        }
    }
    expect(")");

    for (int i = nargs - 1; i >= 0; i--) gen_pop(REG_A0 + i);

    type_t *fnty;
    if (fsym) {
        fnty = fsym->ty;
        ei_jal_label(REG_RA, fsym->label);
    } else {
        gen_pop(REG_T0);
        fnty = callee.ty->kind == TY_PTR ? callee.ty->base : callee.ty;
        if (fnty->kind != TY_FUNC)
            zcc_error_at_tok("called object is not a function");
        ei_jalr(REG_RA, REG_T0, 0);
    }

    val_t r = { .ty = fnty->kind == TY_FUNC ? fnty->base : ty_int };
    return r;
}

static val_t postfix(val_t v);

static val_t primary(void) {

    val_t v = { .ty = ty_int };

    if (accept("(")) {
        /* A cast, or a parenthesised expression. `(` followed by a
         * type is the only thing that distinguishes them, and a
         * typedef name makes that require the symbol table -- which is
         * why is_type_start() consults it. */
        if (is_type_start(tk)) {
            type_t *ty = typename_();
            expect(")");
            val_t x = rvalue(unary());
            gen_cast(x.ty, ty);
            v.ty = ty;
            v.is_lval = 0;
            return v;
        }
        v = expr();
        expect(")");
        return postfix(v);
    }

    if (tk->kind == TK_NUM) {
        ei_li(REG_A0, tk->val);
        v.ty = tk->is_unsigned ? ty_uint : ty_int;
        tk = tk->next;
        return v;
    }

    if (tk->kind == TK_STR) {
        int l = emit_data_label(tk->str, tk->str_len, 1);
        ei_la_label(REG_A0, l, 0);
        v.ty = ty_pointer_to(ty_char);
        tk = tk->next;
        return postfix(v);
    }

    if (at("sizeof")) {
        tk = tk->next;
        int size;
        if (at("(") && is_type_start(tk->next)) {
            tk = tk->next;
            type_t *t = typename_();
            expect(")");
            size = t->size;
        } else {
            emit_mark_t m = emit_mark();
            val_t x = unary();
            emit_rollback(m);
            size = x.ty->size;
        }
        ei_li(REG_A0, (uint32_t)size);
        v.ty = ty_uint;
        return v;
    }

    if (tk->kind == TK_IDENT) {
        symbol_t *s = find_sym(tk->text);
        char *name = tk->text;
        tk = tk->next;

        if (!s) {
            /* An undeclared name used as a call is an error, not an
             * implicit int declaration. C89 allowed the latter and it
             * is the single most common way a typo becomes a link-time
             * mystery rather than a compile-time message. */
            zcc_error_at_tok("'%s' is not declared", name);
        }

        if (s->kind == SYM_ENUMCONST) {
            ei_li(REG_A0, s->enum_val);
            v.ty = ty_int;
            return postfix(v);
        }

        if (s->kind == SYM_FUNC && at("(")) {
            if (!s->referenced) { s->referenced = 1; s->first_use = tk; }
            tk = tk->next;
            v = funcall(s, v);
            return postfix(v);
        }

        gen_sym_addr(s);
        v.ty = s->ty;
        v.is_lval = 1;
        return postfix(v);
    }

    zcc_error_at_tok("unexpected '%s'", tk->text);
    return v;
}

static val_t postfix(val_t v) {

    for (;;) {

        if (at("(")) {
            tk = tk->next;
            val_t c = rvalue(v);
            v = funcall(NULL, c);
            continue;
        }

        if (at("[")) {
            tk = tk->next;
            val_t base = rvalue(v);
            if (base.ty->kind != TY_PTR)
                zcc_error_at_tok("subscripted value is not a pointer or array");
            gen_push();
            val_t idx = rvalue(expr());
            if (!ty_is_integer(idx.ty))
                zcc_error_at_tok("array subscript is not an integer");
            expect("]");
            int esz = base.ty->base->size;
            if (esz != 1) { ei_li(REG_T1, (uint32_t)esz); ei_mul(REG_A0, REG_A0, REG_T1); }
            gen_pop(REG_T0);
            ei_add(REG_A0, REG_T0, REG_A0);
            v.ty = base.ty->base;
            v.is_lval = 1;
            continue;
        }

        if (at(".") || at("->")) {
            int arrow = at("->");
            tk = tk->next;
            if (arrow) v = rvalue(v);
            type_t *sty = v.ty;
            if (arrow) {
                if (sty->kind != TY_PTR)
                    zcc_error_at_tok("'->' applied to something that is not a pointer");
                sty = sty->base;
            }
            if (sty->kind != TY_STRUCT && sty->kind != TY_UNION)
                zcc_error_at_tok("'%s' applied to a non-struct",
                                 arrow ? "->" : ".");
            if (tk->kind != TK_IDENT)
                zcc_error_at_tok("expected a member name");
            member_t *m = find_member(sty, tk->text);
            if (!m)
                zcc_error_at_tok("no member named '%s' in %s '%s'", tk->text,
                                 sty->kind == TY_UNION ? "union" : "struct",
                                 sty->tag ? sty->tag : "<anonymous>");
            tk = tk->next;
            if (m->offset) ei_addi(REG_A0, REG_A0, m->offset);
            v.ty = m->ty;
            v.is_lval = 1;
            continue;
        }

        if (at("++") || at("--")) {
            int inc = at("++");
            tk = tk->next;
            if (!v.is_lval) zcc_error_at_tok("'%s' needs an lvalue",
                                             inc ? "++" : "--");
            type_t *ty = v.ty;
            int step = (ty->kind == TY_PTR) ? ty->base->size : 1;

            /* Post-increment: keep the old value in a1 while the new
             * one is written back through the address in t0. a1 rather
             * than the stack because nothing between here and the end
             * of this sequence can call anything. */
            ei_mv(REG_T0, REG_A0);
            ei_load(REG_A1, REG_T0, 0, ty->size, ty->is_unsigned);
            ei_addi(REG_A0, REG_A1, inc ? step : -step);
            ei_store(REG_A0, REG_T0, 0, ty->size);
            ei_mv(REG_A0, REG_A1);
            v.ty = ty;
            v.is_lval = 0;
            continue;
        }

        return v;
    }
}

static val_t unary(void) {

    if (accept("+")) return rvalue(unary());

    if (accept("-")) {
        val_t v = rvalue(unary());
        ei_sub(REG_A0, REG_ZERO, REG_A0);
        return v;
    }

    if (accept("!")) {
        val_t v = rvalue(unary());
        (void)v;
        ei_slti(REG_A0, REG_A0, 1, 1);      /* sltiu a0, a0, 1  ==  a0 == 0 */
        val_t r = { .ty = ty_int };
        return r;
    }

    if (accept("~")) {
        val_t v = rvalue(unary());
        ei_xori(REG_A0, REG_A0, -1);
        return v;
    }

    if (at("&")) {
        tk = tk->next;
        val_t v = unary();
        if (!v.is_lval) {
            /* &function is the one lvalue-less case that is legal:
             * a function designator already denotes its own address. */
            if (v.ty->kind == TY_FUNC) { v.ty = ty_pointer_to(v.ty); return v; }
            zcc_error_at_tok("'&' needs an lvalue");
        }
        val_t r = { .ty = ty_pointer_to(v.ty) };
        return r;
    }

    if (at("*")) {
        tk = tk->next;
        val_t v = rvalue(unary());
        if (v.ty->kind != TY_PTR)
            zcc_error_at_tok("'*' applied to something that is not a pointer");
        val_t r = { .ty = v.ty->base, .is_lval = 1 };
        return postfix(r);
    }

    if (at("++") || at("--")) {
        int inc = at("++");
        tk = tk->next;
        val_t v = unary();
        if (!v.is_lval) zcc_error_at_tok("'%s' needs an lvalue", inc ? "++" : "--");
        type_t *ty = v.ty;
        int step = (ty->kind == TY_PTR) ? ty->base->size : 1;
        ei_mv(REG_T0, REG_A0);
        ei_load(REG_A0, REG_T0, 0, ty->size, ty->is_unsigned);
        ei_addi(REG_A0, REG_A0, inc ? step : -step);
        ei_store(REG_A0, REG_T0, 0, ty->size);
        val_t r = { .ty = ty };
        return r;
    }

    if (at("sizeof")) return primary();

    return primary();
}

/*
 * Binary operator emission.
 *
 * By the time this runs, the left operand is on the stack and the
 * right is in a0. Popping the left into t0 gives `t0 OP a0`, which is
 * the operand order the caller expects -- getting it backwards makes
 * subtraction and every comparison silently wrong in a way that
 * addition and multiplication hide.
 */
static val_t gen_binop(const char *op, type_t *lt, type_t *rt) {

    val_t r = { .ty = ty_int };
    int uns = lt->is_unsigned || rt->is_unsigned;

    if (lt->kind == TY_FLOAT || rt->kind == TY_FLOAT)
        zcc_error_at_tok("floating-point arithmetic is not supported "
                         "(see docs/zcc.md)");

    /* pointer arithmetic */
    if (!strcmp(op, "+") || !strcmp(op, "-")) {
        if (lt->kind == TY_PTR && ty_is_integer(rt)) {
            int esz = lt->base->size;
            gen_pop(REG_T0);
            if (esz != 1) { ei_li(REG_T1, (uint32_t)esz); ei_mul(REG_A0, REG_A0, REG_T1); }
            if (op[0] == '+') ei_add(REG_A0, REG_T0, REG_A0);
            else ei_sub(REG_A0, REG_T0, REG_A0);
            r.ty = lt;
            return r;
        }
        if (ty_is_integer(lt) && rt->kind == TY_PTR && op[0] == '+') {
            int esz = rt->base->size;
            gen_pop(REG_T0);
            if (esz != 1) { ei_li(REG_T1, (uint32_t)esz); ei_mul(REG_T0, REG_T0, REG_T1); }
            ei_add(REG_A0, REG_T0, REG_A0);
            r.ty = rt;
            return r;
        }
        if (lt->kind == TY_PTR && rt->kind == TY_PTR && op[0] == '-') {
            int esz = lt->base->size;
            gen_pop(REG_T0);
            ei_sub(REG_A0, REG_T0, REG_A0);
            if (esz != 1) { ei_li(REG_T1, (uint32_t)esz); ei_div(REG_A0, REG_A0, REG_T1, 0); }
            r.ty = ty_int;
            return r;
        }
    }

    gen_pop(REG_T0);

    if (!strcmp(op, "+"))       ei_add(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, "-"))  ei_sub(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, "*"))  ei_mul(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, "/"))  ei_div(REG_A0, REG_T0, REG_A0, uns);
    else if (!strcmp(op, "%"))  ei_rem(REG_A0, REG_T0, REG_A0, uns);
    else if (!strcmp(op, "&"))  ei_and(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, "|"))  ei_or(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, "^"))  ei_xor(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, "<<")) ei_sll(REG_A0, REG_T0, REG_A0);
    else if (!strcmp(op, ">>")) {
        /* Right shift follows the LEFT operand's signedness only. A
         * shift is not a usual-arithmetic-conversions context, so
         * `signed >> unsigned` is still arithmetic. Using `uns` here
         * would turn `x >> 1` into a logical shift whenever the shift
         * count happened to be an unsigned variable. */
        if (lt->is_unsigned) ei_srl(REG_A0, REG_T0, REG_A0);
        else ei_sra(REG_A0, REG_T0, REG_A0);
        r.ty = lt;
        return r;
    }
    else if (!strcmp(op, "==")) { ei_xor(REG_A0, REG_T0, REG_A0); ei_slti(REG_A0, REG_A0, 1, 1); return r; }
    else if (!strcmp(op, "!=")) { ei_xor(REG_A0, REG_T0, REG_A0); ei_slt(REG_A0, REG_ZERO, REG_A0, 1); return r; }
    else if (!strcmp(op, "<"))  { ei_slt(REG_A0, REG_T0, REG_A0, uns); return r; }
    else if (!strcmp(op, ">"))  { ei_slt(REG_A0, REG_A0, REG_T0, uns); return r; }
    else if (!strcmp(op, "<=")) { ei_slt(REG_A0, REG_A0, REG_T0, uns); ei_xori(REG_A0, REG_A0, 1); return r; }
    else if (!strcmp(op, ">=")) { ei_slt(REG_A0, REG_T0, REG_A0, uns); ei_xori(REG_A0, REG_A0, 1); return r; }
    else zcc_error_at_tok("internal: unknown operator '%s'", op);

    /* The result type of an arithmetic operator is the wider/unsigned
     * of the two, which at 32 bits everywhere reduces to signedness. */
    r.ty = uns ? ty_uint : ty_int;
    if (lt->kind == TY_PTR) r.ty = lt;
    return r;
}

/*
 * The binary precedence ladder.
 *
 * Written as a table-driven loop rather than eleven near-identical
 * functions. The levels are in C's order and the table IS the grammar,
 * which makes a precedence mistake something you can see rather than
 * something you have to trace.
 */
typedef struct { int level; const char *op; } binop_t;

static const binop_t binops[] = {
    { 1,  "*" }, { 1,  "/" }, { 1,  "%" },
    { 2,  "+" }, { 2,  "-" },
    { 3,  "<<" }, { 3, ">>" },
    { 4,  "<" }, { 4,  ">" }, { 4, "<=" }, { 4, ">=" },
    { 5,  "==" }, { 5, "!=" },
    { 6,  "&" },
    { 7,  "^" },
    { 8,  "|" },
    { 0, NULL }
};

#define MAX_BIN_LEVEL 8

static const char *peek_binop(int level) {
    if (tk->kind != TK_PUNCT) return NULL;
    /* '&&' and '||' must not be mistaken for '&' and '|': the lexer
     * already produced them as single tokens, so a plain text compare
     * is enough, but only because of that. */
    for (int i = 0; binops[i].op; i++)
        if (binops[i].level == level && !strcmp(tk->text, binops[i].op))
            return binops[i].op;
    return NULL;
}

static val_t binary(int level) {

    if (level == 0) return unary();

    val_t l = binary(level - 1);
    for (;;) {
        const char *op = peek_binop(level);
        if (!op) return l;
        tk = tk->next;
        l = rvalue(l);
        gen_push();
        val_t r = rvalue(binary(level - 1));
        l = gen_binop(op, l.ty, r.ty);
    }
}

static val_t logand(void) {
    val_t l = binary(MAX_BIN_LEVEL);
    if (!at("&&")) return l;

    int lfalse = emit_new_label(), lend = emit_new_label();
    l = rvalue(l);
    ei_branch_label("beq", REG_A0, REG_ZERO, lfalse);

    while (accept("&&")) {
        val_t r = rvalue(binary(MAX_BIN_LEVEL));
        (void)r;
        ei_branch_label("beq", REG_A0, REG_ZERO, lfalse);
    }

    ei_li(REG_A0, 1);
    ei_jal_label(REG_ZERO, lend);
    emit_place_label(lfalse);
    ei_mv(REG_A0, REG_ZERO);
    emit_place_label(lend);

    val_t v = { .ty = ty_int };
    return v;
}

static val_t logor(void) {
    val_t l = logand();
    if (!at("||")) return l;

    int ltrue = emit_new_label(), lend = emit_new_label();
    l = rvalue(l);
    ei_branch_label("bne", REG_A0, REG_ZERO, ltrue);

    while (accept("||")) {
        val_t r = rvalue(logand());
        (void)r;
        ei_branch_label("bne", REG_A0, REG_ZERO, ltrue);
    }

    ei_mv(REG_A0, REG_ZERO);
    ei_jal_label(REG_ZERO, lend);
    emit_place_label(ltrue);
    ei_li(REG_A0, 1);
    emit_place_label(lend);

    val_t v = { .ty = ty_int };
    return v;
}

static val_t conditional(void) {

    val_t c = logor();
    if (!at("?")) return c;
    tk = tk->next;

    c = rvalue(c);
    int lelse = emit_new_label(), lend = emit_new_label();
    ei_branch_label("beq", REG_A0, REG_ZERO, lelse);

    val_t a = rvalue(expr());
    ei_jal_label(REG_ZERO, lend);
    expect(":");
    emit_place_label(lelse);
    val_t b = rvalue(conditional());
    emit_place_label(lend);

    /* The result type is the first arm's, unless it is void or the
     * second arm is a pointer. Enough for real code without
     * implementing the full composite-type rules, which exist mostly
     * to define the type of expressions nobody writes. */
    val_t v = { .ty = (a.ty->kind == TY_VOID || b.ty->kind == TY_PTR) ? b.ty : a.ty };
    return v;
}

static void check_assignable(type_t *lt, type_t *rt) {
    if (lt->kind == TY_STRUCT || lt->kind == TY_UNION) {
        if (!ty_same(lt, rt))
            zcc_error_at_tok("incompatible struct assignment");
        return;
    }
    if (ty_is_scalar(lt) && ty_is_scalar(rt)) return;
    zcc_error_at_tok("incompatible types in assignment");
}

static val_t assign_expr(void) {

    val_t l = conditional();

    static const char *cops[] = { "+=", "-=", "*=", "/=", "%=",
                                  "&=", "|=", "^=", "<<=", ">>=", NULL };

    if (at("=")) {
        tk = tk->next;
        if (!l.is_lval) zcc_error_at_tok("assignment to something that is not an lvalue");
        type_t *lt = l.ty;
        gen_push();                             /* the destination address */
        val_t r = rvalue(assign_expr());
        check_assignable(lt, r.ty);
        gen_pop(REG_T0);
        gen_store(lt);
        /* a0 still holds the assigned value, so `a = b = c` and
         * `if ((n = f()))` both work without reloading. For a struct
         * it holds the source address, which is what a struct-valued
         * assignment expression should evaluate to here. */
        val_t v = { .ty = lt };
        return v;
    }

    for (int i = 0; cops[i]; i++) {
        if (!at(cops[i])) continue;
        char op[3];
        op[0] = cops[i][0];
        op[1] = (cops[i][1] == '=') ? 0 : cops[i][1];
        op[2] = 0;
        tk = tk->next;

        if (!l.is_lval) zcc_error_at_tok("'%s' needs an lvalue", cops[i]);
        type_t *lt = l.ty;

        /* The address is evaluated ONCE and kept on the stack, which
         * is what makes `p[i++] += 1` increment i once. Re-parsing the
         * left side would be simpler and wrong. */
        gen_push();                             /* address */
        ei_load(REG_A0, REG_A0, 0, lt->size, lt->is_unsigned);
        gen_push();                             /* current value */
        val_t r = rvalue(assign_expr());
        val_t res = gen_binop(op, ty_decay(lt), r.ty);
        (void)res;
        gen_pop(REG_T0);                        /* address */
        gen_store(lt);
        val_t v = { .ty = lt };
        return v;
    }

    return l;
}

static val_t expr(void) {
    val_t v = assign_expr();
    while (at(",")) {
        tk = tk->next;
        v = assign_expr();
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* initialisers                                                        */

/* Writes a constant initialiser for a global into `bytes`, and records
 * any address-of relocations against `dlabel`. */
static void global_init(type_t *ty, uint8_t *bytes, int off, int dlabel);

static void put_le(uint8_t *p, uint32_t v, int size) {
    for (int i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void global_scalar_init(type_t *ty, uint8_t *bytes, int off, int dlabel) {

    /* A string literal initialising a char* is a pointer to a separate
     * object in .data, so it needs its own label and a relocation --
     * unlike a string initialising a char[], which is handled by the
     * caller as bytes. */
    if (tk->kind == TK_STR && ty->kind == TY_PTR) {
        int l = emit_data_label(tk->str, tk->str_len, 1);
        emit_data_fixup_label(dlabel, off, l, 0);
        tk = tk->next;
        return;
    }

    if (at("&")) {
        tk = tk->next;
        if (tk->kind != TK_IDENT) zcc_error_at_tok("expected a name after '&'");
        symbol_t *s = find_sym(tk->text);
        if (!s || (s->kind != SYM_GLOBAL && s->kind != SYM_FUNC))
            zcc_error_at_tok("'%s' is not a global", tk->text);
        tk = tk->next;
        int addend = 0;
        if (accept("+")) addend = (int)const_expr();
        else if (accept("-")) addend = -(int)const_expr();
        emit_data_fixup_label(dlabel, off, s->label, addend);
        return;
    }

    if (tk->kind == TK_IDENT) {
        symbol_t *s = find_sym(tk->text);
        if (s && s->kind == SYM_FUNC) {
            emit_data_fixup_label(dlabel, off, s->label, 0);
            tk = tk->next;
            return;
        }
    }

    put_le(bytes + off, (uint32_t)const_expr(), ty->size);
}

static void global_init(type_t *ty, uint8_t *bytes, int off, int dlabel) {

    if (ty->kind == TY_ARRAY && tk->kind == TK_STR &&
        ty->base->kind == TY_CHAR) {
        int n = tk->str_len;
        if (ty->array_len >= 0 && n > ty->array_len) n = ty->array_len;
        memcpy(bytes + off, tk->str, (size_t)n);
        tk = tk->next;
        return;
    }

    if (at("{")) {
        tk = tk->next;
        if (ty->kind == TY_ARRAY) {
            int i = 0;
            while (!at("}")) {
                if (ty->array_len >= 0 && i >= ty->array_len)
                    zcc_error_at_tok("too many initialisers for array");
                global_init(ty->base, bytes, off + i * ty->base->size, dlabel);
                i++;
                if (!accept(",")) break;
            }
            expect("}");
            return;
        }
        if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
            member_t *m = ty->members;
            while (!at("}") && m) {
                global_init(m->ty, bytes, off + m->offset, dlabel);
                m = m->next;
                if (!accept(",")) break;
            }
            expect("}");
            return;
        }
        /* `int x = { 5 };` is legal C and appears in generated code. */
        global_init(ty, bytes, off, dlabel);
        expect("}");
        return;
    }

    global_scalar_init(ty, bytes, off, dlabel);
}

/* Local initialisers generate STORES rather than data, because a local
 * is re-initialised on every entry to its block. */
static void local_init(type_t *ty, int base_off);

static void local_scalar_store(type_t *ty, int off) {
    val_t r = rvalue(assign_expr());
    check_assignable(ty, r.ty);
    gen_local_addr(REG_T0, off);
    gen_store(ty);
}

static void local_zero(int off, int size) {
    /* Word stores where alignment allows, bytes otherwise. Worth the
     * four extra lines: a `char buf[512] = {0}` is otherwise 512
     * instructions inline, which is most of a page of code for one
     * declaration. */
    int i = 0;
    if ((off % 4) == 0) {
        for (; i + 4 <= size; i += 4) {
            gen_local_addr(REG_T0, off + i);
            ei_store(REG_ZERO, REG_T0, 0, 4);
        }
    }
    for (; i < size; i++) {
        gen_local_addr(REG_T0, off + i);
        ei_store(REG_ZERO, REG_T0, 0, 1);
    }
}

static void local_init(type_t *ty, int base_off) {

    if (ty->kind == TY_ARRAY && tk->kind == TK_STR &&
        ty->base->kind == TY_CHAR) {
        int n = tk->str_len;
        if (ty->array_len >= 0 && n > ty->array_len) n = ty->array_len;
        for (int i = 0; i < n; i++) {
            ei_li(REG_A0, (uint32_t)(uint8_t)tk->str[i]);
            gen_local_addr(REG_T0, base_off + i);
            ei_store(REG_A0, REG_T0, 0, 1);
        }
        if (ty->array_len > n) local_zero(base_off + n, ty->array_len - n);
        tk = tk->next;
        return;
    }

    if (at("{")) {
        tk = tk->next;
        if (ty->kind == TY_ARRAY) {
            int i = 0;
            while (!at("}")) {
                if (ty->array_len >= 0 && i >= ty->array_len)
                    zcc_error_at_tok("too many initialisers for array");
                local_init(ty->base, base_off + i * ty->base->size);
                i++;
                if (!accept(",")) break;
            }
            expect("}");
            /* Anything the braces did not cover is zero, which is what
             * makes `{0}` the idiom it is. */
            if (ty->array_len > i)
                local_zero(base_off + i * ty->base->size,
                           (ty->array_len - i) * ty->base->size);
            return;
        }
        if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
            member_t *m = ty->members;
            int covered = 0;
            while (!at("}") && m) {
                local_init(m->ty, base_off + m->offset);
                covered = m->offset + m->ty->size;
                m = m->next;
                if (!accept(",")) break;
            }
            expect("}");
            if (ty->size > covered) local_zero(base_off + covered, ty->size - covered);
            return;
        }
        local_init(ty, base_off);
        expect("}");
        return;
    }

    local_scalar_store(ty, base_off);
}

/* ------------------------------------------------------------------ */
/* statements                                                          */

static void push_loop(int brk, int cont) {
    loop_ctx_t *l = zalloc(sizeof(loop_ctx_t));
    l->brk = brk;
    l->cont = cont;
    l->next = loops;
    loops = l;
}

static void pop_loop(void) { loops = loops->next; }

static goto_ref_t *get_goto(const char *name) {
    for (goto_ref_t *g = gotos; g; g = g->next)
        if (!strcmp(g->name, name)) return g;
    goto_ref_t *g = zalloc(sizeof(goto_ref_t));
    g->name = zstrdup(name);
    g->label = emit_new_label();
    g->at = tk;
    g->next = gotos;
    gotos = g;
    return g;
}

/*
 * switch.
 *
 * The dispatch table is emitted AFTER the body, because in one pass
 * the case values are not known until the body has been parsed. So the
 * control expression is stashed in a frame slot, control jumps over
 * the body to the comparison chain, and each case jumps back into it.
 *
 * A jump table would be faster and needs the case values sorted, which
 * needs them all known, which is the same problem -- it would work
 * here too, since the chain is built after the body. It is not done
 * because a chain of comparisons is correct for sparse and dense
 * values alike and this compiler has no benchmark saying otherwise.
 */
typedef struct case_ent {
    struct case_ent *next;
    long val;
    int label;
    int is_default;
} case_ent_t;

static case_ent_t *cur_cases;
static int in_switch;

static void statement(void) {

    if (at("{")) {
        tk = tk->next;
        enter_scope();
        while (!at("}")) {
            if (tk->kind == TK_EOF) zcc_error_at_tok("unterminated block");
            declaration_or_statement();
        }
        tk = tk->next;
        leave_scope();
        return;
    }

    if (accept(";")) return;

    if (at("if")) {
        tk = tk->next;
        expect("(");
        rvalue(expr());
        expect(")");
        int lelse = emit_new_label(), lend = emit_new_label();
        ei_branch_label("beq", REG_A0, REG_ZERO, lelse);
        statement();
        ei_jal_label(REG_ZERO, lend);
        emit_place_label(lelse);
        if (accept("else")) statement();
        emit_place_label(lend);
        return;
    }

    if (at("while")) {
        tk = tk->next;
        int ltop = emit_new_label(), lend = emit_new_label();
        emit_place_label(ltop);
        expect("(");
        rvalue(expr());
        expect(")");
        ei_branch_label("beq", REG_A0, REG_ZERO, lend);
        push_loop(lend, ltop);
        statement();
        pop_loop();
        ei_jal_label(REG_ZERO, ltop);
        emit_place_label(lend);
        return;
    }

    if (at("do")) {
        tk = tk->next;
        int ltop = emit_new_label(), lcont = emit_new_label(), lend = emit_new_label();
        emit_place_label(ltop);
        push_loop(lend, lcont);
        statement();
        pop_loop();
        emit_place_label(lcont);
        expect("while");
        expect("(");
        rvalue(expr());
        expect(")");
        expect(";");
        ei_branch_label("bne", REG_A0, REG_ZERO, ltop);
        emit_place_label(lend);
        return;
    }

    if (at("for")) {
        tk = tk->next;
        expect("(");
        enter_scope();          /* for a declaration in the init clause */

        if (!accept(";")) {
            if (is_type_start(tk)) declaration_or_statement();
            else { expr(); expect(";"); }
        }

        int ltop = emit_new_label(), lcont = emit_new_label(), lend = emit_new_label();
        emit_place_label(ltop);

        if (!at(";")) {
            rvalue(expr());
            ei_branch_label("beq", REG_A0, REG_ZERO, lend);
        }
        expect(";");

        /* The step expression textually precedes the body but must run
         * after it. In one pass that means emitting it here, jumped
         * over, and jumping back -- three extra branches per
         * iteration. Buffering the tokens and re-parsing them after
         * the body would emit better code and is the obvious later
         * improvement; correctness first. */
        int lstep = emit_new_label(), lbody = emit_new_label();
        ei_jal_label(REG_ZERO, lbody);
        emit_place_label(lstep);
        if (!at(")")) expr();
        expect(")");
        ei_jal_label(REG_ZERO, ltop);
        emit_place_label(lbody);

        push_loop(lend, lcont);
        statement();
        pop_loop();
        emit_place_label(lcont);
        ei_jal_label(REG_ZERO, lstep);
        emit_place_label(lend);

        leave_scope();
        return;
    }

    if (at("switch")) {
        tk = tk->next;
        expect("(");
        rvalue(expr());
        expect(")");

        int slot = alloc_local(4, 4);
        gen_local_addr(REG_T0, slot);
        ei_store(REG_A0, REG_T0, 0, 4);

        int ldisp = emit_new_label(), lend = emit_new_label();
        ei_jal_label(REG_ZERO, ldisp);

        case_ent_t *saved = cur_cases;
        int saved_in = in_switch;
        cur_cases = NULL;
        in_switch = 1;

        push_loop(lend, loops ? loops->cont : -1);
        statement();
        pop_loop();

        ei_jal_label(REG_ZERO, lend);
        emit_place_label(ldisp);
        gen_local_addr(REG_T0, slot);
        ei_load(REG_T0, REG_T0, 0, 4, 0);

        int ldefault = -1;
        for (case_ent_t *c = cur_cases; c; c = c->next) {
            if (c->is_default) { ldefault = c->label; continue; }
            ei_li(REG_T1, (uint32_t)c->val);
            ei_branch_label("beq", REG_T0, REG_T1, c->label);
        }
        ei_jal_label(REG_ZERO, ldefault >= 0 ? ldefault : lend);
        emit_place_label(lend);

        cur_cases = saved;
        in_switch = saved_in;
        return;
    }

    if (at("case") || at("default")) {
        int is_default = at("default");
        tk = tk->next;
        if (!in_switch) zcc_error_at_tok("'%s' outside a switch",
                                         is_default ? "default" : "case");
        case_ent_t *c = zalloc(sizeof(case_ent_t));
        c->is_default = is_default;
        if (!is_default) c->val = const_expr();
        c->label = emit_new_label();
        c->next = cur_cases;
        cur_cases = c;
        expect(":");
        emit_place_label(c->label);
        /* A label is not a statement, so what follows may legally be a
         * declaration -- `case 1: int x = 2;` -- or the closing brace. */
        if (!at("}")) declaration_or_statement();
        return;
    }

    if (at("break")) {
        tk = tk->next;
        expect(";");
        if (!loops || loops->brk < 0) zcc_error_at_tok("'break' outside a loop or switch");
        ei_jal_label(REG_ZERO, loops->brk);
        return;
    }

    if (at("continue")) {
        tk = tk->next;
        expect(";");
        loop_ctx_t *l = loops;
        while (l && l->cont < 0) l = l->next;
        if (!l) zcc_error_at_tok("'continue' outside a loop");
        ei_jal_label(REG_ZERO, l->cont);
        return;
    }

    if (at("return")) {
        tk = tk->next;
        if (!at(";")) {
            val_t v = rvalue(expr());
            (void)v;
            if (cur_ret_type->kind == TY_VOID)
                zcc_warn_at_tok("returning a value from a function returning void");
        }
        expect(";");
        ei_jal_label(REG_ZERO, cur_ret_label);
        return;
    }

    if (at("goto")) {
        tk = tk->next;
        if (tk->kind != TK_IDENT) zcc_error_at_tok("expected a label name");
        goto_ref_t *g = get_goto(tk->text);
        tk = tk->next;
        expect(";");
        ei_jal_label(REG_ZERO, g->label);
        return;
    }

    /* A label: IDENT ':' -- distinguishable from an expression only by
     * looking one token ahead, which is why this test sits after every
     * keyword and before the expression fallback. */
    if (tk->kind == TK_IDENT && tok_is(tk->next, ":")) {
        goto_ref_t *g = get_goto(tk->text);
        if (g->defined) zcc_error_at_tok("duplicate label '%s'", tk->text);
        g->defined = 1;
        emit_place_label(g->label);
        tk = tk->next;
        tk = tk->next;
        if (!at("}")) declaration_or_statement();
        return;
    }

    expr();
    expect(";");
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */

static void local_declaration(void) {

    int is_typedef, is_static, is_extern;
    type_t *base = declspec(&is_typedef, &is_static, &is_extern);

    if (accept(";")) return;

    for (;;) {
        char *name = NULL;
        type_t *ty = declarator(base, &name);
        if (!name) zcc_error_at_tok("a declaration needs a name");

        if (is_typedef) {
            push_sym(name, ty, SYM_TYPEDEF);
        } else if (ty->kind == TY_FUNC) {
            /* A function declared inside a block still has file scope.
             * Registered globally so a later call resolves. */
            symbol_t *s = NULL;
            for (symbol_t *g = global_scope.syms; g; g = g->next)
                if (!strcmp(g->name, name)) { s = g; break; }
            if (!s) {
                scope_t *save = scope;
                scope = &global_scope;
                s = push_sym(name, ty, SYM_FUNC);
                int lz = libz_label_for(name);
                if (lz >= 0) { s->label = lz; s->defined = 1; }
                else s->label = emit_new_label();
                scope = save;
            }
        } else if (is_static) {
            /* A static local is a global with a local name. Its
             * initialiser is therefore a constant one, evaluated now,
             * not stores executed on every entry.
             *
             * Including the self-sizing `static const char s[] = "..."`
             * form, which is how a function keeps a string table
             * without a global -- and which this branch got wrong at
             * first, because the sizing lived only in the global path. */
            if (ty->kind == TY_ARRAY && ty->array_len < 0 &&
                tok_is(tk, "=") && tk->next->kind == TK_STR)
                ty = ty_array_of(ty->base, tk->next->str_len);
            if (ty->size == 0) zcc_error_at_tok("'%s' has incomplete type", name);
            symbol_t *s = push_sym(name, ty, SYM_GLOBAL);
            if (accept("=")) {
                /* The space is reserved (and its label placed) BEFORE
                 * the initialiser is parsed, because the initialiser
                 * may take the address of the object it initialises,
                 * and any such relocation is recorded against that
                 * label. The bytes are then poked into the reserved
                 * space, which leaves those relocations valid. */
                uint8_t *bytes = zalloc((size_t)ty->size);
                int l = emit_data_label(NULL, ty->size, ty->align);
                s->label = l;
                global_init(ty, bytes, 0, l);
                emit_data_poke(l, bytes, ty->size);
            } else {
                s->label = emit_bss_label(ty->size, ty->align);
            }
        } else if (is_extern) {
            symbol_t *s = push_sym(name, ty, SYM_GLOBAL);
            s->label = emit_bss_label(ty->size ? ty->size : 4, ty->align);
        } else {
            if (ty->kind == TY_ARRAY && ty->array_len < 0) {
                /* `char s[] = "..."` sizes itself from its initialiser,
                 * which is not known until it is parsed. Peeking at the
                 * string is enough for the only form that occurs. */
                if (tok_is(tk, "=") && tk->next->kind == TK_STR)
                    ty = ty_array_of(ty->base, tk->next->str_len);
                else
                    zcc_error_at_tok("array '%s' needs a size", name);
            }
            if (ty->size == 0) zcc_error_at_tok("'%s' has incomplete type", name);
            symbol_t *s = push_sym(name, ty, SYM_LOCAL);
            s->offset = alloc_local(ty->size, ty->align);
            if (accept("=")) local_init(ty, s->offset);
        }

        if (!accept(",")) break;
    }
    expect(";");
}

static void declaration_or_statement(void) {
    if (is_type_start(tk)) local_declaration();
    else statement();
}

static void function_body(symbol_t *fn) {

    enter_scope();

    cur_locals = 0;
    cur_ret_label = emit_new_label();
    cur_ret_type = fn->ty->base;
    gotos = NULL;

    emit_place_label(fn->label);

    ei_addi(REG_SP, REG_SP, -16);
    ei_store(REG_RA, REG_SP, 12, 4);
    ei_store(REG_S0, REG_SP, 8, 4);
    ei_mv(REG_S0, REG_SP);

    /* Three slots, patched once the frame size is known. Three because
     * the long form is li (two instructions) plus add; the short form
     * is one addi and two nops. Reserving the worst case avoids a
     * shrink pass that would move every label after it. */
    cur_frame_patch = emit_here();
    ei_nop(); ei_nop(); ei_nop();

    /* Parameters are copied from a0..a7 into the frame at entry.
     * Keeping them in registers would be better code and would need a
     * register allocator to know when they die; spilling them all
     * makes every later reference a plain frame access. */
    int i = 0;
    for (type_t *p = fn->ty->params; p; p = p->next, i++) {
        if (!p->tag) continue;              /* an unnamed parameter */
        type_t *pt = zalloc(sizeof(type_t));
        *pt = *p;
        pt->next = NULL;
        symbol_t *s = push_sym(p->tag, pt, SYM_LOCAL);
        s->offset = alloc_local(pt->size, pt->align);
        gen_local_addr(REG_T0, s->offset);
        ei_store(REG_A0 + i, REG_T0, 0, pt->size);
    }

    expect("{");
    while (!at("}")) {
        if (tk->kind == TK_EOF) zcc_error_at_tok("unterminated function body");
        declaration_or_statement();
    }
    tk = tk->next;

    /* Falling off the end of a non-void function returns whatever is
     * in a0. C says that is undefined; warning about it is more
     * trouble than it is worth in a tree full of functions whose last
     * statement is a return inside an if/else. */
    emit_place_label(cur_ret_label);
    ei_mv(REG_SP, REG_S0);
    ei_load(REG_RA, REG_SP, 12, 4, 0);
    ei_load(REG_S0, REG_SP, 8, 4, 0);
    ei_addi(REG_SP, REG_SP, 16);
    ei_jalr(REG_ZERO, REG_RA, 0);

    int frame = align_to(cur_locals, 16);
    int save = emit_here();
    /* Rewind, write the real frame adjustment into the reserved slots,
     * then restore. Exactly three instructions are written back, which
     * is why three were reserved -- writing fewer would leave stale
     * bytes and writing more would overwrite the function's first real
     * instruction. */
    {
        emit_seek(cur_frame_patch);
        if (frame == 0) { ei_nop(); ei_nop(); ei_nop(); }
        else if (frame <= 2048) { ei_addi(REG_SP, REG_SP, -frame); ei_nop(); ei_nop(); }
        else { ei_li(REG_T0, (uint32_t)(-frame)); ei_add(REG_SP, REG_SP, REG_T0); }
        emit_seek(save);
    }

    for (goto_ref_t *g = gotos; g; g = g->next)
        if (!g->defined)
            zcc_error(g->at->file, g->at->line, g->at->col,
                      "label '%s' used but not defined", g->name);

    leave_scope();
}

static void global_declaration(void) {

    int is_typedef, is_static, is_extern;
    type_t *base = declspec(&is_typedef, &is_static, &is_extern);

    if (accept(";")) return;                /* a bare struct/enum definition */

    for (;;) {
        char *name = NULL;
        type_t *ty = declarator(base, &name);
        if (!name) zcc_error_at_tok("a declaration needs a name");

        if (is_typedef) {
            push_sym(name, ty, SYM_TYPEDEF);
            if (!accept(",")) break;
            continue;
        }

        if (ty->kind == TY_FUNC) {
            symbol_t *s = find_sym_local(name);
            if (!s) {
                s = push_sym(name, ty, SYM_FUNC);
                int lz = libz_label_for(name);
                if (!strcmp(name, "main") && main_label >= 0) s->label = main_label;
                else if (lz >= 0) { s->label = lz; s->defined = 1; }
                else s->label = emit_new_label();
            } else {
                s->ty = ty;                 /* a prototype now has params */
            }

            if (at("{")) {
                if (s->defined && libz_label_for(name) >= 0)
                    zcc_error_at_tok("'%s' is provided by the runtime and "
                                     "cannot be redefined", name);
                if (s->defined) zcc_error_at_tok("'%s' is defined twice", name);
                s->defined = 1;
                function_body(s);
                return;
            }
            expect(";");
            return;
        }

        symbol_t *s = find_sym_local(name);
        if (!s) s = push_sym(name, ty, SYM_GLOBAL);
        s->kind = SYM_GLOBAL;
        s->ty = ty;

        if (at("=")) {
            tk = tk->next;
            if (ty->kind == TY_ARRAY && ty->array_len < 0) {
                if (tk->kind == TK_STR) ty = ty_array_of(ty->base, tk->str_len);
                else zcc_error_at_tok("array '%s' needs a size", name);
                s->ty = ty;
            }
            uint8_t *bytes = zalloc((size_t)ty->size);
            int l = emit_data_label(NULL, ty->size, ty->align);
            s->label = l;
            global_init(ty, bytes, 0, l);
            emit_data_poke(l, bytes, ty->size);
        } else {
            if (ty->kind == TY_ARRAY && ty->array_len < 0)
                zcc_error_at_tok("array '%s' needs a size", name);
            if (ty->size == 0) zcc_error_at_tok("'%s' has incomplete type", name);
            if (!s->label) s->label = emit_bss_label(ty->size, ty->align);
        }

        if (!accept(",")) break;
    }
    expect(";");
}

/* ------------------------------------------------------------------ */
/* the entry stub                                                       */

/*
 * .text offset 0, because that is where the kernel starts a process:
 * k_proc_create() sets pc to the image base and sp to the top of the
 * process's block, with a sentinel return address already on the stack
 * (see docs/app_runtime.md).
 *
 * The stub calls main and then makes the Z_SYS_EXIT syscall by hand.
 * By hand rather than by calling an exit() from the runtime, because
 * at this stage there is no runtime -- that is Phase 2. Eight
 * instructions is a cheap price for a compiler that produces a
 * complete, runnable image with no other input.
 *
 * reg_kernel lives at address 0x0000000c, which is reachable with a
 * single `lw t0, 12(zero)` -- no address materialisation needed, which
 * is the one place this compiler gets to be smug.
 */
static void emit_entry_stub(void) {
    entry_stub_at = emit_here();
    main_label = emit_new_label();

    ei_jal_label(REG_RA, main_label);

    ei_addi(REG_SP, REG_SP, -16);
    ei_store(REG_ZERO, REG_SP, 0, 4);       /* z_obj_t.type = Z_NONE */
    ei_store(REG_A0, REG_SP, 4, 4);         /* .val = main's return value */
    ei_li(REG_A0, 1);                       /* Z_SYS_EXIT */
    ei_mv(REG_A1, REG_SP);
    ei_mv(REG_A2, REG_ZERO);
    ei_load(REG_T0, REG_ZERO, 12, 4, 0);    /* reg_kernel */
    ei_jalr(REG_RA, REG_T0, 0);

    /* The kernel does not return from EXIT. Spinning rather than
     * falling into whatever follows makes a kernel that ever does
     * return produce a hang that is obvious, instead of executing the
     * first function in the program as if it were the entry point. */
    int spin = emit_new_label();
    emit_place_label(spin);
    ei_jal_label(REG_ZERO, spin);
}

int parse_entry_label(void) { return main_label; }

/*
 * With the runtime blob present, .text offset 0 is already the
 * runtime's own _start -- the kernel jumps there and it calls main
 * through a word that only the compiler can fill in. So there is no
 * entry stub to emit; there are two words to patch.
 *
 * Recorded as ordinary fixups rather than written directly, because
 * neither value exists yet: main's address is not known until it is
 * defined, and the heap start is the image's _end, which is not known
 * until the image is laid out.
 */
static void libz_entry(int patch_main, int patch_heap) {
    main_label = emit_new_label();
    emit_fixup(patch_main, FIX_ABS32, main_label, 0);
    emit_fixup(patch_heap, FIX_ABS32, emit_image_end_label(), 0);
}

void parse_translation_unit(token_t *tok) {
    parse_translation_unit_ex(tok, -1, -1);
}

void parse_translation_unit_ex(token_t *tok, int patch_main, int patch_heap) {

    tk = tok;
    scope = &global_scope;

    if (patch_main >= 0) libz_entry(patch_main, patch_heap);
    else emit_entry_stub();

    symbol_t *m = push_sym("main", ty_func(ty_int), SYM_FUNC);
    m->label = main_label;

    while (tk->kind != TK_EOF)
        global_declaration();

    if (!m->defined)
        zcc_error(tok->file, 1, 1, "no main() in this translation unit");

    /*
     * A function that is called but never defined.
     *
     * This used to reach the emitter as a label nobody placed, and
     * come out as "internal error: label 26 (?) was never placed" --
     * which is true, useless, and points at the compiler instead of at
     * the program. The commonest way to produce it is not a typo: it
     * is building against libz.h WITHOUT -L, so every printf and
     * malloc is declared and none is resolved.
     *
     * Reported here, once, with the position of the first call, which
     * is what the programmer needs to see.
     */
    for (symbol_t *s = global_scope.syms; s; s = s->next) {
        if (s->kind != SYM_FUNC || s->defined || !s->referenced) continue;
        tk = s->first_use;
        if (libz_nsyms == 0)
            zcc_error_at_tok("'%s' is declared but never defined -- if it "
                             "belongs to the runtime, pass -L <libz dir>",
                             s->name);
        zcc_error_at_tok("'%s' is declared but never defined", s->name);
    }
}
