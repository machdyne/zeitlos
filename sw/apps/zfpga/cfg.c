/*
 * zfpga -- reading a Trellis textual configuration (.config).
 *
 * The format is nextpnr-ecp5's --textcfg output and ecppack's input,
 * documented at https://prjtrellis.readthedocs.io/ ("Textual
 * Configuration Format"). It is read a line at a time and applied a
 * record at a time, so a configuration of any size needs only one
 * record's worth of memory. That is sound because ECP5 tiles are
 * disjoint in CRAM (zfpga.h): configuring them in file order and
 * libtrellis's name order produce the same bits.
 *
 * Refusals, where libtrellis would do something else, are deliberate
 * and are all malformed-input cases nextpnr does not produce:
 *
 *   - a tile configured twice (libtrellis keeps the later one; a
 *     streaming reader cannot un-apply the earlier);
 *   - a .tile after a .tile_group (libtrellis applies every tile before
 *     any group regardless of file order);
 *   - .bram_init with other than 2048 values, or a value over 9 bits;
 *   - a word value of the wrong width.
 *
 * Each is an error that names the line, rather than a bitstream that
 * differs from what ecppack would have made.
 */

#include "zfpga.h"

#define BRAM_WORDS 2048

enum { R_NONE, R_TILE, R_GROUP, R_BRAM, R_OTHER };

typedef struct {
    uint8_t kind;
    uint32_t name, value;   /* offsets into the pool */
    uint32_t f, b;
    int line;
} ent_t;

static struct {
    zf_chip_t *c;
    zdb_t *db;
    const char *path;

    int rec;                /* R_* */
    int rec_line;
    int32_t tile;           /* R_TILE */
    int groups_started;

    ent_t *ents;
    int n_ents, cap_ents;
    char *pool;
    uint32_t pool_len, pool_cap;

    /* R_GROUP: the tile list, as pool offsets */
    uint32_t *gtiles;
    int n_gtiles, cap_gtiles;

    /* R_BRAM */
    uint32_t bram_index;
    uint16_t *bram_vals;
    int n_bram;
} P;

static uint32_t pool_add(const char *s) {
    uint32_t n = (uint32_t)zf_strlen(s) + 1, off;
    if (P.pool_len + n > P.pool_cap) {
        uint32_t cap = P.pool_cap ? P.pool_cap * 2 : 16384;
        char *np;
        while (cap < P.pool_len + n) cap *= 2;
        np = zf_alloc(cap);
        if (P.pool_len) zf_memcpy(np, P.pool, P.pool_len);
        P.pool = np;
        P.pool_cap = cap;
    }
    off = P.pool_len;
    zf_memcpy(P.pool + off, s, n);
    P.pool_len += n;
    return off;
}

static ent_t *ent_new(int line) {
    if (P.n_ents == P.cap_ents) {
        int cap = P.cap_ents ? P.cap_ents * 2 : 256;
        ent_t *ne = zf_alloc(sizeof(ent_t) * (size_t)cap);
        if (P.n_ents) zf_memcpy(ne, P.ents, sizeof(ent_t) * (size_t)P.n_ents);
        P.ents = ne;
        P.cap_ents = cap;
    }
    zf_memset(&P.ents[P.n_ents], 0, sizeof(ent_t));
    P.ents[P.n_ents].line = line;
    return &P.ents[P.n_ents++];
}

/* Converts the collected entries to the pointer form chip.c takes. The
 * pool is stable from here until the record is finished. */
static zf_tent_t *ents_resolve(void) {
    static zf_tent_t *out;
    static int cap;
    int i;
    if (P.n_ents > cap) {
        cap = P.n_ents * 2;
        out = zf_alloc(sizeof(zf_tent_t) * (size_t)cap);
    }
    for (i = 0; i < P.n_ents; i++) {
        out[i].kind = P.ents[i].kind;
        out[i].name = P.pool + P.ents[i].name;
        out[i].value = P.pool + P.ents[i].value;
        out[i].f = P.ents[i].f;
        out[i].b = P.ents[i].b;
        out[i].line = P.ents[i].line;
    }
    return out;
}

static void bram_store(void) {
    zf_chip_t *c = P.c;
    struct zf_bram *b, **pp;
    int a;

    if (P.n_bram != BRAM_WORDS)
        zf_fatal_at(P.path, P.rec_line, ".bram_init %u has %d values, not %d",
            P.bram_index, P.n_bram, BRAM_WORDS);

    /* libtrellis keeps these in a std::map keyed by index: a repeated
     * index replaces, and output is in ascending index order. */
    for (pp = &c->bram; *pp && (*pp)->index < P.bram_index; pp = &(*pp)->next)
        ;
    if (*pp && (*pp)->index == P.bram_index) {
        b = *pp;
    } else {
        b = zf_alloc(sizeof(*b));
        b->index = P.bram_index;
        b->next = *pp;
        *pp = b;
    }

    /* Eight 9-bit words to a 72-bit frame, MSB first -- ecppack's
     * serialise_chip, done now so the words need not be kept. */
    for (a = 0; a < BRAM_WORDS; a += 8) {
        const uint16_t *d = &P.bram_vals[a];
        uint8_t *fr = &b->data[(a / 8) * 9];
        fr[0] = (uint8_t)(d[0] >> 1);
        fr[1] = (uint8_t)((d[0] & 0x01) << 7 | (d[1] >> 2));
        fr[2] = (uint8_t)((d[1] & 0x03) << 6 | (d[2] >> 3));
        fr[3] = (uint8_t)((d[2] & 0x07) << 5 | (d[3] >> 4));
        fr[4] = (uint8_t)((d[3] & 0x0F) << 4 | (d[4] >> 5));
        fr[5] = (uint8_t)((d[4] & 0x1F) << 3 | (d[5] >> 6));
        fr[6] = (uint8_t)((d[5] & 0x3F) << 2 | (d[6] >> 7));
        fr[7] = (uint8_t)((d[6] & 0x7F) << 1 | (d[7] >> 8));
        fr[8] = (uint8_t)d[7];
    }
}

static void group_apply(void) {
    zf_tent_t *e = ents_resolve();
    uint8_t *matched = zf_alloc((size_t)P.n_ents + 1);
    int i;

    for (i = 0; i < P.n_gtiles; i++) {
        const char *tn = P.pool + P.gtiles[i];
        int32_t t = zdb_find_tile(P.db, tn);
        if (t < 0) zf_fatal_at(P.path, P.rec_line, "tile %s does not exist in %s",
            tn, P.db->device);
        chip_apply_tile(P.c, (uint32_t)t, e, P.n_ents, P.path, 1, matched);
    }
    for (i = 0; i < P.n_ents; i++) {
        if ((e[i].kind == ZF_T_WORD || e[i].kind == ZF_T_ENUM) && !matched[i])
            zf_fatal_at(P.path, e[i].line, "config %s %s matched in no tile of the group",
                e[i].kind == ZF_T_WORD ? "word" : "enum", e[i].name);
    }
}

static void record_finish(void) {
    switch (P.rec) {
    case R_TILE:
        chip_apply_tile(P.c, (uint32_t)P.tile, ents_resolve(), P.n_ents,
            P.path, 0, NULL);
        P.c->seen[P.tile] = 1;
        break;
    case R_GROUP:
        group_apply();
        break;
    case R_BRAM:
        bram_store();
        break;
    }
    P.rec = R_NONE;
    P.n_ents = 0;
    P.pool_len = 0;
    P.n_gtiles = 0;
    P.n_bram = 0;
}

static void need_device(int line) {
    if (!P.c->db) zf_fatal_at(P.path, line, ".device must come first");
}

/* Parses the entries on one data line of a .tile or .tile_group.
 * Trellis reads entries as a token stream, so a line may hold several. */
static void tile_entries(char **tok, int n, int line) {
    int i = 0;
    while (i < n) {
        const char *k = tok[i];
        ent_t *e;
        if (zf_streq(k, "arc:") || zf_streq(k, "word:") || zf_streq(k, "enum:")) {
            if (i + 2 >= n) zf_fatal_at(P.path, line, "%s needs two operands", k);
            e = ent_new(line);
            e->kind = (uint8_t)(k[0] == 'a' ? ZF_T_ARC : k[0] == 'w' ? ZF_T_WORD : ZF_T_ENUM);
            e->name = pool_add(tok[i + 1]);
            e->value = pool_add(tok[i + 2]);
            i += 3;
        } else if (zf_streq(k, "unknown:")) {
            const char *s;
            uint32_t f = 0, b = 0;
            if (i + 1 >= n) zf_fatal_at(P.path, line, "unknown: needs a bit");
            s = tok[i + 1];
            if (*s++ != 'F') goto bad;
            if (*s < '0' || *s > '9') goto bad;
            while (*s >= '0' && *s <= '9') f = f * 10 + (uint32_t)(*s++ - '0');
            if (*s++ != 'B') goto bad;
            if (*s < '0' || *s > '9') goto bad;
            while (*s >= '0' && *s <= '9') b = b * 10 + (uint32_t)(*s++ - '0');
            if (*s) goto bad;
            e = ent_new(line);
            e->kind = ZF_T_UNKNOWN;
            e->f = f;
            e->b = b;
            i += 2;
            continue;
bad:
            zf_fatal_at(P.path, line, "bad bit '%s' (want F<frame>B<bit>)", tok[i + 1]);
        } else {
            zf_fatal_at(P.path, line, "unexpected '%s' in a tile record", k);
        }
    }
}

static int hexval(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return 0;
        v = v * 16u + (uint32_t)d;
        if (v > 0xffff) return 0;
    }
    *out = v;
    return 1;
}

static void add_meta(const char *s) {
    zf_chip_t *c = P.c;
    if (c->n_meta == c->max_meta) {
        int cap = c->max_meta ? c->max_meta * 2 : 8;
        char **nm = zf_alloc(sizeof(char *) * (size_t)cap);
        if (c->n_meta) zf_memcpy(nm, c->meta, sizeof(char *) * (size_t)c->n_meta);
        c->meta = nm;
        c->max_meta = cap;
    }
    c->meta[c->n_meta++] = zf_strdup(s);
}

static int starts_verb(const char *line, const char *verb, const char **rest) {
    size_t n = zf_strlen(verb), i;
    while (*line == ' ' || *line == '\t') line++;
    for (i = 0; i < n; i++)
        if (line[i] != verb[i]) return 0;
    if (line[n] && line[n] != ' ' && line[n] != '\t') return 0;
    *rest = line + n;
    return 1;
}

void cfg_load(zf_chip_t *c, zdb_t *db, const char *dbdir, const char *path) {
    zf_reader_t *r = zf_alloc(sizeof(*r));
    char *line;
    char *tok[64];
    const char *rest;
    /* Comments (.comment) may precede .device; they are kept here until
     * the chip exists. */
    char **early = NULL;
    int n_early = 0;

    zf_memset(&P, 0, sizeof(P));
    zf_memset(c, 0, sizeof(*c));
    P.c = c;
    P.db = db;
    P.path = path;
    P.bram_vals = zf_alloc(sizeof(uint16_t) * BRAM_WORDS);

    zf_reader_open(r, path);

    while ((line = zf_reader_line(r)) != NULL) {
        int ln = r->lineno;
        int n;

        /* .comment keeps its text verbatim, so it is recognised before
         * the line is tokenised. libtrellis skips exactly one character
         * after the verb and takes the rest of the line. */
        if (starts_verb(line, ".comment", &rest)) {
            record_finish();
            if (!*rest) zf_fatal_at(path, ln, ".comment with no text");
            rest++;
            if (c->db) {
                add_meta(rest);
            } else {
                char **ne = zf_alloc(sizeof(char *) * (size_t)(n_early + 1));
                if (n_early) zf_memcpy(ne, early, sizeof(char *) * (size_t)n_early);
                ne[n_early++] = zf_strdup(rest);
                early = ne;
            }
            P.rec = R_OTHER;
            continue;
        }

        n = zf_tokens(line, tok, 64);
        if (n == 0) continue;
        if (n == 64) zf_fatal_at(path, ln, "too many tokens on one line");

        if (tok[0][0] == '.') {
            const char *v = tok[0];
            record_finish();
            P.rec_line = ln;

            if (zf_streq(v, ".device")) {
                int i;
                if (n != 2) zf_fatal_at(path, ln, ".device takes one name");
                if (c->db) zf_fatal_at(path, ln, "second .device");
                zdb_load(db, dbdir, tok[1]);
                chip_init(c, db);
                for (i = 0; i < n_early; i++) add_meta(early[i]);
                P.rec = R_OTHER;
            } else if (zf_streq(v, ".variant")) {
                zf_fatal_at(path, ln, "device variants are not supported");
            } else if (zf_streq(v, ".sysconfig")) {
                need_device(ln);
                if (n != 3) zf_fatal_at(path, ln, ".sysconfig takes a key and a value");
                if (zf_streq(tok[1], "MCCLK_FREQ"))
                    c->mcclk_freq = zf_strdup(tok[2]);
                else if (zf_streq(tok[1], "COMPRESS_CONFIG"))
                    c->compress_config = zf_streq(tok[2], "ON");
                /* others are recorded by libtrellis and used by nothing */
                P.rec = R_OTHER;
            } else if (zf_streq(v, ".tile")) {
                need_device(ln);
                if (n != 2) zf_fatal_at(path, ln, ".tile takes one name");
                if (P.groups_started)
                    zf_fatal_at(path, ln, ".tile after .tile_group is not supported");
                P.tile = zdb_find_tile(db, tok[1]);
                if (P.tile < 0)
                    zf_fatal_at(path, ln, "tile %s does not exist in %s", tok[1], db->device);
                if (c->seen[P.tile])
                    zf_fatal_at(path, ln, "tile %s configured twice", tok[1]);
                P.rec = R_TILE;
            } else if (zf_streq(v, ".tile_group")) {
                int i;
                need_device(ln);
                if (!P.groups_started) {
                    /* Every tile is configured before any group,
                     * as libtrellis's to_chip() does. */
                    chip_default_unseen(c);
                    P.groups_started = 1;
                }
                for (i = 1; i < n; i++) {
                    if (P.n_gtiles == P.cap_gtiles) {
                        int cap = P.cap_gtiles ? P.cap_gtiles * 2 : 16;
                        uint32_t *ng = zf_alloc(sizeof(uint32_t) * (size_t)cap);
                        if (P.n_gtiles) zf_memcpy(ng, P.gtiles, sizeof(uint32_t) * (size_t)P.n_gtiles);
                        P.gtiles = ng;
                        P.cap_gtiles = cap;
                    }
                    P.gtiles[P.n_gtiles++] = pool_add(tok[i]);
                }
                P.rec = R_GROUP;
            } else if (zf_streq(v, ".bram_init")) {
                uint32_t idx;
                need_device(ln);
                if (n != 2 || !zf_parse_uint(tok[1], &idx) || (tok[1][0] == '0' && tok[1][1]))
                    zf_fatal_at(path, ln, ".bram_init takes a decimal index");
                P.bram_index = idx;
                P.rec = R_BRAM;
            } else {
                zf_fatal_at(path, ln, "unrecognised config entry %s", v);
            }
            continue;
        }

        switch (P.rec) {
        case R_TILE:
        case R_GROUP:
            tile_entries(tok, n, ln);
            break;
        case R_BRAM: {
            int i;
            for (i = 0; i < n; i++) {
                uint32_t v;
                if (!hexval(tok[i], &v) || v > 0x1ff)
                    zf_fatal_at(path, ln, "bad BRAM value '%s'", tok[i]);
                if (P.n_bram == BRAM_WORDS)
                    zf_fatal_at(path, ln, "more than %d BRAM values", BRAM_WORDS);
                P.bram_vals[P.n_bram++] = (uint16_t)v;
            }
            break;
        }
        default:
            zf_fatal_at(path, ln, "'%s' is not inside a record", tok[0]);
        }
    }
    record_finish();
    zf_reader_close(r);

    if (!c->db) zf_fatal("%s: no .device line", path);
    if (!P.groups_started) chip_default_unseen(c);
}
