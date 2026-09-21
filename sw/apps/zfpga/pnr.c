/*
 * zfpga -- pnr: a physical netlist (.zn) to a Trellis .config.
 *
 * Phase 3 of docs/zfpga.md: MANUAL placement. Every cell names its site
 * and every connection is given as a Trellis arc, so this does no
 * placing and no routing -- it translates. What it knows is what
 * nextpnr-ecp5 0.6's bitstream writer (ecp5/bitstream.cc) knows about
 * turning placed cells into tile settings, and tests/run.sh checks the
 * translation against nextpnr byte for byte (docs/zfpga.md sec. 12).
 *
 * The netlist is PHYSICAL (sec. 12.1): a LUT's INIT is in physical pin
 * order with the used pins listed, and a flip-flop says which tile wire
 * its clock and reset arrive on. Choosing those is a router's job
 * (Phase 4); stating them is what makes this phase a pure translation.
 *
 * Format, one statement per line, '#' comments:
 *
 *   device LFE5U-25F
 *   package CABGA256            (for pin names; default none)
 *   speed 6                     (for the metadata only; default 6)
 *   comb R2C5 SLICEB 1 mode=CCU2 init=0x000E pins=AD inject=NO
 *   ff   R2C5 SLICEA 0 clk=CLK0@clk lsr=- gsr=DISABLED sd=1
 *   io   A2 dir=OUTPUT type=LVCMOS33
 *   dcc  R0C31 TDCC0
 *   arc  R2C5:PLC2 A0 H02W0701
 *
 * Phase 4 adds routing: instead of arcs, a netlist may declare nets by
 * their terminals, as global wires, and leave the routes to route.c:
 *
 *   net clk R1C29/JQ0 R2C5/MUXCLK0 R2C2/MUXCLK1
 *   sinks clk R2C3/MUXCLK0 R2C4/MUXCLK1        (more sinks for a declared net)
 *   ff   R2C5 SLICEA 0 clk=?@clk
 *
 * `?` asks for the clock (or reset) wire to be read back from whatever
 * the router chose for the slice's MUXCLKn (MUXLSRn). A netlist either
 * gives arcs or declares nets, not both: the router cannot see wires
 * that hand-written arcs have taken.
 *
 * Refused, by name: every cell type but these four; LUT modes other
 * than LOGIC and CCU2; IO types other than single-ended LVCMOS/LVTTL;
 * a clock buffer with a clock enable.
 */

#include "zfpga.h"

/* -- the configuration being built -------------------------------------
 * One list per tile, in insertion order, emitted grouped as libtrellis's
 * TileConfig does: arcs, words, enums, unknowns. Tiles are emitted in
 * database order, which is name order, which is std::map order. */

enum { E_ARC, E_WORD, E_ENUM, E_UNK };

typedef struct cent {
    struct cent *next;
    uint8_t kind;
    const char *a, *b;
} cent_t;

typedef struct {
    cent_t *head, *tail;
} ctile_t;

static zdb_t *db;
static ctile_t *ct;
static const char *zpath;

static void add(uint32_t tile, int kind, const char *a, const char *b) {
    cent_t *e = zf_alloc(sizeof(*e));
    e->kind = (uint8_t)kind;
    e->a = a;
    e->b = b;
    if (ct[tile].tail) ct[tile].tail->next = e; else ct[tile].head = e;
    ct[tile].tail = e;
}

static char *cat(const char *a, const char *b) {
    size_t la = zf_strlen(a), lb = zf_strlen(b);
    char *s = zf_alloc(la + lb + 1);
    zf_memcpy(s, a, la);
    zf_memcpy(s + la, b, lb + 1);
    return s;
}

static char *cat3(const char *a, const char *b, const char *c) {
    return cat(cat(a, b), c);
}

/* -- the netlist -------------------------------------------------------- */

typedef struct {
    uint32_t tile;
    int slice, lc, line;
    const char *mode, *inject;
    uint32_t init;
    unsigned pins;          /* bit 0..3 = A..D used */
} comb_t;

typedef struct {
    uint32_t tile;
    int slice, lc, line;
    int clkw, lsrw;         /* tile wire 0/1, or -1 for none */
    const char *clknet, *lsrnet;
    const char *gsr, *sd, *regset, *lsrmode, *cemux, *clkmux, *srmode, *lsrmux;
} ff_t;

typedef struct {
    uint32_t pio;
    int line, tristate;
    const char *dir, *type;
    const char *hysteresis, *slewrate, *pullmode, *clamp, *drive, *opendrain;
    const char *datamux_oddr, *datamux_oreg, *datamux_mddr, *trimux_tsreg;
} io_t;

typedef struct {
    uint32_t tile;
    const char *sink, *source;
    int line;
} arcl_t;

#define GROW(arr, n, cap, T) do { if ((n) == (cap)) {                     \
    int nc_ = (cap) ? (cap) * 2 : 64;                                     \
    T *na_ = zf_alloc(sizeof(T) * (size_t)nc_);                           \
    if (n) zf_memcpy(na_, (arr), sizeof(T) * (size_t)(n));                \
    (arr) = na_; (cap) = nc_; } } while (0)

static comb_t *combs; static int n_comb, cap_comb;
static ff_t *ffs;     static int n_ff, cap_ff;
static io_t *ios;     static int n_io, cap_io;
static arcl_t *arcs;  static int n_arc, cap_arc;

typedef struct {
    const char *name;
    gwire_t src;
    gwire_t *sinks;
    int n_sinks, line;
} znet_t;
static znet_t *nets;  static int n_net, cap_net;
static int arcs_explicit;
static int route_line;

/* The router's arcs join the explicit list, attributed to their net. */
static void routed_arc(int net, uint32_t tile, const char *sink, const char *source) {
    arcl_t *a;
    route_line = nets[net].line;
    GROW(arcs, n_arc, cap_arc, arcl_t);
    a = &arcs[n_arc++];
    a->tile = tile;
    a->sink = sink;
    a->source = source;
    a->line = route_line;
}

/* -- parsing helpers ---------------------------------------------------- */

/* "R2C5" + ":PLC2" -> tile index, or refuse. */
static uint32_t tile_of(const char *site, const char *type, int line) {
    char name[64];
    int32_t t;
    zf_fmt(name, sizeof(name), "%s:%s", site, type);
    t = zdb_find_tile(db, name);
    if (t < 0) zf_fatal_at(zpath, line, "no %s tile at %s", type, site);
    return (uint32_t)t;
}

static int slice_of(const char *s, int line) {
    if (s[0] == 'S' && s[1] == 'L' && s[2] == 'I' && s[3] == 'C' && s[4] == 'E' &&
            s[5] >= 'A' && s[5] <= 'D' && !s[6])
        return s[5] - 'A';
    zf_fatal_at(zpath, line, "bad slice '%s' (SLICEA..SLICED)", s);
}

static int lc_of(const char *s, int line) {
    if ((s[0] == '0' || s[0] == '1') && !s[1]) return s[0] - '0';
    zf_fatal_at(zpath, line, "bad LUT/FF index '%s' (0 or 1)", s);
}

typedef struct { const char *key; const char **dst; } opt_t;

/* key=value tokens into the table. Flags (no '=') are handled by the
 * caller before this sees them. Values are copied: the line is reused. */
static void options(char **tok, int n, const opt_t *tab, int line) {
    int i, k;
    for (i = 0; i < n; i++) {
        char *eq = tok[i];
        while (*eq && *eq != '=') eq++;
        if (!*eq) zf_fatal_at(zpath, line, "expected key=value, got '%s'", tok[i]);
        *eq = 0;
        for (k = 0; tab[k].key; k++)
            if (zf_streq(tab[k].key, tok[i])) break;
        if (!tab[k].key) zf_fatal_at(zpath, line, "unknown option '%s'", tok[i]);
        *tab[k].dst = zf_strdup(eq + 1);
    }
}

/* CLK0@net -> wire 0, net "net"; "-" -> -1 */
static void wire_net(const char *v, const char *wbase, int *w, const char **net, int line) {
    size_t n = zf_strlen(wbase);
    size_t i;
    if (!v || zf_streq(v, "-")) { *w = -1; *net = NULL; return; }
    if (v[0] == '?' && v[1] == '@' && v[2]) { *w = -2; *net = zf_strdup(v + 2); return; }
    for (i = 0; i < n; i++)
        if (v[i] != wbase[i]) goto bad;
    if ((v[n] != '0' && v[n] != '1') || v[n + 1] != '@' || !v[n + 2]) goto bad;
    *w = v[n] - '0';
    *net = zf_strdup(v + n + 2);
    return;
bad:
    zf_fatal_at(zpath, line, "'%s': want %s0@net, %s1@net, ?@net or -", v, wbase, wbase);
}

/* -- IO ----------------------------------------------------------------- */

/* Single-ended types and their bank voltage: nextpnr's get_vccio(),
 * restricted to what Phase 3 supports. */
static const struct { const char *type, *vcc; } iotypes[] = {
    { "LVCMOS33", "3V3" }, { "LVTTL33", "3V3" }, { "LVCMOS25", "2V5" },
    { "LVCMOS18", "1V8" }, { "LVCMOS15", "1V5" }, { "LVCMOS12", "1V2" },
};

static const char *vcc_of(const char *type) {
    unsigned i;
    for (i = 0; i < sizeof(iotypes) / sizeof(iotypes[0]); i++)
        if (zf_streq(type, iotypes[i].type)) return iotypes[i].vcc;
    return NULL;
}

/* A pin name in the netlist's package, or a site "R0C4.A". */
static uint32_t pio_of(const char *pin, const char *package, int line) {
    uint32_t i;
    const char *dot = pin;
    while (*dot && *dot != '.') dot++;
    if (*dot) {
        for (i = 0; i < db->n_pio; i++) {
            char site[24];
            zf_fmt(site, sizeof(site), "R%uC%u.%c", db->pio[i].row, db->pio[i].col,
                db->pio[i].letter);
            if (zf_streq(site, pin)) return i;
        }
        zf_fatal_at(zpath, line, "no PIO at %s", pin);
    }
    if (!package) zf_fatal_at(zpath, line, "pin %s needs a 'package' line (or use R<r>C<c>.<A-D>)", pin);
    for (i = 0; i < db->n_pin; i++)
        if (zf_streq(ZDB_STR(db, db->pin[i].package), package) &&
                zf_streq(ZDB_STR(db, db->pin[i].pin), pin))
            return db->pin[i].pio;
    zf_fatal_at(zpath, line, "package %s has no pin %s", package, pin);
}

/* -- the baseline -------------------------------------------------------
 * nextpnr's config_empty_*(): parsed out of the .zdb's BASE section,
 * which is a .config fragment (ext/nextpnr-base/). */

static void load_baseline(void) {
    const char *p = db->baseline, *end = db->baseline + db->baseline_len;
    char line[256];
    int32_t tile = -1;
    while (p < end) {
        size_t n = 0;
        char *tok[4];
        int nt;
        while (p < end && *p != '\n') {
            if (n < sizeof(line) - 1) line[n++] = *p;
            p++;
        }
        if (p < end) p++;
        line[n] = 0;
        nt = zf_tokens(line, tok, 4);
        if (nt == 0) continue;
        if (zf_streq(tok[0], ".device")) continue;
        if (zf_streq(tok[0], ".tile") && nt == 2) {
            tile = zdb_find_tile(db, tok[1]);
            if (tile < 0) zf_fatal("database baseline names unknown tile %s", tok[1]);
            continue;
        }
        if (tile < 0) zf_fatal("database baseline: entry outside a tile");
        if (zf_streq(tok[0], "arc:") && nt == 3)
            add((uint32_t)tile, E_ARC, zf_strdup(tok[1]), zf_strdup(tok[2]));
        else if (zf_streq(tok[0], "enum:") && nt == 3)
            add((uint32_t)tile, E_ENUM, zf_strdup(tok[1]), zf_strdup(tok[2]));
        else if (zf_streq(tok[0], "unknown:") && nt == 2)
            add((uint32_t)tile, E_UNK, zf_strdup(tok[1]), NULL);
        else
            zf_fatal("database baseline: cannot read '%s'", tok[0]);
    }
}

/* -- flip-flop wire comparison -----------------------------------------
 * nextpnr writes CLKk.CLKMUX (and LSRk.*) when the net bound to tile
 * wire CLKk is the flip-flop's clock net -- a pointer comparison in
 * which "no net" equals "no net". So a flip-flop with no reset sets the
 * LSR settings for every LSR wire nothing is using. Reproduced exactly;
 * docs/zfpga.md sec. 12.1.
 *
 * The net on a wire is known from the flip-flops that use it. A wire
 * driven by an arc that no flip-flop claims carries some other net,
 * which equals nothing here. */

#define NET_OTHER ((const char *)1)

static const char *bound_net(uint32_t tile, int clk, int k) {
    const char *net = NULL;
    const char *wire = clk ? (k ? "CLK1" : "CLK0") : (k ? "LSR1" : "LSR0");
    int i;
    for (i = 0; i < n_ff; i++) {
        const ff_t *f = &ffs[i];
        int w = clk ? f->clkw : f->lsrw;
        const char *fn = clk ? f->clknet : f->lsrnet;
        if (f->tile != tile || w != k) continue;
        if (net && !zf_streq(net, fn))
            zf_fatal_at(zpath, f->line, "%s in %s carries two nets, %s and %s",
                wire, ZDB_STR(db, db->tile[tile].name), net, fn);
        net = fn;
    }
    if (net) return net;
    for (i = 0; i < n_arc; i++)
        if (arcs[i].tile == tile && zf_streq(arcs[i].sink, wire))
            return NET_OTHER;
    return NULL;
}

static int same_net(const char *a, const char *b) {
    if (a == NET_OTHER || b == NET_OTHER) return 0;
    if (!a || !b) return a == b;
    return zf_streq(a, b);
}

/* -- generation --------------------------------------------------------- */

static const char *SL[4] = { "SLICEA", "SLICEB", "SLICEC", "SLICED" };

static void gen_comb(const comb_t *c) {
    char k[16], lc[2];
    char *init = zf_alloc(17);
    int i;
    lc[0] = (char)('0' + c->lc); lc[1] = 0;
    for (i = 0; i < 16; i++) init[i] = (c->init >> (15 - i)) & 1 ? '1' : '0';
    add(c->tile, E_ENUM, cat(SL[c->slice], ".MODE"), c->mode);
    zf_fmt(k, sizeof(k), ".K%d.INIT", c->lc);
    add(c->tile, E_WORD, cat(SL[c->slice], k), init);
    add(c->tile, E_ENUM, cat3(SL[c->slice], ".CCU2.INJECT1_", lc),
        zf_streq(c->mode, "CCU2") ? c->inject : "_NONE_");
    for (i = 0; i < 4; i++) {
        if (!(c->pins & (1u << i))) {
            char m[16];
            zf_fmt(m, sizeof(m), ".%c%dMUX", "ABCD"[i], c->lc);
            add(c->tile, E_ENUM, cat(SL[c->slice], m), "1");
        }
    }
}

static void gen_ff(const ff_t *f) {
    char r[16];
    int k;
    const char *s = SL[f->slice];
    add(f->tile, E_ENUM, cat(s, ".GSR"), f->gsr);
    zf_fmt(r, sizeof(r), ".REG%d.SD", f->lc);
    add(f->tile, E_ENUM, cat(s, r), f->sd);
    zf_fmt(r, sizeof(r), ".REG%d.REGSET", f->lc);
    add(f->tile, E_ENUM, cat(s, r), f->regset);
    zf_fmt(r, sizeof(r), ".REG%d.LSRMODE", f->lc);
    add(f->tile, E_ENUM, cat(s, r), f->lsrmode);
    add(f->tile, E_ENUM, cat(s, ".CEMUX"), f->cemux);
    for (k = 0; k < 2; k++) {
        if (same_net(bound_net(f->tile, 0, k), f->lsrnet)) {
            add(f->tile, E_ENUM, k ? "LSR1.SRMODE" : "LSR0.SRMODE", f->srmode);
            add(f->tile, E_ENUM, k ? "LSR1.LSRMUX" : "LSR0.LSRMUX", f->lsrmux);
        }
    }
    for (k = 0; k < 2; k++)
        if (same_net(bound_net(f->tile, 1, k), f->clknet))
            add(f->tile, E_ENUM, k ? "CLK1.CLKMUX" : "CLK0.CLKMUX", f->clkmux);
}

static void gen_io(const io_t *o) {
    const zdb_pio_t *p = &db->pio[o->pio];
    char pio[8];
    const char *base;
    uint32_t pt = (uint32_t)p->pio_tile, ct_ = (uint32_t)p->pic_tile;
    zf_fmt(pio, sizeof(pio), "PIO%c", p->letter);
    base = cat3(o->dir, "_", o->type);
    add(pt, E_ENUM, cat(pio, ".BASE_TYPE"), base);
    add(ct_, E_ENUM, cat(pio, ".BASE_TYPE"), base);
    if (!zf_streq(o->dir, "INPUT") && !o->tristate) {
        if (p->tie_tile < 0)
            zf_fatal_at(zpath, o->line, "PIO R%uC%u.%c has no tristate tie; cannot be an output",
                p->row, p->col, p->letter);
        add((uint32_t)p->tie_tile, E_ENUM, ZDB_STR(db, p->tie_enum), "0");
    }
    if (zf_streq(o->dir, "INPUT") || zf_streq(o->dir, "BIDIR"))
        add(pt, E_ENUM, cat(pio, ".HYSTERESIS"), o->hysteresis ? o->hysteresis : "ON");
    if (o->slewrate) add(pt, E_ENUM, cat(pio, ".SLEWRATE"), o->slewrate);
    if (o->pullmode) add(pt, E_ENUM, cat(pio, ".PULLMODE"), o->pullmode);
    if (o->clamp) add(pt, E_ENUM, cat(pio, ".CLAMP"), o->clamp);
    if (o->drive) {
        /* nextpnr: DRIVE only on 3V3 LVCMOS; ignored with a warning
         * elsewhere, a "Trellis limitation". Refused here instead. */
        if (!zf_streq(o->type, "LVCMOS33"))
            zf_fatal_at(zpath, o->line, "drive= is only supported on LVCMOS33");
        add(pt, E_ENUM, cat(pio, ".DRIVE"), o->drive);
    }
    if (o->opendrain) add(pt, E_ENUM, cat(pio, ".OPENDRAIN"), o->opendrain);
    if (o->datamux_oddr && !zf_streq(o->datamux_oddr, "PADDO"))
        add(ct_, E_ENUM, cat(pio, ".DATAMUX_ODDR"), o->datamux_oddr);
    if (o->datamux_oreg && !zf_streq(o->datamux_oreg, "PADDO"))
        add(ct_, E_ENUM, cat(pio, ".DATAMUX_OREG"), o->datamux_oreg);
    if (o->datamux_mddr && !zf_streq(o->datamux_mddr, "PADDO"))
        add(ct_, E_ENUM, cat(pio, ".DATAMUX_MDDR"), o->datamux_mddr);
    if (o->trimux_tsreg && !zf_streq(o->trimux_tsreg, "PADDT"))
        add(ct_, E_ENUM, cat(pio, ".TRIMUX_TSREG"), o->trimux_tsreg);
}

/* nextpnr's init_io_banks(): a bank's voltage is set by its non-input
 * IO, and every BANKREF<n> tile of that bank gets BANK.VCCIO. */
static void gen_banks(void) {
    const char *vcc[16];
    int i;
    uint32_t t;
    zf_memset(vcc, 0, sizeof(vcc));
    for (i = 0; i < n_io; i++) {
        const io_t *o = &ios[i];
        int bank = db->pio[o->pio].bank;
        const char *v = vcc_of(o->type);
        if (zf_streq(o->dir, "INPUT")) continue;
        if (bank >= 16) zf_fatal_at(zpath, o->line, "bank %d out of range", bank);
        if (vcc[bank] && !zf_streq(vcc[bank], v))
            zf_fatal_at(zpath, o->line, "incompatible IO voltages %s and %s on bank %d",
                vcc[bank], v, bank);
        vcc[bank] = v;
    }
    for (t = 0; t < db->n_tile; t++) {
        const char *ty = ZDB_STR(db, db->type[db->tile[t].type].name);
        int bank = 0, j;
        if (!(ty[0] == 'B' && ty[1] == 'A' && ty[2] == 'N' && ty[3] == 'K' &&
                ty[4] == 'R' && ty[5] == 'E' && ty[6] == 'F'))
            continue;
        /* std::stoi(type.substr(7)): leading digits, "2A" is bank 2 */
        for (j = 7; ty[j] >= '0' && ty[j] <= '9'; j++) bank = bank * 10 + (ty[j] - '0');
        if (j == 7 || bank >= 16) continue;
        if (vcc[bank]) add(t, E_ENUM, "BANK.VCCIO", vcc[bank]);
    }
}

/* -- output ------------------------------------------------------------- */

static void emit(const char *path, const char *device, const char *package,
        const char *speed) {
    zf_writer_t *w = zf_alloc(sizeof(*w));
    char buf[256];
    uint32_t t;
    int kind;
    zf_writer_open(w, path);
#define PUT(s) zf_writer_bytes(w, (const uint8_t *)(s), (uint32_t)zf_strlen(s))
    PUT(".device "); PUT(device); PUT("\n\n");
    /* nextpnr's metadata, "Part: <device>-<speed><package>". It lands in
     * the bitstream header, so it matters to byte identity. */
    zf_fmt(buf, sizeof(buf), ".comment Part: %s-%s%s\n\n", device, speed,
        package ? package : "");
    PUT(buf);
    for (t = 0; t < db->n_tile; t++) {
        const cent_t *e;
        if (!ct[t].head) continue;
        PUT(".tile "); PUT(ZDB_STR(db, db->tile[t].name)); PUT("\n");
        for (kind = E_ARC; kind <= E_UNK; kind++) {
            for (e = ct[t].head; e; e = e->next) {
                if (e->kind != kind) continue;
                PUT(kind == E_ARC ? "arc: " : kind == E_WORD ? "word: " :
                    kind == E_ENUM ? "enum: " : "unknown: ");
                PUT(e->a);
                if (e->b) { PUT(" "); PUT(e->b); }
                PUT("\n");
            }
        }
        PUT("\n");
    }
#undef PUT
    zf_writer_close(w);
}

/* -- the command -------------------------------------------------------- */

static const char *default_config(const char *in) {
    size_t n = zf_strlen(in), dot = n, i;
    char *out;
    for (i = n; i > 0; i--) {
        if (in[i - 1] == '/') break;
        if (in[i - 1] == '.') { dot = i - 1; break; }
    }
    out = zf_alloc(dot + 5);
    zf_memcpy(out, in, dot);
    zf_memcpy(out + dot, ".cfg", 5);        /* 8.3: .config is too long */
    return out;
}

int cmd_pnr(int argc, char **argv, const char *dbdir_default) {
    const char *in = NULL, *out = NULL, *dbdir = dbdir_default;
    const char *device = NULL, *package = NULL, *speed = "6";
    zf_reader_t *r = zf_alloc(sizeof(*r));
    char *line, *tok[32];
    int i;

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (zf_streq(argv[i], "-D") && i + 1 < argc) dbdir = argv[++i];
        else if (argv[i][0] == '-') zf_fatal("pnr: unknown option %s", argv[i]);
        else if (!in) in = argv[i];
        else zf_fatal("pnr: more than one input");
    }
    if (!in) zf_fatal("usage: zfpga pnr IN.zn [-o OUT.cfg] [-D DBDIR]");
    if (!dbdir) zf_fatal("no database directory; give -D DIR");
    if (!out) out = default_config(in);
    zpath = in;
    db = zf_alloc(sizeof(*db));

    zf_reader_open(r, in);
    while ((line = zf_reader_line(r)) != NULL) {
        int ln = r->lineno;
        int n = zf_tokens(line, tok, 32);
        if (n == 0) continue;
        if (n == 32) zf_fatal_at(in, ln, "too many words on one line");

        if (zf_streq(tok[0], "device")) {
            if (n != 2) zf_fatal_at(in, ln, "device takes one name");
            if (device) zf_fatal_at(in, ln, "second device line");
            device = zf_strdup(tok[1]);
            zdb_load(db, dbdir, device);
            if (db->hdr->sect[ZDB_S_BASE].count == 0)
                zf_note("warning: database has no baseline; output will not match nextpnr");
            ct = zf_alloc(sizeof(ctile_t) * db->n_tile);
            continue;
        }
        if (!device) zf_fatal_at(in, ln, "'device' must come first");

        if (zf_streq(tok[0], "package") && n == 2) {
            package = zf_strdup(tok[1]);
        } else if (zf_streq(tok[0], "speed") && n == 2) {
            speed = zf_strdup(tok[1]);
        } else if (zf_streq(tok[0], "comb")) {
            comb_t *c;
            const char *init = NULL, *pins = NULL;
            opt_t tab[] = { { "mode", NULL }, { "init", &init }, { "pins", &pins },
                { "inject", NULL }, { NULL, NULL } };
            if (n < 4) zf_fatal_at(in, ln, "comb SITE SLICEx LC [options]");
            GROW(combs, n_comb, cap_comb, comb_t);
            c = &combs[n_comb++];
            zf_memset(c, 0, sizeof(*c));
            c->line = ln;
            c->tile = tile_of(tok[1], "PLC2", ln);
            c->slice = slice_of(tok[2], ln);
            c->lc = lc_of(tok[3], ln);
            c->mode = "LOGIC";
            c->inject = "YES";
            c->pins = 0xf;
            tab[0].dst = &c->mode;
            tab[3].dst = &c->inject;
            options(tok + 4, n - 4, tab, ln);
            if (!zf_streq(c->mode, "LOGIC") && !zf_streq(c->mode, "CCU2"))
                zf_fatal_at(in, ln, "LUT mode %s is not supported yet (LOGIC, CCU2)", c->mode);
            if (init && (!zf_parse_uint(init, &c->init) || c->init > 0xffff))
                zf_fatal_at(in, ln, "init '%s' is not a 16-bit value", init);
            if (pins) {
                const char *p;
                c->pins = 0;
                if (!zf_streq(pins, "-"))
                    for (p = pins; *p; p++) {
                        if (*p < 'A' || *p > 'D')
                            zf_fatal_at(in, ln, "pins '%s': letters A-D, or -", pins);
                        c->pins |= 1u << (*p - 'A');
                    }
            }
        } else if (zf_streq(tok[0], "ff")) {
            ff_t *f;
            const char *clk = NULL, *lsr = NULL;
            opt_t tab[] = { { "clk", &clk }, { "lsr", &lsr }, { "gsr", NULL },
                { "sd", NULL }, { "regset", NULL }, { "lsrmode", NULL },
                { "cemux", NULL }, { "clkmux", NULL }, { "srmode", NULL },
                { "lsrmux", NULL }, { NULL, NULL } };
            if (n < 4) zf_fatal_at(in, ln, "ff SITE SLICEx LC [options]");
            GROW(ffs, n_ff, cap_ff, ff_t);
            f = &ffs[n_ff++];
            zf_memset(f, 0, sizeof(*f));
            f->line = ln;
            f->tile = tile_of(tok[1], "PLC2", ln);
            f->slice = slice_of(tok[2], ln);
            f->lc = lc_of(tok[3], ln);
            f->gsr = "ENABLED"; f->sd = "0"; f->regset = "RESET"; f->lsrmode = "LSR";
            f->cemux = "1"; f->clkmux = "CLK"; f->srmode = "LSR_OVER_CE"; f->lsrmux = "LSR";
            tab[2].dst = &f->gsr; tab[3].dst = &f->sd; tab[4].dst = &f->regset;
            tab[5].dst = &f->lsrmode; tab[6].dst = &f->cemux; tab[7].dst = &f->clkmux;
            tab[8].dst = &f->srmode; tab[9].dst = &f->lsrmux;
            options(tok + 4, n - 4, tab, ln);
            wire_net(clk, "CLK", &f->clkw, &f->clknet, ln);
            wire_net(lsr, "LSR", &f->lsrw, &f->lsrnet, ln);
        } else if (zf_streq(tok[0], "io")) {
            io_t *o;
            int j, m = 2;
            opt_t tab[] = { { "dir", NULL }, { "type", NULL }, { "hysteresis", NULL },
                { "slewrate", NULL }, { "pullmode", NULL }, { "clamp", NULL },
                { "drive", NULL }, { "opendrain", NULL }, { "datamux_oddr", NULL },
                { "datamux_oreg", NULL }, { "datamux_mddr", NULL },
                { "trimux_tsreg", NULL }, { NULL, NULL } };
            if (n < 2) zf_fatal_at(in, ln, "io PIN [options]");
            GROW(ios, n_io, cap_io, io_t);
            o = &ios[n_io++];
            zf_memset(o, 0, sizeof(*o));
            o->line = ln;
            o->pio = pio_of(tok[1], package, ln);
            o->dir = "INPUT";
            o->type = "LVCMOS33";
            tab[0].dst = &o->dir; tab[1].dst = &o->type; tab[2].dst = &o->hysteresis;
            tab[3].dst = &o->slewrate; tab[4].dst = &o->pullmode; tab[5].dst = &o->clamp;
            tab[6].dst = &o->drive; tab[7].dst = &o->opendrain; tab[8].dst = &o->datamux_oddr;
            tab[9].dst = &o->datamux_oreg; tab[10].dst = &o->datamux_mddr;
            tab[11].dst = &o->trimux_tsreg;
            /* the one flag: the IO's tristate input is driven */
            for (j = 2; j < n; j++) {
                if (zf_streq(tok[j], "tristate")) o->tristate = 1;
                else tok[m++] = tok[j];
            }
            options(tok + 2, m - 2, tab, ln);
            if (!zf_streq(o->dir, "INPUT") && !zf_streq(o->dir, "OUTPUT") &&
                    !zf_streq(o->dir, "BIDIR"))
                zf_fatal_at(in, ln, "dir %s: INPUT, OUTPUT or BIDIR", o->dir);
            if (!vcc_of(o->type))
                zf_fatal_at(in, ln, "IO type %s is not supported yet (single-ended LVCMOS/LVTTL)",
                    o->type);
            for (j = 0; j < n_io - 1; j++)
                if (ios[j].pio == o->pio)
                    zf_fatal_at(in, ln, "pin %s already used on line %d", tok[1], ios[j].line);
        } else if (zf_streq(tok[0], "dcc")) {
            /* Without a clock enable nextpnr's write_dcc() writes
             * nothing: the buffer is routing, and its arcs are in the
             * netlist. With one it writes a tile group -- not yet. */
            if (n != 3) zf_fatal_at(in, ln, "dcc SITE NAME (clock enable not supported yet)");
        } else if (zf_streq(tok[0], "arc")) {
            arcl_t *a;
            int32_t t;
            const zdb_mux_t *mx;
            if (n != 4) zf_fatal_at(in, ln, "arc TILE SINK SOURCE");
            t = zdb_find_tile(db, tok[1]);
            if (t < 0) zf_fatal_at(in, ln, "no tile %s", tok[1]);
            mx = zdb_find_mux(db, &db->type[db->tile[t].type], tok[2]);
            if (!mx) zf_fatal_at(in, ln, "tile %s has no mux driving %s", tok[1], tok[2]);
            if (!zdb_find_arc(db, mx, tok[3]))
                zf_fatal_at(in, ln, "%s in %s cannot be driven from %s", tok[2], tok[1], tok[3]);
            arcs_explicit = 1;
            GROW(arcs, n_arc, cap_arc, arcl_t);
            a = &arcs[n_arc++];
            a->tile = (uint32_t)t;
            a->sink = zf_strdup(tok[2]);
            a->source = zf_strdup(tok[3]);
            a->line = ln;
        } else if (zf_streq(tok[0], "sinks")) {
            /* A net with more sinks than fit on one line: a clock
             * reaching every flip-flop's slice. */
            znet_t *nt = NULL;
            gwire_t *ns;
            int j;
            if (n < 3) zf_fatal_at(in, ln, "sinks NAME SINK...");
            for (j = n_net - 1; j >= 0; j--)
                if (zf_streq(nets[j].name, tok[1])) { nt = &nets[j]; break; }
            if (!nt) zf_fatal_at(in, ln, "sinks for undeclared net %s", tok[1]);
            ns = zf_alloc(sizeof(gwire_t) * (size_t)(nt->n_sinks + n - 2));
            zf_memcpy(ns, nt->sinks, sizeof(gwire_t) * (size_t)nt->n_sinks);
            for (j = 0; j < n - 2; j++)
                if (!route_parse_wire(tok[2 + j], &ns[nt->n_sinks + j]))
                    zf_fatal_at(in, ln, "'%s' is not a wire (R<row>C<col>/<name>)", tok[2 + j]);
            nt->sinks = ns;
            nt->n_sinks += n - 2;
        } else if (zf_streq(tok[0], "net")) {
            znet_t *nt;
            int j;
            if (n < 4) zf_fatal_at(in, ln, "net NAME SOURCE SINK...");
            if (!n_net) route_init(db);
            GROW(nets, n_net, cap_net, znet_t);
            nt = &nets[n_net++];
            nt->name = zf_strdup(tok[1]);
            nt->line = ln;
            if (!route_parse_wire(tok[2], &nt->src))
                zf_fatal_at(in, ln, "'%s' is not a wire (R<row>C<col>/<name>)", tok[2]);
            nt->n_sinks = n - 3;
            nt->sinks = zf_alloc(sizeof(gwire_t) * (size_t)nt->n_sinks);
            for (j = 0; j < nt->n_sinks; j++)
                if (!route_parse_wire(tok[3 + j], &nt->sinks[j]))
                    zf_fatal_at(in, ln, "'%s' is not a wire (R<row>C<col>/<name>)", tok[3 + j]);
        } else {
            zf_fatal_at(in, ln, "unknown statement '%s'", tok[0]);
        }
    }
    zf_reader_close(r);
    if (!device) zf_fatal("%s: no device line", in);

    /* Two cells in one site would silently merge their settings. */
    for (i = 0; i < n_comb; i++) {
        int j;
        for (j = 0; j < i; j++)
            if (combs[j].tile == combs[i].tile && combs[j].slice == combs[i].slice &&
                    combs[j].lc == combs[i].lc)
                zf_fatal_at(in, combs[i].line, "LUT site already used on line %d", combs[j].line);
    }
    for (i = 0; i < n_ff; i++) {
        int j;
        for (j = 0; j < i; j++)
            if (ffs[j].tile == ffs[i].tile && ffs[j].slice == ffs[i].slice &&
                    ffs[j].lc == ffs[i].lc)
                zf_fatal_at(in, ffs[i].line, "FF site already used on line %d", ffs[j].line);
    }

    if (n_net) {
        uint32_t expanded = 0;
        int k;
        if (arcs_explicit)
            zf_fatal("%s: a netlist gives arcs or declares nets, not both", in);
        rnet_t *rn = zf_alloc(sizeof(rnet_t) * (size_t)n_net);
        route_stats_t st;
        int bad, sink;
        for (i = 0; i < n_net; i++) {
            rn[i].src = nets[i].src;
            rn[i].sinks = nets[i].sinks;
            rn[i].n_sinks = nets[i].n_sinks;
        }
        bad = route_all(rn, n_net, routed_arc, &st, &sink);
        if (bad == -2)
            zf_fatal("%s: congestion did not resolve in %u iterations (%u wires still shared)",
                in, st.iterations, st.overused);
        if (bad >= 0 && sink < 0)
            zf_fatal_at(in, nets[bad].line, "%s is already a terminal of another net",
                route_wire_str(&nets[bad].src));
        if (bad >= 0)
            zf_fatal_at(in, nets[bad].line, "cannot route net %s to %s", nets[bad].name,
                route_wire_str(&nets[bad].sinks[sink]));
        zf_note("routed %d nets: %u arcs, %u iterations, %u reroutes, %u wires searched",
            n_net, st.arcs, st.iterations, st.reroutes, st.expanded);
        (void)expanded; (void)k;

        /* Read each flip-flop's clock and reset wire back from the arc
         * the router chose into its slice's MUXCLKn / MUXLSRn. */
        for (i = 0; i < n_ff; i++) {
            ff_t *f = &ffs[i];
            int pass;
            for (pass = 0; pass < 2; pass++) {
                int *w = pass ? &f->lsrw : &f->clkw;
                char mux[16];
                int j, found = 0;
                if (*w != -2) continue;
                zf_fmt(mux, sizeof(mux), pass ? "MUXLSR%d" : "MUXCLK%d", f->slice);
                for (j = 0; j < n_arc; j++) {
                    if (arcs[j].tile != f->tile || !zf_streq(arcs[j].sink, mux)) continue;
                    if (zf_streq(arcs[j].source, pass ? "LSR0" : "CLK0")) *w = 0;
                    else if (zf_streq(arcs[j].source, pass ? "LSR1" : "CLK1")) *w = 1;
                    else zf_fatal_at(in, f->line, "%s was routed from %s", mux, arcs[j].source);
                    found = 1;
                }
                if (!found)
                    zf_fatal_at(in, f->line, "no net was routed to %s in %s", mux,
                        ZDB_STR(db, db->tile[f->tile].name));
            }
        }
    }

    /* nextpnr's order: baseline, routing, banks, cells. Only the order
     * of overlapping settings could matter, and there are none, but
     * keeping the order keeps the output diffable against nextpnr's. */
    load_baseline();
    for (i = 0; i < n_arc; i++) add(arcs[i].tile, E_ARC, arcs[i].sink, arcs[i].source);
    gen_banks();
    for (i = 0; i < n_comb; i++) gen_comb(&combs[i]);
    for (i = 0; i < n_ff; i++) gen_ff(&ffs[i]);
    for (i = 0; i < n_io; i++) gen_io(&ios[i]);

    emit(out, device, package, speed);
    return 0;
}
