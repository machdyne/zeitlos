/*
 * ask -- index reader and query engine. See aidx.h.
 *
 * No window code, no messaging, no printf. Builds on the host against
 * stdio so test/hosttest.c can run it over a pack built by tools/ask.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "aidx.h"

/* Records why loading failed; defined below, used by the file shim
 * above its definition. */
static void ai_note(const char *s);

/* ------------------------------------------------------------------ *
 * The file shim
 * ------------------------------------------------------------------ */

#ifdef ASK_HOST

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *ai_root = "";
void ai_set_root(const char *r) { ai_root = r; }

static void joinroot(char *out, int cap, const char *path)
{
    int n = 0;
    const char *p = ai_root;
    while (*p && n < cap - 1) out[n++] = *p++;
    p = path;
    while (*p && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
}

int ai_open(ai_file_t *f, const char *path)
{
    char full[512];
    joinroot(full, sizeof(full), path);
    FILE *fp = fopen(full, "rb");
    f->fp = fp;
    f->handle = fp ? 1 : -1;
    return fp ? 0 : -1;
}
int ai_read(ai_file_t *f, void *buf, int len)
{ return (int)fread(buf, 1, (size_t)len, (FILE *)f->fp); }
int ai_seek(ai_file_t *f, uint32_t off)
{ return fseek((FILE *)f->fp, (long)off, SEEK_SET) == 0 ? 0 : -1; }
void ai_close(ai_file_t *f)
{ if (f->handle >= 0 && f->fp) fclose((FILE *)f->fp); f->handle = -1; f->fp = 0; }
int ai_size(const char *path)
{
    char full[512]; struct stat st;
    joinroot(full, sizeof(full), path);
    if (stat(full, &st) != 0) return -1;
    return (int)st.st_size;
}
int ai_listdirs(const char *path, char *names, int maxnames)
{
    char full[512];
    joinroot(full, sizeof(full), path);
    DIR *d = opendir(full);
    if (!d) { ai_note("/ask is empty or unreadable"); return 0; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < maxnames) {
        if (e->d_name[0] == '.') continue;
        char sub[600];
        int k = 0;
        while (full[k] && k < 500) { sub[k] = full[k]; k++; }
        sub[k++] = '/';
        const char *s = e->d_name;
        while (*s && k < 590) sub[k++] = *s++;
        sub[k] = 0;
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int j = 0;
        while (e->d_name[j] && j < 12) { names[n * 13 + j] = e->d_name[j]; j++; }
        names[n * 13 + j] = 0;
        n++;
    }
    closedir(d);
    if (!n) ai_note("/ask has no subdirectories");
    return n;
}

#else   /* device */

#include "../../common/zfsapp.h"
#include "../../common/zfs.h"    /* Z_FS_TYPE_DIR */

int ai_open(ai_file_t *f, const char *path)
{
    f->fp = 0;
    f->handle = fs_open_read(path);
    return f->handle < 0 ? -1 : 0;
}
int ai_read(ai_file_t *f, void *buf, int len)
{ return fs_read_chunk(f->handle, buf, len); }
int ai_seek(ai_file_t *f, uint32_t off)
{
    /* fs_seek() is 1 for success, 0 for failure (zfsapp.h). This shim
     * is 0 for success. Returning fs_seek()'s value verbatim made
     * every seek on the device look like a failure while the host's
     * fseek() -- 0 for success -- looked fine, which is why the host
     * test passed and the board returned nothing for every query. */
    return fs_seek(f->handle, off) ? 0 : -1;
}
void ai_close(ai_file_t *f)
{ if (f->handle >= 0) fs_close_handle(f->handle); f->handle = -1; }
int ai_size(const char *path)
{ return fs_size((char *)path); }

int ai_listdirs(const char *path, char *names, int maxnames)
{
    /* fs_list_into() rather than fs_list(): the latter takes two
     * mallocs plus one per entry and hands the caller the job of
     * freeing all of them, which zfsapp.h itself advises against for
     * anything but a one-off.
     *
     * Entries come back as NUL-terminated strings back to back, each
     * already a full "/"-prefixed path. A pack is a subdirectory of
     * /ask and the only thing that lives there, so everything here is
     * a candidate; one whose index.zak is missing or whose dataset id
     * does not match is rejected by load_pack(), which is the right
     * place to decide what makes a pack valid. */
    static char buf[2048];
    static uint8_t types[AI_LIST_MAX];
    uint32_t count = 0, trunc = 0;
    uint32_t i;
    int out = 0;
    const char *p;

    if (!fs_list_into(path, buf, sizeof(buf), types, AI_LIST_MAX,
                      &count, &trunc)) {
        /* An EMPTY directory is not distinguishable from a failure
         * here -- k_fs_list() reports failure for both, as zflist.c
         * records. Either way there is nothing to load. */
        ai_note("/ask is empty or unreadable");
        return 0;
    }

    p = buf;
    for (i = 0; i < count && out < maxnames; i++) {
        /* Entries are full "/"-prefixed paths (zfs.h); a pack is named
         * by its basename. */
        const char *base = p, *s = p;
        int j = 0;
        while (*s) { if (*s == '/') base = s + 1; s++; }

        /* INDEXED BY ENTRY, NOT BY OUTPUT.
         *
         * This read types[out] until now, so the first non-directory
         * entry desynchronised the type array from the names for
         * everything after it -- one stray file in /ask and every pack
         * following it was tested against the wrong type byte. */
        if (types[i] == Z_FS_TYPE_DIR) {
            while (base[j] && j < 12) { names[out * 13 + j] = base[j]; j++; }
            names[out * 13 + j] = 0;
            out++;
        }
        p = s + 1;
    }
    if (!out) ai_note("/ask has no subdirectories");
    return out;
}

#endif

/* ------------------------------------------------------------------ *
 * Why loading failed
 *
 * "no packs in /ask" was the app's whole vocabulary for four quite
 * different situations: nothing there, an index this build cannot
 * read, files from two different distributions mixed together, and --
 * by far the most likely on a first run -- malloc refusing 400KB
 * because sw/os/kernel.h was never told `ask` needs the HUGE tier.
 *
 * Guessing between those over a serial console is exactly the kind of
 * time this costs, so the reason is recorded and shown.
 * ------------------------------------------------------------------ */

static char ai_err[64];

static void ai_note(const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof(ai_err) - 1) { ai_err[i] = s[i]; i++; }
    ai_err[i] = 0;
}

const char *ai_error(void) { return ai_err[0] ? ai_err : "no packs found"; }

/* ------------------------------------------------------------------ *
 * Small helpers -- no stdio, no float
 *
 * sw/apps/read/read.c's header records that adding one "%lu" links the
 * whole newlib formatter back in, which on this toolchain is ~100KB
 * against an app that should be a fraction of that. So: none.
 * ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void scopy(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int scat(char *dst, int cap, int at, const char *src)
{
    while (*src && at < cap - 1) dst[at++] = *src++;
    dst[at] = 0;
    return at;
}

static int ucat(char *dst, int cap, int at, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n && at < cap - 1) dst[at++] = tmp[--n];
    dst[at] = 0;
    return at;
}

/* ------------------------------------------------------------------ *
 * Header validation
 * ------------------------------------------------------------------ */

static bool hdr_ok(const uint8_t *h, uint32_t magic, uint32_t dsid,
                   uint32_t *count, uint32_t *a, uint32_t *b, uint32_t *flags)
{
    uint32_t w[8], i, sum = 0;
    for (i = 0; i < 8; i++) w[i] = rd32(h + i * 4);
    for (i = 0; i < 7; i++) sum += w[i];
    if (w[0] != magic) return false;
    if (w[1] != AI_FORMAT_VERSION) return false;
    if (sum != w[7]) return false;
    /* dsid 0 means "adopt whatever this file says" -- only index.zak
     * gets that, and every other file in the pack is then checked
     * against it. Mixing files from two distributions produces
     * confidently ranked results pointing at the wrong paragraphs,
     * and nothing else in the system would notice. */
    if (dsid && w[6] != dsid) return false;
    if (count) *count = w[3];
    if (a) *a = w[4];
    if (b) *b = w[5];
    if (flags) *flags = w[2];
    return true;
}

/* Reads a whole file into a fresh allocation, past its header.
 * Returns NULL on any failure. */
static void *load_body(const char *path, uint32_t magic, uint32_t dsid,
                       uint32_t *count, uint32_t *a, uint32_t *b,
                       uint32_t *nbytes,
                       void (*progress)(uint32_t, uint32_t, const char *),
                       const char *what)
{
    ai_file_t f;
    uint8_t hdr[AI_HDR_SIZE];
    int total, got;
    uint8_t *buf;

    if (ai_open(&f, path) != 0) { ai_note("missing file in pack"); return NULL; }
    if (ai_read(&f, hdr, AI_HDR_SIZE) != AI_HDR_SIZE) {
        ai_close(&f); ai_note("truncated index file"); return NULL;
    }
    if (!hdr_ok(hdr, magic, dsid, count, a, b, NULL)) {
        ai_close(&f);
        /* Magic and version are a format mismatch; a good header with
         * the wrong dsid means two distributions got mixed on the
         * card, which is the failure worth naming separately because
         * it otherwise produces confident wrong answers. */
        if (rd32(hdr) != magic) ai_note("not an ask index file");
        else if (rd32(hdr + 4) != AI_FORMAT_VERSION)
            ai_note("index built by another version");
        else ai_note("pack files are from different builds");
        return NULL;
    }

    total = ai_size(path);
    if (total <= AI_HDR_SIZE) { ai_close(&f); ai_note("empty index file"); return NULL; }
    total -= AI_HDR_SIZE;

    buf = (uint8_t *)malloc((size_t)total);
    if (!buf) {
        ai_close(&f);
        /* The overwhelmingly likely first-run failure: an app's heap
         * and stack share one allocation, and without the kernel.h
         * entry `ask` gets Z_PROC_STACK_SIZE_DEFAULT's 16KB against a
         * coarse array of several hundred KB. See
         * sw/apps/ask/INTEGRATION.md. */
        ai_note("out of memory -- needs HUGE tier");
        return NULL;
    }

    got = 0;
    while (got < total) {
        /* 8KB at a time so progress moves and the caller's bar is not
         * a single jump from 0 to done on a 500KB file. */
        int want = total - got;
        int n;
        if (want > 8192) want = 8192;
        n = ai_read(&f, buf + got, want);
        if (n <= 0) break;
        got += n;
        if (progress) progress((uint32_t)got, (uint32_t)total, what);
    }
    ai_close(&f);
    if (got != total) { free(buf); ai_note("short read from card"); return NULL; }
    if (nbytes) *nbytes = (uint32_t)total;
    return buf;
}

/* Proves the shim on whatever this is running on. See aidx.h. */
int ai_selftest(const char *path)
{
    ai_file_t f;
    uint8_t a[8], b[8];
    int i;

    if (ai_open(&f, path) != 0) return -1;
    if (ai_read(&f, a, 8) != 8) { ai_close(&f); return -2; }
    /* Read the same eight bytes again, this time by seeking back to
     * the start. A shim whose seek reports failure -- or silently does
     * not move -- differs here and nowhere else. */
    if (ai_seek(&f, 0) != 0) { ai_close(&f); return -3; }
    if (ai_read(&f, b, 8) != 8) { ai_close(&f); return -4; }
    ai_close(&f);
    for (i = 0; i < 8; i++) if (a[i] != b[i]) return -5;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Pack loading
 * ------------------------------------------------------------------ */

static ai_pack_t g_packs[AI_PACKS_MAX];
static int g_npacks;
static int g_failed;

static void path_join(char *out, int cap, const char *dir, const char *name)
{
    int n = scat(out, cap, 0, dir);
    n = scat(out, cap, n, "/");
    scat(out, cap, n, name);
}

static bool load_pack(ai_pack_t *p, const char *askroot, const char *name,
                      void (*progress)(uint32_t, uint32_t, const char *))
{
    char path[64];
    uint8_t *idx;
    uint32_t n, a, b, nb;

    memset(p, 0, sizeof(*p));
    scopy(p->name, sizeof(p->name), name);
    path_join(p->dir, sizeof(p->dir), askroot, name);
    scat(p->cardroot, sizeof(p->cardroot), 0, "/ark/");
    scat(p->cardroot, sizeof(p->cardroot), 5, name);

    /* index.zak first, and with dsid 0 -- it is the file that DEFINES
     * the dataset id every other file is then checked against. */
    path_join(path, sizeof(path), p->dir, "index.zak");

    /* Before trusting anything, prove seek works here. Loading itself
     * is purely sequential, so a broken seek loads a pack perfectly
     * and then returns nothing for every query -- which looks like a
     * retrieval problem rather than an I/O one. */
    {
        int rv = ai_selftest(path);
        if (rv != 0) {
            /* Say WHICH step failed. The first version reported
             * "file seek is broken" for all of them, including a
             * failure to OPEN -- which is what happens when the
             * kernel's handle table is exhausted, and sent a real
             * debugging session after the wrong thing entirely. */
            if (rv == -1) ai_note("cannot open index -- out of file handles?");
            else if (rv == -2 || rv == -4) ai_note("short read from card");
            else if (rv == -3) ai_note("file seek failed");
            else ai_note("seek returned the wrong bytes");
            return false;
        }
    }

    idx = (uint8_t *)load_body(path, AI_MAGIC_INDEX, 0, &n, &a, &b, &nb,
                               NULL, NULL);
    if (!idx) return false;

    {
        ai_file_t f;
        uint8_t hdr[AI_HDR_SIZE];
        if (ai_open(&f, path) != 0) { free(idx); return false; }
        if (ai_read(&f, hdr, AI_HDR_SIZE) != AI_HDR_SIZE) {
            ai_close(&f); free(idx); return false;
        }
        ai_close(&f);
        p->dsid  = rd32(hdr + 24);
        p->flags = rd32(hdr + 8);
    }

    if (nb < 48) { free(idx); return false; }
    p->ndocs      = rd32(idx +  0);
    p->nchunks    = rd32(idx +  4);
    p->coarse_dim = rd32(idx +  8);
    p->fine_dim   = rd32(idx + 12);
    p->nterms     = rd32(idx + 16);
    p->vocab      = rd32(idx + 20);
    p->encoder_id = rd32(idx + 40);
    free(idx);

    if (!p->nchunks || !p->coarse_dim || p->coarse_dim > 256) {
        ai_note("index.zak has impossible dimensions");
        return false;
    }
    if (p->fine_dim > 256) {
        ai_note("index.zak has impossible dimensions");
        return false;
    }

    /* Coarse vectors. OPTIONAL: a pack built with `dense = no`
     * (tools/ask) has none, and is answered by BM25 alone -- which
     * measures BETTER than the fused result with an untrained encoder,
     * and holds nothing resident at all. */
    path_join(path, sizeof(path), p->dir, "coarse.zcv");
    if (ai_size(path) > 0) {
        p->coarse = (int8_t *)load_body(path, AI_MAGIC_COARSE, p->dsid,
                                        &n, &a, &b, &nb, progress, name);
        if (!p->coarse) return false;
        if (n != p->nchunks || a != p->coarse_dim
            || nb < (uint32_t)p->nchunks * p->coarse_dim) {
            free(p->coarse); p->coarse = NULL;
            ai_note("coarse.zcv disagrees with index.zak");
            return false;
        }
        p->resident += nb;
    }

    /* encoder -- used on every query, so also resident */
    if (p->flags & AI_FLAG_HAS_ENCODER) {
        path_join(path, sizeof(path), p->dir, "encoder.zmd");
        uint8_t *e = (uint8_t *)load_body(path, AI_MAGIC_ENCODER, p->dsid,
                                          &n, &a, &b, &nb, progress, name);
        if (e) {
            uint32_t vecbytes = n * a;
            uint32_t off = vecbytes;
            off = (off + 3u) & ~3u;
            if (off + n * 2 + n * 4 <= nb) {
                p->vocab   = n;
                p->enc_vec = (int8_t *)e;
                p->enc_idf = (uint16_t *)(void *)(e + off);
                off += n * 2;
                off = (off + 3u) & ~3u;
                p->enc_off  = (uint32_t *)(void *)(e + off);
                off += n * 4;
                p->enc_pool = (char *)(e + off);
                p->enc_pool_len = nb - off;
                if (a != p->fine_dim) p->fine_dim = a;
                p->resident += nb;
            } else {
                free(e);
            }
        }
    }

    /* Per-chunk token counts, for BM25's length normalisation.
     *
     * Streamed out of chunks.zct's byte-length column at load. Without
     * it BM25 runs with b = 0, and a long chunk that happens to repeat
     * one query term outscores a short one that is actually about the
     * subject -- "how to start a fire" returned the I2C document,
     * which is full of the I2C START condition, ahead of the survival
     * manual. */
    {
        ai_file_t f;
        uint8_t hdr[AI_HDR_SIZE];
        uint32_t nrec = 0, i;
        uint64_t sum = 0;

        path_join(path, sizeof(path), p->dir, "chunks.zct");
        if (ai_open(&f, path) == 0) {
            if (ai_read(&f, hdr, AI_HDR_SIZE) == AI_HDR_SIZE
                && hdr_ok(hdr, AI_MAGIC_CHUNKS, p->dsid, &nrec,
                          NULL, NULL, NULL)
                && nrec == p->nchunks) {
                p->lens = (uint16_t *)malloc(nrec * sizeof(uint16_t));
            }
            if (p->lens) {
                static uint8_t rec[16 * 64];
                i = 0;
                while (i < nrec) {
                    uint32_t want = nrec - i;
                    int got, k;
                    if (want > 64) want = 64;
                    got = ai_read(&f, rec, (int)(want * 16));
                    if (got < (int)(want * 16)) break;
                    for (k = 0; k < (int)want; k++) {
                        uint32_t bytes = rd32(rec + k * 16 + 8);
                        uint32_t tok = bytes / AI_BYTES_PER_TOKEN;
                        if (tok > 65535) tok = 65535;
                        if (!tok) tok = 1;
                        p->lens[i + k] = (uint16_t)tok;
                        sum += tok;
                    }
                    i += want;
                }
                if (i < nrec) { free(p->lens); p->lens = NULL; }
                else {
                    p->avg_len = (uint32_t)(sum / (nrec ? nrec : 1));
                    if (!p->avg_len) p->avg_len = 1;
                    p->resident += nrec * sizeof(uint16_t);
                }
            }
            ai_close(&f);
        }
    }

    /* avg document length for BM25, from lexicon.zlx's header */
    {
        ai_file_t f;
        uint8_t hdr[AI_HDR_SIZE];
        path_join(path, sizeof(path), p->dir, "lexicon.zlx");
        if (ai_open(&f, path) == 0) {
            if (ai_read(&f, hdr, AI_HDR_SIZE) == AI_HDR_SIZE
                && hdr_ok(hdr, AI_MAGIC_LEXICON, p->dsid, &n, &a, &b, NULL)) {
                p->nterms = n;
                p->avg_len_q8 = b;
            }
            ai_close(&f);
        }
    }

    p->ok = true;
    return true;
}

int ai_load_all(const char *askroot,
                void (*progress)(uint32_t, uint32_t, const char *))
{
    char names[AI_PACKS_MAX * 13];
    int n, i;

    ai_free_all();
    g_failed = 0;
    n = ai_listdirs(askroot, names, AI_PACKS_MAX);
    for (i = 0; i < n; i++) {
        if (g_npacks >= AI_PACKS_MAX) break;
        ai_err[0] = 0;
        if (load_pack(&g_packs[g_npacks], askroot, &names[i * 13], progress)) {
            g_npacks++;
        } else {
            if (!ai_err[0]) ai_note("pack rejected");
            g_failed++;
        }
    }
    /* A PARTIAL load keeps its reason. Two packs where three were
     * installed is a corpus that silently shrank, and the app has no
     * other way to say so -- the status line would read "N passages"
     * and look entirely healthy. Running out of memory on the second
     * pack is the realistic case, since each is a fresh allocation of
     * several hundred KB into a first-fit pool. */
    if (g_npacks && !g_failed) ai_err[0] = 0;
    return g_npacks;
}

void ai_free_all(void)
{
    int i;
    for (i = 0; i < g_npacks; i++) {
        if (g_packs[i].coarse)  free(g_packs[i].coarse);
        if (g_packs[i].enc_vec) free(g_packs[i].enc_vec);
        if (g_packs[i].lens)    free(g_packs[i].lens);
        memset(&g_packs[i], 0, sizeof(g_packs[i]));
    }
    g_npacks = 0;
}

int ai_pack_count(void) { return g_npacks; }
int ai_failed_count(void) { return g_failed; }
const ai_pack_t *ai_pack(int i)
{ return (i >= 0 && i < g_npacks) ? &g_packs[i] : NULL; }
uint32_t ai_resident(void)
{
    uint32_t t = 0;
    int i;
    for (i = 0; i < g_npacks; i++) t += g_packs[i].resident;
    return t;
}

/* ------------------------------------------------------------------ *
 * Tokenising -- must match tools/ask/lib/lexicon.py's _TOKEN exactly
 *
 * [A-Za-z0-9_]+ with . and - allowed INSIDE, lowercased, length >= 2,
 * stopwords dropped. Keeping digits and punctuation joined is the
 * whole point: `0x7000_0600`, `rv32im` and `nextpnr-ecp5` have to
 * survive as single terms, because they are exactly the queries the
 * dense half is worst at.
 * ------------------------------------------------------------------ */

static const char *const AI_STOP[] = {
    "a","an","and","are","as","at","be","by","for","from","has","have",
    "how","i","in","is","it","its","of","on","or","that","the","this",
    "to","was","were","what","when","where","which","who","will","with",
    "you","your", 0
};

static bool is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool is_stop(const char *t)
{
    int i;
    for (i = 0; AI_STOP[i]; i++)
        if (!strcmp(t, AI_STOP[i])) return true;
    return false;
}

/* Extracts up to `max` tokens. Returns the count. */
static int tokenize(const char *s, char out[][32], int max)
{
    int n = 0, i = 0;
    while (s[i] && n < max) {
        int j;
        if (!is_word(s[i])) { i++; continue; }
        j = 0;
        while (s[i] && j < 31) {
            if (is_word(s[i])) {
                char c = s[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                out[n][j++] = c;
                i++;
            } else if ((s[i] == '.' || s[i] == '-') && is_word(s[i + 1])) {
                out[n][j++] = s[i++];
            } else break;
        }
        out[n][j] = 0;
        if (j >= 2 && !is_stop(out[n])) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * Query encoding -- the bow encoder, integer only
 *
 * acc[d] += idf(term) * vec[term][d], then renormalise to int8.
 * Matches BowEncoder._pool() in tools/ask/lib/embed.py, with the
 * float multiply replaced by Q8.8 idf and the L2 normalisation done
 * by integer square root.
 * ------------------------------------------------------------------ */

static uint32_t isqrt32(uint32_t v)
{
    uint32_t op = v, res = 0, one = 1uL << 30;
    while (one > op) one >>= 2;
    while (one) {
        if (op >= res + one) { op -= res + one; res += one << 1; }
        res >>= 1;
        one >>= 2;
    }
    return res;
}

static int enc_find(const ai_pack_t *p, const char *term)
{
    int lo = 0, hi = (int)p->vocab - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *s = p->enc_pool + p->enc_off[mid];
        int c = strcmp(term, s);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static uint32_t encode_query(const ai_pack_t *p, const char *text,
                             int8_t *out, uint32_t maxdim)
{
    int32_t acc[256];
    char toks[AI_TERMS_MAX][32];
    int ntok, i, hits = 0;
    uint32_t d, dim = p->fine_dim;
    int64_t sq = 0;
    uint32_t norm;

    if (!p->enc_vec || dim == 0 || dim > maxdim) return 0;
    for (d = 0; d < dim; d++) acc[d] = 0;

    ntok = tokenize(text, toks, AI_TERMS_MAX);
    for (i = 0; i < ntok; i++) {
        int k = enc_find(p, toks[i]);
        if (k < 0) continue;
        {
            const int8_t *v = p->enc_vec + (uint32_t)k * dim;
            int32_t w = (int32_t)p->enc_idf[k];      /* Q8.8 */
            for (d = 0; d < dim; d++) acc[d] += (int32_t)v[d] * w;
            hits++;
        }
    }
    if (!hits) return 0;

    for (d = 0; d < dim; d++) sq += (int64_t)acc[d] * acc[d];
    if (sq <= 0) return 0;
    /* scale so the largest component lands near 127 without overflow:
     * out = acc * 127 / |acc| */
    while (sq > 0x3FFFFFFFLL) {
        for (d = 0; d < dim; d++) acc[d] >>= 1;
        sq = 0;
        for (d = 0; d < dim; d++) sq += (int64_t)acc[d] * acc[d];
    }
    norm = isqrt32((uint32_t)sq);
    if (!norm) return 0;
    for (d = 0; d < dim; d++) {
        /* ROUND TO NEAREST, not truncate.
         *
         * The packer quantises document vectors with numpy's rint()
         * (tools/ask/lib/pack.py). C integer division truncates toward
         * zero, so this produced a query vector consistently one step
         * SMALLER in magnitude than the reference on most components
         * -- 1 where Python had 2, -13 where Python had -14.
         *
         * Each component is off by less than one unit of 127, which
         * looks negligible and is not: the score is a sum over 128 of
         * them, and a systematic bias in the same direction on every
         * component moves the total enough to reorder candidates that
         * are close. It changed the top hit for real queries while
         * leaving most of the ranking intact, which is the hardest
         * kind of wrong to notice.
         *
         * Half added with the sign of the numerator, so rounding is
         * symmetric about zero the way rint() is for these values. */
        int32_t num = acc[d] * 127;
        int32_t half = (int32_t)(norm >> 1);
        int32_t v = (num >= 0) ? (num + half) / (int32_t)norm
                               : (num - half) / (int32_t)norm;
        if (v > 127) v = 127;
        if (v < -127) v = -127;
        out[d] = (int8_t)v;
    }
    return dim;
}

/* ------------------------------------------------------------------ *
 * Shortlist -- a min-heap on score
 * ------------------------------------------------------------------ */

static void heap_push(ai_query_t *q, int32_t score, uint32_t chunk, int pack)
{
    int i;
    if (q->nshort < AI_SHORTLIST) {
        i = q->nshort++;
        q->shortlist[i].score = score;
        q->shortlist[i].chunk = chunk;
        q->shortlist[i].pack = (int8_t)pack;
        while (i > 0) {
            int par = (i - 1) / 2;
            if (q->shortlist[par].score <= q->shortlist[i].score) break;
            { ai_cand_t t = q->shortlist[par];
              q->shortlist[par] = q->shortlist[i]; q->shortlist[i] = t; }
            i = par;
        }
        return;
    }
    if (score <= q->shortlist[0].score) return;
    q->shortlist[0].score = score;
    q->shortlist[0].chunk = chunk;
    q->shortlist[0].pack = (int8_t)pack;
    i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < q->nshort && q->shortlist[l].score < q->shortlist[m].score) m = l;
        if (r < q->nshort && q->shortlist[r].score < q->shortlist[m].score) m = r;
        if (m == i) break;
        { ai_cand_t t = q->shortlist[m];
          q->shortlist[m] = q->shortlist[i]; q->shortlist[i] = t; }
        i = m;
    }
}

/* ------------------------------------------------------------------ *
 * Reading a chunk record and its strings
 * ------------------------------------------------------------------ */

/* Reads a NUL-terminated string out of a pool.
 *
 * ONE BLOCK, then scan in memory. This read a byte at a time, and on
 * the device every one of those is a syscall into FatFs -- roughly 224
 * of them per hit across a heading, a path and a title, times eight
 * hits, which is where 15 to 30 seconds of apparent hang after
 * "searching 100%" came from. On the host the same loop is a buffered
 * fread and costs nothing, so it never showed up in testing.
 *
 * A pool string is bounded by `cap`, so one read of `cap` bytes always
 * contains the whole string plus its terminator, except at the very
 * end of the file where a short read is fine and the NUL is supplied
 * below. */
static bool read_str(ai_file_t *f, uint32_t base, uint32_t off,
                     char *out, int cap)
{
    int got, i;

    out[0] = 0;
    if (!off) return true;
    if (ai_seek(f, base + off) != 0) return false;

    got = ai_read(f, out, cap - 1);
    if (got < 0) { out[0] = 0; return false; }
    out[got] = 0;
    /* The pool is NUL-separated; the read almost certainly ran past
     * the end of this string into the next one. */
    for (i = 0; i < got; i++) if (!out[i]) return true;
    out[got] = 0;
    return true;
}

static bool fill_hit(ai_hit_t *h, const ai_pack_t *p, uint32_t chunk)
{
    ai_file_t f;
    uint8_t rec[16], hdr[AI_HDR_SIZE];
    uint32_t nrec, recsz, poolbase, doc, headoff;
    char path[64];

    h->pack = p;
    h->chunk = chunk;
    h->path[0] = h->title[0] = h->head[0] = 0;

    /* chunks.zct: header, then count*16 records, then the string pool */
    path_join(path, sizeof(path), p->dir, "chunks.zct");
    if (ai_open(&f, path) != 0) return false;
    if (ai_read(&f, hdr, AI_HDR_SIZE) != AI_HDR_SIZE) { ai_close(&f); return false; }
    if (!hdr_ok(hdr, AI_MAGIC_CHUNKS, p->dsid, &nrec, &recsz, NULL, NULL)) {
        ai_close(&f); return false;
    }
    if (chunk >= nrec) { ai_close(&f); return false; }
    poolbase = AI_HDR_SIZE + nrec * 16u;
    if (ai_seek(&f, AI_HDR_SIZE + chunk * 16u) != 0) { ai_close(&f); return false; }
    if (ai_read(&f, rec, 16) != 16) { ai_close(&f); return false; }
    doc      = rd32(rec + 0);
    h->off   = rd32(rec + 4);
    h->len   = rd32(rec + 8);
    headoff  = rd32(rec + 12);
    read_str(&f, poolbase, headoff, h->head, AI_HEAD_MAX);
    ai_close(&f);

    /* docs.zdt: header, then count*16 records, then the pool */
    path_join(path, sizeof(path), p->dir, "docs.zdt");
    if (ai_open(&f, path) != 0) return false;
    if (ai_read(&f, hdr, AI_HDR_SIZE) != AI_HDR_SIZE) { ai_close(&f); return false; }
    if (!hdr_ok(hdr, AI_MAGIC_DOCS, p->dsid, &nrec, &recsz, NULL, NULL)) {
        ai_close(&f); return false;
    }
    if (doc >= nrec) { ai_close(&f); return false; }
    poolbase = AI_HDR_SIZE + nrec * 16u;
    if (ai_seek(&f, AI_HDR_SIZE + doc * 16u) != 0) { ai_close(&f); return false; }
    if (ai_read(&f, rec, 16) != 16) { ai_close(&f); return false; }
    {
        uint32_t poff = rd32(rec + 0), toff = rd32(rec + 4);
        char rel[AI_PATH_MAX];
        read_str(&f, poolbase, poff, rel, sizeof(rel));
        read_str(&f, poolbase, toff, h->title, AI_TITLE_MAX);
        /* the packer stores a card-relative path like
         * "ark/arklite/codex/00000042.md"; the device needs it
         * absolute. */
        h->path[0] = '/';
        scopy(h->path + 1, AI_PATH_MAX - 1, rel);
    }
    ai_close(&f);
    return true;
}

/* ------------------------------------------------------------------ *
 * Fine re-rank
 * ------------------------------------------------------------------ */

/* Reads one fine vector from an ALREADY OPEN fine.zfv and scores it.
 *
 * The handle is held across slices rather than opened per candidate:
 * 512 opens and closes of the same file is most of the cost of a
 * re-rank, and on FatFs an open is a directory walk. `dim` and `n`
 * come from the caller, which read the header once. */
static int32_t fine_score(ai_file_t *f, uint32_t chunk, uint32_t n,
                          uint32_t dim, const int8_t *qv)
{
    int8_t v[256];
    int32_t s = 0;
    uint32_t d;

    if (chunk >= n || dim == 0 || dim > sizeof(v)) return 0;
    if (ai_seek(f, AI_HDR_SIZE + chunk * dim) != 0) return 0;
    if (ai_read(f, v, (int)dim) != (int)dim) return 0;
    for (d = 0; d < dim; d++) s += (int32_t)v[d] * qv[d];
    return s;
}

/* Opens pack `pi`'s fine.zfv into q->fz, closing whatever was there.
 * Returns false if the pack has no usable fine index. */
static bool fine_open(ai_query_t *q, int pi, uint32_t *n, uint32_t *dim)
{
    const ai_pack_t *p = &g_packs[pi];
    uint8_t hdr[AI_HDR_SIZE];
    char path[64];

    if (q->fz_pack == pi) return true;      /* n/dim already cached */
    if (q->fz_pack >= 0) ai_close(&q->fz);
    q->fz_pack = -1;

    if (!(p->flags & AI_FLAG_HAS_FINE)) return false;
    path_join(path, sizeof(path), p->dir, "fine.zfv");
    if (ai_open(&q->fz, path) != 0) return false;
    if (ai_read(&q->fz, hdr, AI_HDR_SIZE) != AI_HDR_SIZE) {
        ai_close(&q->fz); return false;
    }
    if (!hdr_ok(hdr, AI_MAGIC_FINE, p->dsid, n, dim, NULL, NULL)) {
        ai_close(&q->fz); return false;
    }
    q->fz_pack = pi;
    return true;
}

/* ------------------------------------------------------------------ *
 * The lexical half -- BM25 over the packed lexicon
 *
 * This is the half that MEASURES BETTER. On the 63-question gold set
 * (tools/ask, `ask eval`) BM25 alone finds the answer at rank 1 for 42
 * of 63 and within the top ten for 62 of 63, against 40 and 55 for the
 * untrained dense encoder. It is also the half that handles the
 * queries dense retrieval is worst at -- an exact rare token like
 * `lakritz` or `0x7000_0600`, which an embedding blurs toward whatever
 * it resembles.
 *
 * -- what is on the card --
 *
 *   lexicon.zlx  header, then nterms records of
 *                {term_off, post_off, post_len, df}, sorted by term,
 *                then the term string pool.
 *   post.zlp     header, then varint chunk-id deltas each followed by
 *                a one-byte term frequency.
 *
 * Neither is resident. The dictionary is binary-searched in place --
 * about fifteen probes for twenty thousand terms, two seeks each --
 * and a term's postings are read as one block.
 *
 * -- LENGTH NORMALISATION, AND A CLAIM THAT WAS FALSE --
 *
 * This ran with b = 0 -- no length normalisation -- on the reasoning
 * that the packed index carried no per-chunk token count, and the
 * comment here asserted that tools/ask did the same "so the gold-set
 * numbers and the device agree about what they are measuring."
 *
 * THAT WAS NEVER TRUE. lib/lexicon.py has always used b = 0.75. Every
 * number quoted from `ask eval` was therefore measuring a better
 * system than the hardware was running, and the difference showed:
 * "how to start a fire" returned the I2C document, which is full of
 * the I2C START condition, ahead of the survival manual. Without the
 * length term, a long chunk repeating one query term outscores a short
 * one that is actually about the subject.
 *
 * The per-chunk count did not need a new file after all. chunks.zct's
 * third word is the chunk's BYTE length, already there for the
 * preview, and bytes over AI_BYTES_PER_TOKEN is a good enough proxy.
 * load_pack() streams that column into p->lens at startup -- two bytes
 * a chunk, 20KB for arklite -- and b is 0.75 on both sides now.
 * ------------------------------------------------------------------ */

/* log2(x) in Q8, for x in Q8. Enough for an idf weight and it costs
 * no libm -- picorv32 has no FPU (rtl/boards.vh) so a float log would
 * be a libgcc call per term. */
static int32_t log2_q8(uint32_t x_q8)
{
    int32_t e = 0;
    uint32_t v = x_q8;
    int i;

    if (!v) return 0;
    while (v >= (2u << 8)) { v >>= 1; e++; }
    while (v < (1u << 8)) { v <<= 1; e--; }
    /* v is now in [1,2) in Q8; refine the fraction bit by bit. */
    {
        int32_t frac = 0;
        uint32_t m = v;
        for (i = 0; i < 8; i++) {
            m = (m * m) >> 8;
            frac <<= 1;
            if (m >= (2u << 8)) { m >>= 1; frac |= 1; }
        }
        return (e << 8) | frac;
    }
}

/* BM25 idf, Q8: log(1 + (N - df + 0.5) / (df + 0.5)), matching
 * Lexicon.idf() in tools/ask/lib/lexicon.py. */
static int32_t bm25_idf_q8(uint32_t n, uint32_t df)
{
    uint32_t num = (n * 2 + 1) - (df * 2);      /* 2*(N - df + 0.5) */
    uint32_t den = df * 2 + 1;                  /* 2*(df + 0.5)     */
    if (df >= n) return 0;
    /* ln(1+r) = log2(1+r) * ln2; ln2 in Q8 is 177. */
    return (log2_q8(((num << 8) / den) + (1u << 8)) * 177) >> 8;
}

static uint32_t read_varint(const uint8_t *p, uint32_t *i, uint32_t len)
{
    uint32_t v = 0;
    int sh = 0;
    while (*i < len) {
        uint8_t b = p[(*i)++];
        v |= (uint32_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    return v;
}

/* Adds `sc` to `chunk`'s score, admitting it if there is budget. */
static void lexmap_add(ai_query_t *q, int pack, uint32_t chunk, int32_t sc)
{
    uint32_t h = (chunk * 2654435761u) >> (32 - AI_LEXMAP_BITS);
    uint32_t i;
    for (i = 0; i < AI_LEXMAP_SIZE; i++) {
        uint32_t k = (h + i) & (AI_LEXMAP_SIZE - 1);
        if (!q->lexmap[k].used) {
            /* Not present. Admit only while this pack has budget --
             * see AI_LEXMAP_ADMIT for why refusing here is safe. */
            if (q->lex_admitted >= q->lex_budget) return;
            q->lexmap[k].used = 1;
            q->lexmap[k].chunk = chunk;
            q->lexmap[k].pack = (int8_t)pack;
            q->lexmap[k].score = sc;
            q->lex_admitted++;
            return;
        }
        if (q->lexmap[k].chunk == chunk && q->lexmap[k].pack == pack) {
            q->lexmap[k].score += sc;
            return;
        }
    }
}

/* Binary-searches the dictionary for `term`. Returns true and fills
 * post_off/post_len/df, or false. */
static bool lex_find(const ai_pack_t *p, ai_file_t *f, uint32_t poolbase,
                     const char *term, uint32_t *post_off,
                     uint32_t *post_len, uint32_t *df)
{
    int lo = 0, hi = (int)p->nterms - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint8_t rec[16];
        char name[48];
        int c;

        if (ai_seek(f, AI_HDR_SIZE + (uint32_t)mid * 16u) != 0) return false;
        if (ai_read(f, rec, 16) != 16) return false;
        if (!read_str(f, poolbase, rd32(rec + 0), name, sizeof(name)))
            return false;

        c = strcmp(term, name);
        if (c == 0) {
            *post_off = rd32(rec + 4);
            *post_len = rd32(rec + 8);
            *df = rd32(rec + 12);
            return true;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return false;
}

/* Builds q->lplan for pack `pi`: every query term in its dictionary,
 * rarest first. Opens lexicon.zlx once for all of them -- the first
 * version opened it once PER TERM, and on a large card every open
 * builds a fast-seek map by walking the file's cluster chain. */
static void lex_plan(ai_query_t *q, int pi)
{
    const ai_pack_t *p = &g_packs[pi];
    char toks[AI_TERMS_MAX][32];
    int ntok = tokenize(q->text, toks, AI_TERMS_MAX);
    ai_file_t f;
    uint8_t hdr[AI_HDR_SIZE];
    uint32_t nterms, poolbase;
    char path[64];
    int t, k;

    q->nplan = 0;
    q->plan_i = 0;
    q->plan_done = 0;
    q->plan_cid = 0;
    q->lex_admitted = 0;
    /* Each pack gets an equal share of the admit limit, so a first
     * pack full of common matches cannot crowd a second pack's rare
     * ones out of the accumulator. */
    q->lex_budget = AI_LEXMAP_ADMIT / (uint32_t)(g_npacks ? g_npacks : 1);

    if (!p->nterms || !ntok) return;
    path_join(path, sizeof(path), p->dir, "lexicon.zlx");
    if (ai_open(&f, path) != 0) return;
    if (ai_read(&f, hdr, AI_HDR_SIZE) != AI_HDR_SIZE
        || !hdr_ok(hdr, AI_MAGIC_LEXICON, p->dsid, &nterms, NULL, NULL, NULL)) {
        ai_close(&f);
        return;
    }
    poolbase = AI_HDR_SIZE + nterms * 16u;
    for (t = 0; t < ntok; t++) {
        uint32_t off = 0, len = 0, df = 0;
        if (!lex_find(p, &f, poolbase, toks[t], &off, &len, &df)) continue;
        if (!len || !df) continue;
        /* A term repeated in the query is kept twice, as
         * tools/ask/lib/lexicon.py's score() counts it twice. */
        k = q->nplan++;
        q->lplan[k].off = off;
        q->lplan[k].len = len;
        q->lplan[k].df = df;
        q->lplan[k].idf = bm25_idf_q8(p->nchunks, df);
        /* insertion sort, ascending df: rarest first */
        while (k > 0 && q->lplan[k - 1].df > q->lplan[k].df) {
            ai_lplan_t tmp = q->lplan[k - 1];
            q->lplan[k - 1] = q->lplan[k];
            q->lplan[k] = tmp;
            k--;
        }
    }
    ai_close(&f);
}

/* Scores one block of the current plan term's postings. Returns true
 * when the pack's plan is exhausted. */
static bool lex_block(ai_query_t *q, int pi)
{
    const ai_pack_t *p = &g_packs[pi];
    static uint8_t buf[AI_POST_BLOCK];
    uint32_t len, want, i = 0;
    int32_t idf;
    bool last;
    char path[64];
    int got;

    if (q->plan_i >= q->nplan) return true;

    if (q->lf_pack != pi) {
        if (q->lf_pack >= 0) { ai_close(&q->lf); q->lf_pack = -1; }
        path_join(path, sizeof(path), p->dir, "post.zlp");
        if (ai_open(&q->lf, path) != 0) { q->plan_i = q->nplan; return true; }
        q->lf_pack = pi;
    }

    len = q->lplan[q->plan_i].len;
    idf = q->lplan[q->plan_i].idf;
    want = len - q->plan_done;
    if (want > AI_POST_BLOCK) want = AI_POST_BLOCK;
    last = (q->plan_done + want == len);

    got = -1;
    if (ai_seek(&q->lf, AI_HDR_SIZE + q->lplan[q->plan_i].off + q->plan_done) == 0)
        got = ai_read(&q->lf, buf, (int)want);
    if (got != (int)want) {
        /* A short read is a damaged card. Abandon this term rather
         * than score half of it and pretend. */
        got = 0;
        last = true;
    }

    /* A record is a varint (at most 5 bytes) and a tf byte. Parse only
     * whole records, unless this block reaches the end of the list; the
     * unparsed tail is re-read at the start of the next block. */
    while (i < (uint32_t)got) {
        uint32_t start = i, d, tf;
        if (!last && i + 6 > (uint32_t)got) break;
        d = read_varint(buf, &i, (uint32_t)got);
        if (i >= (uint32_t)got) { i = start; break; }
        tf = buf[i++];
        q->plan_cid += d;
        if (!tf) continue;
        {
            uint32_t cid = q->plan_cid;
            int32_t k1 = AI_BM25_K1_Q8;
            int32_t tfq = (int32_t)tf << 8;
            int32_t norm = 1 << 8;          /* 1.0 when no lengths */
            int32_t num, den, sat;

            /* idf * tf*(k1+1) / (tf + k1*(1 - b + b*dl/avgdl)), Q8.
             * The length term is what stops a long chunk winning on
             * sheer repetition. */
            if (p->lens && cid < p->nchunks && p->avg_len) {
                int32_t ratio = (int32_t)(((uint32_t)p->lens[cid] << 8)
                                          / p->avg_len);
                if (ratio > (8 << 8)) ratio = 8 << 8;
                norm = (1 << 8) - AI_BM25_B_Q8
                     + (int32_t)(((int64_t)AI_BM25_B_Q8 * ratio) >> 8);
                if (norm < 16) norm = 16;
            }
            num = tfq * ((k1 + (1 << 8)) >> 4);
            den = (tfq + (int32_t)(((int64_t)k1 * norm) >> 8)) >> 4;
            sat = den ? (num / den) : 0;
            lexmap_add(q, pi, cid, (int32_t)(((int64_t)idf * sat) >> 8));
        }
    }

    q->plan_done += i;
    if (last || q->plan_done >= len) {
        q->plan_i++;
        q->plan_done = 0;
        q->plan_cid = 0;
    }
    return q->plan_i >= q->nplan;
}

/* ------------------------------------------------------------------ *
 * The query state machine
 * ------------------------------------------------------------------ */

bool ai_query_begin(ai_query_t *q, const char *text, int want)
{
    int i;
    /* Close a handle the PREVIOUS query may still be holding, before
     * memset erases the fact that it exists. A second query typed
     * while the first was mid-re-rank leaked one file handle every
     * time, and Z_FS_MAX_OPEN is 8. */
    if (q->fz_pack >= 0) { ai_close(&q->fz); q->fz_pack = -1; }
    if (q->lf_pack >= 0) { ai_close(&q->lf); q->lf_pack = -1; }
    memset(q, 0, sizeof(*q));
    if (!g_npacks) return false;
    q->want = (want > 0 && want <= AI_HITS_MAX) ? want : 8;
    q->phase = AI_Q_ENCODE;
    q->fz_pack = -1;
    q->fz.handle = -1;
    q->lf_pack = -1;
    q->lf.handle = -1;
    q->nplan = -1;
    /* Only packs with a coarse array contribute scan work. A
     * lexical-only pack (`dense = no`) has none, so total stays 0 and
     * the app shows an indeterminate status instead of a bar stuck at
     * zero for the whole query. */
    for (i = 0; i < g_npacks; i++)
        if (g_packs[i].coarse) q->total += g_packs[i].nchunks;
    /* Kept verbatim: the query is re-encoded once per pack, because
     * packs may carry different encoders and a vector from one is
     * meaningless in another's space. */
    scopy(q->text, AI_QUERY_MAX, text);
    return true;
}

void ai_query_cancel(ai_query_t *q)
{
    q->phase = AI_Q_CANCELLED;
    if (q->lf_pack >= 0) { ai_close(&q->lf); q->lf_pack = -1; }
    /* Release the fine index handle. Z_FS_MAX_OPEN is small and
     * shared by every process (see sw/apps/read/read.c's own note on
     * running out of handles), so a cancelled query that kept one
     * would eventually starve the file dialogs. */
    if (q->fz_pack >= 0) { ai_close(&q->fz); q->fz_pack = -1; }
}

bool ai_query_step(ai_query_t *q)
{
    switch (q->phase) {

    case AI_Q_ENCODE: {
        int i, any = 0;
        /* A lexical-only pack has no coarse array and no encoder;
         * skip straight to BM25. */
        /* Every pack, once. Cheap (a few thousand MACs each) and it
         * removes the re-encode that used to happen per pack during
         * the scan and PER SHORTLIST ENTRY during the re-rank. */
        for (i = 0; i < g_npacks; i++) {
            q->pdim[i] = g_packs[i].coarse
                ? encode_query(&g_packs[i], q->text,
                               q->pvec[i], sizeof(q->pvec[i]))
                : 0;
            if (q->pdim[i]) any = 1;
        }
        q->phase = any ? AI_Q_SCAN : AI_Q_LEXICAL;
        q->lex_pack = 0;
        q->lex_term = 0;
        q->nplan = -1;
        q->pack_i = 0;
        q->scan_i = 0;
        return false;
    }

    case AI_Q_SCAN: {
        const ai_pack_t *p = &g_packs[q->pack_i];
        const int8_t *qv = q->pvec[q->pack_i];
        uint32_t dim = p->coarse_dim;
        uint32_t end = q->scan_i + AI_SCAN_SLICE;
        uint32_t i;

        if (!q->pdim[q->pack_i]) {
            /* This pack contributes no dense candidates -- either it
             * has no coarse array (`dense = no`) or no query term is
             * in its vocabulary.
             *
             * Its chunks are NOT counted as scanned. ai_query_begin()
             * only adds packs that have a coarse array to `total`, so
             * counting them here made scanned exceed total and the
             * status line read "searching 645%" on a card holding one
             * lexical pack and one dense one. */
            q->scan_i = p->nchunks;
        } else {
            if (dim > q->pdim[q->pack_i]) dim = q->pdim[q->pack_i];
            if (end > p->nchunks) end = p->nchunks;
            for (i = q->scan_i; i < end; i++) {
                const int8_t *v = p->coarse + (uint32_t)i * p->coarse_dim;
                int32_t s = 0;
                uint32_t d;
                for (d = 0; d < dim; d++) s += (int32_t)v[d] * qv[d];
                heap_push(q, s, i, q->pack_i);
            }
            q->scanned += end - q->scan_i;
            q->scan_i = end;
        }

        if (q->scan_i >= p->nchunks) {
            q->pack_i++;
            q->scan_i = 0;
            if (q->pack_i >= g_npacks) {
                /* Sort the shortlist by (pack, chunk) before the
                 * re-rank.
                 *
                 * It comes out of the heap in no useful order, so the
                 * re-rank was 512 seeks scattered across fine.zfv. In
                 * chunk order the same 512 reads walk the file
                 * forwards, and since a 128-dimension vector is a
                 * quarter of a 512-byte sector, four consecutive ones
                 * come from the same sector FatFs just read.
                 *
                 * Shell sort: no recursion, no scratch array, and
                 * 512 elements is far too many for the insertion sort
                 * used elsewhere in this file.
                 */
                int gap, i2, j2;
                for (gap = q->nshort / 2; gap > 0; gap /= 2) {
                    for (i2 = gap; i2 < q->nshort; i2++) {
                        ai_cand_t t = q->shortlist[i2];
                        uint32_t tk = ((uint32_t)(uint8_t)t.pack << 24)
                                    | (t.chunk & 0xffffff);
                        for (j2 = i2; j2 >= gap; j2 -= gap) {
                            ai_cand_t *c = &q->shortlist[j2 - gap];
                            uint32_t ck = ((uint32_t)(uint8_t)c->pack << 24)
                                        | (c->chunk & 0xffffff);
                            if (ck <= tk) break;
                            q->shortlist[j2] = *c;
                        }
                        q->shortlist[j2] = t;
                    }
                }
                q->phase = AI_Q_RERANK;
                q->rr_i = 0;
            }
        }
        return false;
    }

    case AI_Q_LEXICAL: {
        /* One pack's plan per step, then one postings block per step:
         * everything here is card I/O, so each piece gets its own
         * slice and ESC stays immediate. */
        if (q->lex_pack < g_npacks) {
            if (q->nplan < 0) {
                lex_plan(q, q->lex_pack);
                return false;
            }
            if (!lex_block(q, q->lex_pack)) return false;
            q->lex_pack++;
            q->nplan = -1;
            return false;
        }
        if (q->lf_pack >= 0) { ai_close(&q->lf); q->lf_pack = -1; }

        /* Collect the accumulator's best AI_FUSE_DEPTH. */
        {
            uint32_t i;
            int k;
            q->nlex = 0;
            for (i = 0; i < AI_LEXMAP_SIZE; i++) {
                if (!q->lexmap[i].used) continue;
                if (q->nlex < AI_FUSE_DEPTH) {
                    q->lex[q->nlex].score = q->lexmap[i].score;
                    q->lex[q->nlex].chunk = q->lexmap[i].chunk;
                    q->lex[q->nlex].pack = q->lexmap[i].pack;
                    q->nlex++;
                } else {
                    int worst = 0;
                    for (k = 1; k < q->nlex; k++)
                        if (q->lex[k].score < q->lex[worst].score) worst = k;
                    if (q->lexmap[i].score > q->lex[worst].score) {
                        q->lex[worst].score = q->lexmap[i].score;
                        q->lex[worst].chunk = q->lexmap[i].chunk;
                        q->lex[worst].pack = q->lexmap[i].pack;
                    }
                }
            }
            /* Descending, so position IS rank for the fusion below. */
            for (k = 1; k < q->nlex; k++) {
                ai_cand_t t = q->lex[k];
                int j = k - 1;
                while (j >= 0 && q->lex[j].score < t.score) {
                    q->lex[j + 1] = q->lex[j];
                    j--;
                }
                q->lex[j + 1] = t;
            }
        }
        q->phase = AI_Q_SELECT;
        return false;
    }

    case AI_Q_RERANK: {
        /* AI_RERANK_SLICE at a time, with the pack's fine.zfv held
         * open across slices.
         *
         * This used to do the whole shortlist in ONE step, opening
         * and closing fine.zfv 512 times and re-encoding the query
         * 512 times -- seconds of frozen window immediately after the
         * progress bar hit 100%, which is precisely what the slicing
         * everywhere else exists to prevent. */
        uint32_t end = q->rr_i + AI_RERANK_SLICE;

        if (end > (uint32_t)q->nshort) end = (uint32_t)q->nshort;

        while (q->rr_i < end) {
            int pi = q->shortlist[q->rr_i].pack;
            if (pi >= 0 && q->pdim[pi]
                && fine_open(q, pi, &q->fz_n, &q->fz_dim)) {
                uint32_t d = q->fz_dim;
                if (d > q->pdim[pi]) d = q->pdim[pi];
                q->shortlist[q->rr_i].score =
                    fine_score(&q->fz, q->shortlist[q->rr_i].chunk,
                               q->fz_n, d, q->pvec[pi]);
            }
            q->rr_i++;
        }

        if (q->rr_i < (uint32_t)q->nshort) return false;

        if (q->fz_pack >= 0) { ai_close(&q->fz); q->fz_pack = -1; }
        q->phase = AI_Q_LEXICAL;
        q->lex_pack = 0;
        q->lex_term = 0;
        q->nplan = -1;
        return false;
    }

    case AI_Q_SELECT: {
        int i, k;
        int n = q->nshort;
        static ai_cand_t dense[AI_FUSE_DEPTH];
        static ai_cand_t fused[AI_FUSE_DEPTH * 2];
        int ndense = 0, nfused = 0;

        /* Dense top AI_FUSE_DEPTH, by repeated selection. */
        for (k = 0; k < AI_FUSE_DEPTH && k < n; k++) {
            int best = -1;
            int32_t bs = -0x7fffffff;
            for (i = 0; i < n; i++) {
                if (q->shortlist[i].pack < 0) continue;
                if (q->shortlist[i].score > bs) {
                    bs = q->shortlist[i].score; best = i;
                }
            }
            if (best < 0) break;
            dense[ndense++] = q->shortlist[best];
            q->shortlist[best].pack = -1;
        }

        /* RECIPROCAL RANK FUSION.
         *
         * Scale-free, one constant, and it does not need BM25 scores
         * and int8 dot products to be comparable -- which they are
         * not. It is also what fuses results from two PACKS, which may
         * carry different encoders whose vectors share no space. Same
         * mechanism, no extra machinery.
         *
         * Score is 1/(K+rank) in Q16, summed. K = 60, the value
         * tools/ask uses, so the device and the gold-set numbers agree
         * about what they are computing. */
        /* LEXICAL GOES IN FIRST, and the order matters.
         *
         * The selection below takes the first candidate with a
         * strictly greater score, so on a TIE the earlier entry wins
         * -- and a tie is the common case, because a hit at rank 1 in
         * one list and nowhere in the other scores exactly 65536/60
         * either way.
         *
         * Dense went first until now, which handed every such tie to
         * the half that measures WORSE (40/63 against BM25's 42/63 at
         * rank 1, 55/63 against 62/63 at rank 10). It is also how "how
         * to delete a file" returned Alice in Wonderland ahead of the
         * Unix guide: the untrained encoder liked Alice, BM25 liked
         * the guide, and the tie went the wrong way.
         *
         * tools/ask's own fusion (_rank_all in ask) adds lexical
         * first, so this ALSO makes the device agree with the numbers
         * `ask eval` reports -- which it did not before. */
        for (i = 0; i < q->nlex && nfused < (int)(sizeof(fused)/sizeof(fused[0])); i++) {
            fused[nfused] = q->lex[i];
            fused[nfused].score = (int32_t)(65536 / (60 + i));
            nfused++;
        }
        for (i = 0; i < ndense; i++) {
            int found = -1;
            for (k = 0; k < nfused; k++)
                if (fused[k].chunk == dense[i].chunk
                    && fused[k].pack == dense[i].pack) { found = k; break; }
            if (found >= 0) {
                fused[found].score +=
                    (int32_t)((65536 / (60 + i)) * AI_FUSE_DENSE_Q8 / 256);
            } else if (nfused < (int)(sizeof(fused)/sizeof(fused[0]))) {
                fused[nfused] = dense[i];
                fused[nfused].score =
                    (int32_t)((65536 / (60 + i)) * AI_FUSE_DENSE_Q8 / 256);
                nfused++;
            }
        }

        n = nfused;
        q->nhits = 0;
        for (k = 0; k < q->want && k < n; k++) {
            int best = -1;
            int32_t bs = -0x7fffffff;
            for (i = 0; i < n; i++) {
                if (fused[i].pack < 0) continue;
                if (fused[i].score > bs) { bs = fused[i].score; best = i; }
            }
            if (best < 0) break;
            {
                const ai_pack_t *p = &g_packs[fused[best].pack];
                ai_hit_t *h = &q->hits[q->nhits];
                if (fill_hit(h, p, fused[best].chunk)) {
                    /* 0..1000, monotone in the fine dot product. Not a
                     * probability and not comparable across queries --
                     * see zask.h. It exists so the app can SHOW it,
                     * because a confident wrong answer that looks like
                     * a right one is this program's worst failure. */
                    /* 0..1000, monotone in the fused rank score. Not
                     * a probability and not comparable across queries
                     * -- see zask.h. It exists so the app can SHOW it,
                     * because a confident wrong answer that looks like
                     * a right one is this program's worst failure.
                     *
                     * Two lists at rank 1 give 2*65536/60 = 2184; one
                     * alone at rank 1 gives 1092. Scaled so a hit both
                     * halves agree on lands near the top of the range.
                     */
                    int32_t s = (bs * 1000) / 2200;
                    if (s < 0) s = 0;
                    if (s > 1000) s = 1000;
                    h->score = s;
                    q->nhits++;
                }
                fused[best].pack = -1;
            }
        }
        q->phase = AI_Q_DONE;
        return true;
    }

    case AI_Q_DONE:
    case AI_Q_CANCELLED:
    default:
        return true;
    }
}

/* ------------------------------------------------------------------ */

int ai_preview(const ai_hit_t *h, char *buf, int maxlen)
{
    ai_file_t f;
    int want = (int)h->len;
    int got;
    if (want > maxlen - 1) want = maxlen - 1;
    if (ai_open(&f, h->path) != 0) return -1;
    if (ai_seek(&f, h->off) != 0) { ai_close(&f); return -1; }
    got = ai_read(&f, buf, want);
    ai_close(&f);
    if (got < 0) got = 0;
    buf[got] = 0;
    return got;
}

bool ai_launch_arg(const ai_hit_t *h, char *buf, int buflen)
{
    int n = scat(buf, buflen, 0, h->path);
    if (n >= buflen - 12) return false;
    n = scat(buf, buflen, n, "#");
    n = ucat(buf, buflen, n, h->off);
    return n < buflen - 1;
}

int ai_browse(const ai_pack_t *p, uint32_t from, int max,
              char titles[][AI_TITLE_MAX], char paths[][AI_PATH_MAX])
{
    ai_file_t f;
    uint8_t hdr[AI_HDR_SIZE], rec[16];
    uint32_t nrec, poolbase;
    char path[64];
    int n = 0;

    path_join(path, sizeof(path), p->dir, "docs.zdt");
    if (ai_open(&f, path) != 0) return 0;
    if (ai_read(&f, hdr, AI_HDR_SIZE) != AI_HDR_SIZE) { ai_close(&f); return 0; }
    if (!hdr_ok(hdr, AI_MAGIC_DOCS, p->dsid, &nrec, NULL, NULL, NULL)) {
        ai_close(&f); return 0;
    }
    poolbase = AI_HDR_SIZE + nrec * 16u;
    while (n < max && from + (uint32_t)n < nrec) {
        uint32_t poff, toff;
        char rel[AI_PATH_MAX];
        if (ai_seek(&f, AI_HDR_SIZE + (from + n) * 16u) != 0) break;
        if (ai_read(&f, rec, 16) != 16) break;
        poff = rd32(rec + 0);
        toff = rd32(rec + 4);
        read_str(&f, poolbase, toff, titles[n], AI_TITLE_MAX);
        read_str(&f, poolbase, poff, rel, sizeof(rel));
        paths[n][0] = '/';
        scopy(paths[n] + 1, AI_PATH_MAX - 1, rel);
        n++;
    }
    ai_close(&f);
    return n;
}
