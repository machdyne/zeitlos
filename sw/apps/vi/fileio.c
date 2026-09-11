/*
 * POSIX file descriptors, backed by the Zeitlos filesystem.
 *
 * newlib's _open/_read/_write/_close/_lseek are weak in
 * sw/common/zeitlos.c and have always failed: nothing written for this
 * machine reads a file through newlib, because sw/common/zfsapp.h is
 * the filesystem API and every app calls it directly.
 *
 * Ported code does not know that. nextvi opens its file with open(),
 * reads it with read(), and writes it back with write() -- and got
 * -1 from the first of those, which it reports as an empty buffer.
 *
 * So this is the adapter: a small descriptor table over fs_open_read()
 * and friends. It belongs to this app rather than to the runtime
 * because a real implementation needs zfsapp.o, and linking that into
 * every binary that includes zeitlos.c would put a filesystem in
 * programs that never touch one.
 *
 * -- Descriptors 0, 1 and 2 are not ours --
 *
 * They are the terminal, and _read/_write in zeitlos.c handle them
 * through z_stdin_hook/z_stdout_hook. Everything here starts at 3.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../common/zeitlos.h"
#include "../../common/zfsapp.h"

void uart_putc(char c);     /* sw/common/zeitlos.c */
int16_t uart_getc(void);
bool uart_rx_empty(void);

/* Z_FS_MAX_OPEN is 8 in the kernel (sw/common/zfs.h), so a table
 * larger than that could hand out descriptors that can never be backed
 * by an open file -- the failure would arrive at the first read rather
 * than at the open, which is the wrong end. */
#define FD_BASE  3
#define FD_MAX   8

typedef struct {
    bool     used;
    int      handle;        /* zfsapp handle */
    uint32_t pos;           /* zfsapp has no tell(), so it is tracked here */
    uint32_t size;          /* at open time -- see _fstat() */
    bool     writable;
} fd_t;

static fd_t fds[FD_MAX];

static fd_t *fd_get(int fd) {
    if (fd < FD_BASE || fd >= FD_BASE + FD_MAX) return NULL;
    fd_t *f = &fds[fd - FD_BASE];
    return f->used ? f : NULL;
}

int _open(const char *pathname, int flags, ...) {

    int slot;
    for (slot = 0; slot < FD_MAX; slot++)
        if (!fds[slot].used) break;

    if (slot == FD_MAX) { errno = EMFILE; return -1; }

    int h;
    bool writable = false;

    /*
     * O_RDWR is mapped to fs_open_rw(), which does NOT truncate --
     * that distinction cost a data-loss bug in `posix` when `>>` was
     * built on fs_open_write() and silently replaced a file's contents
     * with NUL padding (docs/posix.md). Getting it right here matters
     * more: this is where an editor saves.
     */
    if ((flags & O_ACCMODE) == O_RDONLY) {
        h = fs_open_read(pathname);
    } else if (flags & O_TRUNC || flags & O_CREAT) {
        h = fs_open_write(pathname);
        writable = true;
    } else {
        h = fs_open_rw(pathname);
        writable = true;
    }

    if (h < 0) { errno = ENOENT; return -1; }

    fds[slot].used = true;
    fds[slot].handle = h;
    fds[slot].pos = 0;
    fds[slot].writable = writable;

    /* Recorded at open, because zfsapp can size a PATH and not an open
     * handle -- and _fstat() has to answer for the handle. See its own
     * comment for why a wrong answer here is expensive. */
    {
        int n = fs_size((char *)pathname);
        fds[slot].size = n > 0 ? (uint32_t)n : 0;
    }

    return FD_BASE + slot;
}

ssize_t _read(int fd, void *ptr, size_t len) {

    /* The terminal, not a file.
     *
     * This file's _read OVERRIDES the weak one in sw/common/zeitlos.c,
     * so it owns descriptors 0-2 as well and has to do what that one
     * did for them -- including the UART fallback when no hook is
     * installed.
     *
     * The first version returned EBADF there, which silently removed
     * the console path's input: `vi` with no posix painted its screen
     * and then received nothing. Overriding a function means taking on
     * everything it did, not just the part being changed. */
    if (fd < FD_BASE) {

        if (fd == 0 && z_stdin_hook)
            return (ssize_t)z_stdin_hook((char *)ptr, (uint32_t)len);

        if (fd != 0) { errno = EBADF; return -1; }

        /* The serial console, one byte at a time -- nextvi's term_read
         * asks for exactly one and blocking here is correct. */
        unsigned char *p = ptr;
        size_t i;
        for (i = 0; i < len; i++) {
            while (uart_rx_empty()) /* wait */;
            int c = uart_getc();
            p[i] = (unsigned char)c;
            if (p[i] == 0x0d) p[i] = 0x0a;   /* ICRNL, as on the port */
            return (ssize_t)(i + 1);
        }
        return (ssize_t)len;
    }

    fd_t *f = fd_get(fd);
    if (!f) { errno = EBADF; return -1; }

    int n = fs_read_chunk(f->handle, ptr, (int)len);
    if (n < 0) { errno = EIO; return -1; }

    f->pos += (uint32_t)n;
    return n;
}

ssize_t _write(int fd, const void *ptr, size_t len) {

    if (fd < FD_BASE) {
        if (fd == 1 && z_stdout_hook) {
            z_stdout_hook((const char *)ptr, (uint32_t)len);
            return (ssize_t)len;
        }
        /* stderr, and stdout with no hook: the serial console, with
         * the same LF->CRLF expansion zeitlos.c's _write does. */
        const unsigned char *p = ptr;
        for (size_t i = 0; i < len; i++) {
            if (p[i] == 0x0a) uart_putc(0x0d);
            uart_putc((char)p[i]);
        }
        return (ssize_t)len;
    }

    fd_t *f = fd_get(fd);
    if (!f) { errno = EBADF; return -1; }
    if (!f->writable) { errno = EBADF; return -1; }

    int n = fs_write_chunk(f->handle, ptr, (int)len);
    if (n < 0) { errno = EIO; return -1; }

    f->pos += (uint32_t)n;
    return n;
}

int _close(int fd) {

    if (fd < FD_BASE) return 0;

    fd_t *f = fd_get(fd);
    if (!f) { errno = EBADF; return -1; }

    fs_close_handle(f->handle);
    f->used = false;
    return 0;
}

off_t _lseek(int fd, off_t off, int whence) {

    if (fd < FD_BASE) return 0;

    fd_t *f = fd_get(fd);
    if (!f) { errno = EBADF; return -1; }

    uint32_t target;

    if (whence == SEEK_SET) target = (uint32_t)off;
    else if (whence == SEEK_CUR) target = f->pos + (uint32_t)off;
    else {
        /* SEEK_END. zfsapp has no size-of-open-handle call, so this
         * cannot be answered without the path -- and nextvi does not
         * use it. Failing is better than seeking somewhere plausible
         * and wrong. */
        errno = EINVAL;
        return -1;
    }

    if (fs_seek(f->handle, target) < 0) { errno = EIO; return -1; }
    f->pos = target;
    return (off_t)target;
}

/*
 * fstat, and st_size MATTERS.
 *
 * The first version left st_size at 0, on the reasoning that zfsapp
 * can size a path and not an open handle, and that nextvi reads until
 * read() returns 0 so would not care.
 *
 * It cares a great deal. lbuf_rd() (sw/ext/nextvi/lbuf.c) sizes its
 * read buffer from fstat and falls back to **1048575 bytes** when the
 * size is 0 -- so every file opened allocated a megabyte, and `vi`
 * needed a 2MB memory tier to start. The size is recorded at open()
 * instead, where there is still a path to ask about.
 *
 * The general lesson is worth more than the fix: a stub that returns
 * "no information" is not neutral. Callers have fallbacks, and a
 * fallback chosen for a hosted system can be wildly wrong here.
 */
int _fstat(int fd, struct stat *st) {

    if (!st) { errno = EFAULT; return -1; }

    memset(st, 0, sizeof(*st));

    if (fd < FD_BASE) {
        st->st_mode = S_IFCHR;
        return 0;
    }

    fd_t *f = fd_get(fd);
    if (!f) { errno = EBADF; return -1; }

    st->st_mode = S_IFREG;
    st->st_size = (off_t)f->size;
    return 0;
}
