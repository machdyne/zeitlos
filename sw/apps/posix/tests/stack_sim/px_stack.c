/*
 * posix's builtins, each run in job.c's coroutine on the RISC-V CPU
 * (the simulator), printing how deep each went into the coroutine's
 * stack -- zco_stack_used(). See Makefile for how to run it.
 *
 * The commands are the heaviest of each kind, on the fixture run.sh
 * builds: diff of two 6000-line files, unzip of a deflated zip, tar of a
 * .tgz, find over the whole tree (deep/ is 34 levels), grep, sort, a
 * pipeline, cp -r and rm -r. Each yields at every px_intr() (a slice
 * of 0), as often as it ever can. 64 KB of stack here, so that nothing
 * overflows while it is measured.
 *
 * The simulator lacks FS_STAT, FS_LIST_EX and FS_RENAME: fs_stat() is
 * answered below, `ls -l` and `mv` are left out (`ls` is the same
 * function; `mv` is one call).
 */

#include <stdio.h>
#include <string.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "posix.h"
#include "zco.h"

/* sh.c's `port` calls it; main.c is not linked here */
bool px_switch_port(void *conn, const char *name, char *err, uint32_t cap) {
    (void)conn; (void)name; snprintf(err, cap, "not here"); return false;
}

/* The simulator has no FS_STAT (sim/simos.c): a file is what opens, a
 * directory is what lists. Enough for find, tar, unzip, cp -r. */
int __real_fs_stat(const char *path, z_fs_info_t *info);
int __wrap_fs_stat(const char *path, z_fs_info_t *info) {
    static char buf[256];
    uint32_t count = 0, truncated = 0;
    int h;
    memset(info, 0, sizeof(*info));
    /* listing first: the simulator's fopen() opens a directory too */
    if (!strcmp(path, "/") || fs_list_into(path, buf, sizeof(buf), 0, 4, &count, &truncated)) {
        info->type = Z_FS_TYPE_DIR;
        return 1;
    }
    if ((h = fs_open_read(path)) >= 0) {
        fs_close_handle(h);
        info->type = Z_FS_TYPE_FILE;
        info->size = (uint32_t)fs_size((char *)path);
        return 1;
    }
    return 0;
}

static unsigned long bytes;
static char head[160];
static void discard(void *ctx, const char *b, int n) {
    (void)ctx;
    for (int i = 0; i < n && bytes + (unsigned long)i < sizeof(head) - 1; i++)
        head[bytes + (unsigned long)i] = b[i] == '\r' || b[i] == '\n' ? '|' : b[i];
    bytes += (unsigned long)n;
}

static const char *const cmds[] = {
    "find /",
    "find tree -name *.c",
    "diff dbig.txt dbig2.txt",
    "diff diffs/rand8.a diffs/rand8.b",
    "unzip -o arch/t.zip -d sim_unzip",
    "unzip -o arch/big.zip -d sim_unzip",
    "unzip -l arch/t.zip",
    "tar -xf arch/t.tgz -C sim_tar -o",
    "tar -xf arch/big.tgz -C sim_tar -o",
    "tar -tf arch/t.tar",
    "zcat notes.gz",
    "gunzip -k -f notes.gz",
    "grep 7 dbig.txt",
    "sort words.txt",
    "uniq words.txt",
    "tail -5 dbig.txt",
    "head -5 dbig.txt",
    "wc dbig.txt",
    "cat notes.txt numbers.txt",
    "cat dbig.txt | grep 7 | sort",
    "ls /",
    "echo a b c > sim_echo.txt",
    "cp -r tree sim_copy",
    "rm -r sim_copy",
    "ps",
    "free",
    "help",
};

int main(void) {
    /* on the process stack, as main.c has it -- so that malloc() from
     * inside a command works (main.c's main() says why) */
    uint8_t mem[64 * 1024 + PX_JOB_SLACK] __attribute__((aligned(16)));
    char err[64];
    size_t peak = 0;
    const char *peak_cmd = "";

    px_fd_init();
    px_chdir("/", err, sizeof(err));
    px_job_init(mem, sizeof(mem), 0);

    printf("posix command stack on rv32 (bytes of the coroutine's %u):\n",
           (unsigned)px_job_stack_size());
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        static px_shell_t sh;
        px_shell_t *ended;
        int r;
        unsigned long slices = 0;
        px_shell_init(&sh, discard, NULL);
        bytes = 0;
        memset(head, 0, sizeof(head));
        px_job_submit(&sh, cmds[i]);
        while ((r = px_job_step(&ended)) == PX_STEP_RAN || r == PX_STEP_BLOCKED) slices++;
        printf("  %6u  %-36s status %d, %lu bytes out, %lu slices%s\n",
               (unsigned)px_job_stack_last(), cmds[i], sh.status, bytes, slices,
               r == PX_STEP_DONE ? "" : "  OVERFLOW");
        if (sh.status && sh.status != 1) printf("          %.100s\n", head);
        else if (sh.status == 1 || !bytes) printf("          %.100s\n", head);
        if (px_job_stack_last() > peak) { peak = px_job_stack_last(); peak_cmd = cmds[i]; }
    }
    printf("deepest: %u bytes (%s)\n", (unsigned)peak, peak_cmd);
    return 0;
}
