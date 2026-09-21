/*
 * zfpga -- synth, the back end: the gate graph to LUT4s, and a .zl.
 *
 * LUT MAPPING by priority cuts (Mishchenko et al., "Combinational and
 * sequential mapping with priority cuts", 2007, in miniature): every
 * gate keeps a few cuts of at most four inputs, ranked by depth and then
 * by area flow; the cover is taken from the outputs, the flip-flop
 * inputs and the carry-chain operands down. Each LUT's INIT is its
 * cone simulated over the 16 patterns of its inputs.
 *
 * CARRY CHAINS become ccu2 cells, two bits each. Driving each half's D
 * input with a constant 1 separates the half's LUT4 (which then reads
 * INIT[8..15]) from its LUT2 (INIT[0..3]): the LUT4 is the propagate,
 * a^b for an add or ~(a^b) for a subtract, and the LUT2 is `a`, the
 * generate when a == b. So an add is 0x060A and a subtract 0x090A; a
 * padding half, 0x0100, passes the carry through. yosys's arith_map
 * does the same with a different constant; tools/simcheck.py checks
 * the result against yosys either way.
 *
 * Nets keep the Verilog names where there is one -- ctr[3], led -- so
 * the .zl reads as the design it came from (docs/zfpga-formats.md).
 */

#include "synth.h"

static graph_t *G;
static const module_t *M;

#define CMAX 8

typedef struct {
    int n;
    int leaf[4];
    int depth;
    uint32_t af;
} cut_t;

static cut_t (*cuts)[CMAX + 1];
static uint8_t *ncuts;
static int *best_depth;
static uint32_t *best_af;
static int *fanout;

static int is_logic(int g) {
    int op = G->g[g].op;
    return op == G_NOT || op == G_AND || op == G_OR || op == G_XOR || op == G_MUX;
}

static int fanins(int g, int *f) {
    const gate_t *q = &G->g[g];
    switch (q->op) {
    case G_NOT: f[0] = q->a; return 1;
    case G_AND: case G_OR: case G_XOR: f[0] = q->a; f[1] = q->b; return 2;
    case G_MUX: f[0] = q->a; f[1] = q->b; f[2] = q->c; return 3;
    }
    return 0;
}

/* the union of two sorted leaf sets, or -1 if more than four */
static int merge(const cut_t *a, const cut_t *b, cut_t *r) {
    int i = 0, j = 0;
    r->n = 0;
    while (i < a->n || j < b->n) {
        int v;
        if (j >= b->n || (i < a->n && a->leaf[i] < b->leaf[j])) v = a->leaf[i++];
        else if (i >= a->n || b->leaf[j] < a->leaf[i]) v = b->leaf[j++];
        else { v = a->leaf[i]; i++; j++; }
        if (r->n == 4) return -1;
        r->leaf[r->n++] = v;
    }
    return 0;
}

static int better(const cut_t *a, const cut_t *b) {
    if (a->depth != b->depth) return a->depth < b->depth;
    if (a->af != b->af) return a->af < b->af;
    return a->n < b->n;
}

static void cut_cost(cut_t *c) {
    int k;
    c->depth = 0;
    c->af = 16;
    for (k = 0; k < c->n; k++) {
        int l = c->leaf[k];
        if (is_logic(l)) {
            if (best_depth[l] > c->depth) c->depth = best_depth[l];
            c->af += best_af[l] / (uint32_t)(fanout[l] ? fanout[l] : 1);
        }
    }
    c->depth++;
}

static void add_cut(int g, cut_t *c) {
    int k, j;
    for (k = 0; k < ncuts[g]; k++) {
        const cut_t *o = &cuts[g][k];
        if (o->n == c->n) {
            for (j = 0; j < c->n && o->leaf[j] == c->leaf[j]; j++) ;
            if (j == c->n) return;
        }
    }
    cut_cost(c);
    if (ncuts[g] < CMAX) { cuts[g][ncuts[g]++] = *c; }
    else if (better(c, &cuts[g][CMAX - 1])) cuts[g][CMAX - 1] = *c;
    else return;
    for (k = ncuts[g] - 1; k > 0 && better(&cuts[g][k], &cuts[g][k - 1]); k--) {
        cut_t t = cuts[g][k]; cuts[g][k] = cuts[g][k - 1]; cuts[g][k - 1] = t;
    }
}

/* cuts of a fanin, as a mapping input: its own cuts, and itself */
static int fanin_cuts(int f, cut_t *out) {
    int n = 0, k;
    if (f <= 1) { out[0].n = 0; return 1; }                /* a constant: no input */
    if (is_logic(f)) for (k = 0; k < ncuts[f]; k++) out[n++] = cuts[f][k];
    out[n].n = 1;
    out[n].leaf[0] = f;
    return n + 1;
}

static void map_cuts(void) {
    int g, k;
    for (g = 2; g < G->n_g; g++) {
        int f[3], nf, i, j, l;
        cut_t c0[CMAX + 1], c1[CMAX + 1], c2[CMAX + 1], t, u;
        int n0, n1, n2;
        if (!is_logic(g)) continue;
        nf = fanins(g, f);
        n0 = fanin_cuts(f[0], c0);
        n1 = nf > 1 ? fanin_cuts(f[1], c1) : 1;
        n2 = nf > 2 ? fanin_cuts(f[2], c2) : 1;
        if (nf < 2) c1[0].n = 0;
        if (nf < 3) c2[0].n = 0;
        for (i = 0; i < n0; i++)
            for (j = 0; j < n1; j++) {
                if (merge(&c0[i], &c1[j], &t) < 0) continue;
                for (l = 0; l < n2; l++) {
                    if (merge(&t, &c2[l], &u) < 0) continue;
                    add_cut(g, &u);
                }
            }
        if (!ncuts[g]) zf_fatal("synth: no cut for gate %d", g);
        best_depth[g] = cuts[g][0].depth;
        best_af[g] = cuts[g][0].af;
    }
    (void)k;
}

/* -- truth tables ------------------------------------------------------------- */

static uint16_t *ttv;
static uint32_t *tts, ttstamp;

static uint16_t tt(int g, const cut_t *c) {
    int k;
    uint16_t r;
    static const uint16_t var[4] = { 0xAAAA, 0xCCCC, 0xF0F0, 0xFF00 };
    for (k = 0; k < c->n; k++) if (c->leaf[k] == g) return var[k];
    if (g == 0) return 0;
    if (g == 1) return 0xFFFF;
    if (tts[g] == ttstamp) return ttv[g];
    {
        const gate_t *q = &G->g[g];
        switch (q->op) {
        case G_NOT: r = (uint16_t)~tt(q->a, c); break;
        case G_AND: r = (uint16_t)(tt(q->a, c) & tt(q->b, c)); break;
        case G_OR: r = (uint16_t)(tt(q->a, c) | tt(q->b, c)); break;
        case G_XOR: r = (uint16_t)(tt(q->a, c) ^ tt(q->b, c)); break;
        case G_MUX: {
            uint16_t s = tt(q->c, c);
            r = (uint16_t)((s & tt(q->a, c)) | (~s & tt(q->b, c)));
            break;
        }
        default: zf_fatal("synth: a cut does not bound its cone (gate %d)", g);
        }
    }
    tts[g] = ttstamp;
    ttv[g] = r;
    return r;
}

/* -- names -------------------------------------------------------------------- */

static const char *net(int g) {
    char buf[32];
    if (g == 0) return "0";
    if (g == 1) return "1";
    if (g < G->n_g && G->gname && G->gname[g]) return G->gname[g];
    zf_fmt(buf, sizeof(buf), "_n%d", g);
    return zf_strdup(buf);
}

/* -- the .lpf: LOCATE COMP "x" SITE "y"; IOBUF PORT "x" K=V ...; ---------------- */

typedef struct { const char *port, *site, *opts; } pin_t;
static pin_t *pins;
static int n_pins, cap_pins;

static pin_t *pin_of(const char *port, int create) {
    int i;
    for (i = 0; i < n_pins; i++) if (zf_streq(pins[i].port, port)) return &pins[i];
    if (!create) return NULL;
    if (n_pins == cap_pins) {
        int cap = cap_pins ? cap_pins * 2 : 64;
        pin_t *np = zf_alloc(sizeof(pin_t) * (size_t)cap);
        if (n_pins) zf_memcpy(np, pins, sizeof(pin_t) * (size_t)n_pins);
        pins = np;
        cap_pins = cap;
    }
    zf_memset(&pins[n_pins], 0, sizeof(pin_t));
    pins[n_pins].port = zf_strdup(port);
    pins[n_pins].opts = "";
    return &pins[n_pins++];
}

static const char *quoted(const char **p) {
    char buf[128];
    int n = 0;
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
    if (**p != '"') return NULL;
    (*p)++;
    while (**p && **p != '"' && n < 127) buf[n++] = *(*p)++;
    if (**p == '"') (*p)++;
    buf[n] = 0;
    return zf_strdup(buf);
}

static int word(const char **p, const char *w) {
    size_t n = zf_strlen(w), i;
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
    for (i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != w[i]) return 0;
    }
    *p += n;
    return 1;
}

static void read_lpf(const char *path) {
    uint32_t len;
    uint8_t *raw = zf_read_all(path, &len);
    char *s;
    uint32_t i;
    const char *p;
    if (!raw) zf_fatal("cannot open %s%s", path, zf_83_hint(path));
    s = zf_alloc(len + 1);
    zf_memcpy(s, raw, len);
    s[len] = 0;
    for (i = 0; i < len; i++)                   /* comments: # to end of line */
        if (s[i] == '#') while (i < len && s[i] != '\n') s[i++] = ' ';
    p = s;
    while (*p) {
        const char *stmt = p;
        while (*p && *p != ';') p++;
        {
            char *one = zf_alloc((size_t)(p - stmt) + 1);
            const char *q = one;
            zf_memcpy(one, stmt, (size_t)(p - stmt));
            if (word(&q, "LOCATE") && word(&q, "COMP")) {
                const char *port = quoted(&q), *site;
                if (port && word(&q, "SITE") && (site = quoted(&q)) != NULL) pin_of(port, 1)->site = site;
            } else {
                q = one;
                if (word(&q, "IOBUF") && word(&q, "PORT")) {
                    const char *port = quoted(&q);
                    char opts[256];
                    int n = 0;
                    opts[0] = 0;
                    while (port && *q) {
                        char kv[64];
                        int k = 0;
                        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                        while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && k < 63)
                            kv[k++] = *q++;
                        kv[k] = 0;
                        if (k) {
                            int j;
                            for (j = 0; kv[j] && kv[j] != '='; j++)
                                if (kv[j] >= 'A' && kv[j] <= 'Z') kv[j] = (char)(kv[j] + 32);
                            n += zf_fmt(opts + n, (int)sizeof(opts) - n, " %s", kv);
                        }
                    }
                    if (port) pin_of(port, 1)->opts = zf_strdup(opts);
                }
            }
        }
        if (*p) p++;
    }
}

/* -- the .zl ------------------------------------------------------------------ */

static zf_writer_t *W;

static void out(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    zf_vfmt(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    zf_writer_bytes(W, (const uint8_t *)buf, (uint32_t)zf_strlen(buf));
}

/* a net for a gate that must be driven: a constant needs a LUT */
static int need_const[2];
static const char *drv(int g) {
    if (g <= 1) { need_const[g] = 1; return g ? "_one" : "_zero"; }
    return net(g);
}

void synth_map_write(const module_t *m, graph_t *g, const char *path,
        const char *device, const char *package, const char *lpf) {
    int i, k, n_lut = 0, n_cells = 0;
    int *req, *stack, sp = 0, n_ff = 0, n_ch = 0;
    uint8_t *live, *ff_live, *ch_live;
    int *ch_hi;
    M = m;
    G = g;

    cuts = zf_alloc(sizeof(*cuts) * (size_t)G->n_g);
    ncuts = zf_alloc((size_t)G->n_g);
    best_depth = zf_alloc(sizeof(int) * (size_t)G->n_g);
    best_af = zf_alloc(sizeof(uint32_t) * (size_t)G->n_g);
    fanout = zf_alloc(sizeof(int) * (size_t)G->n_g);
    for (i = 2; i < G->n_g; i++) {
        int f[3], nf = fanins(i, f);
        for (k = 0; k < nf; k++) fanout[f[k]]++;
    }
    map_cuts();

    /* What is live: the outputs, and everything they reach -- through
     * logic, through a flip-flop to its inputs, through a carry chain to
     * its operands. The rest is swept: yosys keeps only the four bits of
     * an 8-bit counter an LED reads, and so should this. */
    live = zf_alloc((size_t)G->n_g);
    ff_live = zf_alloc((size_t)G->n_ff + 1);
    ch_live = zf_alloc((size_t)G->n_ch + 1);
    ch_hi = zf_alloc(sizeof(int) * (size_t)(G->n_ch + 1));
    for (i = 0; i < G->n_ch; i++) ch_hi[i] = -1;
    stack = zf_alloc(sizeof(int) * (size_t)G->n_g);
#define LIVE(x) do { int l_ = (x); if (l_ > 1 && !live[l_]) { live[l_] = 1; stack[sp++] = l_; } } while (0)
    for (i = 0; i < G->n_io; i++) if (G->io[i].output) LIVE(G->io[i].gate);
    while (sp) {
        int x = stack[--sp], f[3], nf;
        const gate_t *q = &G->g[x];
        nf = fanins(x, f);
        for (k = 0; k < nf; k++) LIVE(f[k]);
        if (q->op == G_Q && !ff_live[q->a]) {
            const sff_t *ff = &G->ff[q->a];
            ff_live[q->a] = 1;
            LIVE(ff->d); LIVE(ff->clk);
            if (ff->ce >= 0) LIVE(ff->ce);
            if (ff->lsr >= 0) LIVE(ff->lsr);
        }
        if (q->op == G_SUM || q->op == G_COUT) {
            /* sum bit i needs operand bits 0..i; the carry out needs all */
            const chain_t2 *ch = &G->ch[q->a];
            int top = q->op == G_COUT ? ch->n - 1 : q->b;
            ch_live[q->a] = 1;
            for (k = ch_hi[q->a] + 1; k <= top; k++) { LIVE(ch->x[k]); LIVE(ch->y[k]); }
            if (top > ch_hi[q->a]) ch_hi[q->a] = top;
        }
    }
#undef LIVE

    /* the cover, from the roots down */
    req = zf_alloc(sizeof(int) * (size_t)G->n_g);
#define ROOT(x) do { int r_ = (x); if (r_ > 1 && is_logic(r_) && !req[r_]) { req[r_] = 1; stack[sp++] = r_; } } while (0)
    for (i = 0; i < G->n_io; i++) if (G->io[i].output) ROOT(G->io[i].gate);
    for (i = 0; i < G->n_ff; i++) {
        if (!ff_live[i]) continue;
        ROOT(G->ff[i].d); ROOT(G->ff[i].clk);
        if (G->ff[i].ce >= 0) ROOT(G->ff[i].ce);
        if (G->ff[i].lsr >= 0) ROOT(G->ff[i].lsr);
    }
    for (i = 0; i < G->n_ch; i++)
        if (ch_live[i])
            for (k = 0; k <= ch_hi[i]; k++) { ROOT(G->ch[i].x[k]); ROOT(G->ch[i].y[k]); }
    while (sp) {
        int x = stack[--sp];
        const cut_t *c = &cuts[x][0];
        for (k = 0; k < c->n; k++) ROOT(c->leaf[k]);
    }
#undef ROOT

    /* every refusal before the output is opened, so none leaves a
     * partial .zl behind (tests/run.sh caught this one) */
    if (lpf) read_lpf(lpf);
    for (i = 0; i < G->n_io; i++) {
        pin_t *p = pin_of(G->io[i].name, 0);
        if (!p || !p->site)
            zf_fatal("port %s has no pin: give an .lpf with LOCATE COMP \"%s\" SITE ... (-l)",
                G->io[i].name, G->io[i].name);
    }
    for (i = 0; i < G->n_ff; i++) {
        const sff_t *f = &G->ff[i];
        if (!ff_live[i]) continue;
        if (f->clk <= 1) zf_fatal("flip-flop %s has a constant clock", f->name);
        if (f->ce == 0) zf_fatal("flip-flop %s is never enabled", f->name);
        if (f->lsr >= 0 && f->lsr <= 1) zf_fatal("flip-flop %s has a constant reset", f->name);
    }
    W = zf_alloc(sizeof(*W));
    zf_writer_open(W, path);
    out("# synthesised by zfpga synth from %s\ndevice %s\npackage %s\n\n", m->file, device, package);

    for (i = 0; i < G->n_io; i++) {
        const sio_t *o = &G->io[i];
        pin_t *p = pin_of(o->name, 0);
        if (!p || !p->site)
            zf_fatal("port %s has no pin: give an .lpf with LOCATE COMP \"%s\" SITE ... (-l)",
                o->name, o->name);
        out("%s %s %s net=%s%s\n", o->output ? "output" : "input", o->name, p->site,
            o->output ? drv(o->gate) : net(o->gate), p->opts);
    }
    out("\n");

    ttv = zf_alloc(sizeof(uint16_t) * (size_t)G->n_g);
    tts = zf_alloc(sizeof(uint32_t) * (size_t)G->n_g);
    for (i = 2; i < G->n_g; i++) {
        const cut_t *c;
        static const char *pn[4] = { "a", "b", "c", "d" };
        if (!req[i]) continue;
        c = &cuts[i][0];
        ttstamp++;
        out("lut _l%d init=0x%04X", i, tt(i, c));
        for (k = 0; k < c->n; k++) out(" %s=%s", pn[k], net(c->leaf[k]));
        out(" z=%s\n", net(i));
        n_lut++;
    }

    for (i = 0; i < G->n_ch; i++) {
        const chain_t2 *ch = &G->ch[i];
        int nb = ch_hi[i] + 1, cells = (nb + 1) / 2;     /* bits above the last used sum go */
        if (!ch_live[i]) continue;
        n_ch++;
        for (k = 0; k < cells; k++) {
            int b0 = 2 * k, b1 = 2 * k + 1;
            char cin[32], cout[32];
            if (k == 0) zf_fmt(cin, sizeof(cin), "%d", ch->sub ? 1 : 0);
            else zf_fmt(cin, sizeof(cin), "_ch%d_c%d", i, k - 1);
            if (k == cells - 1) zf_fmt(cout, sizeof(cout), "%s", ch->cout_used ? net(g_make(G, G_COUT, i, 0, 0)) : "-");
            else zf_fmt(cout, sizeof(cout), "_ch%d_c%d", i, k);
            out("ccu2 _ch%d_%d init0=0x%s inject0=NO a0=%s b0=%s c0=0 d0=1 s0=%s",
                i, k, ch->sub ? "090A" : "060A", net(ch->x[b0]), net(ch->y[b0]),
                net(g_make(G, G_SUM, i, b0, 0)));
            if (b1 < nb)
                out(" init1=0x%s inject1=NO a1=%s b1=%s c1=0 d1=1 s1=%s", ch->sub ? "090A" : "060A",
                    net(ch->x[b1]), net(ch->y[b1]), net(g_make(G, G_SUM, i, b1, 0)));
            else
                out(" init1=0x0100 inject1=NO a1=0 b1=0 c1=0 d1=1 s1=-");
            out(" cin=%s cout=%s\n", cin, cout);
            n_cells++;
        }
    }

    for (i = 0; i < G->n_ff; i++) {
        const sff_t *f = &G->ff[i];
        if (!ff_live[i]) continue;
        n_ff++;
        if (f->clk <= 1) zf_fatal("flip-flop %s has a constant clock", f->name);
        out("ff %s d=%s q=%s clk=%s", f->name, drv(f->d), net(g_make(G, G_Q, i, 0, 0)), net(f->clk));
        if (f->ce >= 0) {
            if (f->ce == 0) zf_fatal("flip-flop %s is never enabled", f->name);
            if (f->ce > 1) out(" ce=%s", net(f->ce));
        }
        if (f->lsr >= 0) {
            if (f->lsr <= 1) zf_fatal("flip-flop %s has a constant reset", f->name);
            out(" lsr=%s srmode=ASYNC lsrmux=%s", net(f->lsr), f->lsr_inv ? "INV" : "LSR");
        }
        out(" regset=%s\n", f->regset ? "SET" : "RESET");
    }
    if (need_const[0]) { out("lut _zero init=0x0000 z=_zero\n"); n_lut++; }
    if (need_const[1]) { out("lut _one init=0xFFFF z=_one\n"); n_lut++; }
    zf_writer_close(W);
    zf_note("synthesised %d LUTs, %d flip-flops, %d carry chains (%d cells)",
        n_lut, n_ff, n_ch, n_cells);
}

/* -- the command ------------------------------------------------------------------ */

int cmd_synth(int argc, char **argv) {
    const char *outp = NULL, *lpf = NULL, *device = "LFE5U-25F", *package = "CABGA256", *top = NULL;
    const char *ins[32];
    int n_in = 0, i;
    lib_t *lib = zf_alloc(sizeof(*lib));
    module_t *m = zf_alloc(sizeof(*m));
    graph_t *g = zf_alloc(sizeof(*g));
    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (zf_streq(argv[i], "-l") && i + 1 < argc) lpf = argv[++i];
        else if (zf_streq(argv[i], "-d") && i + 1 < argc) device = argv[++i];
        else if (zf_streq(argv[i], "-p") && i + 1 < argc) package = argv[++i];
        else if (zf_streq(argv[i], "-t") && i + 1 < argc) top = argv[++i];
        else if (argv[i][0] == '-') zf_fatal("synth: unknown option %s", argv[i]);
        else if (n_in == 32) zf_fatal("synth: more than 32 input files");
        else ins[n_in++] = argv[i];
    }
    if (!n_in)
        zf_fatal("usage: zfpga synth IN.v [MORE.v ...] [-t TOP] [-l PINS.lpf] [-o OUT.zl] [-d DEVICE] [-p PACKAGE]");
    if (!outp) {
        /* named after the first file */
        const char *in = ins[0];
        size_t n = zf_strlen(in), dot = n, j;
        char *o;
        for (j = n; j > 0; j--) { if (in[j - 1] == '/') break; if (in[j - 1] == '.') { dot = j - 1; break; } }
        o = zf_alloc(dot + 4);
        zf_memcpy(o, in, dot);
        zf_memcpy(o + dot, ".zl", 4);
        outp = o;
    }
    for (i = 0; i < n_in; i++) synth_parse(ins[i], lib);
    synth_flatten(lib, top, m);
    if (m->n_inst) zf_note("top %s: %d instance%s flattened", m->name, m->n_inst, m->n_inst == 1 ? "" : "s");
    synth_elab(m, g);
    synth_map_write(m, g, outp, device, package, lpf);
    return 0;
}
