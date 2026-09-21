/*
 * zfpga -- synth: the Verilog front end and the logic it builds.
 *
 * synth_parse.c  source -> module DEFINITIONS (moddef_t), declarations
 *                unevaluated: a width can depend on a parameter an
 *                instance overrides
 * synth_flat.c   definitions -> one FLAT module (module_t): the top and
 *                every instance beneath it, parameters and widths
 *                evaluated per instance, every name prefixed with its
 *                instance path (u_fifo.count)
 * synth_elab.c   the flat module -> a bit-level gate graph
 * synth_map.c    the gate graph -> LUT4s, carry chains, a .zl
 *
 * docs/zfpga.md sec. 19 and 24.
 */

#ifndef SYNTH_H
#define SYNTH_H

#include "zfpga.h"

/* -- the syntax tree -------------------------------------------------- */

enum {
    E_NUM, E_ID, E_UNARY, E_BINARY, E_TERNARY, E_CONCAT, E_REPL, E_INDEX, E_RANGE,
    E_CALL          /* f(args): a function, or $clog2 / $signed / $unsigned */
};

enum {
    OP_NOT = 1, OP_LNOT, OP_NEG, OP_PLUS, OP_RAND, OP_ROR, OP_RXOR, OP_RNAND, OP_RNOR, OP_RXNOR,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_SHL, OP_SHR, OP_ASHR,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_AND, OP_XOR, OP_XNOR, OP_OR, OP_LAND, OP_LOR
};

typedef struct expr {
    int kind, op, line;
    const char *file;
    struct expr *a, *b, *c;         /* operands; E_INDEX: a[b]; E_RANGE: a[b:c], or with op
                                       RANGE_UP a[b +: c], RANGE_DOWN a[b -: c]; E_INDEX of
                                       a memory word's bit: a is itself an E_INDEX */
    struct expr **list;             /* E_CONCAT items; E_REPL: count a, list; E_CALL: args */
    int n;
    const char *name;               /* E_ID, E_CALL */
    int width;                      /* E_NUM: 0 = unsized */
    int is_signed;                  /* E_NUM: 's */
    uint64_t value;                 /* E_NUM */
} expr_t;

enum { RANGE_FIXED, RANGE_UP, RANGE_DOWN };  /* E_RANGE's op: [h:l], [s +: w], [s -: w] */

enum { S_BLOCK, S_IF, S_NB, S_BA, S_CASE, S_FOR };

typedef struct caseitem {
    expr_t **vals;                  /* NULL for default */
    int n;
    struct stmt *body;
} caseitem_t;

typedef struct stmt {
    int kind, line;
    const char *file;
    expr_t *lhs, *rhs, *cond;       /* S_NB, S_BA: lhs rhs; S_IF, S_CASE, S_FOR: cond */
    struct stmt *then, *els;
    struct stmt **list;             /* S_BLOCK */
    int n;
    caseitem_t *items;              /* S_CASE */
    int n_items;
    /* S_FOR: for (var = init; cond; var = step) then */
    const char *var;
    expr_t *init, *step;
} stmt_t;

/* -- definitions: what the parser produces -------------------------------- */

enum { SIG_WIRE, SIG_REG, SIG_IN, SIG_OUT, SIG_INT, SIG_FUNC };

typedef struct {
    const char *name;
    int kind, is_reg, is_signed, line;
    expr_t *msb, *lsb;              /* NULL: one bit */
    expr_t *alo, *ahi;              /* a memory: name [alo:ahi] */
    expr_t *init;                   /* reg x = ...; (wire x = ... is an assign) */
} decl_t;

typedef struct {
    const char *name;
    expr_t *value;
    expr_t *msb, *lsb;              /* a declared range, parameter [7:0] P; or NULL */
    int local, line, is_signed;
} pdecl_t;

typedef struct {
    const char *name;               /* .name(e), or NULL for positional */
    expr_t *e;                      /* NULL: .name() unconnected */
} conn_t;

typedef struct {
    const char *mod, *name;
    conn_t *params; int n_params;
    conn_t *ports; int n_ports;
    int line;
} inst_t;

typedef struct {
    const char *name;
    expr_t *msb, *lsb;              /* the result's range, NULL for one bit */
    int is_signed, line;
    decl_t *decls; int n_decl;      /* inputs (SIG_IN, in order), then locals */
    stmt_t *body;
} funcdef_t;

typedef struct {
    expr_t *lhs, *rhs;
    int line;
    const char *file;
} assign_t;

typedef struct {
    int comb;                       /* always @(*) */
    const char *clk;                /* posedge clock */
    const char *rst;                /* async reset, or NULL */
    int rst_neg;                    /* negedge rst */
    stmt_t *body;
    int line;
    const char *file;
} always_t;

typedef struct {
    const char *name, *file;
    int line;
    const char **ports; int n_ports;            /* port order, for positional connection */
    decl_t *decls; int n_decl, cap_decl;
    pdecl_t *pdecls; int n_pdecl, cap_pdecl;
    assign_t *assigns; int n_assign, cap_assign;
    always_t *always; int n_always, cap_always;
    inst_t *insts; int n_inst, cap_inst;
    funcdef_t *funcs; int n_func, cap_func;
} moddef_t;

typedef struct {
    moddef_t **mods;
    int n_mod, cap_mod;
} lib_t;

/* Reads a file into the library: every module in it, the preprocessor
 * applied (`define, `ifdef, `include). */
void synth_parse(const char *path, lib_t *lib);
moddef_t *lib_find(const lib_t *lib, const char *name);

/* -- the flat module: what the elaborator consumes ---------------------------- */

typedef struct {
    const char *name;
    int kind, msb, lsb, width, line;
    int is_reg, is_signed;
    expr_t *init;
    const char *file;
} sig_t;

typedef struct {
    const char *name;
    uint64_t value;
    int width;                      /* 0: 32 */
    int is_signed;
    int ranged;                     /* width from a declared range: selects are checked */
} param_t;

/* A memory: its words are signals name[lo] .. name[hi], each `width`
 * bits, first at sig0. */
typedef struct {
    const char *name;
    int sig0, lo, hi, depth;
} mem_t;

/* A function, flattened: its result, inputs and locals are signals of
 * kind SIG_FUNC, only ever read and written inside a call. */
typedef struct {
    const char *name;
    int result;                     /* the signal for the result */
    int *inputs; int n_inputs;
    int *locals; int n_locals;
    stmt_t *body;
    int line;
    int active;                     /* recursion guard */
} func_t;

typedef struct {
    const char *name, *file;        /* the top */
    sig_t *sigs; int n_sig, cap_sig;
    param_t *params; int n_param, cap_param;
    assign_t *assigns; int n_assign, cap_assign;
    always_t *always; int n_always, cap_always;
    mem_t *mems; int n_mem, cap_mem;
    func_t *funcs; int n_func, cap_func;
    int n_inst;                     /* instances flattened, for the summary */
} module_t;

void synth_flatten(const lib_t *lib, const char *top, module_t *m);

int sig_find(const module_t *m, const char *name);
int param_find(const module_t *m, const char *name);
int mem_find(const module_t *m, const char *name);
int func_find(const module_t *m, const char *name);

/* Constant expressions: parameters, loop variables, $clog2. */
uint64_t const_eval(const module_t *m, const expr_t *e);
int const_is(const module_t *m, const expr_t *e);           /* constant? (no side effects) */
void range_bounds(const module_t *m, const expr_t *e, int *hi, int *lo);   /* E_RANGE, start constant */
int self_width(const module_t *m, const expr_t *e);
int expr_signed(const module_t *m, const expr_t *e);

/* Loop variables in scope while a for loop unrolls (synth_elab.c). */
int loopvar_find(const char *name, uint64_t *value);

/* -- the gate graph -------------------------------------------------------- */

enum {
    G_C0, G_C1, G_IN, G_Q, G_NOT, G_AND, G_OR, G_XOR, G_MUX, G_SUM, G_COUT
};

typedef struct {
    uint8_t op;
    int a, b, c;
    int d;
} gate_t;

typedef struct {
    int n, sub;
    int *x, *y;
    int cin;
    int cout_used;
} chain_t2;

typedef struct {
    int d, clk, ce, lsr;
    int regset;
    int async, lsr_inv;
    const char *name;
} sff_t;

typedef struct {
    const char *name;
    int gate;
    int output;
    const char *pin, *opts;
} sio_t;

typedef struct {
    gate_t *g; int n_g, cap_g;
    uint32_t *h; uint32_t h_cap;
    chain_t2 *ch; int n_ch, cap_ch;
    sff_t *ff; int n_ff, cap_ff;
    sio_t *io; int n_io, cap_io;
    const char **gname;
} graph_t;

int g_make(graph_t *G, int op, int a, int b, int c);
int g_not(graph_t *G, int a);
int g_and(graph_t *G, int a, int b);
int g_or(graph_t *G, int a, int b);
int g_xor(graph_t *G, int a, int b);
int g_mux(graph_t *G, int s, int a, int b);

void synth_elab(module_t *m, graph_t *g);
void synth_map_write(const module_t *m, graph_t *G, const char *out,
    const char *device, const char *package, const char *lpf);

#endif
