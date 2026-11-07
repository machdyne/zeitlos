/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What sftp.c and scp.c share: the byte queues, paths, the .part rule,
 * `ls -l` lines. See xfer.h.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "xfer_int.h"

// -- the generic calls --

void xfer_base_init(xfer_t *x, const char *home, uint32_t flags, xfer_log_fn log, void *user) {
	x->log = log;
	x->user = user;
	x->flags = flags;
	snprintf(x->home, sizeof(x->home), "%s", home && *home ? home : "/");
}

void xfer_logf(xfer_t *x, const char *fmt, ...) {
	char line[XFER_PATH + 96];
	va_list ap;
	if (!x->log) return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	x->log(x->user, line);
}

uint32_t xfer_in_room(const xfer_t *x) { return XFER_IN - x->in_len; }

void xfer_put(xfer_t *x, const uint8_t *d, uint32_t n) {
	if (n > XFER_IN - x->in_len) n = XFER_IN - x->in_len;	// never: the window says so
	uint32_t tail = (x->in_head + x->in_len) % XFER_IN;
	uint32_t first = XFER_IN - tail < n ? XFER_IN - tail : n;
	memcpy(x->in + tail, d, first);
	memcpy(x->in, d + first, n - first);
	x->in_len += n;
}

uint32_t xfer_in_peek(const xfer_t *x, uint8_t *dst, uint32_t n) {
	if (n > x->in_len) n = x->in_len;
	uint32_t first = XFER_IN - x->in_head < n ? XFER_IN - x->in_head : n;
	memcpy(dst, x->in + x->in_head, first);
	memcpy(dst + first, x->in, n - first);
	return n;
}

void xfer_in_take(xfer_t *x, uint32_t n) {
	if (n > x->in_len) n = x->in_len;
	x->in_head = (x->in_head + n) % XFER_IN;
	x->in_len -= n;
	if (!x->in_len) x->in_head = 0;
}

const uint8_t *xfer_in_front(const xfer_t *x, uint32_t *n) {
	uint32_t first = XFER_IN - x->in_head;
	*n = x->in_len < first ? x->in_len : first;
	return x->in + x->in_head;
}

void xfer_eof(xfer_t *x) { x->eof = true; }

uint32_t xfer_step(xfer_t *x, int budget) { return x->step(x, budget); }

const uint8_t *xfer_out(const xfer_t *x, uint32_t *n) {
	*n = x->out_len;
	return x->out;
}

void xfer_sent(xfer_t *x, uint32_t n) {
	if (n > x->out_len) n = x->out_len;
	memmove(x->out, x->out + n, x->out_len - n);
	x->out_len -= n;
}

bool xfer_emit(xfer_t *x, const void *d, uint32_t n) {
	if (n > xfer_out_room(x)) return false;
	memcpy(x->out + x->out_len, d, n);
	x->out_len += n;
	return true;
}

bool xfer_done(const xfer_t *x, uint32_t *status) {
	if (!x->done || x->out_len) return false;
	if (status) *status = x->status;
	return true;
}

void xfer_free(xfer_t *x) {
	if (!x) return;
	x->abort(x);
	memset(x, 0, sizeof(*x));
	free(x);
}

// -- paths --

#define PART_ROOM 5		// ".part"

bool xfer_path(char out[XFER_PATH], const char *home, const char *in, uint32_t n) {
	char buf[2 * XFER_PATH];
	uint32_t hl = 0, len = 0;

	for (uint32_t i = 0; i < n; i++)
		if ((uint8_t)in[i] < 0x20 || in[i] == 0x7f || in[i] == '\\') return false;
	if (n >= XFER_PATH) return false;
	// "~" and "~/..." mean home, as a shell has it
	if (n && in[0] == '~' && (n == 1 || in[1] == '/')) { in++; n--; }
	else if (n && in[0] == '/') home = "";
	hl = (uint32_t)strlen(home);
	memcpy(buf, home, hl);
	buf[hl] = '/';
	memcpy(buf + hl + 1, in, n);
	buf[hl + 1 + n] = 0;

	// Component by component into out: "." dropped, ".." pops.
	const char *p = buf;
	out[0] = 0;
	while (*p) {
		while (*p == '/') p++;
		const char *e = p;
		while (*e && *e != '/') e++;
		uint32_t cl = (uint32_t)(e - p);
		if (!cl || (cl == 1 && p[0] == '.')) { p = e; continue; }
		if (cl == 2 && p[0] == '.' && p[1] == '.') {
			char *slash = strrchr(out, '/');
			if (slash) { *slash = 0; len = (uint32_t)(slash - out); }
			p = e;
			continue;
		}
		if (len + 1 + cl >= XFER_PATH - PART_ROOM) return false;
		out[len++] = '/';
		memcpy(out + len, p, cl);
		len += cl;
		out[len] = 0;
		p = e;
	}
	if (!len) { out[0] = '/'; out[1] = 0; }
	return true;
}

bool xfer_join(char out[XFER_PATH], const char *dir, const char *name) {
	uint32_t dl = (uint32_t)strlen(dir), nl = (uint32_t)strlen(name);
	if (dl == 1 && dir[0] == '/') dl = 0;
	if (!nl || dl + 1 + nl >= XFER_PATH - PART_ROOM) return false;
	if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return false;
	memmove(out, dir, dl);
	out[dl] = '/';
	memcpy(out + dl + 1, name, nl + 1);
	return true;
}

const char *xfer_base(const char *path) {
	const char *s = strrchr(path, '/');
	return (s && s[1]) ? s + 1 : path;
}

const char *xfer_strerror(int32_t err) {
	switch (-err) {
	case Z_ENOENT: return "No such file or directory";
	case Z_EEXIST: return "File exists";
	case Z_ENOTDIR: return "Not a directory";
	case Z_EISDIR: return "Is a directory";
	case Z_ENOTEMPTY: return "Directory not empty";
	case Z_ENOSPC: return "No space left on device";
	case Z_EROFS: return "Read-only file system";
	case Z_EACCES: case Z_EPERM: return "Permission denied";
	case Z_EBUSY: return "In use by another program";
	case Z_EMFILE: return "Too many open files";
	case Z_EXDEV: return "Not on the same volume";
	case Z_ENAMETOOLONG: return "Name too long";
	case Z_EINVAL: return "Invalid name";
	case Z_ENODEV: return "The filesystem is not running";
	case Z_ETIMEDOUT: return "The filesystem did not answer";
	default: return "Input/output error";
	}
}

// -- the .part rule --

static void part_name(char out[XFER_PATH], const char *path) {
	snprintf(out, XFER_PATH, "%s.part", path);
}

int32_t xfer_part_open(const char *path, uint32_t extra_flags) {
	char part[XFER_PATH];
	z_fs_info_t fi;
	part_name(part, path);
	if (nsfs_stat(part, &fi) >= 0) {
		int32_t r = nsfs_unlink(part);		// a stale one, from a transfer that died
		if (r < 0) return r;
	}
	return nsfs_open(part, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC | extra_flags, NULL);
}

int32_t xfer_part_finish(int32_t fd, const char *path, bool *kept) {
	char part[XFER_PATH];
	z_fs_info_t fi;
	int32_t r;
	*kept = false;
	part_name(part, path);
	r = nsfs_close(fd);			// the last of the data, and the size, reach the card
	if (r < 0) { nsfs_unlink(part); return r; }
	// FAT renames onto nothing: the old file goes first.
	if (nsfs_stat(path, &fi) >= 0) {
		if (fi.type == Z_FS_TYPE_DIR) { nsfs_unlink(part); return -Z_EISDIR; }
		r = nsfs_unlink(path);
		if (r < 0) { nsfs_unlink(part); return r; }
	}
	r = nsfs_rename(part, path);
	if (r < 0) *kept = true;		// the old one is gone: the new one stays, as NAME.part
	return r;
}

void xfer_part_abort(int32_t fd, const char *path) {
	char part[XFER_PATH];
	part_name(part, path);
	nsfs_close(fd);
	nsfs_unlink(part);
}

// -- `ls -l` --

static const char *const MON[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

// Days since 1970 to a civil date (Howard Hinnant's algorithm).
static void civil(uint32_t days, uint32_t *y, uint32_t *m, uint32_t *d) {
	int32_t z = (int32_t)days + 719468;
	int32_t era = z / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (uint32_t)(yoe + era * 400) + (*m <= 2);
}

uint32_t xfer_longname(char *out, uint32_t cap, const char *name, uint32_t size,
		bool dir, bool rdonly, uint32_t mtime, uint32_t now) {
	char when[32];
	const char *mode = dir ? "drwxr-xr-x" : rdonly ? "-r--r--r--" : "-rw-r--r--";
	if (!mtime) snprintf(when, sizeof(when), "%s", "Jan  1  1980");
	else {
		uint32_t y, m, d;
		civil(mtime / 86400, &y, &m, &d);
		// within six months of now: the time; otherwise the year, as ls
		if (now && mtime <= now + 3600 && now - mtime < 183u * 86400u)
			snprintf(when, sizeof(when), "%s %2lu %02lu:%02lu", MON[m - 1], (unsigned long)d,
				(unsigned long)(mtime / 3600 % 24), (unsigned long)(mtime / 60 % 60));
		else
			snprintf(when, sizeof(when), "%s %2lu  %4lu", MON[m - 1], (unsigned long)d, (unsigned long)y);
	}
	int n = snprintf(out, cap, "%s    1 zeitlos  zeitlos  %10lu %s %s", mode,
		(unsigned long)size, when, name);
	if (n < 0) n = 0;
	if ((uint32_t)n >= cap) n = (int)cap - 1;
	return (uint32_t)n;
}
