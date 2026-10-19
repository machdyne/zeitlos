/*
 * posix -- cp -r and rm -r (docs/posix.md).
 *
 * A file of its own, beside tools.c: the walk (sw/common/zwalk.h,
 * docs/zwalk.md) and its state are all that is here.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "../../common/zwalk.h"
#include "posix.h"

/* ------------------------------------------------------------------ */
/* cp -r and rm -r                                                     */

/*
 * On sw/common/zwalk.c (docs/zwalk.md), the walker the file browser
 * uses: one filesystem call per zw_step(), so Ctrl+C is looked for
 * between every chunk copied and every entry removed, and zw_cancel()
 * deletes the file copy was half-way through. Its working block is
 * malloc()ed for the command (32 KB, or 8 KB if that is all there is)
 * and freed after. Its limits are its own: ZW_DEPTH (16) folders deep;
 * a copy never goes into an existing name, nor into its own source.
 */
static zwalk_t tree_walk;

/* `what` is "cp" or "rm"; dst NULL for rm. */
void px_tree_op(px_shell_t *sh, const char *what, const char *src, const char *dst) {

    char s[PX_PATH_MAX], d[PX_PATH_MAX];
    z_fs_info_t fi;
    zwalk_t *w = &tree_walk;
    uint32_t cap = 32768;
    void *mem;

    if (!px_resolve(src, s, sizeof(s)) || (dst && !px_resolve(dst, d, sizeof(d)))) {
        px_printf(sh, "%s: bad path\n", what);
        sh->status = 1;
        return;
    }
    if (!dst && !strcmp(s, "/")) {
        px_printf(sh, "%s: refusing to remove /\n", what);
        sh->status = 1;
        return;
    }
    /* As cp and mv: an existing directory as the destination means
     * "into it". */
    if (dst && fs_stat(d, &fi) && fi.type == Z_FS_TYPE_DIR) {
        const char *base = strrchr(s, '/');
        size_t n = strlen(d);
        base = base ? base + 1 : s;
        if (n + 1 + strlen(base) + 1 > sizeof(d)) {
            px_printf(sh, "%s: path too long\n", what);
            sh->status = 1;
            return;
        }
        if (d[n - 1] != '/') d[n++] = '/';
        strcpy(d + n, base);
    }

    mem = malloc(cap);
    if (!mem) { cap = 8192; mem = malloc(cap); }
    if (!mem) { px_printf(sh, "%s: out of memory\n", what); sh->status = 1; return; }

    if (!zw_start(w, dst ? ZW_COPY : ZW_DELETE, s, dst ? d : NULL, mem, cap)) {
        px_printf(sh, "%s: %s: %s\n", what, w->err == ZW_E_EXISTS ? d : src,
                  zw_strerror(w->err));
        free(mem);
        sh->status = 1;
        return;
    }

    int r;
    do {
        if (px_intr(sh)) { zw_cancel(w); r = ZW_DONE; break; }
        r = zw_step(w);
    } while (r == ZW_MORE || r == ZW_HIT);

    if (r == ZW_FAIL) {
        px_printf(sh, "%s: %s: %s\n", what, w->hit, zw_strerror(w->err));
        zw_cancel(w);
        sh->status = 1;
    } else if (!px_intr(sh) && (w->truncated || w->skipped)) {
        px_printf(sh, "%s: %lu entr%s skipped (deeper than %d, or a folder too big to list)\n",
                  what, (unsigned long)w->skipped, w->skipped == 1 ? "y" : "ies", ZW_DEPTH);
        sh->status = 1;
    }
    free(mem);
}
