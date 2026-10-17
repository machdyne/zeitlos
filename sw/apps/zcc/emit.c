/*
 * zcc -- the emitter.
 *
 * RV32IM instruction encoding, a label/fixup table, three sections,
 * and a ZEXE writer. This is the only file that knows what a RISC-V
 * instruction looks like, and the only one that knows what the output
 * format is.
 *
 * -- Why there is no assembler and no linker --
 *
 * A ZEXE image (docs/executables.md) is a 16-byte header and a flat
 * blob loaded at a fixed virtual base, with .bss as a NUMBER rather
 * than as bytes. Every app in the tree is linked to 0x80000000 because
 * the MTU remaps that base per process, so there is exactly one
 * address space and it is known at compile time.
 *
 * That removes both tools. There is nothing to assemble, because the
 * parser can emit instruction words directly; and nothing to link,
 * because a linker's entire job is the fixup table below, which is
 * small when there is only one translation unit and one fixed base.
 *
 * -- Layout --
 *
 *   0x80000000  .text     code, entry stub first
 *               .data     initialised data and string literals
 *               .bss      counted, never emitted; the header's bss_size
 *
 * .data follows .text rather than being interleaved so that a single
 * base + offset describes every global, and so the ZEXE bss_size means
 * what the format says it means: everything after _edata.
 */

#include <string.h>

#include "zcc.h"

#define IMAGE_BASE 0x80000000u

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                 */

typedef struct {
    uint8_t *p;
    int len, cap;
} buf_t;

static void buf_need(buf_t *b, int n) {
    if (b->len + n <= b->cap) return;
    int cap = b->cap ? b->cap : 4096;
    while (b->len + n > cap) cap *= 2;
    uint8_t *np = zalloc((size_t)cap);
    if (b->p) memcpy(np, b->p, (size_t)b->len);
    b->p = np;
    b->cap = cap;
}

static void buf_put(buf_t *b, const void *src, int n) {
    buf_need(b, n);
    if (src) memcpy(b->p + b->len, src, (size_t)n);
    else memset(b->p + b->len, 0, (size_t)n);
    b->len += n;
}

static void buf_align(buf_t *b, int align) {
    while (b->len % align) buf_put(b, NULL, 1);
}

static buf_t text, data;
static int bss_size;

/* -- the peephole (docs/zcc.md, "The peephole") --
 *
 * The code generator is an accumulator machine with a memory stack, and
 * most of what it emits is that machine's overhead rather than the
 * program: `r = zr * zr >> 12` was 27 instructions, two of them a mul
 * and a shift. emit_word() rewrites the last few instructions as each
 * new one arrives, for three shapes that cannot change what the program
 * does (see peep()).
 *
 * SAFE BY CONSTRUCTION, not by care: nothing that has been OBSERVED may
 * change. Every way the rest of zcc can observe a position in .text --
 * emit_here(), a label placed, a fixup recorded, a word patched, a seek
 * or rollback -- moves `peep_barrier` past it, and peep() only ever
 * touches words at or after the barrier. So no branch target, no
 * recorded patch site and no fixup can move or change.
 *
 * Off while code is being REWRITTEN below the high-water mark (the
 * frame-size slots parse.c seeks back to fill), where shrinking would
 * leave stale words; and off with -nopeep (main.c), for comparison. */
int emit_peephole = 1;
static int peep_barrier;
static int text_hw;                 /* the furthest .text has reached */

static void peep_freeze(int upto) { if (upto > peep_barrier) peep_barrier = upto; }

/* ------------------------------------------------------------------ */
/* labels and fixups                                                    */

typedef enum { LBL_TEXT, LBL_DATA, LBL_BSS, LBL_IMAGE_END, LBL_NONE } lbl_sec_t;

typedef struct {
    lbl_sec_t sec;
    int offset;         /* within its section */
    int placed;
    const char *name;   /* diagnostics only */
} label_t;

static label_t *labels;
static int nlabels, labels_cap;

typedef struct {
    int at;             /* .text byte offset of the instruction to patch */
    fix_kind_t kind;
    int label;
    int addend;
} fixup_t;

static fixup_t *fixups;
static int nfixups, fixups_cap;

/* Fixups that live in .data rather than in an instruction: the address
 * of a global stored in another global's initialiser, which is what
 * `char *p = "hello";` and `int *q = &n;` compile to. */
typedef struct {
    int data_off;
    int label;
    int addend;
} datafix_t;

static datafix_t *datafixes;
static int ndatafixes, datafixes_cap;

int emit_new_label(void) {
    if (nlabels == labels_cap) {
        labels_cap = labels_cap ? labels_cap * 2 : 256;
        label_t *n = zalloc(sizeof(label_t) * (size_t)labels_cap);
        if (labels) memcpy(n, labels, sizeof(label_t) * (size_t)nlabels);
        labels = n;
    }
    labels[nlabels].sec = LBL_NONE;
    labels[nlabels].placed = 0;
    return nlabels++;
}

void emit_place_label(int label) {
    peep_freeze(text.len);
    labels[label].sec = LBL_TEXT;
    labels[label].offset = text.len;
    labels[label].placed = 1;
}

int emit_data_label(const void *bytes, int len, int align) {
    buf_align(&data, align);
    int l = emit_new_label();
    labels[l].sec = LBL_DATA;
    labels[l].offset = data.len;
    labels[l].placed = 1;
    buf_put(&data, bytes, len);
    return l;
}

/*
 * .bss is counted, not stored.
 *
 * Alignment has to be applied to the running counter rather than to a
 * buffer, and the counter starts wherever .data ends -- so bss offsets
 * are not final until the image is laid out. They are recorded relative
 * to the start of .bss and biased at write time, which is why LBL_BSS
 * is a separate section rather than just more .data.
 */
int emit_bss_label(int len, int align) {
    while (bss_size % align) bss_size++;
    int l = emit_new_label();
    labels[l].sec = LBL_BSS;
    labels[l].offset = bss_size;
    labels[l].placed = 1;
    bss_size += len;
    return l;
}

void emit_data_fixup_label(int data_label, int off, int target_label, int addend) {
    if (ndatafixes == datafixes_cap) {
        datafixes_cap = datafixes_cap ? datafixes_cap * 2 : 64;
        datafix_t *n = zalloc(sizeof(datafix_t) * (size_t)datafixes_cap);
        if (datafixes) memcpy(n, datafixes, sizeof(datafix_t) * (size_t)ndatafixes);
        datafixes = n;
    }
    datafixes[ndatafixes].data_off = labels[data_label].offset + off;
    datafixes[ndatafixes].label = target_label;
    datafixes[ndatafixes].addend = addend;
    ndatafixes++;
}

void emit_fixup(int at, fix_kind_t kind, int label, int addend) {
    peep_freeze(at + 4);
    if (nfixups == fixups_cap) {
        fixups_cap = fixups_cap ? fixups_cap * 2 : 256;
        fixup_t *n = zalloc(sizeof(fixup_t) * (size_t)fixups_cap);
        if (fixups) memcpy(n, fixups, sizeof(fixup_t) * (size_t)nfixups);
        fixups = n;
    }
    fixups[nfixups].at = at;
    fixups[nfixups].kind = kind;
    fixups[nfixups].label = label;
    fixups[nfixups].addend = addend;
    nfixups++;
}

/* The caller may patch the word that comes next, so it is frozen too. */
int emit_here(void) { peep_freeze(text.len + 4); return text.len; }

void emit_prepend_blob(const void *bytes, int len) {
    if (text.len)
        zcc_fatal("internal error: the runtime blob must be placed before "
                  "any code is emitted");
    buf_put(&text, bytes, len);
    peep_freeze(text.len);
    text_hw = text.len;
}

int emit_pin_text_label(int offset) {
    int l = emit_new_label();
    labels[l].sec = LBL_TEXT;
    labels[l].offset = offset;
    labels[l].placed = 1;
    return l;
}

int emit_image_end_label(void) {
    int l = emit_new_label();
    labels[l].sec = LBL_IMAGE_END;
    labels[l].offset = 0;
    labels[l].placed = 1;
    return l;
}

void emit_seek(int off) { text.len = off; peep_barrier = off; }

void emit_data_poke(int label, const void *src, int len) {
    memcpy(data.p + labels[label].offset, src, (size_t)len);
}

emit_mark_t emit_mark(void) {
    emit_mark_t m;
    m.text_len = text.len;
    m.data_len = data.len;
    m.bss_size = bss_size;
    m.nlabels = nlabels;
    m.nfixups = nfixups;
    m.ndatafixes = ndatafixes;
    return m;
}

/* Discards everything emitted since the mark, including any labels and
 * string literals created along the way. Safe because a rolled-back
 * region is by construction unreachable: nothing outside it can hold a
 * label id it created, since those ids had not been handed out when
 * the mark was taken. */
void emit_rollback(emit_mark_t m) {
    text.len = m.text_len;
    peep_barrier = text.len;            /* the discarded code is gone */
    text_hw = text.len;
    data.len = m.data_len;
    bss_size = m.bss_size;
    nlabels = m.nlabels;
    nfixups = m.nfixups;
    ndatafixes = m.ndatafixes;
}

static uint32_t read_word(int at);
static int peep(uint32_t insn);

static void put_word(uint32_t insn) {
    uint8_t b[4] = { (uint8_t)insn, (uint8_t)(insn >> 8),
                     (uint8_t)(insn >> 16), (uint8_t)(insn >> 24) };
    buf_put(&text, b, 4);
    if (text.len > text_hw) text_hw = text.len;
}

void emit_word(uint32_t insn) {
    if (emit_peephole && text.len == text_hw && peep(insn)) return;
    put_word(insn);
}

void emit_patch_word(int at, uint32_t insn) {
    peep_freeze(at + 4);
    text.p[at + 0] = (uint8_t)insn;
    text.p[at + 1] = (uint8_t)(insn >> 8);
    text.p[at + 2] = (uint8_t)(insn >> 16);
    text.p[at + 3] = (uint8_t)(insn >> 24);
}

static uint32_t read_word(int at) {
    return (uint32_t)text.p[at] | ((uint32_t)text.p[at + 1] << 8) |
           ((uint32_t)text.p[at + 2] << 16) | ((uint32_t)text.p[at + 3] << 24);
}

/* ------------------------------------------------------------------ */
/* instruction encoding                                                 */

static uint32_t enc_r(int op, int rd, int f3, int rs1, int rs2, int f7) {
    return (uint32_t)op | ((uint32_t)rd << 7) | ((uint32_t)f3 << 12) |
           ((uint32_t)rs1 << 15) | ((uint32_t)rs2 << 20) | ((uint32_t)f7 << 25);
}

static uint32_t enc_i(int op, int rd, int f3, int rs1, int imm) {
    return (uint32_t)op | ((uint32_t)rd << 7) | ((uint32_t)f3 << 12) |
           ((uint32_t)rs1 << 15) | (((uint32_t)imm & 0xfff) << 20);
}

static uint32_t enc_s(int op, int f3, int rs1, int rs2, int imm) {
    uint32_t i = (uint32_t)imm;
    return (uint32_t)op | ((i & 0x1f) << 7) | ((uint32_t)f3 << 12) |
           ((uint32_t)rs1 << 15) | ((uint32_t)rs2 << 20) | (((i >> 5) & 0x7f) << 25);
}

static uint32_t enc_b(int op, int f3, int rs1, int rs2, int imm) {
    uint32_t i = (uint32_t)imm;
    return (uint32_t)op | (((i >> 11) & 1) << 7) | (((i >> 1) & 0xf) << 8) |
           ((uint32_t)f3 << 12) | ((uint32_t)rs1 << 15) | ((uint32_t)rs2 << 20) |
           (((i >> 5) & 0x3f) << 25) | (((i >> 12) & 1) << 31);
}

static uint32_t enc_u(int op, int rd, uint32_t imm20) {
    return (uint32_t)op | ((uint32_t)rd << 7) | (imm20 << 12);
}

static uint32_t enc_j(int op, int rd, int imm) {
    uint32_t i = (uint32_t)imm;
    return (uint32_t)op | ((uint32_t)rd << 7) |
           (((i >> 12) & 0xff) << 12) | (((i >> 11) & 1) << 20) |
           (((i >> 1) & 0x3ff) << 21) | (((i >> 20) & 1) << 31);
}

#define OP_LUI    0x37
#define OP_AUIPC  0x17
#define OP_JAL    0x6f
#define OP_JALR   0x67
#define OP_BRANCH 0x63
#define OP_LOAD   0x03
#define OP_STORE  0x23
#define OP_IMM    0x13
#define OP_REG    0x33

/* -- the peephole's rules -- */

#define F_OP(w)   ((int)((w) & 0x7f))
#define F_RD(w)   ((int)(((w) >> 7) & 31))
#define F_F3(w)   ((int)(((w) >> 12) & 7))
#define F_RS1(w)  ((int)(((w) >> 15) & 31))
#define F_RS2(w)  ((int)(((w) >> 20) & 31))
#define F_F7(w)   ((int)((w) >> 25))
#define F_IMM(w)  ((int32_t)(w) >> 20)

#define R_ZERO 0
#define R_SP   2
#define R_T0   5
#define R_S0   8
#define R_A0   10

/* The last-but-k word, if it may be rewritten (at or after the barrier). */
static int peep_at(int k, uint32_t *w) {
    int at = text.len - 4 * k;
    if (at < peep_barrier || at < 0) return 0;
    *w = read_word(at);
    return 1;
}

/* Only ever called with text.len == text_hw (emit_word checks), so
 * the dropped words are the newest and the mark comes down with them. */
static void peep_drop(int k) { text.len -= 4 * k; text_hw = text.len; }

/* Sets a0 from something that reads neither memory below s0's frame
 * nor any register but zero and s0: li, lui, a local's address, a
 * local's value. Moving it across a push/pop cannot change it. */
static int peep_simple_a0(uint32_t w) {
    if (F_RD(w) != R_A0) return 0;
    if (F_OP(w) == OP_LUI) return 1;
    if (F_OP(w) == OP_IMM && F_F3(w) == 0 &&
        (F_RS1(w) == R_ZERO || F_RS1(w) == R_S0)) return 1;
    if (F_OP(w) == OP_LOAD && F_RS1(w) == R_S0) return 1;
    return 0;
}

/* Returns 1 if `insn` was absorbed (the words before it rewritten so
 * that together they do exactly what they did with it appended). */
static int peep(uint32_t insn) {
    uint32_t w1, w2, w3, w4;

    /* 1. A local's value: `addi a0,s0,off` then `lw a0,0(a0)` is
     *    `lw a0,off(s0)`. Any load width. */
    if (F_OP(insn) == OP_LOAD && F_RD(insn) == R_A0 && F_RS1(insn) == R_A0 &&
        F_IMM(insn) == 0 && peep_at(1, &w1) &&
        F_OP(w1) == OP_IMM && F_F3(w1) == 0 && F_RD(w1) == R_A0 &&
        F_RS1(w1) == R_S0) {
        peep_drop(1);
        put_word(enc_i(OP_LOAD, R_A0, F_F3(insn), R_S0, F_IMM(w1)));
        return 1;
    }

    /* 2. No push and pop round a simple right operand:
     *      addi sp,sp,-16 / sw a0,0(sp) / X / lw R,0(sp) / addi sp,sp,16
     *    is `mv R,a0` / X when X only sets a0 from a constant or a local
     *    (peep_simple_a0): R ends up with what the pop gave it, a0 with
     *    what X gave it, and the stack slot was never read again. */
    if (insn == enc_i(OP_IMM, R_SP, 0, R_SP, 16) &&
        peep_at(1, &w1) && peep_at(2, &w2) && peep_at(3, &w3) && peep_at(4, &w4) &&
        F_OP(w1) == OP_LOAD && F_F3(w1) == 2 && F_RS1(w1) == R_SP && F_IMM(w1) == 0 &&
        F_RD(w1) != R_A0 && F_RD(w1) != R_SP && F_RD(w1) != R_S0 && F_RD(w1) != R_ZERO &&
        peep_simple_a0(w2) &&
        w3 == enc_s(OP_STORE, 2, R_SP, R_A0, 0) &&
        w4 == enc_i(OP_IMM, R_SP, 0, R_SP, -16)) {
        int r = F_RD(w1);
        peep_drop(4);
        put_word(enc_i(OP_IMM, r, 0, R_A0, 0));        /* mv r,a0 */
        put_word(w2);
        return 1;
    }

    /* 3. A constant right operand: `li a0,K` then `op a0,R,a0` is
     *    `opi a0,R,K`. R keeps the left operand, as it would have. */
    if (F_OP(insn) == OP_REG && F_RD(insn) == R_A0 && F_RS2(insn) == R_A0 &&
        F_RS1(insn) != R_A0 && peep_at(1, &w1) &&
        F_OP(w1) == OP_IMM && F_F3(w1) == 0 && F_RD(w1) == R_A0 &&
        F_RS1(w1) == R_ZERO) {
        int k = F_IMM(w1), f3 = F_F3(insn), f7 = F_F7(insn), rs1 = F_RS1(insn);
        uint32_t out = 0;
        if (f7 == 0x00 && (f3 == 0 || f3 == 2 || f3 == 3 || f3 == 4 ||
                           f3 == 6 || f3 == 7))
            out = enc_i(OP_IMM, R_A0, f3, rs1, k);      /* add slt sltu xor or and */
        else if (f7 == 0x20 && f3 == 0 && k != -2048)
            out = enc_i(OP_IMM, R_A0, 0, rs1, -k);      /* sub */
        else if (k >= 0 && k <= 31 && f7 == 0x00 && (f3 == 1 || f3 == 5))
            out = enc_i(OP_IMM, R_A0, f3, rs1, k);      /* sll srl */
        else if (k >= 0 && k <= 31 && f7 == 0x20 && f3 == 5)
            out = enc_i(OP_IMM, R_A0, 5, rs1, k | 0x400); /* sra */
        if (out) {
            peep_drop(1);
            put_word(out);
            return 1;
        }
    }

    return 0;
}

/* For an assignment to a local (parse.c): if the destination's address
 * was the very last thing emitted, as `addi a0,s0,off`, and nothing has
 * observed it, take it back and give its offset -- the store can then
 * go straight to off(s0), with no push and pop round the value. */
int emit_take_local_addr(int *off) {
    uint32_t w1;
    if (!emit_peephole || text.len != text_hw) return 0;
    if (!peep_at(1, &w1)) return 0;
    if (F_OP(w1) != OP_IMM || F_F3(w1) != 0 || F_RD(w1) != R_A0 ||
        F_RS1(w1) != R_S0) return 0;
    *off = F_IMM(w1);
    peep_drop(1);
    text_hw = text.len;
    return 1;
}

void ei_lui(int rd, uint32_t imm20)   { emit_word(enc_u(OP_LUI, rd, imm20 & 0xfffff)); }
void ei_auipc(int rd, uint32_t imm20) { emit_word(enc_u(OP_AUIPC, rd, imm20 & 0xfffff)); }
void ei_addi(int rd, int rs1, int imm){ emit_word(enc_i(OP_IMM, rd, 0, rs1, imm)); }
void ei_andi(int rd, int rs1, int imm){ emit_word(enc_i(OP_IMM, rd, 7, rs1, imm)); }
void ei_xori(int rd, int rs1, int imm){ emit_word(enc_i(OP_IMM, rd, 4, rs1, imm)); }
void ei_slli(int rd, int rs1, int sh) { emit_word(enc_i(OP_IMM, rd, 1, rs1, sh & 31)); }
void ei_srli(int rd, int rs1, int sh) { emit_word(enc_i(OP_IMM, rd, 5, rs1, sh & 31)); }
void ei_srai(int rd, int rs1, int sh) { emit_word(enc_i(OP_IMM, rd, 5, rs1, (sh & 31) | 0x400)); }

void ei_add(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 0, rs1, rs2, 0x00)); }
void ei_sub(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 0, rs1, rs2, 0x20)); }
void ei_sll(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 1, rs1, rs2, 0x00)); }
void ei_srl(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 5, rs1, rs2, 0x00)); }
void ei_sra(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 5, rs1, rs2, 0x20)); }
void ei_xor(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 4, rs1, rs2, 0x00)); }
void ei_or (int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 6, rs1, rs2, 0x00)); }
void ei_and(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 7, rs1, rs2, 0x00)); }

void ei_mul(int rd, int rs1, int rs2) { emit_word(enc_r(OP_REG, rd, 0, rs1, rs2, 0x01)); }
void ei_div(int rd, int rs1, int rs2, int u) { emit_word(enc_r(OP_REG, rd, u ? 5 : 4, rs1, rs2, 0x01)); }
void ei_rem(int rd, int rs1, int rs2, int u) { emit_word(enc_r(OP_REG, rd, u ? 7 : 6, rs1, rs2, 0x01)); }

void ei_slt(int rd, int rs1, int rs2, int u) { emit_word(enc_r(OP_REG, rd, u ? 3 : 2, rs1, rs2, 0x00)); }
void ei_slti(int rd, int rs1, int imm, int u){ emit_word(enc_i(OP_IMM, rd, u ? 3 : 2, rs1, imm)); }

void ei_load(int rd, int rs1, int imm, int size, int is_unsigned) {
    int f3 = (size == 1) ? (is_unsigned ? 4 : 0)
           : (size == 2) ? (is_unsigned ? 5 : 1)
           : 2;
    emit_word(enc_i(OP_LOAD, rd, f3, rs1, imm));
}

void ei_store(int rs2, int rs1, int imm, int size) {
    int f3 = (size == 1) ? 0 : (size == 2) ? 1 : 2;
    emit_word(enc_s(OP_STORE, f3, rs1, rs2, imm));
}

void ei_jalr(int rd, int rs1, int imm) { emit_word(enc_i(OP_JALR, rd, 0, rs1, imm)); }
void ei_nop(void) { ei_addi(REG_ZERO, REG_ZERO, 0); }
void ei_mv(int rd, int rs) { ei_addi(rd, rs, 0); }

/*
 * li: one instruction when the value fits a 12-bit signed immediate,
 * two otherwise.
 *
 * The +0x800 before the shift is the standard lui/addi correction, and
 * it is the single easiest thing to get wrong in a RISC-V code
 * generator: addi's immediate is SIGNED, so when the low 12 bits have
 * their top bit set they subtract, and the upper part has to be
 * pre-incremented to compensate. Omitting it produces code that is
 * correct for slightly over half of all constants, which is exactly
 * the distribution that makes a bug survive testing.
 */
void ei_li(int rd, uint32_t v) {
    int32_t sv = (int32_t)v;
    if (sv >= -2048 && sv < 2048) {
        ei_addi(rd, REG_ZERO, sv);
        return;
    }
    uint32_t hi = (v + 0x800u) >> 12;
    int32_t lo = (int32_t)(v & 0xfffu);
    if (lo & 0x800) lo -= 0x1000;
    ei_lui(rd, hi);
    if (lo) ei_addi(rd, rd, lo);
}

void ei_jal_label(int rd, int label) {
    emit_fixup(text.len, FIX_JAL, label, 0);
    emit_word(enc_j(OP_JAL, rd, 0));
}

void ei_branch_label(const char *op, int rs1, int rs2, int label) {
    int f3;
    if (!strcmp(op, "beq")) f3 = 0;
    else if (!strcmp(op, "bne")) f3 = 1;
    else if (!strcmp(op, "blt")) f3 = 4;
    else if (!strcmp(op, "bge")) f3 = 5;
    else if (!strcmp(op, "bltu")) f3 = 6;
    else f3 = 7;                            /* bgeu */
    emit_fixup(text.len, FIX_BRANCH, label, 0);
    emit_word(enc_b(OP_BRANCH, f3, rs1, rs2, 0));
}

/* Address of a label into a register: lui/addi, both fixed up later.
 * Always two instructions, never optimised down to one even when the
 * address would fit -- addresses are not known until layout, and a
 * pass that shrank instructions would move everything after it. */
void ei_la_label(int rd, int label, int addend) {
    emit_fixup(text.len, FIX_HI20, label, addend);
    ei_lui(rd, 0);
    emit_fixup(text.len, FIX_LO12_I, label, addend);
    ei_addi(rd, rd, 0);
}

/* ------------------------------------------------------------------ */
/* layout and output                                                    */

static uint32_t label_addr(int l, int text_base, int data_base, int bss_base) {
    if (!labels[l].placed)
        zcc_fatal("internal error: label %d (%s) was never placed", l,
                  labels[l].name ? labels[l].name : "?");
    switch (labels[l].sec) {
    case LBL_TEXT: return (uint32_t)(text_base + labels[l].offset);
    case LBL_DATA: return (uint32_t)(data_base + labels[l].offset);
    case LBL_BSS:  return (uint32_t)(bss_base + labels[l].offset);
    case LBL_IMAGE_END: return (uint32_t)(bss_base + bss_size);
    default:
        zcc_fatal("internal error: label %d has no section", l);
        return 0;
    }
}

int emit_write_zexe(const char *path, int entry_label, int verbose) {

    /* Alignment of .data to 4 is not cosmetic: every global of int or
     * pointer type is reached with lw/sw, and RV32 without the Zicclsm
     * misaligned-access extension traps on an unaligned word. picorv32
     * does not fault, it silently returns the wrong bytes, which is
     * worse. */
    buf_align(&data, 4);

    int text_base = (int)IMAGE_BASE;
    int data_base = text_base + text.len;
    int bss_base  = data_base + data.len;

    /* instruction fixups */
    for (int i = 0; i < nfixups; i++) {
        fixup_t *f = &fixups[i];
        uint32_t target = label_addr(f->label, text_base, data_base, bss_base)
                        + (uint32_t)f->addend;
        uint32_t insn = read_word(f->at);
        int32_t rel = (int32_t)(target - (uint32_t)(text_base + f->at));

        switch (f->kind) {
        case FIX_ABS32:
            /* Not an instruction at all: a data word inside the
             * runtime blob that only the compiler can fill in --
             * main's address, and where the heap starts. See
             * docs/libz.md. */
            emit_patch_word(f->at, target);
            break;

        case FIX_JAL:
            if (rel < -(1 << 20) || rel >= (1 << 20)) {
                zcc_printf("zcc: jump out of range (%d bytes); the image is "
                           "too large for a single jal\n", rel);
                return -1;
            }
            emit_patch_word(f->at, (insn & 0x00000fff) |
                                   enc_j(0, 0, rel));
            break;

        case FIX_BRANCH:
            if (rel < -(1 << 12) || rel >= (1 << 12)) {
                /* Only reachable from a very large function body. The
                 * fix, if it ever fires, is to invert the branch over
                 * a jal -- worth doing then, not before. */
                zcc_printf("zcc: conditional branch out of range (%d bytes); "
                           "split the function\n", rel);
                return -1;
            }
            emit_patch_word(f->at, (insn & 0x01fff07f) |
                                   enc_b(0, (int)((insn >> 12) & 7),
                                         (int)((insn >> 15) & 31),
                                         (int)((insn >> 20) & 31), rel));
            break;

        case FIX_HI20: {
            uint32_t hi = (target + 0x800u) >> 12;
            emit_patch_word(f->at, (insn & 0x00000fff) | (hi << 12));
            break;
        }
        case FIX_LO12_I: {
            int32_t lo = (int32_t)(target & 0xfffu);
            if (lo & 0x800) lo -= 0x1000;
            emit_patch_word(f->at, (insn & 0x000fffff) |
                                   (((uint32_t)lo & 0xfff) << 20));
            break;
        }
        case FIX_LO12_S: {
            int32_t lo = (int32_t)(target & 0xfffu);
            if (lo & 0x800) lo -= 0x1000;
            uint32_t l = (uint32_t)lo;
            emit_patch_word(f->at, (insn & 0x01fff07f) |
                                   ((l & 0x1f) << 7) | (((l >> 5) & 0x7f) << 25));
            break;
        }
        }
    }

    /* data fixups -- absolute 32-bit addresses inside initialisers */
    for (int i = 0; i < ndatafixes; i++) {
        datafix_t *f = &datafixes[i];
        uint32_t v = label_addr(f->label, text_base, data_base, bss_base)
                   + (uint32_t)f->addend;
        data.p[f->data_off + 0] = (uint8_t)v;
        data.p[f->data_off + 1] = (uint8_t)(v >> 8);
        data.p[f->data_off + 2] = (uint8_t)(v >> 16);
        data.p[f->data_off + 3] = (uint8_t)(v >> 24);
    }

    (void)entry_label;   /* the entry stub is always at .text offset 0 */

    /* The image is assembled in one buffer and written in one call.
     * See zcc_port.h on why whole-file I/O is what the target wants
     * rather than a concession. */
    int total = 16 + text.len + data.len;
    uint8_t *image = zalloc((size_t)total);

    memcpy(image, "ZEXE", 4);
    image[4] = 1; image[5] = 0;                     /* version 1 */
    image[6] = 0; image[7] = 0;             /* flags 0: default stack */
    uint32_t bss = (uint32_t)bss_size;
    image[8]  = (uint8_t)bss;
    image[9]  = (uint8_t)(bss >> 8);
    image[10] = (uint8_t)(bss >> 16);
    image[11] = (uint8_t)(bss >> 24);
    /* entry: 0 means "the base address", which is where the entry stub
     * or the runtime's _start already is. */
    image[12] = image[13] = image[14] = image[15] = 0;

    memcpy(image + 16, text.p, (size_t)text.len);
    if (data.len) memcpy(image + 16 + text.len, data.p, (size_t)data.len);

    if (zio_write_file(path, image, total) != 0) {
        zcc_printf("zcc: cannot write %s\n", path);
        return -1;
    }

    if (verbose)
        zcc_printf("zcc: %s: text %d, data %d, bss %d (image %d, file %d)\n",
                   path, text.len, data.len, bss_size,
                   text.len + data.len + bss_size, total);

    return 0;
}
