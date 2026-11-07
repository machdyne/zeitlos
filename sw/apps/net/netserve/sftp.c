/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * An SFTP server: version 3 of the protocol (draft-ietf-secsh-filexfer-
 * 02), the one OpenSSH, PuTTY's psftp, WinSCP, FileZilla and Cyberduck
 * all speak, with the OpenSSH extensions those clients look for. See
 * xfer.h for how it sits between the SSH engine and the files, and
 * docs/netserve.md, "Copying files: sftp and scp".
 *
 * -- Requests in, a few at a time --
 *
 * A request is assembled from the input ring into `req` and answered,
 * at most `budget` of them a step. Every request but WRITE is small (a
 * path or two); WRITE's data can be up to MAX_WRITE bytes, more than any
 * buffer here, so only its header is assembled and the data is written
 * to the file as it arrives -- the input ring is what the client may
 * send ahead, and each part is passed on as soon as it is there.
 *
 * A request waits, unanswered, until there is room for its whole reply:
 * READ's data goes straight from the file into the output.
 *
 * -- Limits, and saying so --
 *
 * limits@openssh.com tells a client what we take: reads of MAX_READ,
 * writes of MAX_WRITE, SFTP_FILES files open. A client that does not
 * ask is still served: a longer READ gets a short answer (allowed by the
 * protocol, and every client asks again for the rest), a longer WRITE is
 * streamed like any other, and a request that is too long for `req` --
 * which nothing ordinary sends -- is answered FAILURE and skipped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xfer_int.h"

#define FXP_INIT           1
#define FXP_VERSION        2
#define FXP_OPEN           3
#define FXP_CLOSE          4
#define FXP_READ           5
#define FXP_WRITE          6
#define FXP_LSTAT          7
#define FXP_FSTAT          8
#define FXP_SETSTAT        9
#define FXP_FSETSTAT      10
#define FXP_OPENDIR       11
#define FXP_READDIR       12
#define FXP_REMOVE        13
#define FXP_MKDIR         14
#define FXP_RMDIR         15
#define FXP_REALPATH      16
#define FXP_STAT          17
#define FXP_RENAME        18
#define FXP_READLINK      19
#define FXP_SYMLINK       20
#define FXP_STATUS       101
#define FXP_HANDLE       102
#define FXP_DATA         103
#define FXP_NAME         104
#define FXP_ATTRS        105
#define FXP_EXTENDED     200
#define FXP_EXTENDED_REPLY 201

#define FX_OK              0
#define FX_EOF             1
#define FX_NO_SUCH_FILE    2
#define FX_PERMISSION_DENIED 3
#define FX_FAILURE         4
#define FX_BAD_MESSAGE     5
#define FX_OP_UNSUPPORTED  8

#define PF_READ     0x01
#define PF_WRITE    0x02
#define PF_APPEND   0x04
#define PF_CREAT    0x08
#define PF_TRUNC    0x10
#define PF_EXCL     0x20

#define A_SIZE      0x00000001u
#define A_UIDGID    0x00000002u
#define A_PERM      0x00000004u
#define A_TIME      0x00000008u
#define A_EXT       0x80000000u

#define SFTP_FILES  4               // files open at once, per session
#define SFTP_DIRS   2               // folders being listed
#define REQ_MAX     1100            // any request but WRITE's data: two paths and change
#define MAX_READ    2048            // the data in one DATA reply
#define MAX_WRITE   32768           // advertised; any length is streamed
#define MAX_PACKET  (MAX_WRITE + 1024)	// advertised
#define MAX_ACCEPT  (256u * 1024 + 1024)	// taken: OpenSSH's own limit, for clients that do not ask
#define HANDLE_LEN  4               // 'F'/'D', index, generation (2)

typedef struct {
	bool used;
	uint16_t gen;
	int32_t fd;
	bool part;                      // writing NAME.part, to become NAME at CLOSE
	bool wrote, append, failed;
	uint32_t pos;                   // where fd is: a seek only when a request is elsewhere
	uint32_t moved;                 // bytes read or written, for the console
	char path[XFER_PATH];
} fh_t;

typedef struct {
	bool used;
	uint16_t gen;
	nsfs_dir_t *d;
	bool ended;
	bool held;                      // an entry read but not yet sent
	char name[XFER_PATH];
	z_fs_info_t info;
} dh_t;

typedef struct {
	xfer_t x;
	bool inited;
	uint8_t req[REQ_MAX];
	uint32_t req_have;
	uint32_t pkt_len;               // the request being assembled; 0 between requests
	bool have_len;
	// a WRITE's data, on its way to the file
	bool wstream;
	uint32_t wid, wleft;
	int32_t werr;
	fh_t *wfh;
	// the rest of a request too long to assemble
	uint32_t skip;
	fh_t fh[SFTP_FILES];
	dh_t dh[SFTP_DIRS];
	uint16_t gen;
} sftp_t;

// -- wire helpers --

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct { const uint8_t *p; uint32_t n; bool bad; } rd_t;

static uint8_t rd_u8(rd_t *r) {
	if (r->n < 1) { r->bad = true; return 0; }
	r->n--;
	return *r->p++;
}

static uint32_t rd_u32(rd_t *r) {
	if (r->n < 4) { r->bad = true; r->n = 0; return 0; }
	uint32_t v = be32(r->p);
	r->p += 4; r->n -= 4;
	return v;
}

static uint64_t rd_u64(rd_t *r) {
	uint64_t hi = rd_u32(r);
	return (hi << 32) | rd_u32(r);
}

static const uint8_t *rd_str(rd_t *r, uint32_t *len) {
	uint32_t l = rd_u32(r);
	if (r->bad || l > r->n) { r->bad = true; r->n = 0; *len = 0; return (const uint8_t *)""; }
	const uint8_t *s = r->p;
	r->p += l; r->n -= l;
	*len = l;
	return s;
}

// Skips an ATTRS, keeping the size if there is one.
static uint32_t rd_attrs(rd_t *r, bool *has_size, uint64_t *size) {
	uint32_t f = rd_u32(r);
	*has_size = false;
	if (f & A_SIZE) { *size = rd_u64(r); *has_size = true; }
	if (f & A_UIDGID) { rd_u32(r); rd_u32(r); }
	if (f & A_PERM) rd_u32(r);
	if (f & A_TIME) { rd_u32(r); rd_u32(r); }
	if (f & A_EXT) {
		uint32_t n = rd_u32(r), l;
		while (n-- && !r->bad) { rd_str(r, &l); rd_str(r, &l); }
	}
	return f;
}

// A reply is built in place at the end of the output, its length
// filled in when it is finished.
typedef struct { uint8_t *p; uint32_t n, cap; bool bad; } wr_t;

static void wr_bytes(wr_t *w, const void *d, uint32_t n) {
	if (w->bad || w->n + n > w->cap) { w->bad = true; return; }
	memcpy(w->p + w->n, d, n);
	w->n += n;
}

static void wr_u8(wr_t *w, uint8_t v) { wr_bytes(w, &v, 1); }

static void wr_u32(wr_t *w, uint32_t v) {
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	wr_bytes(w, b, 4);
}

static void wr_u64(wr_t *w, uint64_t v) { wr_u32(w, (uint32_t)(v >> 32)); wr_u32(w, (uint32_t)v); }

static void wr_str(wr_t *w, const void *s, uint32_t n) { wr_u32(w, n); wr_bytes(w, s, n); }
static void wr_cstr(wr_t *w, const char *s) { wr_str(w, s, (uint32_t)strlen(s)); }

static void begin(sftp_t *s, wr_t *w, uint8_t type) {
	w->p = s->x.out + s->x.out_len;
	w->cap = xfer_out_room(&s->x);
	w->n = 0;
	w->bad = false;
	wr_u32(w, 0);
	wr_u8(w, type);
}

static bool finish(sftp_t *s, wr_t *w) {
	if (w->bad) return false;
	uint32_t len = w->n - 4;
	w->p[0] = (uint8_t)(len >> 24); w->p[1] = (uint8_t)(len >> 16);
	w->p[2] = (uint8_t)(len >> 8);  w->p[3] = (uint8_t)len;
	s->x.out_len += w->n;
	return true;
}

static void status(sftp_t *s, uint32_t id, uint32_t code, const char *msg) {
	wr_t w;
	begin(s, &w, FXP_STATUS);
	wr_u32(&w, id);
	wr_u32(&w, code);
	wr_cstr(&w, msg);
	wr_cstr(&w, "en");
	finish(s, &w);
}

static uint32_t code_of(int32_t err) {
	switch (-err) {
	case Z_ENOENT: case Z_ENOTDIR: return FX_NO_SUCH_FILE;
	case Z_EACCES: case Z_EPERM: case Z_EROFS: return FX_PERMISSION_DENIED;
	default: return FX_FAILURE;
	}
}

static void status_err(sftp_t *s, uint32_t id, int32_t err) {
	status(s, id, code_of(err), xfer_strerror(err));
}

static void readonly(sftp_t *s, uint32_t id) {
	status(s, id, FX_PERMISSION_DENIED, "Read-only: apps.netserve.sftp is \"read\"");
}

static void wr_attrs(wr_t *w, const z_fs_info_t *fi) {
	uint32_t t = z_fs_info_time(fi);
	bool dir = fi->type == Z_FS_TYPE_DIR;
	wr_u32(w, A_SIZE | A_PERM | A_TIME);
	wr_u64(w, dir ? 0 : fi->size);
	wr_u32(w, dir ? 040755u : (fi->attr & Z_FS_ATTR_RDONLY) ? 0100444u : 0100644u);
	wr_u32(w, t);
	wr_u32(w, t);
}

// -- handles --

static void wr_handle(wr_t *w, char kind, int idx, uint16_t gen) {
	uint8_t h[HANDLE_LEN] = { (uint8_t)kind, (uint8_t)idx, (uint8_t)(gen >> 8), (uint8_t)gen };
	wr_str(w, h, HANDLE_LEN);
}

static fh_t *file_of(sftp_t *s, const uint8_t *h, uint32_t n) {
	if (n != HANDLE_LEN || h[0] != 'F' || h[1] >= SFTP_FILES) return NULL;
	fh_t *f = &s->fh[h[1]];
	return (f->used && f->gen == (uint16_t)((h[2] << 8) | h[3])) ? f : NULL;
}

static dh_t *dir_of(sftp_t *s, const uint8_t *h, uint32_t n) {
	if (n != HANDLE_LEN || h[0] != 'D' || h[1] >= SFTP_DIRS) return NULL;
	dh_t *d = &s->dh[h[1]];
	return (d->used && d->gen == (uint16_t)((h[2] << 8) | h[3])) ? d : NULL;
}

// The path in a request, made absolute. False (with the reply sent) if
// it is not one this server can use.
static bool path_arg(sftp_t *s, rd_t *r, uint32_t id, char out[XFER_PATH]) {
	uint32_t n;
	const uint8_t *p = rd_str(r, &n);
	if (r->bad) { status(s, id, FX_BAD_MESSAGE, "Malformed request"); return false; }
	if (!xfer_path(out, s->x.home, (const char *)p, n)) {
		status(s, id, FX_FAILURE, "Name too long, or not a name this machine takes");
		return false;
	}
	return true;
}

static bool can_write(sftp_t *s, uint32_t id) {
	if (!(s->x.flags & XFER_READONLY)) return true;
	readonly(s, id);
	return false;
}

// Moves fd to `off`, if it is not there.
static int32_t seek_to(fh_t *f, uint32_t off) {
	if (f->pos == off) return 0;
	int32_t r = nsfs_seek(f->fd, off);
	if (r >= 0) f->pos = off;
	return r;
}

static void close_file(sftp_t *s, fh_t *f, bool abort) {
	if (f->part) {
		if (abort) {
			xfer_part_abort(f->fd, f->path);
			xfer_logf(&s->x, "sftp: %s not received in full -- the old file is as it was", f->path);
		}
	} else nsfs_close(f->fd);
	f->used = false;
}

// -- the requests --

static void do_init(sftp_t *s) {
	wr_t w;
	begin(s, &w, FXP_VERSION);
	wr_u32(&w, 3);
	wr_cstr(&w, "posix-rename@openssh.com"); wr_cstr(&w, "1");
	wr_cstr(&w, "statvfs@openssh.com"); wr_cstr(&w, "2");
	wr_cstr(&w, "fsync@openssh.com"); wr_cstr(&w, "1");
	wr_cstr(&w, "limits@openssh.com"); wr_cstr(&w, "1");
	finish(s, &w);
	s->inited = true;
}

static void do_open(sftp_t *s, rd_t *r, uint32_t id) {
	char path[XFER_PATH];
	z_fs_info_t fi;
	bool hs;
	uint64_t sz;
	int slot = -1;

	if (!path_arg(s, r, id, path)) return;
	uint32_t pf = rd_u32(r);
	rd_attrs(r, &hs, &sz);
	if (r->bad) { status(s, id, FX_BAD_MESSAGE, "Malformed request"); return; }
	if ((pf & (PF_WRITE | PF_APPEND | PF_CREAT | PF_TRUNC)) && !can_write(s, id)) return;
	for (int i = 0; i < SFTP_FILES; i++) if (!s->fh[i].used) { slot = i; break; }
	if (slot < 0) { status(s, id, FX_FAILURE, "Too many open files"); return; }

	bool exists = nsfs_stat(path, &fi) >= 0;
	if (exists && fi.type == Z_FS_TYPE_DIR) { status(s, id, FX_FAILURE, "Is a directory"); return; }
	if (exists && (pf & PF_CREAT) && (pf & PF_EXCL)) { status(s, id, FX_FAILURE, "File exists"); return; }
	if (!exists && !(pf & PF_CREAT)) { status(s, id, FX_NO_SUCH_FILE, "No such file or directory"); return; }

	fh_t *f = &s->fh[slot];
	memset(f, 0, sizeof(*f));
	snprintf(f->path, sizeof(f->path), "%s", path);
	int32_t fd;
	if ((pf & PF_WRITE) && ((pf & PF_TRUNC) || !exists)) {
		// The whole file is being sent: NAME.part, renamed at CLOSE.
		fd = xfer_part_open(path, 0);
		f->part = true;
	} else {
		uint32_t zf = (pf & PF_WRITE) ? ((pf & PF_READ) ? Z_O_RDWR : Z_O_WRONLY) : Z_O_RDONLY;
		if (pf & PF_CREAT) zf |= Z_O_CREAT;
		fd = nsfs_open(path, zf, NULL);
	}
	if (fd < 0) { status_err(s, id, fd); return; }
	f->fd = fd;
	f->used = true;
	f->gen = ++s->gen;
	f->append = (pf & PF_APPEND) != 0;
	wr_t w;
	begin(s, &w, FXP_HANDLE);
	wr_u32(&w, id);
	wr_handle(&w, 'F', slot, f->gen);
	finish(s, &w);
}

static void do_close(sftp_t *s, rd_t *r, uint32_t id) {
	uint32_t n;
	const uint8_t *h = rd_str(r, &n);
	fh_t *f = file_of(s, h, n);
	dh_t *d = dir_of(s, h, n);
	if (d) {
		nsfs_closedir(d->d);
		d->used = false;
		status(s, id, FX_OK, "Success");
		return;
	}
	if (!f) { status(s, id, FX_FAILURE, "No such handle"); return; }
	if (f->part) {
		bool kept = false;
		int32_t rc = f->failed ? -Z_EIO : xfer_part_finish(f->fd, f->path, &kept);
		if (f->failed) xfer_part_abort(f->fd, f->path);
		f->used = false;
		if (rc < 0) {
			char m[XFER_PATH + 64];
			if (kept) snprintf(m, sizeof(m), "Received, but kept as %s.part: %s", f->path, xfer_strerror(rc));
			else snprintf(m, sizeof(m), "%s -- the old file is as it was", xfer_strerror(rc));
			xfer_logf(&s->x, "sftp: %s: %s", f->path, m);
			status(s, id, FX_FAILURE, m);
			return;
		}
		xfer_logf(&s->x, "sftp: received %s (%lu bytes)", f->path, (unsigned long)f->moved);
		status(s, id, FX_OK, "Success");
		return;
	}
	int32_t rc = nsfs_close(f->fd);
	f->used = false;
	if (f->wrote) xfer_logf(&s->x, "sftp: changed %s", f->path);
	else if (f->moved) xfer_logf(&s->x, "sftp: sent %s (%lu bytes)", f->path, (unsigned long)f->moved);
	if (rc < 0) status_err(s, id, rc);
	else status(s, id, FX_OK, "Success");
}

static void do_read(sftp_t *s, rd_t *r, uint32_t id) {
	uint32_t n;
	const uint8_t *h = rd_str(r, &n);
	uint64_t off = rd_u64(r);
	uint32_t len = rd_u32(r);
	fh_t *f = file_of(s, h, n);
	if (r->bad) { status(s, id, FX_BAD_MESSAGE, "Malformed request"); return; }
	if (!f) { status(s, id, FX_FAILURE, "No such handle"); return; }
	if (off >= 0xFFFFFFFFull) { status(s, id, FX_EOF, "End of file"); return; }
	// header: length, type, id, data length
	uint32_t room = xfer_out_room(&s->x) - 13;
	if (len > MAX_READ) len = MAX_READ;
	if (len > room) len = room;
	int32_t rc = seek_to(f, (uint32_t)off);
	if (rc >= 0) rc = nsfs_read(f->fd, s->x.out + s->x.out_len + 13, len);
	if (rc < 0) { f->pos = 0xFFFFFFFFu; status_err(s, id, rc); return; }
	f->pos += (uint32_t)rc;
	if (!rc) { status(s, id, FX_EOF, "End of file"); return; }
	f->moved += (uint32_t)rc;
	wr_t w;
	begin(s, &w, FXP_DATA);
	wr_u32(&w, id);
	wr_u32(&w, (uint32_t)rc);
	w.n += (uint32_t)rc;            // already there, read in place
	finish(s, &w);
}

// WRITE's header; the data follows, through write_data().
static void do_write(sftp_t *s, rd_t *r, uint32_t id) {
	uint32_t n;
	const uint8_t *h = rd_str(r, &n);
	uint64_t off = rd_u64(r);
	uint32_t dlen = rd_u32(r);
	fh_t *f = file_of(s, h, n);

	s->wstream = true;
	s->wid = id;
	s->wleft = dlen;
	s->wfh = f;
	s->werr = 0;
	if (!f) { s->werr = -Z_EBADF; return; }
	if (off + dlen > 0xFFFFFFFFull) { s->werr = -Z_ENOSPC; return; }	// FAT's limit
	uint32_t at = (uint32_t)off;
	if (f->append) {
		z_fs_info_t fi;
		if (nsfs_fstat(f->fd, &fi) >= 0) at = fi.size;
	}
	s->werr = seek_to(f, at);
	if (s->werr < 0) f->pos = 0xFFFFFFFFu;
}

static void write_done(sftp_t *s) {
	fh_t *f = s->wfh;
	s->wstream = false;
	if (s->werr == -Z_EBADF && !f) { status(s, s->wid, FX_FAILURE, "No such handle"); return; }
	if (s->werr < 0) {
		if (f) f->failed = true;		// a .part with a hole is never put in place
		status_err(s, s->wid, s->werr);
		return;
	}
	status(s, s->wid, FX_OK, "Success");
}

// As much of the WRITE's data as has arrived. Returns bytes used.
static uint32_t write_data(sftp_t *s) {
	uint32_t n;
	const uint8_t *p = xfer_in_front(&s->x, &n);
	if (n > s->wleft) n = s->wleft;
	if (!n) return 0;
	if (!s->werr && s->wfh) {
		int32_t rc = nsfs_write(s->wfh->fd, p, n);
		if (rc < 0) { s->werr = rc; s->wfh->pos = 0xFFFFFFFFu; }
		else if ((uint32_t)rc < n) { s->werr = -Z_ENOSPC; s->wfh->pos = 0xFFFFFFFFu; }
		else { s->wfh->pos += n; s->wfh->moved += n; s->wfh->wrote = true; }
	}
	xfer_in_take(&s->x, n);
	s->wleft -= n;
	if (!s->wleft) write_done(s);
	return n;
}

static void reply_attrs(sftp_t *s, uint32_t id, const z_fs_info_t *fi) {
	wr_t w;
	begin(s, &w, FXP_ATTRS);
	wr_u32(&w, id);
	wr_attrs(&w, fi);
	finish(s, &w);
}

static void do_stat(sftp_t *s, rd_t *r, uint32_t id) {
	char path[XFER_PATH];
	z_fs_info_t fi;
	if (!path_arg(s, r, id, path)) return;
	int32_t rc = nsfs_stat(path, &fi);
	if (rc < 0) status_err(s, id, rc);
	else reply_attrs(s, id, &fi);
}

static void do_fstat(sftp_t *s, rd_t *r, uint32_t id) {
	uint32_t n;
	z_fs_info_t fi;
	const uint8_t *h = rd_str(r, &n);
	fh_t *f = file_of(s, h, n);
	if (!f) { status(s, id, FX_FAILURE, "No such handle"); return; }
	int32_t rc = nsfs_fstat(f->fd, &fi);
	if (rc < 0) status_err(s, id, rc);
	else reply_attrs(s, id, &fi);
}

// Only the size can be changed; the times and permissions a client
// sends (`put -p`) are taken and not kept -- FAT has a date, which the
// card's clock sets, and one read-only bit.
static void do_setstat(sftp_t *s, rd_t *r, uint32_t id, bool by_handle) {
	char path[XFER_PATH];
	fh_t *f = NULL;
	bool hs;
	uint64_t sz = 0;
	if (by_handle) {
		uint32_t n;
		const uint8_t *h = rd_str(r, &n);
		f = file_of(s, h, n);
		if (!f) { status(s, id, FX_FAILURE, "No such handle"); return; }
	} else if (!path_arg(s, r, id, path)) return;
	rd_attrs(r, &hs, &sz);
	if (r->bad) { status(s, id, FX_BAD_MESSAGE, "Malformed request"); return; }
	if (!hs) { status(s, id, FX_OK, "Success"); return; }
	if (!can_write(s, id)) return;
	if (sz > 0xFFFFFFFFull) { status(s, id, FX_FAILURE, "File too large"); return; }
	int32_t rc;
	if (f) {
		rc = nsfs_ftruncate(f->fd, (uint32_t)sz);
		f->pos = 0xFFFFFFFFu;
		f->wrote = true;
	} else {
		int32_t fd = nsfs_open(path, Z_O_WRONLY, NULL);
		rc = fd;
		if (fd >= 0) {
			rc = nsfs_ftruncate(fd, (uint32_t)sz);
			int32_t c = nsfs_close(fd);
			if (rc >= 0) rc = c;
		}
	}
	if (rc < 0) status_err(s, id, rc);
	else status(s, id, FX_OK, "Success");
}

static void do_opendir(sftp_t *s, rd_t *r, uint32_t id) {
	char path[XFER_PATH];
	int slot = -1;
	if (!path_arg(s, r, id, path)) return;
	for (int i = 0; i < SFTP_DIRS; i++) if (!s->dh[i].used) { slot = i; break; }
	if (slot < 0) { status(s, id, FX_FAILURE, "Too many folders open"); return; }
	dh_t *d = &s->dh[slot];
	memset(d, 0, sizeof(*d));
	int32_t rc = nsfs_opendir(path, &d->d);
	if (rc < 0) { status_err(s, id, rc); return; }
	d->used = true;
	d->gen = ++s->gen;
	wr_t w;
	begin(s, &w, FXP_HANDLE);
	wr_u32(&w, id);
	wr_handle(&w, 'D', slot, d->gen);
	finish(s, &w);
}

// As many entries as fit the room there is; the one that does not fit
// is held for the next READDIR.
static void do_readdir(sftp_t *s, rd_t *r, uint32_t id) {
	uint32_t n, count = 0;
	const uint8_t *h = rd_str(r, &n);
	dh_t *d = dir_of(s, h, n);
	uint32_t now = nsfs_now();
	char ln[XFER_PATH + 64];
	wr_t w;

	if (!d) { status(s, id, FX_FAILURE, "No such handle"); return; }
	begin(s, &w, FXP_NAME);
	wr_u32(&w, id);
	uint32_t at_count = w.n;
	wr_u32(&w, 0);
	for (;;) {
		if (!d->held) {
			if (d->ended) break;
			int32_t rc = nsfs_readdir(d->d, d->name, sizeof(d->name), &d->info);
			if (rc <= 0) { d->ended = true; break; }	// an error ends the listing early
			d->held = true;
		}
		uint32_t mark = w.n;
		bool dir = d->info.type == Z_FS_TYPE_DIR;
		uint32_t ll = xfer_longname(ln, sizeof(ln), d->name, dir ? 0 : d->info.size, dir,
			(d->info.attr & Z_FS_ATTR_RDONLY) != 0, z_fs_info_time(&d->info), now);
		wr_cstr(&w, d->name);
		wr_str(&w, ln, ll);
		wr_attrs(&w, &d->info);
		if (w.bad) { w.bad = false; w.n = mark; break; }
		d->held = false;
		count++;
		if (count >= 64) break;
	}
	if (!count) { status(s, id, FX_EOF, "End of folder"); return; }
	w.p[at_count] = (uint8_t)(count >> 24); w.p[at_count + 1] = (uint8_t)(count >> 16);
	w.p[at_count + 2] = (uint8_t)(count >> 8); w.p[at_count + 3] = (uint8_t)count;
	finish(s, &w);
}

static void do_remove(sftp_t *s, rd_t *r, uint32_t id, bool dir) {
	char path[XFER_PATH];
	z_fs_info_t fi;
	if (!path_arg(s, r, id, path)) return;
	if (!can_write(s, id)) return;
	int32_t rc = nsfs_stat(path, &fi);
	if (rc >= 0 && dir && fi.type != Z_FS_TYPE_DIR) rc = -Z_ENOTDIR;
	if (rc >= 0 && !dir && fi.type == Z_FS_TYPE_DIR) rc = -Z_EISDIR;
	if (rc >= 0) rc = nsfs_unlink(path);
	if (rc < 0) { status_err(s, id, rc); return; }
	xfer_logf(&s->x, "sftp: removed %s%s", path, dir ? "/" : "");
	status(s, id, FX_OK, "Success");
}

static void do_mkdir(sftp_t *s, rd_t *r, uint32_t id) {
	char path[XFER_PATH];
	if (!path_arg(s, r, id, path)) return;
	if (!can_write(s, id)) return;
	int32_t rc = nsfs_mkdir(path);
	if (rc < 0) { status_err(s, id, rc); return; }
	xfer_logf(&s->x, "sftp: made %s/", path);
	status(s, id, FX_OK, "Success");
}

static void do_realpath(sftp_t *s, rd_t *r, uint32_t id) {
	char path[XFER_PATH];
	if (!path_arg(s, r, id, path)) return;
	wr_t w;
	begin(s, &w, FXP_NAME);
	wr_u32(&w, id);
	wr_u32(&w, 1);
	wr_cstr(&w, path);
	wr_cstr(&w, path);
	wr_u32(&w, 0);                  // no attributes
	finish(s, &w);
}

// RENAME refuses to replace (v3: the new name must not exist); the
// posix-rename@openssh.com extension replaces a file, as rename(2).
static void do_rename(sftp_t *s, rd_t *r, uint32_t id, bool replace) {
	char from[XFER_PATH], to[XFER_PATH];
	z_fs_info_t fi;
	if (!path_arg(s, r, id, from) || !path_arg(s, r, id, to)) return;
	if (!can_write(s, id)) return;
	if (replace && strcmp(from, to) && nsfs_stat(to, &fi) >= 0) {
		if (fi.type == Z_FS_TYPE_DIR) { status(s, id, FX_FAILURE, "Is a directory"); return; }
		int32_t rc = nsfs_unlink(to);
		if (rc < 0) { status_err(s, id, rc); return; }
	}
	int32_t rc = nsfs_rename(from, to);
	if (rc < 0) { status_err(s, id, rc); return; }
	xfer_logf(&s->x, "sftp: renamed %s to %s", from, to);
	status(s, id, FX_OK, "Success");
}

static void do_extended(sftp_t *s, rd_t *r, uint32_t id) {
	uint32_t n;
	const uint8_t *name = rd_str(r, &n);
	wr_t w;
#define IS(lit) (n == sizeof(lit) - 1 && !memcmp(name, lit, n))
	if (IS("posix-rename@openssh.com")) { do_rename(s, r, id, true); return; }
	if (IS("limits@openssh.com")) {
		begin(s, &w, FXP_EXTENDED_REPLY);
		wr_u32(&w, id);
		wr_u64(&w, MAX_PACKET);
		wr_u64(&w, MAX_READ);
		wr_u64(&w, MAX_WRITE);
		wr_u64(&w, SFTP_FILES);
		finish(s, &w);
		return;
	}
	if (IS("fsync@openssh.com")) {
		uint32_t hn;
		const uint8_t *h = rd_str(r, &hn);
		fh_t *f = file_of(s, h, hn);
		if (!f) { status(s, id, FX_FAILURE, "No such handle"); return; }
		int32_t rc = nsfs_fsync(f->fd);
		if (rc < 0) status_err(s, id, rc);
		else status(s, id, FX_OK, "Success");
		return;
	}
	if (IS("statvfs@openssh.com")) {
		char path[XFER_PATH];
		z_fs_vfs_t v;
		if (!path_arg(s, r, id, path)) return;
		int32_t rc = nsfs_statvfs(path, &v);
		if (rc < 0) { status_err(s, id, rc); return; }
		begin(s, &w, FXP_EXTENDED_REPLY);
		wr_u32(&w, id);
		wr_u64(&w, 1024);           // f_bsize: the figures are in KB
		wr_u64(&w, 1024);           // f_frsize
		wr_u64(&w, v.total_kb);     // f_blocks
		wr_u64(&w, v.free_kb);      // f_bfree
		wr_u64(&w, v.free_kb);      // f_bavail
		wr_u64(&w, 0);              // f_files: FAT does not count them
		wr_u64(&w, 0);
		wr_u64(&w, 0);
		wr_u64(&w, 0);              // f_fsid
		wr_u64(&w, (s->x.flags & XFER_READONLY) ? 1 : 0);   // ST_RDONLY
		wr_u64(&w, 255);            // f_namemax
		finish(s, &w);
		return;
	}
#undef IS
	status(s, id, FX_OP_UNSUPPORTED, "Not supported");
}

static void dispatch(sftp_t *s, const uint8_t *p, uint32_t len) {
	rd_t r = { p, len, false };
	uint8_t type = rd_u8(&r);

	if (type == FXP_INIT) { do_init(s); return; }
	uint32_t id = rd_u32(&r);
	if (r.bad) return;                  // nothing to answer to
	if (!s->inited) { status(s, id, FX_FAILURE, "INIT first"); return; }
	switch (type) {
	case FXP_OPEN:     do_open(s, &r, id); return;
	case FXP_CLOSE:    do_close(s, &r, id); return;
	case FXP_READ:     do_read(s, &r, id); return;
	case FXP_LSTAT:
	case FXP_STAT:     do_stat(s, &r, id); return;
	case FXP_FSTAT:    do_fstat(s, &r, id); return;
	case FXP_SETSTAT:  do_setstat(s, &r, id, false); return;
	case FXP_FSETSTAT: do_setstat(s, &r, id, true); return;
	case FXP_OPENDIR:  do_opendir(s, &r, id); return;
	case FXP_READDIR:  do_readdir(s, &r, id); return;
	case FXP_REMOVE:   do_remove(s, &r, id, false); return;
	case FXP_MKDIR:    do_mkdir(s, &r, id); return;
	case FXP_RMDIR:    do_remove(s, &r, id, true); return;
	case FXP_REALPATH: do_realpath(s, &r, id); return;
	case FXP_RENAME:   do_rename(s, &r, id, false); return;
	case FXP_EXTENDED: do_extended(s, &r, id); return;
	case FXP_WRITE:    status(s, id, FX_BAD_MESSAGE, "Malformed request"); return;
	case FXP_READLINK:
	case FXP_SYMLINK:
		status(s, id, FX_OP_UNSUPPORTED, "FAT has no links");
		return;
	}
	status(s, id, FX_OP_UNSUPPORTED, "Not supported");
}

// Room a request's reply needs before it is answered.
static uint32_t room_for(const uint8_t *req) {
	if (req[0] == FXP_READ) {
		uint32_t want = be32(req + 1 + 4 + 4 + HANDLE_LEN + 8);
		if (want > MAX_READ) want = MAX_READ;
		return 13 + want;
	}
	if (req[0] == FXP_READDIR) return 1024;
	return 400;
}

// The header bytes a request needs before it can be acted on.
static uint32_t need_of(const sftp_t *s) {
	uint32_t whole = s->pkt_len < REQ_MAX ? s->pkt_len : REQ_MAX, n;
	if (s->req_have < 1) return 1;
	if (s->req[0] != FXP_WRITE) return whole;
	if (s->req_have < 9) n = 9;
	else if (be32(s->req + 5) > 64) return whole;		// not one of our handles: skipped
	else n = 9 + be32(s->req + 5) + 12;
	return n < whole ? n : whole;
}

// The request assembled is a WRITE's complete header (its data to come).
static bool write_header(const sftp_t *s) {
	return s->req[0] == FXP_WRITE && s->req_have >= 9 && be32(s->req + 5) <= 64 &&
		s->req_have == 9 + be32(s->req + 5) + 12;
}

static void fatal(sftp_t *s, const char *why) {
	xfer_logf(&s->x, "sftp: %s -- ending the session", why);
	s->x.done = true;
	s->x.status = 1;
}

static uint32_t sftp_step(xfer_t *x, int budget) {
	sftp_t *s = (sftp_t *)x;
	uint32_t used = 0;

	while (budget > 0 && !x->done) {
		if (s->wstream) {
			uint32_t n = write_data(s);
			used += n;
			if (!n) break;
			if (!s->wstream) budget--;
			continue;
		}
		if (s->skip) {
			uint32_t n = x->in_len < s->skip ? x->in_len : s->skip;
			if (!n) break;
			xfer_in_take(x, n);
			used += n;
			s->skip -= n;
			continue;
		}
		if (!s->have_len) {
			uint8_t b[4];
			if (x->in_len < 4) break;
			xfer_in_peek(x, b, 4);
			xfer_in_take(x, 4);
			used += 4;
			s->pkt_len = be32(b);
			if (s->pkt_len < 1 || s->pkt_len > MAX_ACCEPT) {
				fatal(s, "a request of impossible length");
				break;
			}
			s->have_len = true;
			s->req_have = 0;
		}
		uint32_t need = need_of(s);
		if (s->req_have < need) {
			uint32_t k = need - s->req_have;
			if (k > x->in_len) k = x->in_len;
			if (!k) break;
			xfer_in_peek(x, s->req + s->req_have, k);
			xfer_in_take(x, k);
			used += k;
			s->req_have += k;
			continue;                   // need_of() may want more once it sees the type
		}
		// A whole request, or a WRITE's header: wait for room to answer.
		if (xfer_out_room(x) < room_for(s->req) && x->out_len) break;
		if (xfer_out_room(x) < 64) break;

		uint32_t rest = s->pkt_len - s->req_have;
		s->have_len = false;
		if (write_header(s)) {
			uint32_t dlen = be32(s->req + need - 4);
			if (dlen != rest) {
				status(s, be32(s->req + 1), FX_BAD_MESSAGE, "Malformed request");
				s->skip = rest;
				budget--;
				continue;
			}
			do_write(s, &(rd_t){ s->req + 5, need - 5, false }, be32(s->req + 1));
			if (!rest) write_done(s);
			continue;
		}
		if (rest) {
			// longer than any request we take
			if (s->req_have >= 5)
				status(s, be32(s->req + 1), FX_FAILURE, "Request too long for this server");
			s->skip = rest;
			budget--;
			continue;
		}
		dispatch(s, s->req, s->pkt_len);
		budget--;
	}

	// The client has finished, and so have we.
	if (x->eof && !x->in_len && !s->wstream && !s->skip && !s->have_len && !x->done) {
		x->done = true;
		x->status = 0;
	}
	return used;
}

static void sftp_abort(xfer_t *x) {
	sftp_t *s = (sftp_t *)x;
	for (int i = 0; i < SFTP_FILES; i++) if (s->fh[i].used) close_file(s, &s->fh[i], true);
	for (int i = 0; i < SFTP_DIRS; i++) if (s->dh[i].used) { nsfs_closedir(s->dh[i].d); s->dh[i].used = false; }
}

xfer_t *xfer_sftp(const char *home, uint32_t flags, xfer_log_fn log, void *user) {
	sftp_t *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	xfer_base_init(&s->x, home, flags, log, user);
	s->x.step = sftp_step;
	s->x.abort = sftp_abort;
	return &s->x;
}
