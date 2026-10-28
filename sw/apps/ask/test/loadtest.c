/*
 * ask -- the incremental loader, tested on the host.
 *
 *   make -C sw/apps/ask/test loadtest
 *   ./sw/apps/ask/test/loadtest <card-root> [<card-root> ...]
 *
 * Each <card-root> is a tools/ask build output (tools/ask/out/zdocs).
 * Built with AddressSanitizer by the Makefile, and with aidx.c's
 * malloc/free routed through counters here, so "frees everything"
 * is checked as a number after every stop, not only as a leak report
 * at exit.
 *
 * What it checks, per card:
 *
 *  1. ai_load_all() and ai_load_step() run by hand give the same
 *     packs, and each pack's lens[] and avg_len equal the old rule --
 *     chunks.zct's byte length / AI_BYTES_PER_TOKEN, clamped 1..65535
 *     -- computed here independently from chunks.zct with stdio. A pack
 *     with lens.zln is checked against that rule too, which is what
 *     says the packer and the fallback agree.
 *  2. The same card with lens.zln removed (copied to a temp dir) loads
 *     to the identical state by the chunks.zct scan: the old-pack path.
 *  3. Stopped after every possible step, the loader leaves no file open
 *     and no allocation beyond the packs it reports loaded; and the
 *     packs it reports are whole (same lens as the full load).
 *  4. Every request failing in turn, as an error and as an early end
 *     of file: nothing open, nothing leaked, and either the pack loads
 *     the same, or it fails with a reason, or it loads with a warning
 *     and no lens -- never lens that differ.
 *  5. The requests and bytes a load takes, printed for the record.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../common/zpaths.h"
#include "../aidx.h"

/* -- allocation counting: aidx.c is compiled with -Dmalloc=t_malloc
 * -Dfree=t_free (Makefile) -- */

static long live;
void *t_malloc(size_t n) { void *p = malloc(n); if (p) live++; return p; }
void t_free(void *p) { if (p) live--; free(p); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* -- the reference: chunk lengths read straight out of chunks.zct -- */

typedef struct {
    char      name[16];
    uint32_t  nchunks, avg_len, nterms, avg_len_q8, dsid, flags;
    uint16_t *lens;             /* NULL if none */
} snap_t;

static snap_t ref[AI_PACKS_MAX], full[AI_PACKS_MAX];
static int nref, nfull;

static uint32_t rd32(const uint8_t *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint16_t *ref_lens(const char *root, const char *pack, uint32_t *n, uint32_t *avg)
{
    char path[1024];
    uint8_t hdr[32], rec[16];
    uint64_t sum = 0;
    uint16_t *l;
    uint32_t i;
    FILE *f;
    snprintf(path, sizeof(path), "%s%s/%s/chunks.zct", root, Z_DIR_ASK_PACKS, pack);
    f = fopen(path, "rb");
    if (!f || fread(hdr, 1, 32, f) != 32) { if (f) fclose(f); return NULL; }
    *n = rd32(hdr + 12);
    l = malloc(*n * 2 + 2);
    for (i = 0; i < *n; i++) {
        uint32_t t;
        if (fread(rec, 1, 16, f) != 16) { free(l); fclose(f); return NULL; }
        t = rd32(rec + 8) / AI_BYTES_PER_TOKEN;
        if (t > 65535) t = 65535;
        if (!t) t = 1;
        l[i] = (uint16_t)t;
        sum += t;
    }
    fclose(f);
    *avg = (uint32_t)(sum / (*n ? *n : 1));
    if (!*avg) *avg = 1;
    return l;
}

static void snap(snap_t *s, int *ns)
{
    int i;
    *ns = ai_pack_count();
    for (i = 0; i < *ns; i++) {
        const ai_pack_t *p = ai_pack(i);
        snprintf(s[i].name, sizeof(s[i].name), "%s", p->name);
        s[i].nchunks = p->nchunks;
        s[i].avg_len = p->avg_len;
        s[i].nterms = p->nterms;
        s[i].avg_len_q8 = p->avg_len_q8;
        s[i].dsid = p->dsid;
        s[i].flags = p->flags;
        free(s[i].lens);
        s[i].lens = NULL;
        if (p->lens) {
            s[i].lens = malloc(p->nchunks * 2);
            memcpy(s[i].lens, p->lens, p->nchunks * 2);
        }
    }
}

static const snap_t *find(const snap_t *s, int ns, const char *name)
{
    int i;
    for (i = 0; i < ns; i++) if (!strcmp(s[i].name, name)) return &s[i];
    return NULL;
}

/* Every loaded pack equals its counterpart in `want`; returns false
 * (and says why) if not. A pack without lens is a mismatch unless
 * `lens_optional`. */
static bool same_as(const snap_t *want, int nwant, bool lens_optional, const char *ctx)
{
    int i;
    bool ok = true;
    for (i = 0; i < ai_pack_count(); i++) {
        const ai_pack_t *p = ai_pack(i);
        const snap_t *w = find(want, nwant, p->name);
        if (!w) { CHECK(0, "%s: unexpected pack %s", ctx, p->name); return false; }
        /* nterms and avg_len_q8 come from lexicon.zlx's header, which
         * may fail to read with a warning (ai_warning()). */
        if (p->nchunks != w->nchunks || p->dsid != w->dsid
            || p->flags != w->flags
            || (!ai_warning()[0] && (p->nterms != w->nterms
                                     || p->avg_len_q8 != w->avg_len_q8))) {
            CHECK(0, "%s: %s: header fields differ", ctx, p->name);
            ok = false;
        }
        if (!p->lens) {
            if (!lens_optional) { CHECK(0, "%s: %s: no lens", ctx, p->name); ok = false; }
            continue;
        }
        if (p->avg_len != w->avg_len || memcmp(p->lens, w->lens, p->nchunks * 2)) {
            CHECK(0, "%s: %s: lens differ (avg %u vs %u)", ctx, p->name,
                  p->avg_len, w->avg_len);
            ok = false;
        }
    }
    return ok;
}

/* -- a copy of the card without lens.zln: an old pack -- */

static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t n;
    if (!a) return -1;
    b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
    return 0;
}

static bool make_old(const char *root, char *out, int cap)
{
    char src[600], dst[600], sub[600];
    DIR *d, *e;
    struct dirent *pe, *fe;
    snprintf(out, cap, "/tmp/asklt.XXXXXX");
    if (!mkdtemp(out)) return false;
    snprintf(dst, sizeof(dst), "%s/data", out); mkdir(dst, 0755);
    snprintf(dst, sizeof(dst), "%s%s", out, Z_DIR_ASK_PACKS); mkdir(dst, 0755);
    snprintf(src, sizeof(src), "%s%s", root, Z_DIR_ASK_PACKS);
    d = opendir(src);
    if (!d) return false;
    while ((pe = readdir(d))) {
        if (pe->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s/%s", src, pe->d_name);
        e = opendir(sub);
        if (!e) continue;
        snprintf(dst, sizeof(dst), "%.40s%s/%.200s", out, Z_DIR_ASK_PACKS, pe->d_name);
        mkdir(dst, 0755);
        while ((fe = readdir(e))) {
            char f1[900], f2[900];
            if (fe->d_name[0] == '.' || !strcmp(fe->d_name, "lens.zln")) continue;
            snprintf(f1, sizeof(f1), "%s/%s", sub, fe->d_name);
            snprintf(f2, sizeof(f2), "%s/%s", dst, fe->d_name);
            copy_file(f1, f2);
        }
        closedir(e);
    }
    closedir(d);
    return true;
}

static void rm_tree(const char *dir)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);
}

/* -- the tests -- */

static int count_steps(const char *root)
{
    ai_load_t L;
    int n = 0;
    ai_set_root(root);
    ai_load_begin(&L, Z_DIR_ASK_PACKS);
    while (ai_load_step(&L) == AI_LOAD_RUNNING) {
        int pm = ai_load_permille(&L);
        CHECK(pm >= 0 && pm <= 999, "permille %d out of range", pm);
        n++;
    }
    ai_free_all();
    return n;
}

static void test_card(const char *root, bool expect_lensfile)
{
    char old[64] = "";
    ai_load_t L;
    int i, steps, k;
    uint32_t req, bytes;

    printf("%s\n", root);
    ai_set_root(root);

    /* 1. the reference, from chunks.zct directly */
    memset(&ai_host, 0, sizeof(ai_host));
    CHECK(ai_load_all(Z_DIR_ASK_PACKS, NULL) > 0, "nothing loaded: %s", ai_error());
    req = ai_host.requests; bytes = ai_host.bytes;
    snap(full, &nfull);
    nref = nfull;
    for (i = 0; i < nfull; i++) {
        ref[i] = full[i];
        ref[i].lens = NULL;
        ref[i].lens = ref_lens(root, full[i].name, &ref[i].nchunks, &ref[i].avg_len);
        CHECK(ref[i].lens != NULL, "cannot read %s's chunks.zct", full[i].name);
    }
    CHECK(same_as(ref, nref, false, "full load vs chunks.zct"), "lens");
    CHECK(!ai_warning()[0], "unexpected warning: %s", ai_warning());
    ai_free_all();
    CHECK(live == 0, "load_all then free_all leaves %ld allocations", live);
    CHECK(ai_host.open_now == 0, "%d files open", ai_host.open_now);
    printf("  load:        %u packs, %u requests, %u bytes%s\n",
           (unsigned)nfull, (unsigned)req, (unsigned)bytes,
           expect_lensfile ? " (lens.zln)" : " (no lens.zln: chunks.zct scan)");

    /* 2. the same card without lens.zln */
    if (expect_lensfile && make_old(root, old, sizeof(old))) {
        ai_set_root(old);
        memset(&ai_host, 0, sizeof(ai_host));
        CHECK(ai_load_all(Z_DIR_ASK_PACKS, NULL) == nfull, "old copy: %s", ai_error());
        CHECK(same_as(ref, nref, false, "without lens.zln"), "fallback");
        printf("  old pack:    %u requests, %u bytes (chunks.zct scan)\n",
               (unsigned)ai_host.requests, (unsigned)ai_host.bytes);
        ai_free_all();
        CHECK(live == 0, "old copy leaks %ld", live);
        ai_set_root(root);
    }

    /* 3. stopped after every step */
    steps = count_steps(root);
    CHECK(live == 0, "count_steps leaks %ld", live);
    for (k = 0; k <= steps; k++) {
        int s;
        ai_load_begin(&L, Z_DIR_ASK_PACKS);
        for (s = 0; s < k; s++) if (ai_load_step(&L) != AI_LOAD_RUNNING) break;
        ai_load_stop(&L);
        CHECK(!ai_load_busy(&L), "busy after stop");
        CHECK(ai_load_step(&L) == AI_LOAD_STOPPED || k > steps - 1,
              "a stopped load stepped again");
        CHECK(ai_host.open_now == 0, "stop after %d: %d files open", k, ai_host.open_now);
        CHECK(same_as(full, nfull, false, "stopped"), "stop after %d", k);
        ai_free_all();
        CHECK(live == 0, "stop after %d steps leaks %ld allocations", k, live);
        live = 0;
    }
    printf("  stopped:     at each of %d steps, nothing held\n", steps + 1);

    /* 4. each request failing in turn */
    memset(&ai_host, 0, sizeof(ai_host));
    ai_load_all(Z_DIR_ASK_PACKS, NULL);
    req = ai_host.requests;
    ai_free_all();
    {
        int how, loaded_same = 0, failed = 0, warned = 0;
        for (how = 0; how < 3; how++) {
            uint32_t r;
            for (r = 1; r <= req; r++) {
                int n;
                memset(&ai_host, 0, sizeof(ai_host));
                ai_host.fail_at = r;
                ai_host.fail_how = how == 0 ? 0 : -1;
                ai_host.fail_how_timeout = how == 2;
                n = ai_load_all(Z_DIR_ASK_PACKS, NULL);
                CHECK(ai_host.open_now == 0, "fault at %u: %d open", r, ai_host.open_now);
                CHECK(same_as(full, nfull, true, "fault"), "fault at %u/%d", r, how);
                if (n < nfull) {
                    failed++;
                    CHECK(ai_error()[0] && strcmp(ai_error(), "no packs found"),
                          "fault at %u: no reason given", r);
                    if (how == 2 && !strstr(ai_error(), "timeout")
                        && !strstr(ai_error(), "seek") && !strstr(ai_error(), "empty")
                        && !strstr(ai_error(), "subdirectories"))
                        CHECK(0, "fault at %u: timeout reported as \"%s\"", r, ai_error());
                    if (how == 0 && strstr(ai_error(), "timeout"))
                        CHECK(0, "fault at %u: early EOF reported as timeout", r);
                } else {
                    for (i = 0; i < n; i++) if (!ai_pack(i)->lens) break;
                    if (i < n || ai_warning()[0]) {
                        warned++;
                        CHECK(ai_warning()[0], "fault at %u: lens lost silently", r);
                    } else loaded_same++;
                }
                ai_free_all();
                CHECK(live == 0, "fault at %u leaks %ld", r, live);
                live = 0;
            }
        }
        printf("  faults:      %u requests x 3 kinds: %d loaded whole, "
               "%d warned, %d refused with a reason\n",
               (unsigned)req, loaded_same, warned, failed);
        if (failed) {
            /* show a sample of what a person would read */
            memset(&ai_host, 0, sizeof(ai_host));
            ai_host.fail_at = req - 1; ai_host.fail_how = 0;
            if (ai_load_all(Z_DIR_ASK_PACKS, NULL) < nfull)
                printf("               e.g. \"%s\"\n", ai_error());
            ai_free_all();
            memset(&ai_host, 0, sizeof(ai_host));
            ai_host.fail_at = 3; ai_host.fail_how = -1; ai_host.fail_how_timeout = 1;
            if (ai_load_all(Z_DIR_ASK_PACKS, NULL) < nfull)
                printf("               e.g. \"%s\"\n", ai_error());
            ai_free_all();
            memset(&ai_host, 0, sizeof(ai_host));
            live = 0;
        }
    }

    for (i = 0; i < nref; i++) { free(ref[i].lens); ref[i].lens = NULL; }
    for (i = 0; i < nfull; i++) { free(full[i].lens); full[i].lens = NULL; }
    nref = nfull = 0;

    /* and all of it again on the old-pack copy */
    if (old[0]) { test_card(old, false); rm_tree(old); }
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <card-root> ...\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        char root[1024];
        struct stat st;
        snprintf(root, sizeof(root), "%s/data/ask/%s/lens.zln", argv[i], "");
        (void)st;
        /* a card has lens.zln if any of its packs has */
        {
            char dir[600];
            DIR *d;
            struct dirent *e;
            bool has = false;
            snprintf(dir, sizeof(dir), "%s%s", argv[i], Z_DIR_ASK_PACKS);
            d = opendir(dir);
            while (d && (e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(root, sizeof(root), "%s/%s/lens.zln", dir, e->d_name);
                if (stat(root, &st) == 0) has = true;
            }
            if (d) closedir(d);
            test_card(argv[i], has);
        }
    }
    printf(fails ? "loadtest: %d FAILED\n" : "loadtest: ok\n", fails);
    return fails ? 1 : 0;
}
