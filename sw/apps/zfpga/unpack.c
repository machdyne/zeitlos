/*
 * zfpga -- unpack: a bitstream back to a Trellis .config.
 *
 *   zfpga unpack design.bit [-o design.config] [-D DBDIR]
 *
 * The inverse of pack, and ecpunpack re-expressed: tests/run.sh requires
 * the TEXT to be identical to ecpunpack's (Project Trellis 1.4), which
 * pins down every choice below. With it, a bitstream from anywhere --
 * this machine, a host, a vendor tool -- can be read and edited as text
 * and packed again (docs/zfpga-formats.md).
 *
 * Reading a tile back is libtrellis's tile_cram_to_config(), exactly:
 *
 *   - a mux's driver is the arc whose bits all match, the one with the
 *     MOST bits winning, and a later one (in name order) winning a tie;
 *     an arc with no bits is never printed;
 *   - a word is each group's match, printed only if it is not the
 *     default;
 *   - an enum is the option that matches with the most bits, later
 *     winning ties; printed unless its bit group EQUALS the default's
 *     (a comparison of groups, not of names); "_NONE_" if none matches
 *     but there is a default;
 *   - every set bit no setting accounts for is an `unknown:`. What a
 *     setting accounts for is only the bits it expects to be 1.
 *
 * The bitstream's CRCs are checked, so a damaged file is refused rather
 * than half-read.
 *
 * A bitstream carries things a .config cannot say -- compression, the
 * SPI clock, multiboot, the usercode, the SPI mode -- and ecpunpack drops
 * them. So as not to make an edit-and-repack loop quietly produce a
 * different bitstream, unpack prints the `zfpga pack` command that
 * rebuilds its input byte for byte; tests/run.sh runs it and checks.
 */

#include "zfpga.h"

/* -- a byte stream over the file, with the CRC ------------------------- */

typedef struct {
    zio_file_t *f;
    const char *path;
    uint8_t buf[4096];
    int len, pos;
    uint32_t off;
    uint16_t crc;
    int eof;
} rd_t;

static uint16_t crc_tab[256];

static void crc_init(void) {
    int i, k;
    for (i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (k = 0; k < 8; k++)
            c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1));
        crc_tab[i] = c;
    }
}

static int rd_more(rd_t *r) {
    int n;
    if (r->pos < r->len) return 1;
    if (r->eof) return 0;
    n = zio_read(r->f, r->buf, (int)sizeof(r->buf));
    if (n < 0) zf_fatal("read error on %s", r->path);
    if (n == 0) { r->eof = 1; return 0; }
    r->len = n;
    r->pos = 0;
    return 1;
}

/* no CRC */
static uint8_t raw_byte(rd_t *r) {
    if (!rd_more(r)) zf_fatal("%s: truncated at byte %u", r->path, r->off);
    r->off++;
    return r->buf[r->pos++];
}

static uint8_t get(rd_t *r) {
    uint8_t b = raw_byte(r);
    r->crc = (uint16_t)((r->crc << 8) ^ crc_tab[((r->crc >> 8) ^ b) & 0xff]);
    return b;
}

static void skip(rd_t *r, int n) {
    while (n--) get(r);
}

static uint32_t get32(rd_t *r) {
    uint32_t v = get(r);
    v = v << 8 | get(r);
    v = v << 8 | get(r);
    return v << 8 | get(r);
}

static void check_crc(rd_t *r) {
    uint16_t want = r->crc, got;
    got = (uint16_t)(raw_byte(r) << 8);
    got |= raw_byte(r);
    if (got != want)
        zf_fatal("%s: CRC mismatch at byte %u (computed 0x%04x, file says 0x%04x): damaged",
            r->path, r->off - 2, want, got);
    r->crc = 0;
}

/* -- the ECP5 IDCODEs -> device names --------------------------------- */

static const struct { uint32_t id; const char *name; } ids[] = {
    { 0x21111043, "LFE5U-12F" }, { 0x41111043, "LFE5U-25F" },
    { 0x41112043, "LFE5U-45F" }, { 0x41113043, "LFE5U-85F" },
};

/* -- compressed frames --------------------------------------------------
 * 0 -> 0x00; 100 xxx -> 1 << xxx; 101 xxx -> dictionary[xxx]; 11 + 8
 * bits -> the byte. (libtrellis's own comment on its decoder has 100
 * and 101 the other way round; its code, and its encoder, and ecppack's
 * output, all agree with this.) */

typedef struct { rd_t *r; uint32_t acc; int n; } bitin_t;

static int bit1(bitin_t *b) {
    if (!b->n) { b->acc = get(b->r); b->n = 8; }
    b->n--;
    return (int)(b->acc >> b->n) & 1;
}

static int bits(bitin_t *b, int k) {
    int v = 0;
    while (k--) v = v << 1 | bit1(b);
    return v;
}

static void get_compressed(rd_t *r, uint8_t *out, uint32_t count, const uint8_t *dict) {
    bitin_t b;
    uint32_t i;
    b.r = r; b.acc = 0; b.n = 0;
    for (i = 0; i < count; i++) {
        if (!bit1(&b)) { out[i] = 0; continue; }
        if (bit1(&b)) { out[i] = (uint8_t)bits(&b, 8); continue; }
        if (bit1(&b)) out[i] = dict[bits(&b, 3)];
        else out[i] = (uint8_t)(1u << bits(&b, 3));
    }
    /* anything left in the last byte is padding */
}

/* -- reading a tile back ------------------------------------------------ */

static zf_chip_t *C;
static uint8_t cover[256 * 16];

static int tbit(const zdb_tile_t *t, uint16_t b) {
    return chip_get_bit(C, t->start_frame + ZDB_BIT_FRAME(b), t->start_bit + ZDB_BIT_BIT(b));
}

static int gmatch(const zdb_tile_t *t, uint32_t b0, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t b = C->db->bit[b0 + i];
        if (tbit(t, b) == (int)ZDB_BIT_INV(b)) return 0;
    }
    return 1;
}

/* the bits of a group that are 1 when the group has value v */
static void gcover(const zdb_tile_t *t, uint32_t b0, uint32_t n, int v) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t b = C->db->bit[b0 + i];
        if ((int)ZDB_BIT_INV(b) != v)
            cover[ZDB_BIT_FRAME(b) * t->n_bits + ZDB_BIT_BIT(b)] = 1;
    }
}

static int same_group(uint32_t a0, uint32_t an, uint32_t b0, uint32_t bn) {
    uint32_t i;
    if (an != bn) return 0;
    for (i = 0; i < an; i++)
        if (C->db->bit[a0 + i] != C->db->bit[b0 + i]) return 0;
    return 1;
}

static void put(zf_writer_t *w, const char *s) {
    zf_writer_bytes(w, (const uint8_t *)s, (uint32_t)zf_strlen(s));
}

/* A tile's settings as text, built in a growing buffer. */
static char *tb;
static uint32_t tb_len, tb_cap;

static void tput(const char *s) {
    uint32_t n = (uint32_t)zf_strlen(s);
    if (tb_len + n + 1 > tb_cap) {
        uint32_t cap = tb_cap ? tb_cap * 2 : 4096;
        char *nb;
        while (cap < tb_len + n + 1) cap *= 2;
        nb = zf_alloc(cap);
        if (tb_len) zf_memcpy(nb, tb, tb_len);
        tb = nb;
        tb_cap = cap;
    }
    zf_memcpy(tb + tb_len, s, n + 1);
    tb_len += n;
}

static void tile_text(uint32_t ti) {
    const zdb_t *db = C->db;
    const zdb_tile_t *t = &db->tile[ti];
    const zdb_type_t *ty = &db->type[t->type];
    char line[256];
    uint32_t m, a, k, f, b;
#define EMIT(s) tput(s)

    if ((uint32_t)t->n_frames * t->n_bits > sizeof(cover))
        zf_fatal("tile %s too large", ZDB_STR(db, t->name));
    zf_memset(cover, 0, (size_t)t->n_frames * t->n_bits);

    for (m = 0; m < ty->n_mux; m++) {
        const zdb_mux_t *mx = &db->mux[ty->mux0 + m];
        int32_t best = -1;
        uint32_t bestn = 0;
        for (a = 0; a < mx->n_arc; a++) {
            const zdb_arc_t *ar = &db->arc[mx->arc0 + a];
            if (gmatch(t, ar->bit0, ar->n_bit) && ar->n_bit >= bestn) {
                best = (int32_t)(mx->arc0 + a);
                bestn = ar->n_bit;
            }
        }
        if (best < 0) continue;
        gcover(t, db->arc[best].bit0, db->arc[best].n_bit, 1);
        if (!db->arc[best].n_bit) continue;
        zf_fmt(line, sizeof(line), "arc: %s %s\n", ZDB_STR(db, mx->sink),
            ZDB_STR(db, db->arc[best].source));
        EMIT(line);
    }

    for (k = 0; k < ty->n_word; k++) {
        const zdb_word_t *wd = &db->word[ty->word0 + k];
        const char *def = ZDB_STR(db, wd->defval);
        char val[65];
        uint32_t g, n = wd->n_grp;
        int differs = 0;
        if (n > 64) zf_fatal("word %s wider than 64 bits", ZDB_STR(db, wd->name));
        for (g = 0; g < n; g++) {
            const zdb_grp_t *gr = &db->grp[wd->grp0 + g];
            int v = gmatch(t, gr->bit0, gr->n_bit);
            gcover(t, gr->bit0, gr->n_bit, v);
            val[n - 1 - g] = (char)('0' + v);          /* MSB first */
        }
        val[n] = 0;
        for (g = 0; g < n; g++) if (val[g] != def[g]) differs = 1;
        if (!differs) continue;
        zf_fmt(line, sizeof(line), "word: %s %s\n", ZDB_STR(db, wd->name), val);
        EMIT(line);
    }

    for (k = 0; k < ty->n_enum; k++) {
        const zdb_enum_t *en = &db->en[ty->enum0 + k];
        int32_t best = -1;
        uint32_t bestn = 0, o;
        for (o = 0; o < en->n_opt; o++) {
            const zdb_opt_t *op = &db->opt[en->opt0 + o];
            if (gmatch(t, op->bit0, op->n_bit) && op->n_bit >= bestn) {
                best = (int32_t)o;
                bestn = op->n_bit;
            }
        }
        if (best < 0) {
            if (en->defopt >= 0) {
                zf_fmt(line, sizeof(line), "enum: %s _NONE_\n", ZDB_STR(db, en->name));
                EMIT(line);
            }
            continue;
        }
        {
            const zdb_opt_t *op = &db->opt[en->opt0 + (uint32_t)best];
            gcover(t, op->bit0, op->n_bit, 1);
            if (en->defopt >= 0) {
                const zdb_opt_t *d = &db->opt[en->opt0 + (uint32_t)en->defopt];
                if (same_group(d->bit0, d->n_bit, op->bit0, op->n_bit)) continue;
            }
            zf_fmt(line, sizeof(line), "enum: %s %s\n", ZDB_STR(db, en->name), ZDB_STR(db, op->name));
            EMIT(line);
        }
    }

    for (f = 0; f < t->n_frames; f++)
        for (b = 0; b < t->n_bits; b++)
            if (chip_get_bit(C, t->start_frame + f, t->start_bit + b) && !cover[f * t->n_bits + b]) {
                zf_fmt(line, sizeof(line), "unknown: F%uB%u\n", f, b);
                EMIT(line);
            }
#undef EMIT
}

/*
 * Reading every arc of every mux of every tile back is millions of
 * tests, and a design uses a few dozen tiles of 4,312: the first device
 * run took ~870M instructions for a blinky. A tile's text depends only
 * on its type and its bits, and most tiles repeat one of very few
 * patterns -- all zero, or the type's defaults (IO tiles) -- so each
 * type keeps the last few patterns it has seen with their text, and a
 * tile that matches one reuses it (sec. 18).
 */
#define PCACHE 4
typedef struct { uint16_t *bits; char *text; } pat_t;
static pat_t (*pcache)[PCACHE];
static uint8_t *pcache_n;
static uint16_t pat[256];

/* A tile's bits in frame f, as one word: bit b of the result is tile bit
 * (f, b). The CRAM is in bitstream byte order (chip.c), so bit q of a
 * frame is byte B-1-q/8, bit q%8: the <= 12 bits a tile row holds span at
 * most three bytes, read with a few shifts instead of a call per bit.
 * Getting every tile's pattern bit by bit made this slower than not
 * caching at all. */
static uint32_t frame_bits(uint32_t frame, uint32_t sb, uint32_t nb) {
    uint32_t B = C->bytes_per_frame, q = sb + C->pad_after, i = q >> 3, v = 0, k;
    const uint8_t *fr = C->cram + frame * B;
    for (k = 0; k < 3 && i + k < B; k++) v |= (uint32_t)fr[B - 1 - (i + k)] << (8 * k);
    return (v >> (q & 7)) & ((1u << nb) - 1u);
}

static void tile_pattern(const zdb_tile_t *t) {
    uint32_t f;
    for (f = 0; f < t->n_frames; f++)
        pat[f] = (uint16_t)frame_bits(t->start_frame + f, t->start_bit, t->n_bits);
}

static void tile_out(zf_writer_t *w, uint32_t ti) {
    const zdb_tile_t *t = &C->db->tile[ti];
    uint32_t sz = t->n_frames;
    const char *text = NULL;
    int k;
    if (t->n_bits > 16) zf_fatal("tile %s too large", ZDB_STR(C->db, t->name));
    tile_pattern(t);
    for (k = 0; k < pcache_n[t->type]; k++) {
        const uint16_t *p = pcache[t->type][k].bits;
        uint32_t i;
        for (i = 0; i < sz && p[i] == pat[i]; i++) ;
        if (i == sz) { text = pcache[t->type][k].text; break; }
    }
    if (!text) {
        tb_len = 0;
        tput("");
        tile_text(ti);
        text = tb;
        if (pcache_n[t->type] < PCACHE) {
            pat_t *e = &pcache[t->type][pcache_n[t->type]++];
            e->bits = zf_alloc(sizeof(uint16_t) * sz);
            zf_memcpy(e->bits, pat, sizeof(uint16_t) * sz);
            e->text = zf_strdup(tb);
        }
    }
    if (!*text) return;
    put(w, ".tile ");
    put(w, ZDB_STR(C->db, t->name));
    put(w, "\n");
    put(w, text);
    put(w, "\n");
}

/* -- the command ---------------------------------------------------------- */

#define MAX_META 16
#define MAX_EBR 256

int cmd_unpack(int argc, char **argv, const char *dbdir_default) {
    const char *in = NULL, *out = NULL, *dbdir = dbdir_default;
    rd_t *r = zf_alloc(sizeof(*r));
    zdb_t *db = zf_alloc(sizeof(*db));
    zf_writer_t *w = zf_alloc(sizeof(*w));
    char *meta[MAX_META];
    int n_meta = 0, i, done = 0;
    uint8_t dict[8];
    int have_dict = 0;
    uint16_t *ebr[MAX_EBR];
    int compressed = 0, spi = -1;
    uint32_t ctrl0 = 0x40000000u;
    int ebr_cur = -1, ebr_addr = 0;
    uint32_t t, k;

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (zf_streq(argv[i], "-D") && i + 1 < argc) dbdir = argv[++i];
        else if (argv[i][0] == '-') zf_fatal("unpack: unknown option %s", argv[i]);
        else if (!in) in = argv[i];
        else zf_fatal("unpack: more than one input");
    }
    if (!in) zf_fatal("usage: zfpga unpack IN.bit [-o OUT.cfg] [-D DBDIR]");
    if (!dbdir) zf_fatal("no database directory; give -D DIR");
    if (!out) {
        size_t n = zf_strlen(in), dot = n, j;
        char *o;
        for (j = n; j > 0; j--) { if (in[j - 1] == '/') break; if (in[j - 1] == '.') { dot = j - 1; break; } }
        o = zf_alloc(dot + 5);
        zf_memcpy(o, in, dot);
        zf_memcpy(o + dot, ".cfg", 5);          /* 8.3: .config is too long */
        out = o;
    }
    zf_memset(ebr, 0, sizeof(ebr));
    crc_init();
    r->path = in;
    r->f = zio_open_read(in);
    if (!r->f) zf_fatal("cannot open %s%s", in, zf_83_hint(in));

    /* the metadata header: FF 00, NUL-terminated strings, FF */
    if (raw_byte(r) != 0xFF || raw_byte(r) != 0x00)
        zf_fatal("%s: not an ECP5 .bit (no FF 00 header)", in);
    for (;;) {
        char s[256];
        int n = 0;
        uint8_t c = raw_byte(r);
        if (c == 0xFF) break;
        while (c) {
            if (n < (int)sizeof(s) - 1) s[n++] = (char)c;
            c = raw_byte(r);
        }
        s[n] = 0;
        if (n_meta == MAX_META) zf_fatal("%s: more than %d header strings", in, MAX_META);
        meta[n_meta++] = zf_strdup(s);
    }
    /* the preamble */
    {
        uint32_t win = 0;
        while ((win & 0xffffffffu) != 0xFFFFBDB3u) win = win << 8 | raw_byte(r);
    }

    C = zf_alloc(sizeof(*C));
    while (!done && rd_more(r)) {
        uint8_t op = raw_byte(r);
        if (op == 0xFF) continue;                   /* dummy: no CRC */
        r->crc = (uint16_t)((r->crc << 8) ^ crc_tab[((r->crc >> 8) ^ op) & 0xff]);
        switch (op) {
        case 0x3B:                                  /* LSC_RESET_CRC */
            skip(r, 3);
            r->crc = 0;
            break;
        case 0xE2: {                                /* VERIFY_ID */
            uint32_t id;
            unsigned j;
            skip(r, 3);
            id = get32(r);
            for (j = 0; j < sizeof(ids) / sizeof(ids[0]); j++)
                if (ids[j].id == id) break;
            if (j == sizeof(ids) / sizeof(ids[0]))
                zf_fatal("%s: IDCODE 0x%08x is not an ECP5 zfpga knows", in, id);
            zdb_load(db, dbdir, ids[j].name);
            chip_init(C, db);
            break;
        }
        case 0x22:                                  /* LSC_PROG_CNTRL0 */
            skip(r, 3);
            ctrl0 = get32(r);
            break;
        case 0x23:                                  /* LSC_PROG_CNTRL1 */
        case 0xA2:                                  /* LSC_PROG_SED_CRC */
            skip(r, 3);
            (void)get32(r);
            break;
        case 0x46:                                  /* LSC_INIT_ADDRESS */
        case 0xCE:                                  /* ISC_PROGRAM_SECURITY */
            skip(r, 3);
            break;
        case 0x79:                                  /* SPI_MODE */
            spi = get(r);
            skip(r, 2);
            break;
        case 0x7E:                                  /* JUMP */
            skip(r, 7);
            break;
        case 0x02: {                                /* LSC_WRITE_COMP_DIC */
            int crc = (get(r) & 0x80) != 0, j;
            skip(r, 2);
            for (j = 7; j >= 0; j--) dict[j] = get(r);
            have_dict = 1;
            if (crc) check_crc(r);
            break;
        }
        case 0x82:                                  /* LSC_PROG_INCR_RTI */
        case 0xB8: {                                /* LSC_PROG_INCR_CMP */
            uint8_t p0, p1, p2;
            uint32_t frames, B, BB, fr;
            int crc_each, crc, dummies;
            uint8_t *fb;
            if (!C->db) zf_fatal("%s: frame data before the device ID", in);
            if (op == 0xB8 && !have_dict) zf_fatal("%s: compressed frames before the dictionary", in);
            compressed = op == 0xB8;
            p0 = get(r); p1 = get(r); p2 = get(r);
            crc = (p0 & 0x80) != 0;
            crc_each = crc && !(p0 & 0x40);
            dummies = p0 & 0x0F;
            frames = (uint32_t)p1 << 8 | p2;
            if (frames != C->frames)
                zf_fatal("%s: %u frames, but %s has %u", in, frames, db->device, C->frames);
            B = C->bytes_per_frame;
            BB = op == 0xB8 ? B + (7 - ((B - 1) % 8)) : B;
            fb = zf_alloc(BB);
            for (fr = 0; fr < frames; fr++) {
                uint32_t idx = C->frames - 1 - fr;
                if (op == 0xB8) get_compressed(r, fb, BB, dict);
                else { uint32_t j; for (j = 0; j < B; j++) fb[j] = get(r); }
                /* The CRAM is kept in this byte layout already (chip.c):
                 * the frame is the last B bytes read. */
                zf_memcpy(C->cram + idx * B, fb + (BB - B), B);
                if (crc_each || (crc && fr == frames - 1)) check_crc(r);
                skip(r, dummies);
            }
            break;
        }
        case 0xC2: {                                /* ISC_PROGRAM_USERCODE */
            int crc = (get(r) & 0x80) != 0;
            skip(r, 2);
            C->usercode = get32(r);
            if (crc) check_crc(r);
            r->crc = 0;
            break;
        }
        case 0xF6: {                                /* LSC_EBR_ADDRESS */
            uint32_t d;
            skip(r, 3);
            d = get32(r);
            ebr_cur = (int)((d >> 11) & 0x3FF);
            ebr_addr = (int)(d & 0x7FF);
            if (ebr_cur >= MAX_EBR) zf_fatal("%s: EBR %d out of range", in, ebr_cur);
            if (!ebr[ebr_cur]) ebr[ebr_cur] = zf_alloc(sizeof(uint16_t) * 2048);
            break;
        }
        case 0xB2: {                                /* LSC_EBR_WRITE */
            uint8_t p0 = get(r), p1 = get(r), p2 = get(r);
            int n = p1 << 8 | p2, j;
            if (ebr_cur < 0) zf_fatal("%s: EBR data before an EBR address", in);
            for (j = 0; j < n; j++) {
                uint8_t fr[9];
                uint16_t *e;
                int q;
                if (ebr_addr >= 2048) {
                    ebr_addr = 0;
                    if (++ebr_cur >= MAX_EBR) zf_fatal("%s: EBR out of range", in);
                    if (!ebr[ebr_cur]) ebr[ebr_cur] = zf_alloc(sizeof(uint16_t) * 2048);
                }
                e = ebr[ebr_cur] + ebr_addr;
                for (q = 0; q < 9; q++) fr[q] = get(r);
                e[0] = (uint16_t)(fr[0] << 1 | fr[1] >> 7);
                e[1] = (uint16_t)((fr[1] & 0x7F) << 2 | fr[2] >> 6);
                e[2] = (uint16_t)((fr[2] & 0x3F) << 3 | fr[3] >> 5);
                e[3] = (uint16_t)((fr[3] & 0x1F) << 4 | fr[4] >> 4);
                e[4] = (uint16_t)((fr[4] & 0x0F) << 5 | fr[5] >> 3);
                e[5] = (uint16_t)((fr[5] & 0x07) << 6 | fr[6] >> 2);
                e[6] = (uint16_t)((fr[6] & 0x03) << 7 | fr[7] >> 1);
                e[7] = (uint16_t)((fr[7] & 0x01) << 8 | fr[8]);
                ebr_addr += 8;
            }
            if (p0 & 0x80) check_crc(r);
            break;
        }
        case 0x5E: {                                /* ISC_PROGRAM_DONE */
            int crc = (get(r) & 0x80) != 0;
            skip(r, 2);
            if (crc) check_crc(r);
            done = 1;
            break;
        }
        default:
            zf_fatal("%s: unknown command 0x%02x at byte %u", in, op, r->off - 1);
        }
    }
    zio_close(r->f);
    if (!C->db) zf_fatal("%s: no device ID in the bitstream", in);
    if (!done) zf_note("warning: %s has no PROGRAM DONE", in);

    /* ecpunpack's layout, to the byte */
    zf_writer_open(w, out);
    put(w, ".device ");
    put(w, db->device);
    put(w, "\n\n");
    for (i = 0; i < n_meta; i++) { put(w, ".comment "); put(w, meta[i]); put(w, "\n"); }
    put(w, "\n");
    pcache = zf_alloc(sizeof(*pcache) * db->hdr->sect[ZDB_S_TYPE].count);
    pcache_n = zf_alloc(db->hdr->sect[ZDB_S_TYPE].count);
    for (t = 0; t < db->n_tile; t++) tile_out(w, t);
    for (k = 0; k < MAX_EBR; k++) {
        int j;
        if (!ebr[k]) continue;
        {
            char hdr[32];
            zf_fmt(hdr, sizeof(hdr), ".bram_init %u\n", k);
            put(w, hdr);
        }
        for (j = 0; j < 2048; j++) {
            char v[8];
            zf_fmt(v, sizeof(v), "%03x%c", ebr[k][j], j % 8 == 7 ? '\n' : ' ');
            put(w, v);
        }
        put(w, "\n");
    }
    zf_writer_close(w);

    /* the pack command that rebuilds this bitstream exactly */
    {
        static const struct { uint8_t code; const char *mhz; } fq[] = {
            { 0x01, "4.8" }, { 0x20, "9.7" }, { 0x30, "19.4" }, { 0x38, "38.8" }, { 0x3b, "62.0" },
        };
        static const struct { uint8_t code; const char *name; } sm[] = {
            { 0x49, "fast-read" }, { 0x51, "dual-spi" }, { 0x59, "qspi" },
        };
        char cmd[256];
        int n = zf_fmt(cmd, sizeof(cmd), "zfpga pack %s", out);
        unsigned j;
        if (compressed) n += zf_fmt(cmd + n, (int)sizeof(cmd) - n, " -c");
        for (j = 0; j < sizeof(fq) / sizeof(fq[0]); j++)
            if ((ctrl0 & 0xff) == fq[j].code) n += zf_fmt(cmd + n, (int)sizeof(cmd) - n, " -f %s", fq[j].mhz);
        if (ctrl0 & (1u << 20)) {
            /* multiboot: the address is the BOOTADDR word, which pack
             * writes by value (pack.c), so it is read back by value */
            int32_t tt;
            int cnt;
            uint32_t addr = 0;
            tt = zdb_find_tile_by_type(db, "EFB1_PICB1", &cnt);
            if (tt >= 0) {
                const zdb_tile_t *tl = &db->tile[tt];
                const zdb_word_t *wd = zdb_find_word(db, &db->type[tl->type], "BOOTADDR");
                if (wd) for (j = 0; j < wd->n_grp; j++) {
                    const zdb_grp_t *g = &db->grp[wd->grp0 + j];
                    if (g->n_bit && tbit(tl, db->bit[g->bit0])) addr |= 1u << (16 + j);
                }
            }
            n += zf_fmt(cmd + n, (int)sizeof(cmd) - n, " -a 0x%06x", addr);
        }
        if ((ctrl0 & 0x2E000000u) == 0x2E000000u) n += zf_fmt(cmd + n, (int)sizeof(cmd) - n, " --background");
        if (C->usercode) n += zf_fmt(cmd + n, (int)sizeof(cmd) - n, " -u 0x%08x", C->usercode);
        for (j = 0; j < sizeof(sm) / sizeof(sm[0]); j++)
            if (spi == sm[j].code) n += zf_fmt(cmd + n, (int)sizeof(cmd) - n, " -m %s", sm[j].name);
        zf_note("to rebuild this bitstream exactly: %s", cmd);
    }
    return 0;
}
