/*
 * zfpga -- synth: the Verilog front end and the logic it builds.
 * See synth_parse.c, synth_elab.c, synth_map.c; docs/zfpga.md sec. 19.
 */

#ifndef SYNTH_H
#define SYNTH_H

#include "zfpga.h"

/* -- the syntax tree -------------------------------------------------- */

enum {
    E_NUM, E_ID, E_UNARY, E_BINARY, E_TERNARY, E_CONCAT, E_REPL, E_INDEX, E_RANGE
};

/* Operators, as their token text; one byte codes for the parser. */
enum {
    OP_NOT = 1, OP_LNOT, OP_NEG, OP_PLUS, OP_RAND, OP_ROR, OP_RXOR, OP_RNAND, OP_RNOR, OP_RXNOR,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_SHL, OP_SHR,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_AND, OP_XOR, OP_XNOR, OP_OR, OP_LAND, OP_LOR
};

typedef struct expr {
    int kind, op, line;
    struct expr *a, *b, *c;         /* operands; E_INDEX: a[b]; E_RANGE: a[b:c] */
    struct expr **list;             /* E_CONCAT items; E_REPL: count a, list */
    int n;
    const char *name;               /* E_ID */
    int width;                      /* E_NUM: 0 = unsized */
    uint64_t value;                 /* E_NUM */
} expr_t;

enum { S_BLOCK, S_IF, S_NB, S_BA, S_CASE };

typedef struct caseitem {
    expr_t **vals;                  /* NULL for default */
    int n;
    struct stmt *body;
} caseitem_t;

typedef struct stmt {
    int kind, line;
    expr_t *lhs, *rhs, *cond;       /* S_NB, S_BA: lhs rhs; S_IF, S_CASE: cond */
    struct stmt *then, *els;
    struct stmt **list;             /* S_BLOCK */
    int n;
    caseitem_t *items;              /* S_CASE */
    int n_items;
} stmt_t;

enum { SIG_WIRE, SIG_REG, SIG_IN, SIG_OUT };

typedef struct {
    const char *name;
    int kind, msb, lsb, width, line;
    int is_reg;                     /* declared reg (an output may be too) */
    expr_t *init;                   /* reg x = ...; or wire x = ...; */
} sig_t;

typedef struct {
    const char *name;
    uint64_t value;
    int width;
} param_t;

typedef struct {
    int comb;                       /* always @(*) */
    const char *clk;                /* posedge clock */
    const char *rst;                /* async reset, or NULL */
    int rst_neg;                    /* negedge rst */
    stmt_t *body;
    int line;
} always_t;

typedef struct {
    expr_t *lhs, *rhs;
    int line;
} assign_t;

typedef struct {
    const char *name, *file;
    sig_t *sigs; int n_sig, cap_sig;
    param_t *params; int n_param, cap_param;
    assign_t *assigns; int n_assign, cap_assign;
    always_t *always; int n_always, cap_always;
} module_t;

void synth_parse(const char *path, module_t *m);
int sig_find(const module_t *m, const char *name);
int param_find(const module_t *m, const char *name);
uint64_t const_eval(const module_t *m, const expr_t *e);    /* refuses the non-constant */
int self_width(const module_t *m, const expr_t *e);

/* -- the gate graph -------------------------------------------------------
 * One node per bit-level operation, hashed so that equal logic is one
 * node, constants folded as it is built. */

enum {
    G_C0, G_C1, G_IN, G_Q, G_NOT, G_AND, G_OR, G_XOR, G_MUX, G_SUM, G_COUT
};

typedef struct {
    uint8_t op;
    int a, b, c;                    /* fanins; G_MUX: c ? a : b; G_IN/G_Q/G_SUM/G_COUT: an index */
    int d;                          /* G_SUM: bit in the chain */
} gate_t;

/* An adder or subtractor, as a CCU2 chain: sum[i] = x[i] + y[i] (+ carry)
 * or x[i] - y[i]; cin a gate (constant for now). */
typedef struct {
    int n, sub;
    int *x, *y;
    int cin;
    int cout_used;
} chain_t2;

typedef struct {
    int d, clk, ce, lsr;            /* gates; ce, lsr: -1 for none */
    int regset;                     /* 1: SET */
    int async, lsr_inv;
    const char *name;               /* reg[bit] */
} sff_t;

typedef struct {
    const char *name;               /* port[bit] */
    int gate;                       /* inputs: its G_IN; outputs: what drives it */
    int output;
    const char *pin, *opts;
} sio_t;

typedef struct {
    gate_t *g; int n_g, cap_g;
    uint32_t *h; uint32_t h_cap;
    chain_t2 *ch; int n_ch, cap_ch;
    sff_t *ff; int n_ff, cap_ff;
    sio_t *io; int n_io, cap_io;
    const char **gname;             /* a signal name for a gate, for readable nets */
} graph_t;

int g_make(graph_t *G, int op, int a, int b, int c);
int g_not(graph_t *G, int a);
int g_and(graph_t *G, int a, int b);
int g_or(graph_t *G, int a, int b);
int g_xor(graph_t *G, int a, int b);
int g_mux(graph_t *G, int s, int a, int b);

void synth_elab(module_t *m, graph_t *G);
void synth_map_write(const module_t *m, graph_t *G, const char *out,
    const char *device, const char *package, const char *lpf);

#endif
