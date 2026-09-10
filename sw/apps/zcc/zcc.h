/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zcc -- the Zeitlos C compiler. Shared declarations.
 *
 * Written clean-room from published compiler-construction material and
 * the RISC-V unprivileged specification. See docs/zcc.md for the
 * design, the supported subset, and what is deliberately absent.
 *
 * Indented with SPACES, per README.md: LLM-assisted code in this tree
 * is space-indented so it is visible as such until it has been audited
 * and converted.
 *
 * -- The shape, in one paragraph --
 *
 * One pass. The parser walks the token stream and emits RV32IM machine
 * code as it goes; there is no AST and no intermediate representation.
 * That is not a shortcut taken to save effort, it is what the target
 * makes natural: a ZEXE image (docs/executables.md) is a flat blob at
 * a fixed base with no relocations and no symbol table, so there is no
 * assembler to emit text for and no linker to hand objects to. Code
 * comes out of the parser as instruction words and goes into a buffer.
 *
 * The cost is paid in code quality and is described honestly in
 * docs/zcc.md: expression evaluation is an accumulator model that
 * spills every intermediate to the stack. It is correct, it is
 * roughly three times the size of what GCC produces, and improving it
 * is a self-contained later job that does not touch the front end.
 */

#ifndef ZCC_H
#define ZCC_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "zcc_port.h"

/* Every message the compiler emits goes through one buffer this big. */
#define ZCC_MSG_MAX 1024

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */

/*
 * Every diagnostic carries file, line and column, because a compiler
 * that says only "syntax error" is a compiler nobody can use on a
 * machine with a 80x25 terminal and no scrollback.
 *
 * zcc_error() does not return. There is no error recovery and no
 * attempt to report a second error: a one-pass compiler that continues
 * after a parse error is generating code from a token stream it has
 * lost its place in, and every subsequent message is noise. One real
 * message beats twenty invented ones.
 */
void zcc_error(const char *file, int line, int col, const char *fmt, ...);

/*
 * All of the compiler's output, formatted and sent through zio_out().
 *
 * There is no fprintf here and no FILE anywhere, because the device
 * build has neither. One function, one buffer, both builds.
 */
void zcc_printf(const char *fmt, ...);

/* Reports a message and stops. Used for the handful of failures that
 * are not attached to a source position -- a missing input file, a
 * runtime blob that will not load. */
void zcc_fatal(const char *fmt, ...);

/* snprintf under another name.
 *
 * The host build has the real one and the device build has libz's, and
 * they have different prototypes in different headers. One name here
 * means main.c does not have to know which build it is in -- the same
 * reason zio_* exists. */
int zcc_snprintf(char *buf, int cap, const char *fmt, ...);

/* The formatter itself. zcc does NOT use vsnprintf -- see util.c for
 * the two reasons, one of which is that newlib's costs more than this
 * whole compiler. */
int zcc_vfmt(char *buf, int cap, const char *fmt, va_list ap);
void zcc_error_at_tok(const char *fmt, ...);
void zcc_warn_at_tok(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* tokens                                                              */

typedef enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_NUM,
    TK_STR,
    TK_CHAR,
    TK_PUNCT,
    TK_KEYWORD
} tk_kind_t;

typedef struct token {
    tk_kind_t kind;
    struct token *next;

    const char *file;
    int line, col;

    char *text;             /* identifier / punctuator / keyword spelling */

    uint32_t val;           /* TK_NUM, TK_CHAR */
    int is_unsigned;        /* TK_NUM had a u/U suffix, or did not fit int */

    char *str;              /* TK_STR payload, NOT NUL-terminated by itself */
    int str_len;            /* including the implicit trailing NUL */

    /* Preprocessor bookkeeping. `bol` is "first token on a line", which
     * is how a '#' is told from the modulo-looking one in `a #b` -- the
     * lexer cannot know, so it records the fact and the preprocessor
     * decides. */
    int bol;
    int has_space;
    int no_expand;          /* set on a macro's own name inside its own
                             * expansion, which is what stops
                             * `#define f f(x)` looping forever */
} token_t;

token_t *lex_file(const char *path);
token_t *preprocess(token_t *tok);
void cpp_add_include_dir(const char *dir);
void cpp_define_cli(const char *arg);     /* -DNAME or -DNAME=VALUE */
void cpp_undef_cli(const char *name);     /* -UNAME */

int tok_is(token_t *t, const char *s);
token_t *tok_skip(token_t *t, const char *s);   /* errors if not `s` */

/* ------------------------------------------------------------------ */
/* types                                                               */

typedef enum {
    TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_FLOAT,
    TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_ENUM, TY_FUNC
} ty_kind_t;

typedef struct type type_t;
typedef struct member member_t;

struct type {
    ty_kind_t kind;
    int size;               /* sizeof */
    int align;
    int is_unsigned;

    type_t *base;           /* pointer target, array element, return type */
    int array_len;          /* -1 for an incomplete [] */

    member_t *members;      /* struct/union */
    char *tag;              /* struct/union/enum tag, or NULL */
    int is_complete;

    type_t *params;         /* TY_FUNC: linked by `next` */
    type_t *next;
    int is_variadic;
};

struct member {
    member_t *next;
    type_t *ty;
    char *name;
    int offset;
};

extern type_t *ty_void, *ty_char, *ty_uchar, *ty_short, *ty_ushort;
extern type_t *ty_int, *ty_uint;

/*
 * TY_FLOAT is a SIZE, not a number format.
 *
 * There is no floating point in this compiler and there is none in the
 * SOC either -- rtl/sysctl.v builds picorv32 with no F extension, so
 * even GCC's float is soft-float library calls. What TY_FLOAT exists
 * for is LAYOUT: sw/common/zobj.h's z_obj_t has a `float float32`
 * member in its union, and without a 4-byte type of that name the
 * single most important header in the tree cannot be read at all.
 *
 * So a float can be declared, be a struct or union member, be
 * assigned, and have its address taken. Any arithmetic on one is a
 * clean error rather than integer arithmetic on its bit pattern, which
 * is the only outcome worse than refusing.
 */
extern type_t *ty_float, *ty_double;

type_t *ty_pointer_to(type_t *base);
type_t *ty_array_of(type_t *base, int len);
type_t *ty_func(type_t *ret);
int ty_is_integer(type_t *t);
int ty_is_scalar(type_t *t);
/* An array or function decays to a pointer in almost every context.
 * Applied at exactly the points C says it is, not blanket-applied --
 * `sizeof a` and `&a` are the two that must not decay, and getting
 * that wrong silently produces a compiler where `sizeof(arr)` is 4. */
type_t *ty_decay(type_t *t);

/* ------------------------------------------------------------------ */
/* symbols                                                             */

typedef enum {
    SYM_LOCAL,      /* frame offset from s0 */
    SYM_GLOBAL,     /* address fixed up at output time */
    SYM_FUNC,
    SYM_ENUMCONST,
    SYM_TYPEDEF
} sym_kind_t;

struct token;

typedef struct symbol {
    struct symbol *next;
    sym_kind_t kind;
    char *name;
    type_t *ty;

    int offset;         /* SYM_LOCAL: byte offset from s0, negative */
    int label;          /* SYM_GLOBAL / SYM_FUNC: emitter label id */
    uint32_t enum_val;  /* SYM_ENUMCONST */
    int defined;        /* SYM_FUNC: has a body, or is a runtime entry */
    int referenced;     /* SYM_FUNC: something actually calls it */
    struct token *first_use; /* where, so the diagnostic can point at it */
} symbol_t;

/* ------------------------------------------------------------------ */
/* the emitter (emit.c)                                                */

/*
 * Labels are the only forward reference this compiler has. Everything
 * that cannot be resolved when it is emitted -- a forward branch, a
 * call to a function defined later, the address of a global that has
 * not been laid out yet -- becomes a label id plus a fixup, and the
 * fixups are applied once when the image is written.
 *
 * That is why there is no separate linker: a linker's whole job is
 * this table, and the table is small because a ZEXE image has exactly
 * one address space, known at compile time.
 */
int  emit_new_label(void);
void emit_place_label(int label);       /* label -> here, in .text */

/* Sections. Code and initialised data go in the image; .bss becomes a
 * NUMBER in the ZEXE header, which is the whole point of that format
 * (docs/executables.md), so nothing is ever emitted into it -- only
 * counted. */
int  emit_data_label(const void *bytes, int len, int align);
int  emit_bss_label(int len, int align);
void emit_data_fixup_label(int data_label, int off, int target_label, int addend);

/* Overwrites bytes already reserved by emit_data_label(). Global
 * initialisers are built in a scratch buffer and poked in afterwards,
 * because the space has to be reserved (and its label placed) before
 * the initialiser is parsed -- an initialiser may refer to the object
 * it is initialising. */
void emit_data_poke(int label, const void *src, int len);

/* Moves the .text write cursor, for patching a region reserved
 * earlier. Only ever used to fill in a function's frame adjustment
 * once the frame size is known; see function_body() in parse.c. */
void emit_seek(int off);

/* -- libz support (docs/libz.md) --
 *
 * The runtime blob is copied verbatim to .text offset 0, so every
 * absolute address inside it is already correct: it was linked at
 * 0x80000000, which is where it lands. Must be called before anything
 * else is emitted.
 */
void emit_prepend_blob(const void *bytes, int len);

/* A label pinned to a .text offset that is known in advance -- a slot
 * in the runtime's jump table. Not "placed" in the usual sense,
 * because nothing is emitted there; the blob already contains it. */
int emit_pin_text_label(int offset);

/* Resolves to the image's _end: the address just past .bss. This is
 * what the runtime's heap starts at, and it cannot be known until the
 * whole image has been laid out. */
int emit_image_end_label(void);

/* Instruction emission. These are the only things that know RV32
 * encodings. */
void emit_word(uint32_t insn);

/*
 * A rollback point.
 *
 * `sizeof expr` must not evaluate its operand, but the only way to
 * learn an expression's TYPE in a one-pass compiler is to parse it --
 * and parsing is what emits. So the emitter takes a mark, the parser
 * parses normally, and everything produced is discarded. Cheaper and
 * far less error-prone than a second, non-emitting copy of the
 * expression grammar, which would have to be kept in step with the
 * real one forever.
 */
typedef struct {
    int text_len, data_len, bss_size;
    int nlabels, nfixups, ndatafixes;
} emit_mark_t;

emit_mark_t emit_mark(void);
void emit_rollback(emit_mark_t m);
int  emit_here(void);                   /* current .text offset */
void emit_patch_word(int at, uint32_t insn);

/* Fixup kinds, applied at output time. */
typedef enum {
    FIX_ABS32,      /* a bare 32-bit absolute address, for a data word */
    FIX_JAL,        /* J-type offset from the fixup site to the label */
    FIX_BRANCH,     /* B-type offset */
    FIX_HI20,       /* lui rd, %hi(addr)   -- paired with FIX_LO12 */
    FIX_LO12_I,     /* addi rd, rd, %lo(addr) */
    FIX_LO12_S      /* sw   rs2, %lo(addr)(rd) */
} fix_kind_t;

void emit_fixup(int at, fix_kind_t kind, int label, int addend);

int  emit_write_zexe(const char *path, int entry_label, int verbose);

/* ------------------------------------------------------------------ */
/* registers, by ABI name, for the code generator                      */

enum {
    REG_ZERO = 0, REG_RA = 1, REG_SP = 2, REG_GP = 3, REG_TP = 4,
    REG_T0 = 5, REG_T1 = 6, REG_T2 = 7,
    REG_S0 = 8, REG_S1 = 9,
    REG_A0 = 10, REG_A1 = 11, REG_A2 = 12, REG_A3 = 13,
    REG_A4 = 14, REG_A5 = 15, REG_A6 = 16, REG_A7 = 17
};

/* RV32IM instruction constructors. Named for what they do, not for
 * their encoding class, because every call site cares about the former
 * and none about the latter. */
void ei_lui(int rd, uint32_t imm20);
void ei_auipc(int rd, uint32_t imm20);
void ei_addi(int rd, int rs1, int imm);
void ei_add(int rd, int rs1, int rs2);
void ei_sub(int rd, int rs1, int rs2);
void ei_mul(int rd, int rs1, int rs2);
void ei_div(int rd, int rs1, int rs2, int is_unsigned);
void ei_rem(int rd, int rs1, int rs2, int is_unsigned);
void ei_and(int rd, int rs1, int rs2);
void ei_or(int rd, int rs1, int rs2);
void ei_xor(int rd, int rs1, int rs2);
void ei_sll(int rd, int rs1, int rs2);
void ei_srl(int rd, int rs1, int rs2);
void ei_sra(int rd, int rs1, int rs2);
void ei_slt(int rd, int rs1, int rs2, int is_unsigned);
void ei_slti(int rd, int rs1, int imm, int is_unsigned);
void ei_andi(int rd, int rs1, int imm);
void ei_xori(int rd, int rs1, int imm);
void ei_slli(int rd, int rs1, int sh);
void ei_srli(int rd, int rs1, int sh);
void ei_srai(int rd, int rs1, int sh);
void ei_load(int rd, int rs1, int imm, int size, int is_unsigned);
void ei_store(int rs2, int rs1, int imm, int size);
void ei_jalr(int rd, int rs1, int imm);
void ei_nop(void);
void ei_li(int rd, uint32_t v);         /* one or two instructions */
void ei_mv(int rd, int rs);

/* Control flow that needs a fixup. */
void ei_jal_label(int rd, int label);
void ei_branch_label(const char *op, int rs1, int rs2, int label);
void ei_la_label(int rd, int label, int addend);   /* lui/addi pair */

/* ------------------------------------------------------------------ */
/* the parser / code generator (parse.c)                               */

void parse_translation_unit(token_t *tok);

/* With a runtime blob, the two arguments are .text offsets of words
 * inside it that the compiler must fill in: main's address, and the
 * image's _end (where the heap begins). -1 for a freestanding build,
 * in which zcc emits its own entry stub instead. */
void parse_translation_unit_ex(token_t *tok, int patch_main, int patch_heap);

/* Names the runtime provides, each pinned to its jump-table slot.
 * Registered before parsing so that a declaration of one -- which
 * arrives normally, from libz.h -- resolves to the slot instead of
 * creating an undefined function. */
typedef struct {
    const char *name;
    int label;
} libz_sym_t;

void parse_set_libz(libz_sym_t *syms, int n);
int  parse_entry_label(void);           /* label of main(), or -1 */

/* ------------------------------------------------------------------ */
/* small utilities (util.c)                                            */

void *zalloc(size_t n);

/* Bytes the arena has taken from malloc. The compiler's peak memory,
 * for -v. */
size_t zalloc_total(void);
char *zstrdup(const char *s);
char *zstrndup(const char *s, size_t n);
char *zformat(const char *fmt, ...);

extern int zcc_verbose;

#endif
