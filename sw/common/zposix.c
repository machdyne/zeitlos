/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * POSIX file I/O for apps: newlib's open(), read(), fopen(), fprintf()
 * and the rest reach files, through the filesystem driver `fs`
 * (zfsc.c, zfsproto.h). docs/posix_io.md.
 *
 * Opt in with `POSIX_IO = 1` in an app's Makefile: it links this file
 * and zfsc.o, and puts sw/common/posix (dirent.h) on the include path.
 * newlib's own versions of these calls are weak stubs that fail
 * (zeitlos.c); these are strong and win. An app that does not opt in
 * carries none of it.
 *
 * -- Descriptors --
 *
 * 0, 1 and 2 are the terminal, exactly as without this file: they go
 * to z_term_read()/z_term_write() (zeitlos.c), the hooks or the serial
 * console. From 3 up a descriptor is fs's own, plus 3 -- no table here
 * to keep in step, and no limit but fs's.
 *
 * -- Errors --
 *
 * fs answers -Z_E*, and zfsproto.h chose those numbers to be newlib's
 * errno values, so errno is the result negated.
 *
 * -- The working directory --
 *
 * There was none: a program is started with a name, not a directory.
 * This keeps one per process, "/" at start, so that relative paths and
 * chdir()/getcwd() behave. `..` and `.` are resolved here, since fs
 * takes absolute paths.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "zeitlos.h"
#include "zfsc.h"
#include "posix/dirent.h"

// The host test (sw/apps/fs/tests) builds this with its names changed,
// so the build machine's own C library stays intact underneath.
#ifdef ZPOSIX_HOST_TEST
#define ZP(name) zp_##name
#else
#define ZP(name) name
#endif

#define FD_BASE		3

static char cwd[Z_FS_PATH_MAX] = "/";

static int fail(int32_t r) {
	errno = r < 0 ? (int)-r : EIO;
	return -1;
}

// `path`, made absolute against the working directory, with `.` and
// `..` resolved and repeated slashes dropped. False if too long.
static bool absolute(const char *path, char *out) {
	char tmp[Z_FS_PATH_MAX * 2];
	size_t n = 0, o = 0;
	const char *p;

	if (!path || !path[0]) return false;
	if (path[0] != '/') {
		size_t c = strlen(cwd);
		if (c >= sizeof(tmp)) return false;
		memcpy(tmp, cwd, c);
		n = c;
		tmp[n++] = '/';
	}
	size_t l = strlen(path);
	if (n + l >= sizeof(tmp)) return false;
	memcpy(tmp + n, path, l + 1);

	// Component by component into out.
	out[o++] = '/';
	p = tmp;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char *e = p;
		while (*e && *e != '/') e++;
		size_t len = (size_t)(e - p);
		if (len == 1 && p[0] == '.') {
			// nothing
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			if (o > 1) {
				o--;					// the slash after the last component
				while (o > 1 && out[o - 1] != '/') o--;
			}
		} else {
			if (o + len + 1 >= Z_FS_PATH_MAX) return false;
			memcpy(out + o, p, len);
			o += len;
			out[o++] = '/';
		}
		p = e;
	}
	if (o > 1) o--;						// no trailing slash but on "/"
	out[o] = 0;
	return true;
}

static uint32_t flags_of(int f) {
	uint32_t z = 0;
	switch (f & O_ACCMODE) {
	case O_WRONLY:	z = Z_O_WRONLY; break;
	case O_RDWR:	z = Z_O_RDWR; break;
	default:		z = Z_O_RDONLY; break;
	}
	if (f & O_CREAT) z |= Z_O_CREAT;
	if (f & O_EXCL) z |= Z_O_EXCL;
	if (f & O_TRUNC) z |= Z_O_TRUNC;
	if (f & O_APPEND) z |= Z_O_APPEND;
	return z;
}

static void stat_of(const z_fs_info_t *fi, struct stat *st) {
	memset(st, 0, sizeof(*st));
	if (fi->type == Z_FS_TYPE_DIR) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	} else {
		st->st_mode = S_IFREG | ((fi->attr & Z_FS_ATTR_RDONLY) ? 0444 : 0644);
		st->st_nlink = 1;
		st->st_size = (off_t)fi->size;
	}
	st->st_blksize = 512;
	st->st_blocks = (fi->size + 511) / 512;
	st->st_mtime = st->st_atime = st->st_ctime = (time_t)z_fs_info_time(fi);
}

// -- descriptors --

int ZP(_open)(const char *path, int flags, ...) {
	char abs[Z_FS_PATH_MAX];
	if (!absolute(path, abs)) { errno = ENAMETOOLONG; return -1; }
	int32_t fd = zfs_open(abs, flags_of(flags));
	if (fd < 0) return fail(fd);
	return (int)fd + FD_BASE;
}

ssize_t ZP(_read)(int fd, void *buf, size_t len) {
	if (fd < FD_BASE) return z_term_read(fd, buf, len);
	int32_t n = zfs_read(fd - FD_BASE, buf, (uint32_t)len);
	return n < 0 ? fail(n) : (ssize_t)n;
}

ssize_t ZP(_write)(int fd, const void *buf, size_t len) {
	if (fd < FD_BASE) return z_term_write(fd, buf, len);
	int32_t n = zfs_write(fd - FD_BASE, buf, (uint32_t)len);
	return n < 0 ? fail(n) : (ssize_t)n;
}

int ZP(_close)(int fd) {
	if (fd < FD_BASE) return 0;
	int32_t r = zfs_close(fd - FD_BASE);
	return r < 0 ? fail(r) : 0;
}

off_t ZP(_lseek)(int fd, off_t off, int whence) {
	if (fd < FD_BASE) { errno = ESPIPE; return -1; }
	uint32_t w = whence == SEEK_CUR ? Z_SEEK_CUR :
		whence == SEEK_END ? Z_SEEK_END : Z_SEEK_SET;
	int32_t p = zfs_lseek(fd - FD_BASE, (int32_t)off, w);
	return p < 0 ? fail(p) : (off_t)p;
}

int ZP(_fstat)(int fd, struct stat *st) {
	z_fs_info_t fi;
	if (!st) { errno = EFAULT; return -1; }
	if (fd < FD_BASE) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR | 0666;
		return 0;
	}
	int32_t r = zfs_fstat(fd - FD_BASE, &fi);
	if (r < 0) return fail(r);
	stat_of(&fi, st);
	return 0;
}

int ZP(fsync)(int fd) {
	if (fd < FD_BASE) return 0;
	int32_t r = zfs_fsync(fd - FD_BASE);
	return r < 0 ? fail(r) : 0;
}

int ZP(ftruncate)(int fd, off_t len) {
	if (fd < FD_BASE) { errno = EINVAL; return -1; }
	int32_t r = zfs_ftruncate(fd - FD_BASE, (uint32_t)len);
	return r < 0 ? fail(r) : 0;
}

// -- paths --

int ZP(_stat)(const char *path, struct stat *st) {
	char abs[Z_FS_PATH_MAX];
	z_fs_info_t fi;
	if (!st) { errno = EFAULT; return -1; }
	if (!absolute(path, abs)) { errno = ENAMETOOLONG; return -1; }
	int32_t r = zfs_stat(abs, &fi);
	if (r < 0) return fail(r);
	stat_of(&fi, st);
	return 0;
}

// newlib's stat() does not reach _stat() on every build of it, and
// lstat() is not there at all; the public names, then. No links here,
// so lstat is stat.
int ZP(stat)(const char *path, struct stat *st) { return ZP(_stat)(path, st); }
int ZP(lstat)(const char *path, struct stat *st) { return ZP(_stat)(path, st); }

int ZP(access)(const char *path, int mode) {
	struct stat st;
	if (ZP(_stat)(path, &st) < 0) return -1;
	if ((mode & W_OK) && !(st.st_mode & 0200)) { errno = EACCES; return -1; }
	return 0;
}

int ZP(_unlink)(const char *path) {
	char abs[Z_FS_PATH_MAX];
	if (!absolute(path, abs)) { errno = ENAMETOOLONG; return -1; }
	int32_t r = zfs_unlink(abs);
	return r < 0 ? fail(r) : 0;
}
int ZP(unlink)(const char *path) { return ZP(_unlink)(path); }

int ZP(rmdir)(const char *path) {
	struct stat st;
	if (ZP(_stat)(path, &st) < 0) return -1;
	if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
	return ZP(_unlink)(path);			// fs removes an empty directory
}

int ZP(mkdir)(const char *path, mode_t mode) {
	char abs[Z_FS_PATH_MAX];
	(void)mode;
	if (!absolute(path, abs)) { errno = ENAMETOOLONG; return -1; }
	int32_t r = zfs_mkdir(abs);
	return r < 0 ? fail(r) : 0;
}

int ZP(rename)(const char *from, const char *to) {
	char a[Z_FS_PATH_MAX], b[Z_FS_PATH_MAX];
	if (!absolute(from, a) || !absolute(to, b)) { errno = ENAMETOOLONG; return -1; }
	int32_t r = zfs_rename(a, b);
	return r < 0 ? fail(r) : 0;
}

// No hard links on FAT.
int ZP(_link)(const char *from, const char *to) {
	(void)from; (void)to;
	errno = EMLINK;
	return -1;
}

// -- the working directory --

int ZP(chdir)(const char *path) {
	char abs[Z_FS_PATH_MAX];
	struct stat st;
	if (!absolute(path, abs)) { errno = ENAMETOOLONG; return -1; }
	if (ZP(_stat)(abs, &st) < 0) return -1;
	if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
	memcpy(cwd, abs, strlen(abs) + 1);
	return 0;
}

char *ZP(getcwd)(char *buf, size_t size) {
	size_t n = strlen(cwd) + 1;
	if (!buf) {
		buf = malloc(size > n ? size : n);
		if (!buf) { errno = ENOMEM; return NULL; }
	} else if (size < n) {
		errno = ERANGE;
		return NULL;
	}
	memcpy(buf, cwd, n);
	return buf;
}

// -- directories (posix/dirent.h) --

struct _zeitlos_DIR {
	zfs_dir_t *d;
	struct dirent ent;
};

DIR *ZP(opendir)(const char *path) {
	char abs[Z_FS_PATH_MAX];
	if (!absolute(path, abs)) { errno = ENAMETOOLONG; return NULL; }
	DIR *dp = malloc(sizeof(*dp));
	if (!dp) { errno = ENOMEM; return NULL; }
	dp->d = zfs_opendir(abs);
	if (!dp->d) {
		free(dp);
		errno = ENOENT;
		return NULL;
	}
	if (zfs_dir_error(dp->d) < 0) {
		errno = (int)-zfs_dir_error(dp->d);
		zfs_closedir(dp->d);
		free(dp);
		return NULL;
	}
	return dp;
}

struct dirent *ZP(readdir)(DIR *dp) {
	const z_fs_dirent_t *e;
	if (!dp || !(e = zfs_readdir(dp->d))) return NULL;
	size_t n = e->namelen < sizeof(dp->ent.d_name) - 1 ? e->namelen
		: sizeof(dp->ent.d_name) - 1;
	memcpy(dp->ent.d_name, e->name, n);
	dp->ent.d_name[n] = 0;
	dp->ent.d_type = e->info.type == Z_FS_TYPE_DIR ? DT_DIR : DT_REG;
	dp->ent.d_ino = 1;
	return &dp->ent;
}

int ZP(closedir)(DIR *dp) {
	if (!dp) { errno = EBADF; return -1; }
	zfs_closedir(dp->d);
	free(dp);
	return 0;
}
