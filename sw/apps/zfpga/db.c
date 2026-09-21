/*
 * zfpga -- the packed chip database. Format in zfpga.h; producer is
 * tools/mkzdb.py.
 */

#include "zfpga.h"

/* Devices served by another die's database. LFE5U-12F is a 25F die with
 * a different IDCODE: the tilegrids are byte-identical (docs/zfpga.md
 * sec. 2.2), so one .zdb carries both and lists both as variants. */
static const struct { const char *device, *file; } aliases[] = {
    { "LFE5U-12F", "lfe5u25f" },
};

/* The device name, lower case, without its dash: LFE5U-25F is
 * lfe5u25f.zdb. 8.3, because the card's FatFs has no long names --
 * lfe5u-25f.zdb, nine characters, could not be found there at all
 * (docs/zfpga.md sec. 22). */
/* One database per process: `zfpga build` runs four stages, three of
 * which want it, and it is 1.5MB. */
static zdb_t cached;
static char cached_path[256];

/* Called by zf_release() for every block it frees. If that block held
 * the cached database, the cache is forgotten: `zfpga bram` on a .bit
 * released the memory of an unpack that had loaded the database, and the
 * pack after it read the freed copy (sec. 25). */
void zdb_forget_if(const void *block) {
    if (cached.base && (const void *)cached.base == block) cached.base = NULL;
}

const char *zdb_file_for(const char *device) {
    static char buf[64];
    unsigned i, n = 0;
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (zf_streq(device, aliases[i].device)) return aliases[i].file;
    for (i = 0; device[i] && n < sizeof(buf) - 1; i++) {
        char c = device[i];
        if (c == '-') continue;
        buf[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[n] = 0;
    return buf;
}

static void read_exact(zio_file_t *f, uint8_t *p, uint32_t n, const char *path) {
    while (n) {
        int r = zio_read(f, p, (int)n);
        if (r <= 0) zf_fatal("%s: truncated", path);
        p += r;
        n -= (uint32_t)r;
    }
}

#define CHECK(cond, what) do { if (!(cond)) \
    zf_fatal("%s: corrupt database (%s)", path, what); } while (0)

void zdb_load(zdb_t *db, const char *dir, const char *device) {
    char path[256];
    zdb_hdr_t h;
    zio_file_t *f;
    uint8_t *mem;
    uint32_t i, j, n_str, n_mux, n_arc, n_word, n_grp, n_enum, n_opt, n_bit;
    int found = 0;

    /* One database per process: `zfpga build` runs four stages, three
     * of which want it, and it is 1.5MB. The cache is loaded before
     * build's first mark, so releasing a stage never frees it. */

    zf_fmt(path, sizeof(path), "%s/%s.zdb", dir, zdb_file_for(device));
    if (cached.base && zf_streq(cached_path, path)) {
        uint32_t v;
        *db = cached;
        for (v = 0; v < db->hdr->n_variants; v++)
            if (zf_streq(ZDB_STR(db, db->var[v].name), device)) {
                db->idcode = db->var[v].idcode;
                db->device = ZDB_STR(db, db->var[v].name);
                return;
            }
        zf_fatal("%s does not serve device %s", path, device);
    }
    zf_memset(db, 0, sizeof(*db));

    f = zio_open_read(path);
    if (!f) zf_fatal("no database for %s (looked for %s%s)", device, path, zf_83_hint(path));

    /* The header says how large the whole file is, so it is read in two
     * steps into one exact allocation rather than grown. */
    read_exact(f, (uint8_t *)&h, sizeof(h), path);
    if (h.magic[0] != 'Z' || h.magic[1] != 'D' || h.magic[2] != 'B' || h.magic[3] != '1')
        zf_fatal("%s: not a zfpga database", path);
    if (h.version != ZDB_VERSION)
        zf_fatal("%s: database version %u, this zfpga reads %u -- rebuild it with tools/mkzdb.py",
            path, h.version, ZDB_VERSION);
    if (h.family != ZDB_FAMILY_ECP5)
        zf_fatal("%s: unsupported family %u", path, h.family);
    CHECK(h.size >= sizeof(h) && h.size < 64u * 1024 * 1024, "size");

    mem = zf_alloc_raw(h.size);
    zf_memcpy(mem, &h, sizeof(h));
    read_exact(f, mem + sizeof(h), h.size - (uint32_t)sizeof(h), path);
    zio_close(f);

    db->base = mem;
    db->size = h.size;
    db->hdr = (const zdb_hdr_t *)mem;

    for (i = 0; i < ZDB_NSECT; i++) {
        static const uint32_t recsize[ZDB_NSECT] = {
            1, sizeof(zdb_var_t), sizeof(zdb_tile_t), sizeof(zdb_type_t),
            sizeof(zdb_mux_t), sizeof(zdb_arc_t), sizeof(zdb_word_t),
            sizeof(zdb_grp_t), sizeof(zdb_enum_t), sizeof(zdb_opt_t), 2,
            sizeof(zdb_pio_t), sizeof(zdb_pin_t), 1, sizeof(zdb_fix_t) };
        const zdb_sect_t *s = &h.sect[i];
        CHECK((s->off & 3) == 0, "alignment");
        CHECK(s->off <= h.size && s->count <= (h.size - s->off) / recsize[i],
            "section bounds");
    }

    db->str  = (const char *)(mem + h.sect[ZDB_S_STR].off);
    db->var  = (const zdb_var_t *)(mem + h.sect[ZDB_S_VAR].off);
    db->tile = (const zdb_tile_t *)(mem + h.sect[ZDB_S_TILE].off);
    db->type = (const zdb_type_t *)(mem + h.sect[ZDB_S_TYPE].off);
    db->mux  = (const zdb_mux_t *)(mem + h.sect[ZDB_S_MUX].off);
    db->arc  = (const zdb_arc_t *)(mem + h.sect[ZDB_S_ARC].off);
    db->word = (const zdb_word_t *)(mem + h.sect[ZDB_S_WORD].off);
    db->grp  = (const zdb_grp_t *)(mem + h.sect[ZDB_S_GRP].off);
    db->en   = (const zdb_enum_t *)(mem + h.sect[ZDB_S_ENUM].off);
    db->opt  = (const zdb_opt_t *)(mem + h.sect[ZDB_S_OPT].off);
    db->bit  = (const uint16_t *)(mem + h.sect[ZDB_S_BIT].off);
    db->pio  = (const zdb_pio_t *)(mem + h.sect[ZDB_S_PIO].off);
    db->pin  = (const zdb_pin_t *)(mem + h.sect[ZDB_S_PIN].off);
    db->fix  = (const zdb_fix_t *)(mem + h.sect[ZDB_S_FIX].off);
    db->baseline = (const char *)(mem + h.sect[ZDB_S_BASE].off);
    db->n_tile = h.sect[ZDB_S_TILE].count;
    db->n_pio = h.sect[ZDB_S_PIO].count;
    db->n_pin = h.sect[ZDB_S_PIN].count;
    db->baseline_len = h.sect[ZDB_S_BASE].count;

    /* Validate every index once, here, so that nothing downstream can
     * read outside the file. A damaged card should produce a refusal,
     * not a bitstream built from whatever the bytes past the end were. */
    n_str  = h.sect[ZDB_S_STR].count;
    n_mux  = h.sect[ZDB_S_MUX].count;
    n_arc  = h.sect[ZDB_S_ARC].count;
    n_word = h.sect[ZDB_S_WORD].count;
    n_grp  = h.sect[ZDB_S_GRP].count;
    n_enum = h.sect[ZDB_S_ENUM].count;
    n_opt  = h.sect[ZDB_S_OPT].count;
    n_bit  = h.sect[ZDB_S_BIT].count;

    CHECK(n_str > 0 && db->str[n_str - 1] == 0, "string table");
#define STR_OK(o) CHECK((o) < n_str, "string offset")
    STR_OK(h.source);
    for (i = 0; i < h.sect[ZDB_S_VAR].count; i++) STR_OK(db->var[i].name);
    for (i = 0; i < db->n_tile; i++) {
        const zdb_tile_t *t = &db->tile[i];
        STR_OK(t->name);
        CHECK(t->type < h.sect[ZDB_S_TYPE].count, "tile type");
        CHECK((uint32_t)t->start_frame + t->n_frames <= h.frames, "tile frames");
        CHECK((uint32_t)t->start_bit + t->n_bits <= h.bits_per_frame, "tile bits");
        if (i) CHECK(zf_strcmp(ZDB_STR(db, db->tile[i - 1].name),
            ZDB_STR(db, t->name)) < 0, "tile order");
    }
    for (i = 0; i < h.sect[ZDB_S_TYPE].count; i++) {
        const zdb_type_t *t = &db->type[i];
        STR_OK(t->name);
        CHECK(t->mux0 <= n_mux && t->n_mux <= n_mux - t->mux0, "type muxes");
        CHECK(t->word0 <= n_word && t->n_word <= n_word - t->word0, "type words");
        CHECK(t->enum0 <= n_enum && t->n_enum <= n_enum - t->enum0, "type enums");
        CHECK(t->fix0 <= h.sect[ZDB_S_FIX].count &&
            t->n_fix <= h.sect[ZDB_S_FIX].count - t->fix0, "type fixed connections");
    }
    for (i = 0; i < n_mux; i++) {
        STR_OK(db->mux[i].sink);
        CHECK(db->mux[i].arc0 <= n_arc && db->mux[i].n_arc <= n_arc - db->mux[i].arc0, "mux arcs");
    }
    for (i = 0; i < n_arc; i++) {
        STR_OK(db->arc[i].source);
        CHECK(db->arc[i].bit0 <= n_bit && db->arc[i].n_bit <= n_bit - db->arc[i].bit0, "arc bits");
    }
    for (i = 0; i < n_word; i++) {
        const zdb_word_t *w = &db->word[i];
        STR_OK(w->name);
        STR_OK(w->defval);
        CHECK(w->grp0 <= n_grp && w->n_grp <= n_grp - w->grp0, "word groups");
        CHECK(zf_strlen(ZDB_STR(db, w->defval)) == w->n_grp, "word default");
    }
    for (i = 0; i < n_grp; i++)
        CHECK(db->grp[i].bit0 <= n_bit && db->grp[i].n_bit <= n_bit - db->grp[i].bit0, "group bits");
    for (i = 0; i < n_enum; i++) {
        const zdb_enum_t *e = &db->en[i];
        STR_OK(e->name);
        CHECK(e->opt0 <= n_opt && e->n_opt <= n_opt - e->opt0, "enum options");
        CHECK(e->defopt >= -1 && e->defopt < (int32_t)e->n_opt, "enum default");
    }
    for (i = 0; i < n_opt; i++) {
        STR_OK(db->opt[i].name);
        CHECK(db->opt[i].bit0 <= n_bit && db->opt[i].n_bit <= n_bit - db->opt[i].bit0, "option bits");
    }
    for (i = 0; i < db->n_pio; i++) {
        const zdb_pio_t *p = &db->pio[i];
        CHECK(p->letter >= 'A' && p->letter <= 'D', "pio letter");
        CHECK(p->pio_tile >= 0 && (uint32_t)p->pio_tile < db->n_tile, "pio tile");
        CHECK(p->pic_tile >= 0 && (uint32_t)p->pic_tile < db->n_tile, "pic tile");
        CHECK(p->tie_tile >= -1 && p->tie_tile < (int32_t)db->n_tile, "tie tile");
        STR_OK(p->tie_enum);
    }
    for (i = 0; i < h.sect[ZDB_S_FIX].count; i++) {
        STR_OK(db->fix[i].sink);
        STR_OK(db->fix[i].source);
    }
    for (i = 0; i < db->n_pin; i++) {
        STR_OK(db->pin[i].package);
        STR_OK(db->pin[i].pin);
        CHECK(db->pin[i].pio < db->n_pio, "pin pio");
    }

    /* Whether a bit lies inside the tile it is applied to depends on the
     * tile, so that is checked where it is applied (chip.c). */
    (void)j;
#undef STR_OK

    for (i = 0; i < h.n_variants; i++) {
        if (zf_streq(ZDB_STR(db, db->var[i].name), device)) {
            db->idcode = db->var[i].idcode;
            db->device = ZDB_STR(db, db->var[i].name);
            found = 1;
        }
    }
    if (!found) zf_fatal("%s does not serve device %s", path, device);
    cached = *db;
    zf_fmt(cached_path, sizeof(cached_path), "%s", path);
}

/* -- lookups -----------------------------------------------------------
 * Binary searches over name-sorted runs. The comparison is bytewise
 * unsigned, matching mkzdb.py's sort. */

#define BSEARCH(base, count, field, key, result) do {                   \
    int32_t lo = 0, hi = (int32_t)(count) - 1;                           \
    result = -1;                                                         \
    while (lo <= hi) {                                                   \
        int32_t mid = (lo + hi) / 2;                                     \
        int c = zf_strcmp(ZDB_STR(db, (base)[mid].field), key);          \
        if (c == 0) { result = mid; break; }                             \
        if (c < 0) lo = mid + 1; else hi = mid - 1;                      \
    }                                                                    \
} while (0)

int32_t zdb_find_tile(const zdb_t *db, const char *name) {
    int32_t r;
    BSEARCH(db->tile, db->n_tile, name, name, r);
    return r;
}

int32_t zdb_find_tile_by_type(const zdb_t *db, const char *type, int *count) {
    uint32_t i;
    int32_t first = -1;
    int n = 0;
    for (i = 0; i < db->n_tile; i++) {
        if (zf_streq(ZDB_STR(db, db->type[db->tile[i].type].name), type)) {
            if (first < 0) first = (int32_t)i;
            n++;
        }
    }
    if (count) *count = n;
    return first;
}

const zdb_mux_t *zdb_find_mux(const zdb_t *db, const zdb_type_t *t, const char *sink) {
    int32_t r;
    BSEARCH(db->mux + t->mux0, t->n_mux, sink, sink, r);
    return r < 0 ? NULL : db->mux + t->mux0 + r;
}

const zdb_arc_t *zdb_find_arc(const zdb_t *db, const zdb_mux_t *m, const char *source) {
    int32_t r;
    BSEARCH(db->arc + m->arc0, m->n_arc, source, source, r);
    return r < 0 ? NULL : db->arc + m->arc0 + r;
}

const zdb_word_t *zdb_find_word(const zdb_t *db, const zdb_type_t *t, const char *name) {
    int32_t r;
    BSEARCH(db->word + t->word0, t->n_word, name, name, r);
    return r < 0 ? NULL : db->word + t->word0 + r;
}

const zdb_enum_t *zdb_find_enum(const zdb_t *db, const zdb_type_t *t, const char *name) {
    int32_t r;
    BSEARCH(db->en + t->enum0, t->n_enum, name, name, r);
    return r < 0 ? NULL : db->en + t->enum0 + r;
}

int32_t zdb_find_opt(const zdb_t *db, const zdb_enum_t *e, const char *name) {
    int32_t r;
    BSEARCH(db->opt + e->opt0, e->n_opt, name, name, r);
    return r;
}
