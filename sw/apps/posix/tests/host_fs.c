/*
 * A host-side implementation of the filesystem calls posix uses, so
 * that sh.c and vfs.c can be exercised without a machine.
 *
 * The same idea as sim/simos.c and for the same reason: the ANSWER to
 * the syscall is what matters, not the code behind it. What is
 * reproduced here carefully is the two behaviours the shell actually
 * depends on and could get wrong:
 *
 *  - fs_size() returns 0 for a missing file as well as an empty one
 *    (sw/os/fsapi.h says so and calls it deliberate). zcc's device
 *    port was caught by exactly this, so the stub had better have it
 *    too or the test is easier than the target.
 *  - fs_list_into() packs entries as "/"-prefixed NUL-terminated
 *    strings back to back, which is what bi_ls() has to unpack.
 *
 * Everything is rooted at PX_TEST_ROOT so a test cannot touch the
 * developer's filesystem.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>

#include "zfs.h"

static char root[512];

void host_fs_set_root(const char *dir) {
    snprintf(root, sizeof(root), "%s", dir);
}

static const char *hp(const char *path) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s", root, path);
    return buf;
}

#define MAXH 8
static FILE *handles[MAXH];

static int alloc_handle(FILE *f) {
    for (int i = 0; i < MAXH; i++)
        if (!handles[i]) { handles[i] = f; return i; }
    fclose(f);
    return -1;
}

int fs_size(char *path) {
    struct stat st;
    if (stat(hp(path), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return (int)st.st_size;
}

int fs_open_read(const char *path) {
    FILE *f = fopen(hp(path), "rb");
    return f ? alloc_handle(f) : -1;
}

int fs_open_write(const char *path) {
    FILE *f = fopen(hp(path), "wb");
    return f ? alloc_handle(f) : -1;
}

/* Does NOT truncate, and fails if the file is absent -- which is
 * exactly what makes it the right call for `>>`, and exactly the
 * difference that made the first version of append destroy files. */
int fs_open_rw(const char *path) {
    FILE *f = fopen(hp(path), "r+b");
    return f ? alloc_handle(f) : -1;
}

int fs_read_chunk(int h, void *buf, int cap) {
    if (h < 0 || h >= MAXH || !handles[h]) return -1;
    return (int)fread(buf, 1, (size_t)cap, handles[h]);
}

int fs_write_chunk(int h, const void *buf, int len) {
    if (h < 0 || h >= MAXH || !handles[h]) return -1;
    return (int)fwrite(buf, 1, (size_t)len, handles[h]);
}

int fs_seek(int h, uint32_t off) {
    if (h < 0 || h >= MAXH || !handles[h]) return -1;
    if (fseek(handles[h], (long)off, SEEK_SET) != 0) return -1;
    return (int)ftell(handles[h]);
}

int fs_close_handle(int h) {
    if (h < 0 || h >= MAXH || !handles[h]) return -1;
    fclose(handles[h]);
    handles[h] = NULL;
    return 0;
}

/* 1 on success, 0 on failure -- sw/common/zfsapp.h's convention, which
 * is what the device links. These stubs once returned 0 on success
 * (the KERNEL's fs.c convention), so the host tests passed while on the
 * device a successful `rm` or `mkdir` reported failure and a failed one
 * reported nothing. See docs/posix.md, "Return conventions". */
int fs_unlink(char *path)  { return remove(hp(path)) == 0 ? 1 : 0; }
int fs_mkdir(const char *path) { return mkdir(hp(path), 0777) == 0 ? 1 : 0; }

bool fs_df(uint32_t *total, uint32_t *freek) {
    *total = 1024 * 1024;
    *freek = 512 * 1024;
    return true;
}

int fs_list_into(const char *path, char *buf, uint32_t cap,
                 uint8_t *types, uint32_t max_entries, uint32_t *count,
                 uint32_t *truncated) {

    DIR *d = opendir(hp(path));
    struct dirent *de;
    uint32_t used = 0, n = 0;

    *count = 0;
    *truncated = 0;
    if (!d) return 0;

    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n >= max_entries) { *truncated = 1; break; }

        char entry[300];
        /* A full path, as the kernel returns it (sw/common/zfs.h:
         * "each already a full "/"-prefixed path"). This stub once
         * returned "/name" alone, which hid that `ls src` on a device
         * printed "src/a.c" for every entry. */
        snprintf(entry, sizeof(entry), "%s%s%s", path,
            (path[0] && path[strlen(path) - 1] == '/') ? "" : "/", de->d_name);
        size_t len = strlen(entry) + 1;
        if (used + len > cap) { *truncated = 1; break; }

        char full[1024];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", hp(path), de->d_name);
        int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));

        memcpy(buf + used, entry, len);
        used += (uint32_t)len;
        if (types) types[n] = (uint8_t)(isdir ? 1 : 0);
        n++;
    }
    closedir(d);

    *count = n;
    return 1;
}

/* -- process and launch-argument stubs --
 *
 * `run` is reported rather than performed. There is no second process
 * here and pretending otherwise would test nothing; what the test DOES
 * check is that the shell composed the right argument string, which is
 * the part that was worth getting right. */
static char last_launch_arg[512];

void z_launch_arg_set(const char *arg) {
    snprintf(last_launch_arg, sizeof(last_launch_arg), "%s", arg ? arg : "");
}

/* The real z_launch_arg_take() CONSUMES the pending argument. Modelled
 * here so the transcript shows what a child would actually receive --
 * without it, a stale argument looks like it persists forever and the
 * bug that prompted this looks worse than it is. */
static void host_launch_arg_take(void) { last_launch_arg[0] = 0; }

const char *host_last_launch_arg(void) { return last_launch_arg; }

/* Succeeds only if a file of that name exists at the root.
 *
 * A stub that always succeeded made `&&` and `||` untestable -- every
 * command "worked", so the failure arm was never taken and the test
 * passed while proving nothing. Faithfulness here is not about
 * realism, it is about the test being able to fail. */
/* The exit status the next spawned "child" will report.
 *
 * Derived from the program's name so that a test can exercise both
 * arms of `&&` and `||` without a second mechanism: a file called
 * `fail` exits 1, anything else exits 0. Crude, and it makes the one
 * thing that needs testing -- that the shell uses the CHILD's status
 * rather than whether it started -- testable at all. */
static int next_exit_status;

uint32_t z_proc_run(const char *name) {
    char path[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    f = fopen(path, "rb");
    printf("[proc_run %s arg='%s' -> %s]\n", name, last_launch_arg,
           f ? "ok" : "not found");
    if (!f) return 0;
    fclose(f);
    next_exit_status = strstr(name, "fail") ? 1 : 0;
    host_launch_arg_take();
    return 7;
}

int host_child_status(void) { return next_exit_status; }

/* `reboot` and `jump` (sh.c): there is no FPGA here to reconfigure --
 * the kernel's answer for a board without PROGRAMN (docs/zboot.md). */
int z_jump(uint32_t target) {
    (void)target;
    return -1;
}

/* -- ps, kill, port --
 *
 * A fixed process table. z_proc_kill() marks its process dying rather
 * than removing it, so a `ps` after a `kill` shows which one it was --
 * the only way a case can see the effect. */
#include "zeitlos.h"   /* z_rv, Z_OK */
#include "zproc.h"

static z_proc_info_t host_procs[] = {
    { .pid = 1, .size = 180 * 1024, .flags = Z_PROC_FLAG_ACTIVE | Z_PROC_FLAG_BLOCKED, .name = "wm0", .cpu_ticks = 7320 },
    { .pid = 2, .size = 360 * 1024, .flags = Z_PROC_FLAG_ACTIVE, .name = "net0", .cpu_ticks = 73200 },
    { .pid = 5, .size = 4096 * 1024, .flags = Z_PROC_FLAG_ACTIVE, .name = "posix0", .cpu_ticks = 732 },
    { .pid = 6, .size = 64 * 1024, .flags = Z_PROC_FLAG_ACTIVE | Z_PROC_FLAG_BLOCKED, .name = "", .cpu_ticks = 0 },
};
#define HOST_NPROCS (uint32_t)(sizeof(host_procs) / sizeof(host_procs[0]))

uint32_t z_proc_list(z_proc_info_t *out, uint32_t max, uint32_t *truncated) {
    uint32_t n = HOST_NPROCS < max ? HOST_NPROCS : max;
    memcpy(out, host_procs, n * sizeof(*out));
    if (truncated) *truncated = HOST_NPROCS > max;
    return n;
}

z_rv z_proc_kill(uint32_t pid) {
    for (uint32_t i = 0; i < HOST_NPROCS; i++)
        if (host_procs[i].pid == pid) { host_procs[i].flags |= Z_PROC_FLAG_DIE; return Z_OK; }
    return Z_FAIL;
}

bool z_pid_lookup(const char *name, uint32_t *pid) {
    for (uint32_t i = 0; i < HOST_NPROCS; i++)
        if (host_procs[i].name[0] && !strcmp(host_procs[i].name, name)) { *pid = host_procs[i].pid; return true; }
    return false;
}

/* main.c's, which asks term to switch; here, the answers it can give */
bool px_switch_port(void *conn, const char *name, char *err, uint32_t cap) {
    (void)conn;
    if (strlen(name) > 20) { snprintf(err, cap, "name too long"); return false; }
    return true;
}

/* `free`: fixed figures, the shape of a small board's pool */
bool z_mem_stats(z_mem_stats_args_t *m) {
    m->total = 8192 * 1024; m->used = 3500 * 1024; m->free = 4692 * 1024;
    m->largest_free = 4096 * 1024;
    m->used_blocks = 12; m->free_blocks = 3; m->blocks_used = 15; m->blocks_max = 64;
    return true;
}

/* -- FS_RENAME / FS_STAT / FS_LIST_EX (docs/filesystem.md) --
 *
 * The rules that matter to the shell are reproduced: `to` must not
 * exist, a directory cannot go into itself, and /ram is a DIFFERENT
 * VOLUME -- a rename between it and anywhere else is refused with
 * Z_FS_ERR_XDEV, exactly as the kernel refuses one, so fs_move()'s
 * copy-then-unlink fallback (sw/common/zfsutil.c) runs here for real.
 *
 * Every entry reports the same fixed date, 2026-01-02 03:04, so the
 * expected output of `ls -l` does not depend on when the fixture was
 * made. */

#define HOST_FDATE  (((2026 - 1980) << 9) | (1 << 5) | 2)
#define HOST_FTIME  ((3 << 11) | (4 << 5))

static int host_vol(const char *p) {
    return (!strncasecmp(p, "/ram", 4) && (p[4] == 0 || p[4] == '/')) ? 1 : 0;
}

static void host_info(const struct stat *st, z_fs_info_t *fi) {
    memset(fi, 0, sizeof(*fi));
    int dir = S_ISDIR(st->st_mode);
    fi->size = dir ? 0 : (uint32_t)st->st_size;
    fi->type = dir ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
    fi->attr = dir ? Z_FS_ATTR_DIR : Z_FS_ATTR_ARCHIVE;
    fi->fdate = HOST_FDATE;
    fi->ftime = HOST_FTIME;
}

int fs_stat(const char *path, z_fs_info_t *info) {
    struct stat st;
    if (stat(hp(path), &st) != 0) return 0;
    host_info(&st, info);
    return 1;
}

int fs_rename(const char *from, const char *to, int *err) {
    struct stat st;
    char f[1024];
    size_t n = strlen(from);
    int e = Z_FS_ERR_NONE;

    if (host_vol(from) != host_vol(to)) e = Z_FS_ERR_XDEV;
    else if (!strncasecmp(from, to, n) && (to[n] == 0 || to[n] == '/'))
        e = Z_FS_ERR_INVAL;
    else if (stat(hp(from), &st) != 0) e = Z_FS_ERR_NOENT;
    else if (stat(hp(to), &st) == 0) e = Z_FS_ERR_EXIST;
    else {
        snprintf(f, sizeof(f), "%s", hp(from));
        if (rename(f, hp(to)) != 0) e = Z_FS_ERR_IO;
    }
    if (err) *err = e;
    return e == Z_FS_ERR_NONE;
}

int fs_list_ex(const char *path, char *buf, uint32_t cap,
               z_fs_info_t *info, uint32_t max_entries, uint32_t *count,
               uint32_t *truncated) {
    if (!fs_list_into(path, buf, cap, NULL, max_entries, count, truncated))
        return 0;
    if (info) {
        const char *p = buf;
        for (uint32_t i = 0; i < *count; i++) {
            if (fs_stat(p, &info[i]) == 0) memset(&info[i], 0, sizeof(info[i]));
            while (*p) p++;
            p++;
        }
    }
    return 1;
}

