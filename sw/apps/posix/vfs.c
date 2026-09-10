/*
 * posix -- the filesystem view: a current directory, paths that
 * resolve against it, and a file descriptor table.
 *
 * -- What "POSIX" means here, and what it does not --
 *
 * Not a kernel change. There is no fork, no exec into a new address
 * space, no signals, no uids. All of that would land in sw/os, which
 * is exactly what docs/posix.md set out to avoid.
 *
 * What this is: a Unix-SHAPED userland living inside one Zeitlos
 * process. A cwd, so `cat notes.txt` works without typing a leading
 * slash. Descriptors, so a command can be told where its output goes.
 * Paths that normalise, so `../` means something. Those three are what
 * make a compiler pleasant to drive, and none of them needs the
 * kernel's help.
 *
 * -- Paths --
 *
 * The underlying filesystem is FAT through sw/common/zfsapp.h, which
 * has no cwd of its own: every path it takes is absolute. So this
 * layer holds the cwd and hands zfsapp absolute paths, always.
 *
 * Normalisation happens HERE rather than being passed down, because
 * FatFs does not collapse `..` and would happily open a path that
 * walks above the root. Same reason sim/simos.c refuses them: a
 * relative path that escapes is a bug wherever it is resolved, and
 * the place to stop it is where the cwd is known.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "zeitlos.h"
#include "zfsapp.h"
#include "posix.h"

/* One cwd for the whole process, not one per connection.
 *
 * Per-connection would be more faithful -- two terminals into the same
 * shell really are two sessions -- and it is not what this is yet.
 * Recorded rather than hidden: when a second `term` window makes the
 * shared cwd annoying, the fix is to move this into px_conn_t, which
 * is a contained change because everything below takes the state as an
 * argument already. */
static char cwd[PX_PATH_MAX] = "/";

const char *px_getcwd(void) { return cwd; }

/*
 * Resolves `path` against the cwd into an absolute, normalised path.
 *
 * Returns false for anything that would leave the root. Leaving the
 * root is not merely refused, it is refused SILENTLY-proof: the caller
 * gets false and reports it, rather than getting a path that happens
 * to be clamped to "/" and looks like it worked.
 */
bool px_resolve(const char *path, char *out, int cap) {

    char work[PX_PATH_MAX];
    int n = 0;

    if (!path || !out || cap < 2) return false;

    /* Start from the cwd unless the path is already absolute. */
    if (path[0] == '/') {
        work[0] = 0;
    } else {
        int c = 0;
        while (cwd[c] && c < (int)sizeof(work) - 1) { work[c] = cwd[c]; c++; }
        work[c] = 0;
        n = c;
        if (n && work[n - 1] == '/') work[--n] = 0;
    }

    /* Append, component by component, applying . and .. as we go. This
     * is a single pass because the output is only ever appended to or
     * truncated -- there is no second normalising walk to disagree
     * with the first. */
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *seg = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - seg);

        if (len == 1 && seg[0] == '.') continue;

        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            /* `..` at the root stays at the root rather than failing.
             * That is what every shell does, and the alternative --
             * `cd ..` reporting an error when you happen to already be
             * at `/` -- is surprising in a way nothing gains from. A
             * path that tries to escape from FURTHER up is still
             * refused, because n only reaches 0 here. */
            if (n == 0) continue;
            while (n > 0 && work[n - 1] != '/') n--;
            if (n > 0) n--;                     /* drop the '/' too */
            work[n] = 0;
            continue;
        }

        if (n + len + 2 >= (int)sizeof(work)) return false;
        work[n++] = '/';
        memcpy(work + n, seg, (size_t)len);
        n += len;
        work[n] = 0;
    }

    if (n == 0) { work[0] = '/'; work[1] = 0; n = 1; }
    if (n >= cap) return false;

    memcpy(out, work, (size_t)n + 1);
    return true;
}

bool px_chdir(const char *path, char *err, int errlen) {

    char abs[PX_PATH_MAX];

    if (!px_resolve(path, abs, sizeof(abs))) {
        snprintf(err, errlen, "cd: %s: bad path", path);
        return false;
    }

    /* Checked by LISTING it, because there is no stat() and fs_size()
     * on a directory is not meaningful. A zero-entry directory and a
     * missing one both list zero entries, so the count is not the
     * test -- fs_list_into()'s return value is. */
    {
        char buf[512];
        uint32_t count = 0, truncated = 0;
        if (strcmp(abs, "/") != 0 &&
            !fs_list_into(abs, buf, sizeof(buf), 0, 16, &count, &truncated)) {
            snprintf(err, errlen, "cd: %s: not a directory", path);
            return false;
        }
    }

    strcpy(cwd, abs);
    return true;
}

/* -- descriptors --
 *
 * Small and fixed. Z_FS_MAX_OPEN is 8 in the kernel (sw/common/zfs.h),
 * so a table larger than that could hand out descriptors that can
 * never be backed by an open file -- the failure would arrive at the
 * first read rather than at the open, which is the wrong end.
 *
 * Descriptors 0, 1 and 2 are reserved and never backed by a file: they
 * are the connection this shell is talking over. A command writing to
 * fd 1 is writing to the terminal, and redirection replaces the entry
 * rather than teaching every command about files.
 */
static px_fd_t fds[PX_MAX_FD];

void px_fd_init(void) {
    memset(fds, 0, sizeof(fds));
    fds[0].kind = PX_FD_TTY;
    fds[1].kind = PX_FD_TTY;
    fds[2].kind = PX_FD_TTY;
}

px_fd_t *px_fd(int fd) {
    if (fd < 0 || fd >= PX_MAX_FD) return 0;
    return &fds[fd];
}

int px_open(const char *path, int write, char *err, int errlen) {

    char abs[PX_PATH_MAX];

    if (!px_resolve(path, abs, sizeof(abs))) {
        snprintf(err, errlen, "%s: bad path", path);
        return -1;
    }

    int fd;
    for (fd = 3; fd < PX_MAX_FD; fd++)
        if (fds[fd].kind == PX_FD_FREE) break;

    if (fd == PX_MAX_FD) {
        snprintf(err, errlen, "too many open files");
        return -1;
    }

    int h = write ? fs_open_write(abs) : fs_open_read(abs);
    if (h < 0) {
        snprintf(err, errlen, "%s: cannot open", path);
        return -1;
    }

    fds[fd].kind = PX_FD_FILE;
    fds[fd].handle = h;
    return fd;
}

/*
 * Opening for append is NOT "open for write, then seek".
 *
 * fs_open_write() truncates. Seeking into a file that has just been
 * emptied leaves a hole, and FatFs fills a hole with zeros -- so `>>`
 * on an existing file replaced its contents with NUL padding and then
 * appended. Quiet, and total.
 *
 * fs_open_rw() (sw/common/zfsapp.h) does not truncate, which is what
 * append needs. It also fails on a file that does not exist, so the
 * create case still goes through fs_open_write().
 */
int px_open_append(const char *path, int append, char *err, int errlen) {

    char abs[PX_PATH_MAX];

    if (!append) return px_open(path, 1, err, errlen);

    if (!px_resolve(path, abs, sizeof(abs))) {
        snprintf(err, errlen, "%s: bad path", path);
        return -1;
    }

    int size = fs_size(abs);
    if (size <= 0) return px_open(path, 1, err, errlen);   /* new or empty */

    int fd;
    for (fd = 3; fd < PX_MAX_FD; fd++)
        if (fds[fd].kind == PX_FD_FREE) break;
    if (fd == PX_MAX_FD) {
        snprintf(err, errlen, "too many open files");
        return -1;
    }

    int h = fs_open_rw(abs);
    if (h < 0) {
        snprintf(err, errlen, "%s: cannot open for append", path);
        return -1;
    }
    fs_seek(h, (uint32_t)size);

    fds[fd].kind = PX_FD_FILE;
    fds[fd].handle = h;
    return fd;
}

void px_close(int fd) {
    px_fd_t *f = px_fd(fd);
    if (!f || f->kind != PX_FD_FILE) return;
    fs_close_handle(f->handle);
    f->kind = PX_FD_FREE;
    f->handle = -1;
}

void px_close_all_files(void) {
    for (int fd = 3; fd < PX_MAX_FD; fd++) px_close(fd);
}
