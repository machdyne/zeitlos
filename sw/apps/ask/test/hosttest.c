/*
 * ask -- host test harness.
 *
 *   make -C sw/apps/ask/test
 *   ./sw/apps/ask/test/hosttest <card-root> "a question"
 *
 * <card-root> is a directory laid out exactly like the SD card, which
 * is what `tools/ask/ask build` produces. So this runs THE ENGINE THE
 * DEVICE RUNS over THE PACK THE DEVICE READS, on a machine with a
 * debugger.
 *
 * That matters more here than it usually does. The alternative is
 * finding a byte-offset bug on a 48MHz board, through a 200x300
 * window, over a serial console -- and an index bug does not crash,
 * it shows you a confidently ranked passage about the wrong thing.
 *
 * The comparison that actually validates the format is:
 *
 *     ./tools/ask/ask query dist/arklite.spec "..." -m dense
 *     ./sw/apps/ask/test/hosttest out/arklite "..."
 *
 * Same pack, same question, one path through Python and one through
 * the C that ships. If the rankings disagree, the packer and the
 * reader disagree about the format, and that is a bug that would
 * otherwise reach hardware silently.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../aidx.h"

void ai_set_root(const char *r);

static void on_progress(uint32_t done, uint32_t total, const char *what)
{
    static uint32_t last;
    if (!total) return;
    if (done == total || done - last > (total / 8)) {
        fprintf(stderr, "\r  loading %-10s %3u%%", what,
                (unsigned)(100u * done / total));
        last = (done == total) ? 0 : done;
        if (done == total) fprintf(stderr, "\n");
    }
}

int main(int argc, char **argv)
{
    ai_query_t q;
    int n, i, steps = 0;

    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <card-root> [\"question\"]\n"
            "  <card-root> is a tools/ask build output, e.g. "
            "tools/ask/out/arklite\n", argv[0]);
        return 2;
    }
    ai_set_root(argv[1]);

    n = ai_load_all("/ask", on_progress);
    if (!n) {
        /* The reason, not just the fact. This is the message the app
         * puts on its status line, so checking a card here tells you
         * exactly what the device will say about it. */
        fprintf(stderr, "no packs loaded under %s/ask: %s\n",
                argv[1], ai_error());
        return 1;
    }

    printf("packs: %d   resident: %.2f MB\n", n, ai_resident() / 1e6);
    for (i = 0; i < n; i++) {
        const ai_pack_t *p = ai_pack(i);
        printf("  %-10s dsid 0x%08x  %u docs  %u chunks  "
               "coarse %u  fine %u  vocab %u  enc 0x%08x\n",
               p->name, p->dsid, p->ndocs, p->nchunks,
               p->coarse_dim, p->fine_dim, p->vocab, p->encoder_id);
    }

    if (argc < 3) return 0;

    if (!ai_query_begin(&q, argv[2], 8)) {
        fprintf(stderr, "query refused\n");
        return 1;
    }
    while (!ai_query_step(&q)) {
        steps++;
        if (steps > 1000000) { fprintf(stderr, "runaway\n"); return 1; }
    }

    printf("\nq: \"%s\"\n", argv[2]);
    printf("scanned %u of %u chunks in %d slices, %d hits\n\n",
           q.scanned, q.total, steps, q.nhits);

    for (i = 0; i < q.nhits; i++) {
        const ai_hit_t *h = &q.hits[i];
        char arg[AI_PATH_MAX];
        char prev[220];
        int k, m = 0;

        printf("%d. [%-8s] %s\n", i + 1, h->pack->name, h->title);
        if (h->head[0]) printf("   %s\n", h->head);
        printf("   score %-4d  %s  +%u  %u bytes\n",
               h->score, h->path, h->off, h->len);

        if (ai_launch_arg(h, arg, sizeof(arg)))
            printf("   read arg: %s (%d/%d bytes)\n",
                   arg, (int)strlen(arg) + 1, AI_PATH_MAX);
        else
            printf("   LAUNCH ARG DOES NOT FIT\n");

        if (ai_preview(h, prev, sizeof(prev)) > 0) {
            /* one line, whitespace collapsed, so a preview is
             * comparable against tools/ask's own output */
            char flat[220];
            int sp = 1;
            for (k = 0; prev[k] && m < (int)sizeof(flat) - 1; k++) {
                char c = prev[k];
                if (c == '\n' || c == '\t' || c == '\r') c = ' ';
                if (c == ' ') { if (sp) continue; sp = 1; }
                else sp = 0;
                flat[m++] = c;
            }
            flat[m] = 0;
            printf("   > %.150s\n", flat);
        }
        printf("\n");
    }

    ai_free_all();
    return 0;
}
