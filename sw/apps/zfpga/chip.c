/*
 * zfpga -- configuration memory, and applying tile configuration to it.
 *
 * The semantics here are libtrellis's (TileBitDatabase::config_to_tile_cram
 * and its helpers), reproduced step for step, because the test is byte
 * identity with ecppack and every difference is a failure:
 *
 *   - an ARC sets its bit group (plain bits to 1, inverted bits to 0) and
 *     clears nothing;
 *   - a WORD writes every bit of every group: set for a 1, cleared for a 0;
 *   - an ENUM sets its option's group and clears nothing; "_NONE_" sets
 *     nothing but still counts as set;
 *   - order within a tile: arcs, then enums whose name starts "BASE_",
 *     then words, then the remaining enums, then unknown bits, each in
 *     the order the .config gave them;
 *   - then DEFAULTS, for every word and enum the tile's config did not
 *     name, in name order: words first, then enums (an enum with no
 *     default is left alone).
 *
 * The defaults are the part that matters and is easy to miss. An early
 * version of docs/zfpga.md said a blank ECP5 is all zeros; it is not,
 * because some tiles (IO in particular) have defaults with set bits, and
 * every tile gets its defaults whether or not the .config mentions it.
 */

#include "zfpga.h"

void chip_init(zf_chip_t *c, zdb_t *db) {
    const zdb_hdr_t *h = db->hdr;
    zf_memset(c, 0, sizeof(*c));
    c->db = db;
    c->frames = h->frames;
    c->bpf = h->bits_per_frame;
    c->pad_after = h->pad_after;
    c->bytes_per_frame = (uint32_t)(h->bits_per_frame + h->pad_before + h->pad_after) / 8u;
    if ((uint32_t)(h->bits_per_frame + h->pad_before + h->pad_after) % 8u)
        zf_fatal("frame of %u bits is not whole bytes", h->bits_per_frame);
    c->cram = zf_alloc(c->frames * c->bytes_per_frame);
    c->seen = zf_alloc(db->n_tile);
    c->usercode = 0;
    c->ctrl0 = 0x40000000u;         /* libtrellis Chip::ctrl0 */
}

/*
 * The CRAM is stored in the byte layout the bitstream wants, so that
 * writing a frame is a copy rather than a loop over its bits. ecppack
 * builds each frame as
 *
 *     out[(B - 1) - (ofs / 8)] |= bit << (ofs % 8),  ofs = j + pad_after
 *
 * and chip_set_bit() simply writes to that position directly.
 */
static inline uint32_t cram_index(const zf_chip_t *c, uint32_t frame, uint32_t bit,
        uint8_t *mask) {
    uint32_t ofs = bit + c->pad_after;
    *mask = (uint8_t)(1u << (ofs & 7));
    return frame * c->bytes_per_frame + (c->bytes_per_frame - 1 - (ofs >> 3));
}

/* The byte and mask of a configuration bit, for code outside this file
 * that has to find one (pack.c's -J). */
uint32_t chip_cram_index(const zf_chip_t *c, uint32_t frame, uint32_t bit, uint8_t *mask) {
    return cram_index(c, frame, bit, mask);
}

void chip_set_bit(zf_chip_t *c, uint32_t frame, uint32_t bit, int v) {
    uint8_t m;
    uint32_t i;
    if (frame >= c->frames || bit >= c->bpf)
        zf_fatal("bit F%uB%u outside the device", frame, bit);
    i = cram_index(c, frame, bit, &m);
    if (v) c->cram[i] |= m; else c->cram[i] &= (uint8_t)~m;
}

int chip_get_bit(const zf_chip_t *c, uint32_t frame, uint32_t bit) {
    uint8_t m;
    uint32_t i = cram_index(c, frame, bit, &m);
    return (c->cram[i] & m) != 0;
}

/* -- tile-relative helpers -------------------------------------------- */

/* When non-NULL, tile writes go to this scratch image of one tile
 * (n_frames x n_bits, one byte per bit) instead of the CRAM. Used once
 * per tile type to work out what its defaults produce; see
 * chip_default_unseen(). */
static uint8_t *scratch;

static void tile_bit(zf_chip_t *c, const zdb_tile_t *t, uint32_t f, uint32_t b, int v) {
    if (f >= t->n_frames || b >= t->n_bits)
        zf_fatal("database bit F%uB%u outside tile %s (%ux%u)", f, b,
            ZDB_STR(c->db, t->name), t->n_frames, t->n_bits);
    if (scratch) {
        scratch[f * t->n_bits + b] = (uint8_t)v;
        return;
    }
    chip_set_bit(c, t->start_frame + f, t->start_bit + b, v);
}

static void group_set(zf_chip_t *c, const zdb_tile_t *t, uint32_t bit0, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t b = c->db->bit[bit0 + i];
        tile_bit(c, t, ZDB_BIT_FRAME(b), ZDB_BIT_BIT(b), !ZDB_BIT_INV(b));
    }
}

static void group_clear(zf_chip_t *c, const zdb_tile_t *t, uint32_t bit0, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t b = c->db->bit[bit0 + i];
        tile_bit(c, t, ZDB_BIT_FRAME(b), ZDB_BIT_BIT(b), ZDB_BIT_INV(b));
    }
}

/* `value` is MSB first, as written in .config and in the database's
 * default: character k is bit (len - 1 - k). */
static void word_set(zf_chip_t *c, const zdb_tile_t *t, const zdb_word_t *w,
        const char *value) {
    uint32_t i, n = w->n_grp;
    for (i = 0; i < n; i++) {
        const zdb_grp_t *g = &c->db->grp[w->grp0 + i];
        if (value[n - 1 - i] == '1') group_set(c, t, g->bit0, g->n_bit);
        else group_clear(c, t, g->bit0, g->n_bit);
    }
}

static void opt_set(zf_chip_t *c, const zdb_tile_t *t, const zdb_enum_t *e, int32_t opt) {
    const zdb_opt_t *o = &c->db->opt[e->opt0 + (uint32_t)opt];
    group_set(c, t, o->bit0, o->n_bit);
}

/* -- one tile's configuration ------------------------------------------ */

/* Scratch "was this set" flags, sized for the largest type's lists and
 * reused for every tile. */
static uint8_t *found_w, *found_e;
static uint32_t found_cap_w, found_cap_e;

static void found_reset(const zdb_type_t *ty) {
    if (ty->n_word > found_cap_w) {
        found_cap_w = ty->n_word * 2;
        found_w = zf_alloc(found_cap_w);
    }
    if (ty->n_enum > found_cap_e) {
        found_cap_e = ty->n_enum * 2;
        found_e = zf_alloc(found_cap_e);
    }
    zf_memset(found_w, 0, ty->n_word);
    zf_memset(found_e, 0, ty->n_enum);
}

static int is_base(const char *name) {
    return name[0] == 'B' && name[1] == 'A' && name[2] == 'S' &&
        name[3] == 'E' && name[4] == '_';
}

static int valid_bits(const char *v, uint32_t n) {
    uint32_t i;
    for (i = 0; v[i]; i++)
        if (v[i] != '0' && v[i] != '1') return 0;
    return i == n;
}

/*
 * Applies one tile's entries. `tg` is non-zero for a .tile_group, where
 * libtrellis skips names the tile does not have (the group spans tiles of
 * different types), applies no defaults, and reports through `matched`
 * which names were found in at least one tile.
 */
void chip_apply_tile(zf_chip_t *c, uint32_t tix, const zf_tent_t *e, int n,
        const char *file, int tg, uint8_t *matched) {
    const zdb_t *db = c->db;
    const zdb_tile_t *t = &db->tile[tix];
    const zdb_type_t *ty = &db->type[t->type];
    const char *tyname = ZDB_STR(db, ty->name);
    int i, pass;
    uint32_t k;

    found_reset(ty);

    /* 1. arcs. Not skipped in a tile group: libtrellis's muxes.at()
     * throws for a missing sink either way. */
    for (i = 0; i < n; i++) {
        const zdb_mux_t *m;
        const zdb_arc_t *a;
        if (e[i].kind != ZF_T_ARC) continue;
        m = zdb_find_mux(db, ty, e[i].name);
        if (!m) zf_fatal_at(file, e[i].line, "tile %s (%s) has no mux driving %s",
            ZDB_STR(db, t->name), tyname, e[i].name);
        a = zdb_find_arc(db, m, e[i].value);
        if (!a) zf_fatal_at(file, e[i].line, "sink %s in %s has no driver named %s",
            e[i].name, tyname, e[i].value);
        group_set(c, t, a->bit0, a->n_bit);
    }

    /* 2-4. BASE_ enums, then words, then the other enums. */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < n; i++) {
            if (pass == 1) {
                const zdb_word_t *w;
                if (e[i].kind != ZF_T_WORD) continue;
                w = zdb_find_word(db, ty, e[i].name);
                if (!w) {
                    if (tg) continue;
                    zf_fatal_at(file, e[i].line, "tile type %s has no word %s",
                        tyname, e[i].name);
                }
                if (!valid_bits(e[i].value, w->n_grp))
                    zf_fatal_at(file, e[i].line, "word %s wants %u binary digits, got '%s'",
                        e[i].name, w->n_grp, e[i].value);
                if (matched) matched[i] = 1;
                word_set(c, t, w, e[i].value);
                found_w[w - (db->word + ty->word0)] = 1;
            } else {
                const zdb_enum_t *en;
                int32_t o;
                if (e[i].kind != ZF_T_ENUM) continue;
                if ((pass == 0) != is_base(e[i].name)) continue;
                en = zdb_find_enum(db, ty, e[i].name);
                if (!en) {
                    if (tg) continue;
                    zf_fatal_at(file, e[i].line, "tile type %s has no enum %s",
                        tyname, e[i].name);
                }
                o = zdb_find_opt(db, en, e[i].value);
                if (tg && o < 0) continue;
                if (matched) matched[i] = 1;
                if (!zf_streq(e[i].value, "_NONE_")) {
                    if (o < 0) zf_fatal_at(file, e[i].line, "enum %s has no value %s",
                        e[i].name, e[i].value);
                    opt_set(c, t, en, o);
                }
                found_e[en - (db->en + ty->enum0)] = 1;
            }
        }
    }

    /* 5. unknown bits: set to 1. */
    for (i = 0; i < n; i++)
        if (e[i].kind == ZF_T_UNKNOWN)
            tile_bit(c, t, e[i].f, e[i].b, 1);

    if (tg) return;

    /* 6. defaults, in name order: words, then enums. */
    for (k = 0; k < ty->n_word; k++)
        if (!found_w[k]) {
            const zdb_word_t *w = &db->word[ty->word0 + k];
            word_set(c, t, w, ZDB_STR(db, w->defval));
        }
    for (k = 0; k < ty->n_enum; k++)
        if (!found_e[k]) {
            const zdb_enum_t *en = &db->en[ty->enum0 + k];
            if (en->defopt >= 0) opt_set(c, t, en, en->defopt);
        }
}

/*
 * Defaults for every tile no .tile record named -- on a small design,
 * nearly all 4,312 of them.
 *
 * Applying defaults word by word to each tile cost ~95M instructions on
 * the device for an EMPTY 25F (docs/zfpga.md sec. 11), about eight
 * seconds at 12 MIPS. But an unconfigured tile starts at zero (tiles are
 * disjoint, and nothing else writes it) and receives exactly its type's
 * defaults, so every unconfigured tile of a type ends in the same state.
 * That state is computed once per type, into a scratch image, and the
 * few bits it sets are stamped into each tile. The result is identical
 * by construction; tests/run.sh checks it against ecppack regardless.
 *
 * A blank 25F has 188 bits set in total, so the stamp lists are short.
 */
typedef struct {
    int done;
    uint8_t n_frames, n_bits;
    uint16_t *bits;         /* frame << 4 | bit, of the 1s */
    uint32_t n;
} defimg_t;

void chip_default_unseen(zf_chip_t *c) {
    const zdb_t *db = c->db;
    uint32_t n_types = db->hdr->sect[ZDB_S_TYPE].count, i, k;
    defimg_t *img = zf_alloc(sizeof(defimg_t) * n_types);
    uint8_t *buf = zf_alloc(256 * 16);

    for (i = 0; i < db->n_tile; i++) {
        const zdb_tile_t *t = &db->tile[i];
        defimg_t *d = &img[t->type];
        if (c->seen[i]) continue;

        if (!d->done || d->n_frames != t->n_frames || d->n_bits != t->n_bits) {
            uint32_t sz = (uint32_t)t->n_frames * t->n_bits, n = 0, f, b;
            if (d->done || sz > 256 * 16) {
                /* Same type, different shape: never seen on an
                 * ECP5, but the slow path is always correct. */
                chip_apply_tile(c, i, NULL, 0, "", 0, NULL);
                continue;
            }
            zf_memset(buf, 0, sz);
            scratch = buf;
            chip_apply_tile(c, i, NULL, 0, "", 0, NULL);
            scratch = NULL;
            for (k = 0; k < sz; k++) n += buf[k];
            d->bits = zf_alloc(sizeof(uint16_t) * (n ? n : 1));
            d->n = 0;
            for (f = 0; f < t->n_frames; f++)
                for (b = 0; b < t->n_bits; b++)
                    if (buf[f * t->n_bits + b])
                        d->bits[d->n++] = (uint16_t)(f << 4 | b);
            d->n_frames = t->n_frames;
            d->n_bits = t->n_bits;
            d->done = 1;
        }
        for (k = 0; k < d->n; k++)
            chip_set_bit(c, t->start_frame + (d->bits[k] >> 4),
                t->start_bit + (d->bits[k] & 15u), 1);
    }
}
