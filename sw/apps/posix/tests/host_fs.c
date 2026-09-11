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

int fs_unlink(char *path)  { return remove(hp(path)) == 0 ? 0 : -1; }
int fs_mkdir(const char *path) { return mkdir(hp(path), 0777) == 0 ? 0 : -1; }

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
        snprintf(entry, sizeof(entry), "/%s", de->d_name);
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

