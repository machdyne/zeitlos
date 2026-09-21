/*
 * zfpga -- bram: replace block RAM contents in a finished design.
 *
 *   zfpga bram soc.cfg -f seed.hex -t bios.hex -o soc2.cfg
 *   zfpga bram soc.bit -f seed.hex -t bios.hex -o soc2.bit
 *   zfpga bram -g seed.hex -w 32 -d 1024 [-s 42]
 *
 * -o is required: the input is never overwritten.
 *
 * ecpbram's job, which this tree's build uses to put the BIOS in the SOC
 * after place-and-route: the SOC's RTL initialises its BIOS RAM from a
 * RANDOM seed file, and the seed's contents are then found in the
 * configuration and replaced by the real BIOS -- so the BIOS changes
 * without re-running nextpnr. On the machine, with a .bit, it is how
 * Zeitlos can rebuild its own bitstream with a new BIOS (docs/zfpga.md
 * sec. 25).
 *
 * The matching is ecpbram's exactly (Project Trellis 1.4,
 * libtrellis/tools/ecpbram.cpp), which works on BIT SLICES, not words:
 * each bit column of the seed file, 512 words at a time, is a 512-bit
 * pattern; every block RAM in the design is read as 512-bit slices in
 * each of its six width configurations (1, 2, 4, 9, 18, 36 bits), and a
 * slice equal to a seed pattern is replaced by the matching slice of the
 * new file. That is why the seed must be random: its slices must be
 * unique, and recognisable however synthesis laid the memory out.
 *
 * A .cfg keeps every line but the .bram_init data exactly as it was.
 * (ecpbram re-serialises the whole configuration, and libtrellis then
 * writes each .tile_group with its last tile twice -- harmless, and not
 * copied here; tests/run.sh checks that the two pack to the same bits.)
 * A .bit is unpacked, patched and packed again with the options it was
 * packed with (sec. 18), so it boots from the same address as before.
 */

#include "zfpga.h"

#define EBR_WORDS 2048          /* .bram_init values per block RAM, 9 bits each */
#define SLICE 512

/* -- hex files, as ecpbram reads them ------------------------------------ */

typedef struct {
    uint64_t *w;                /* words, bit 0 the last digit's low bit */
    int n, cap, bits;
} hexfile_t;

static void hex_push(hexfile_t *h, const int *digits, int nd, const char *path, int line) {
    uint64_t v = 0;
    int i;
    if (!nd) return;
    if (nd > 16) zf_fatal_at(path, line, "a word of %d digits: at most 64 bits", nd);
    for (i = 0; i < nd; i++) v = v << 4 | (uint64_t)digits[i];
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 1024;
        uint64_t *nw = zf_alloc(sizeof(uint64_t) * (size_t)cap);
        if (h->n) zf_memcpy(nw, h->w, sizeof(uint64_t) * (size_t)h->n);
        h->w = nw;
        h->cap = cap;
    }
    /* ecpbram: the first word's width is the file's; a narrower later word
     * is padded, a wider one is an inconsistency */
    if (!h->n) h->bits = nd * 4;
    else if (nd * 4 > h->bits) zf_fatal_at(path, line, "inconsistent word width");
    h->w[h->n++] = v;
}

static void hex_read(const char *path, hexfile_t *h) {
    zf_reader_t *r = zf_alloc(sizeof(*r));
    char *line;
    int address = 0;
    zf_memset(h, 0, sizeof(*h));
    zf_reader_open(r, path);
    while ((line = zf_reader_line(r)) != NULL) {
        int digits[17], nd = 0, reading_address = 0;
        const char *p;
        for (p = line;; p++) {
            char c = *p;
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
            else if (c == 'x' || c == 'X' || c == 'z' || c == 'Z') d = 0;
            if (d >= 0) {
                if (nd == 17) zf_fatal_at(path, r->lineno, "a word of more than 16 digits: at most 64 bits");
                digits[nd++] = d;
                continue;
            }
            if (c == '_') continue;
            if (c == '@') {
                if (reading_address || nd) zf_fatal_at(path, r->lineno, "can't parse this line");
                reading_address = 1;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == 0) {
                if (reading_address) {
                    int a = 0, i;
                    for (i = 0; i < nd; i++) a = a << 4 | digits[i];
                    if (a != address)
                        zf_fatal_at(path, r->lineno, "non-contiguous address (expected @%X)", (unsigned)address);
                } else if (nd) {
                    hex_push(h, digits, nd, path, r->lineno);
                    address++;
                }
                nd = 0;
                reading_address = 0;
                if (!c) break;
                continue;
            }
            zf_fatal_at(path, r->lineno, "can't parse this line");
        }
    }
    zf_reader_close(r);
}

/* -- the patterns: 512-bit slices, from -> to ---------------------------- */

typedef struct { uint64_t from[8], to[8]; int used; } pat_t;

static pat_t *pats;
static int n_pats;
static int32_t *phash;
static uint32_t phash_cap;

static uint32_t pkey(const uint64_t *s) {
    uint64_t k = s[0] ^ (s[1] * 0x9E3779B97F4A7C15ull) ^ (s[7] >> 7);
    return (uint32_t)(k ^ (k >> 29));
}

static pat_t *pat_find(const uint64_t *s) {
    uint32_t i;
    for (i = pkey(s) & (phash_cap - 1); phash[i] >= 0; i = (i + 1) & (phash_cap - 1)) {
        pat_t *p = &pats[phash[i]];
        if (!zf_memcmp(p->from, s, sizeof(p->from))) return p;
    }
    return NULL;
}

static void build_patterns(const hexfile_t *f, const hexfile_t *t, const char *fpath) {
    int i, c, k, chunks = f->n / SLICE;
    pats = zf_alloc(sizeof(pat_t) * (size_t)(f->bits * chunks + 1));
    phash_cap = 16;
    while (phash_cap < (uint32_t)(f->bits * chunks) * 2) phash_cap <<= 1;
    phash = zf_alloc(sizeof(int32_t) * phash_cap);
    for (i = 0; i < (int)phash_cap; i++) phash[i] = -1;
    for (i = 0; i < f->bits; i++)
        for (c = 0; c < chunks; c++) {
            pat_t *p = &pats[n_pats];
            uint32_t h;
            zf_memset(p, 0, sizeof(*p));
            for (k = 0; k < SLICE; k++) {
                if ((f->w[c * SLICE + k] >> i) & 1) p->from[k >> 6] |= 1ull << (k & 63);
                if ((t->w[c * SLICE + k] >> i) & 1) p->to[k >> 6] |= 1ull << (k & 63);
            }
            if (pat_find(p->from))
                zf_fatal("%s: bit %d of words %d-%d repeats an earlier slice: the seed must be random "
                    "(zfpga bram -g)", fpath, i, c * SLICE, c * SLICE + SLICE - 1);
            for (h = pkey(p->from) & (phash_cap - 1); phash[h] >= 0; h = (h + 1) & (phash_cap - 1)) ;
            phash[h] = n_pats++;
        }
}

/* -- one block RAM's 2048 nine-bit values, patched ----------------------- */

static int replaced;

static void patch_ebr(uint16_t *d) {
    static const int W[] = { 1, 2, 4, 9, 18, 36 };
    static const int NW[] = { 8, 8, 8, 9, 9, 9 };
    static const int B[] = { 32, 32, 32, 36, 36, 36 };
    int i, j, k;
    for (i = 0; i < 6; i++)
        for (j = 0; j < B[i]; j++) {
            uint64_t s[8];
            pat_t *p;
            zf_memset(s, 0, sizeof(s));
            for (k = 0; k < SLICE; k++) {
                int bn = k * W[i] + j % W[i] + (j / W[i]) * SLICE * W[i];
                if ((d[bn / NW[i]] >> (bn % NW[i])) & 1) s[k >> 6] |= 1ull << (k & 63);
            }
            p = pat_find(s);
            if (!p) continue;
            for (k = 0; k < SLICE; k++) {
                int bn = k * W[i] + j % W[i] + (j / W[i]) * SLICE * W[i];
                int word = bn / NW[i], bit = bn % NW[i];
                if ((p->to[k >> 6] >> (k & 63)) & 1) d[word] = (uint16_t)(d[word] | (1u << bit));
                else d[word] = (uint16_t)(d[word] & ~(1u << bit));
            }
            p->used++;
            replaced++;
        }
}

/* -- a .cfg, every other line kept ----------------------------------------- */

static void put(zf_writer_t *w, const char *s) {
    zf_writer_bytes(w, (const uint8_t *)s, (uint32_t)zf_strlen(s));
}

static int hexval(const char *t, uint32_t *v) {
    uint32_t x = 0;
    if (!*t) return 0;
    for (; *t; t++) {
        int d;
        if (*t >= '0' && *t <= '9') d = *t - '0';
        else if (*t >= 'a' && *t <= 'f') d = 10 + *t - 'a';
        else if (*t >= 'A' && *t <= 'F') d = 10 + *t - 'A';
        else return 0;
        x = x << 4 | (uint32_t)d;
    }
    *v = x;
    return 1;
}

static void flush_ebr(zf_writer_t *w, const uint16_t *d, int n) {
    int j;
    char v[8];
    for (j = 0; j < n; j++) {
        zf_fmt(v, sizeof(v), "%03x%c", d[j], j % 8 == 7 ? '\n' : ' ');
        put(w, v);
    }
    if (n % 8) put(w, "\n");
}

static int n_ebrs;

static void patch_cfg(const char *in, const char *out) {
    zf_reader_t *r = zf_alloc(sizeof(*r));
    zf_writer_t *w = zf_alloc(sizeof(*w));
    uint16_t *d = zf_alloc(sizeof(uint16_t) * EBR_WORDS);
    char *line;
    int in_ebr = 0, n = 0, start = 0;
    zf_reader_open(r, in);
    zf_writer_open(w, out);
    while ((line = zf_reader_line(r)) != NULL) {
        char copy[1024], *tok[16];
        int nt, i;
        zf_fmt(copy, sizeof(copy), "%s", line);
        nt = zf_tokens(copy, tok, 16);
        if (in_ebr) {
            if (nt && tok[0][0] != '.') {
                for (i = 0; i < nt; i++) {
                    uint32_t v;
                    if (n == EBR_WORDS) zf_fatal_at(in, r->lineno, "more than %d values in a .bram_init", EBR_WORDS);
                    if (!hexval(tok[i], &v) || v > 0x1ff)
                        zf_fatal_at(in, r->lineno, "'%s' is not a 9-bit hex value", tok[i]);
                    d[n++] = (uint16_t)v;
                }
                continue;
            }
            /* the data ends: patched, then written as the same layout */
            if (n != EBR_WORDS)
                zf_fatal_at(in, start, ".bram_init has %d values, not %d", n, EBR_WORDS);
            patch_ebr(d);
            flush_ebr(w, d, n);
            in_ebr = 0;
            n_ebrs++;
        }
        if (nt >= 1 && zf_streq(tok[0], ".bram_init")) {
            in_ebr = 1;
            n = 0;
            start = r->lineno;
            zf_memset(d, 0, sizeof(uint16_t) * EBR_WORDS);
        }
        put(w, line);
        put(w, "\n");
    }
    if (in_ebr) {
        if (n != EBR_WORDS) zf_fatal_at(in, start, ".bram_init has %d values, not %d", n, EBR_WORDS);
        patch_ebr(d);
        flush_ebr(w, d, n);
        n_ebrs++;
    }
    zf_reader_close(r);
    zf_writer_close(w);
}

/* -- -g: a random seed, ecpbram's generator ---------------------------------- */

static uint64_t x64;
static uint64_t xorshift64star(void) {
    x64 ^= x64 >> 12;
    x64 ^= x64 << 25;
    x64 ^= x64 >> 27;
    return x64 * 2685821657736338717ull;
}

static int generate(const char *out, uint32_t width, uint32_t depth, int have_seed, uint32_t seed) {
    zf_writer_t *w = zf_alloc(sizeof(*w));
    uint32_t i, j;
    char line[40];
    if (!width || width % 4 || width > 64) zf_fatal("bram -g: width %u: a multiple of 4, at most 64", width);
    if (!depth || depth % SLICE) zf_fatal("bram -g: depth %u: a multiple of %d", depth, SLICE);
    /* ecpbram's seeding: with -s the same file ecpbram makes; without,
     * the time mixed in, as it mixes its pid and the time */
    if (!have_seed) seed = 0;
    x64 = (uint64_t)seed << 32;
    x64 ^= (uint64_t)seed << 20;
    x64 ^= (uint64_t)seed;
    x64 ^= (uint64_t)depth << 16;
    x64 ^= (uint64_t)width << 10;
    xorshift64star(); xorshift64star(); xorshift64star();
    if (!have_seed) {
        uint32_t t = zio_ms();
        x64 ^= (uint64_t)(t / 1000u) << 20;
        x64 ^= (uint64_t)(t % 1000u) * 1000u;
    }
    xorshift64star(); xorshift64star(); xorshift64star();
    zf_writer_open(w, out);
    for (i = 0; i < depth; i++) {
        for (j = 0; j < width / 4; j++) line[j] = "0123456789abcdef"[xorshift64star() & 15];
        line[j++] = '\n';
        zf_writer_bytes(w, (const uint8_t *)line, j);
    }
    zf_writer_close(w);
    return 0;
}

/* -- the command -------------------------------------------------------------- */

static int ends(const char *s, const char *e) {
    size_t n = zf_strlen(s), k = zf_strlen(e);
    return n > k && zf_streq(s + n - k, e);
}

static char *with_ext(const char *in, const char *ext) {
    size_t n = zf_strlen(in), dot = n, j;
    char *o;
    for (j = n; j > 0; j--) { if (in[j - 1] == '/') break; if (in[j - 1] == '.') { dot = j - 1; break; } }
    o = zf_alloc(dot + zf_strlen(ext) + 1);
    zf_memcpy(o, in, dot);
    zf_memcpy(o + dot, ext, zf_strlen(ext) + 1);
    return o;
}

int cmd_bram(int argc, char **argv, const char *dbdir_default) {
    const char *in = NULL, *out = NULL, *from = NULL, *to = NULL, *gen = NULL, *dbdir = dbdir_default;
    uint32_t width = 0, depth = 0, seed = 0;
    int have_seed = 0, i, unused = 0;
    hexfile_t *fh = zf_alloc(sizeof(*fh)), *th = zf_alloc(sizeof(*th));

    for (i = 1; i < argc; i++) {
        if (zf_streq(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (zf_streq(argv[i], "-f") && i + 1 < argc) from = argv[++i];
        else if (zf_streq(argv[i], "-t") && i + 1 < argc) to = argv[++i];
        else if (zf_streq(argv[i], "-g") && i + 1 < argc) gen = argv[++i];
        else if (zf_streq(argv[i], "-w") && i + 1 < argc) { if (!zf_parse_uint(argv[++i], &width)) zf_fatal("-w: a number"); }
        else if (zf_streq(argv[i], "-d") && i + 1 < argc) { if (!zf_parse_uint(argv[++i], &depth)) zf_fatal("-d: a number"); }
        else if (zf_streq(argv[i], "-D") && i + 1 < argc) dbdir = argv[++i];
        else if (zf_streq(argv[i], "-s") && i + 1 < argc) {
            if (!zf_parse_uint(argv[++i], &seed)) zf_fatal("-s: a number");
            have_seed = 1;
        }
        else if (argv[i][0] == '-') zf_fatal("bram: unknown option %s", argv[i]);
        else if (!in) in = argv[i];
        else zf_fatal("bram: more than one input");
    }
    if (gen) return generate(gen, width, depth, have_seed, seed);
    if (!in || !from || !to || !out)
        zf_fatal("usage: zfpga bram IN.{cfg,bit} -f FROM.hex -t TO.hex -o OUT [-D DBDIR]\n"
            "       zfpga bram -g OUT.hex -w WIDTH -d DEPTH [-s SEED]");

    hex_read(from, fh);
    hex_read(to, th);
    /* ecpbram: a short new file is padded with zeros to the seed's depth */
    while (th->n && th->n < fh->n) {
        int zero = 0;
        hex_push(th, &zero, 1, to, 0);
    }
    if (fh->n != th->n) zf_fatal("%s has %d words, %s has %d", from, fh->n, to, th->n);
    if (!fh->n || fh->n % SLICE) zf_fatal("%s: %d words, not a multiple of %d", from, fh->n, SLICE);
    build_patterns(fh, th, from);

    if (ends(in, ".bit")) {
        /* unpack, patch, pack with the options it was packed with */
        const char *t1 = with_ext(out, ".b1"), *t2 = with_ext(out, ".b2");
        char *av[24], opts[160], *tok[16];
        int n = 0, nt, k;
        zf_mark_t mark;
        mark = zf_mark();
        av[n++] = "unpack"; av[n++] = (char *)in; av[n++] = "-o"; av[n++] = (char *)t1;
        av[n++] = "-D"; av[n++] = (char *)dbdir;
        cmd_unpack(n, av, dbdir);
        zf_fmt(opts, sizeof(opts), "%s", unpack_pack_opts);
        zf_release(mark);
        patch_cfg(t1, t2);
        n = 0;
        av[n++] = "pack"; av[n++] = (char *)t2; av[n++] = "-o"; av[n++] = (char *)out;
        av[n++] = "-D"; av[n++] = (char *)dbdir;
        nt = zf_tokens(opts, tok, 16);
        for (k = 0; k < nt; k++) av[n++] = tok[k];
        mark = zf_mark();
        cmd_pack(n, av);
        zf_release(mark);
        zio_remove(t1);
        zio_remove(t2);
    } else {
        patch_cfg(in, out);
    }
    for (i = 0; i < n_pats; i++) if (!pats[i].used) unused++;
    zf_note("%d block RAMs, %d slices replaced%s", n_ebrs, replaced,
        !replaced ? " -- the seed was not found: is it the file the design was built with?" : "");
    if (replaced && unused)
        zf_note("%d of the seed's %d slices were not found in the design", unused, n_pats);
    return 0;
}
