/*
 * nsfs.h on a build machine: a real directory ($NSFS_ROOT) stands in
 * for the card, with FAT's ways kept where they matter to the protocol
 * code -- rename refuses to replace, unlink takes an empty folder, a
 * date with two-second steps. For tests/xfer_host.c and test_xfer.c.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "../nsfs.h"

// Nowhere, until a test names its directory: test_netserve.c and
// test_e2e.c link this without one, and must not reach the host's /home.
static char root[512] = "/nonexistent-nsfs-root";

void nsfs_posix_root(const char *r) { snprintf(root, sizeof(root), "%s", r); }

static void real(char *out, size_t cap, const char *path) {
	snprintf(out, cap, "%s%s", root, path);
}

static int32_t zerr(int e) {
	switch (e) {
	case ENOENT: return -Z_ENOENT;
	case EEXIST: return -Z_EEXIST;
	case ENOTDIR: return -Z_ENOTDIR;
	case EISDIR: return -Z_EISDIR;
	case ENOTEMPTY: return -Z_ENOTEMPTY;
	case ENOSPC: case EFBIG: return -Z_ENOSPC;
	case EROFS: return -Z_EROFS;
	case EACCES: return -Z_EACCES;
	case EPERM: return -Z_EPERM;
	case EBADF: return -Z_EBADF;
	case EMFILE: return -Z_EMFILE;
	case ENAMETOOLONG: return -Z_ENAMETOOLONG;
	case EINVAL: return -Z_EINVAL;
	default: return -Z_EIO;
	}
}

static void info_of(const struct stat *st, z_fs_info_t *fi) {
	struct tm tm;
	time_t t = st->st_mtime;
	memset(fi, 0, sizeof(*fi));
	gmtime_r(&t, &tm);
	fi->type = S_ISDIR(st->st_mode) ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	fi->size = S_ISDIR(st->st_mode) ? 0 : (uint32_t)st->st_size;
	fi->attr = S_ISDIR(st->st_mode) ? Z_FS_ATTR_DIR : Z_FS_ATTR_ARCHIVE;
	if (!(st->st_mode & S_IWUSR)) fi->attr |= Z_FS_ATTR_RDONLY;
	if (tm.tm_year >= 80) {
		fi->fdate = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
		fi->ftime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
	}
}

int32_t nsfs_open(const char *path, uint32_t zf, z_fs_info_t *info) {
	char p[1024];
	struct stat st;
	int f = 0;
	real(p, sizeof(p), path);
	switch (zf & Z_O_ACCMODE) {
	case Z_O_WRONLY: f = O_WRONLY; break;
	case Z_O_RDWR: f = O_RDWR; break;
	default: f = O_RDONLY;
	}
	if (zf & Z_O_CREAT) f |= O_CREAT;
	if (zf & Z_O_TRUNC) f |= O_TRUNC;
	if (zf & Z_O_EXCL) f |= O_EXCL;
	if (zf & Z_O_APPEND) f |= O_APPEND;
	if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return -Z_EISDIR;
	int fd = open(p, f, 0644);
	if (fd < 0) return zerr(errno);
	if (info && fstat(fd, &st) == 0) info_of(&st, info);
	return fd;
}

int32_t nsfs_close(int32_t fd) { return close(fd) ? zerr(errno) : 0; }

int32_t nsfs_read(int32_t fd, void *buf, uint32_t len) {
	ssize_t r = read(fd, buf, len);
	return r < 0 ? zerr(errno) : (int32_t)r;
}

int32_t nsfs_write(int32_t fd, const void *buf, uint32_t len) {
	ssize_t r = write(fd, buf, len);
	return r < 0 ? zerr(errno) : (int32_t)r;
}

int32_t nsfs_seek(int32_t fd, uint32_t off) {
	return lseek(fd, (off_t)off, SEEK_SET) < 0 ? zerr(errno) : 0;
}

int32_t nsfs_fstat(int32_t fd, z_fs_info_t *info) {
	struct stat st;
	if (fstat(fd, &st)) return zerr(errno);
	info_of(&st, info);
	return 0;
}

int32_t nsfs_stat(const char *path, z_fs_info_t *info) {
	char p[1024];
	struct stat st;
	real(p, sizeof(p), path);
	if (stat(p, &st)) return zerr(errno);
	info_of(&st, info);
	return 0;
}

int32_t nsfs_unlink(const char *path) {
	char p[1024];
	struct stat st;
	real(p, sizeof(p), path);
	if (stat(p, &st)) return zerr(errno);
	if (S_ISDIR(st.st_mode)) return rmdir(p) ? zerr(errno) : 0;
	return unlink(p) ? zerr(errno) : 0;
}

int32_t nsfs_mkdir(const char *path) {
	char p[1024];
	real(p, sizeof(p), path);
	return mkdir(p, 0755) ? zerr(errno) : 0;
}

int32_t nsfs_rename(const char *from, const char *to) {
	char a[1024], b[1024];
	struct stat st;
	real(a, sizeof(a), from);
	real(b, sizeof(b), to);
	if (stat(a, &st)) return zerr(errno);
	if (!stat(b, &st)) return -Z_EEXIST;		// FAT's rename replaces nothing
	return rename(a, b) ? zerr(errno) : 0;
}

int32_t nsfs_fsync(int32_t fd) { return fsync(fd) ? zerr(errno) : 0; }
int32_t nsfs_ftruncate(int32_t fd, uint32_t len) { return ftruncate(fd, (off_t)len) ? zerr(errno) : 0; }

int32_t nsfs_statvfs(const char *path, z_fs_vfs_t *out) {
	char p[1024];
	struct statvfs v;
	real(p, sizeof(p), path);
	if (statvfs(p, &v)) return zerr(errno);
	out->total_kb = (uint32_t)((uint64_t)v.f_blocks * v.f_frsize / 1024);
	out->free_kb = (uint32_t)((uint64_t)v.f_bavail * v.f_frsize / 1024);
	return 0;
}

struct nsfs_dir { DIR *d; char path[1024]; };

int32_t nsfs_opendir(const char *path, nsfs_dir_t **out) {
	nsfs_dir_t *h = calloc(1, sizeof(*h));
	real(h->path, sizeof(h->path), path);
	h->d = opendir(h->path);
	if (!h->d) { int32_t e = zerr(errno); free(h); return e; }
	*out = h;
	return 0;
}

int32_t nsfs_readdir(nsfs_dir_t *h, char *name, uint32_t cap, z_fs_info_t *info) {
	struct dirent *e;
	while ((e = readdir(h->d))) {
		char p[2048];
		struct stat st;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;	// as FatFs
		snprintf(p, sizeof(p), "%s/%s", h->path, e->d_name);
		if (stat(p, &st)) continue;
		snprintf(name, cap, "%s", e->d_name);
		info_of(&st, info);
		return 1;
	}
	return 0;
}

void nsfs_closedir(nsfs_dir_t *h) {
	if (!h) return;
	closedir(h->d);
	free(h);
}

uint32_t nsfs_now(void) { return (uint32_t)time(NULL); }
