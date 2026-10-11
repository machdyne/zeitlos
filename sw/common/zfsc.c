/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zfsc.h, and zfsproto.h for the wire format.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "zeitlos.h"
#include "zfsc.h"

// The kernel builds this file too (ZFSC_KERNEL, sw/os/kfs.c): its shell,
// program loading and the old Z_SYS_FS_* calls are clients of `fs` like
// any app. The differences, all here:
//
//  - It never allows drivers to write it, and never hands `fs` a read
//    buffer: what it reads into is often kernel memory or a new
//    process's, physical addresses the kernel cannot pass as one of the
//    calling process's own. Read data always comes back in the reply,
//    and the kernel copies it to wherever it goes.
//  - It has no heap to speak of: directories use a buffer the caller
//    supplies (zfs_opendir_at()).
//  - It may run in any process's syscall -- the request goes out, and
//    the reply comes back, as that process.
#ifdef ZFSC_KERNEL
#define ZFSC_GRANTS	0
#else
#define ZFSC_GRANTS	1
#endif

static uint32_t fs_pid;
static bool fs_known;
static bool opted_in;
static uint32_t seq;
static uint32_t next_timeout;	// for one call, when it may take long

static bool find_fs(bool again) {
	if (fs_known && !again) return true;
	fs_known = z_pid_lookup(Z_FS_SERVICE, &fs_pid);
	return fs_known;
}

// Replies that may still arrive after their caller gave up on them (a
// timeout). z_msg_recv_match() leaves everything else queued, so such a
// reply would sit in the mailbox for good, and enough of them fill it.
// Each is looked for, without waiting, at the start of the next calls,
// and forgotten once found or after STALE_LOOKS tries.
#define STALE_MAX	4
#define STALE_LOOKS	32
static uint32_t stale_tag[STALE_MAX];		// 0: none
static uint8_t stale_looks[STALE_MAX];
static uint8_t stale_n;

static void stale_note(uint32_t tag) {
	int i, k = 0;
	for (i = 0; i < STALE_MAX; i++) {
		if (!stale_tag[i]) { k = i; break; }
		if (stale_looks[i] > stale_looks[k]) k = i;
	}
	if (!stale_tag[k]) stale_n++;
	stale_tag[k] = tag;
	stale_looks[k] = 0;
}

static void stale_drain(void) {
	z_msg_t m;
	int i;
	for (i = 0; i < STALE_MAX; i++) {
		if (!stale_tag[i]) continue;
		if (z_msg_take_match(&m, Z_FS_REPLY, stale_tag[i]) == Z_OK ||
			++stale_looks[i] >= STALE_LOOKS) {
			stale_tag[i] = 0;
			stale_n--;
		}
	}
}

// One request and its reply. `buf`/`blen` is the data blob (what to
// write, or where a read should land). Whatever data the reply carries
// is copied to `out` (at most `outcap` bytes; *outlen says how many).
static int32_t call_once(z_fsreq_t *rq, void *buf, uint32_t blen,
	void *out, uint32_t outcap, uint32_t *outlen, uint32_t *flags);

#ifdef ZFSC_KERNEL
void k_proc_wait_ticks(uint32_t ticks);		// sw/os/kernel.c
#define zfsc_pause()	k_proc_wait_ticks(1)
#else
#define zfsc_pause()	z_proc_wait(1)
#endif

// -- write-behind: small writes collected into one request --
//
// A request to `fs` costs a round trip, ~1-2ms, whatever it carries, so
// a program writing 100-byte lines or 1468-byte TFTP blocks paid that
// per write. Small writes to one file are collected here, up to
// ZFSC_WBUF bytes, and go to `fs` as one request when the buffer fills,
// or before ANY other request this process makes -- a read, a seek, a
// stat, a close, a write elsewhere -- so the process always sees its
// own writes. docs/net_throughput.md, round 4.
//
//  - Only a file whose last write went through directly and succeeded:
//    the first write to a descriptor is never buffered, so writing to a
//    read-only, flash or closed one fails at once, as it should. An
//    error that comes later (a full disk) is returned by the next write,
//    fsync or close of that descriptor.
//  - In an app, the buffer is malloc'd on first use (none: no
//    buffering), and flushed at exit. In the kernel, only for pid 0's
//    own calls (the shell's tget): this file also runs inside other
//    processes' syscalls, and a buffer written as one process and sent
//    as another would be refused by `fs`.
//  - Other processes see the bytes once they are flushed.
#ifndef ZFSC_WBUF
#define ZFSC_WBUF	8192
#endif

#ifdef ZFSC_KERNEL
extern volatile uint32_t z_pid;
#define WB_SELF()	(z_pid == 0)
static uint8_t wb_store[ZFSC_WBUF] __attribute__((aligned(4)));
#define WB_GET()	(wb_store)
#else
#define WB_SELF()	1
static uint8_t *wb_mem;
static bool wb_tried;
static void wb_exit(void);
static uint8_t *wb_get(void) {
	if (!wb_tried) {
		wb_tried = true;
		wb_mem = malloc(ZFSC_WBUF);
		if (wb_mem) atexit(wb_exit);
	}
	return wb_mem;
}
#define WB_GET()	wb_get()
#endif

static uint8_t *wb;			// the buffer, once there is one
static uint32_t wb_len;			// bytes in it
static int32_t wb_fd = -1;		// whose they are
static int32_t wb_ok_fd = -1;		// last written directly, successfully
static int32_t wb_err;			// a flush that failed: for wb_err_fd's next call
static int32_t wb_err_fd = -1;
static bool wb_flushing;

static int32_t write_direct(int32_t fd, const uint8_t *p, uint32_t len);

static void wb_flush(void) {
	if (!wb_len || wb_flushing || !WB_SELF()) return;
	wb_flushing = true;
	int32_t r = write_direct(wb_fd, wb, wb_len);
	if (r < 0 || (uint32_t)r < wb_len) {
		wb_err = r < 0 ? r : -Z_ENOSPC;
		wb_err_fd = wb_fd;
		wb_ok_fd = -1;
	}
	wb_len = 0;
	wb_flushing = false;
}

#ifndef ZFSC_KERNEL
static void wb_exit(void) { wb_flush(); }
#endif

void zfs_flush(void) { wb_flush(); }

// The error a flush met, for the descriptor it was for (once).
static int32_t wb_take_err(int32_t fd) {
	if (wb_err_fd != fd) return 0;
	int32_t e = wb_err;
	wb_err = 0;
	wb_err_fd = -1;
	return e;
}

// call_once(), again while fs says it has no reply slot free
// (-Z_EAGAIN: more clients than slots have replies unread at once,
// sw/apps/fs). A tick each time, for a few seconds at most.
static int32_t call_f(z_fsreq_t *rq, void *buf, uint32_t blen,
	void *out, uint32_t outcap, uint32_t *outlen, uint32_t *flags) {
	int32_t r;
	int tries = 0;
	uint32_t keep;
	wb_flush();			// before anything else: this process sees its writes
	keep = next_timeout;
	for (;;) {
		next_timeout = keep;
		r = call_once(rq, buf, blen, out, outcap, outlen, flags);
		if (r != -Z_EAGAIN || ++tries > 2000) return r;
		zfsc_pause();
	}
}

static int32_t call_once(z_fsreq_t *rq, void *buf, uint32_t blen,
	void *out, uint32_t outcap, uint32_t *outlen, uint32_t *flags) {

	z_blob_t b[2];
	z_obj_t items[2];
	z_obj_table_t t;
	z_obj_t o;
	z_msg_t m;
	uint32_t tag;

	if (outlen) *outlen = 0;
	if (flags) *flags = 0;
	if (!opted_in) {
#if ZFSC_GRANTS
		z_grant_drivers(true);	// fails harmlessly without the window
#endif
		opted_in = true;
	}
	if (!find_fs(false)) return -Z_ENODEV;

	b[0].len = sizeof(*rq);
	b[0].data = (uint8_t *)rq;
	b[1].len = buf ? blen : 0;
	b[1].data = (uint8_t *)buf;
	items[0].type = Z_BLOB; items[0].val.ptr = &b[0];
	items[1].type = Z_BLOB; items[1].val.ptr = &b[1];
	t.len = 2; t.a = items; t.b = NULL;
	o.type = Z_LIST; o.val.ptr = &t;

	if (stale_n) stale_drain();
	tag = ++seq;
	if (!tag) tag = ++seq;			// 0 means "none" to stale_note()
	if (z_msg_new_send(fs_pid, Z_FS_REQ, tag, o) != Z_OK) {
		// A cached pid that has gone stale: `fs` restarted, say.
		if (!find_fs(true) ||
			z_msg_new_send(fs_pid, Z_FS_REQ, tag, o) != Z_OK)
			return -Z_ENODEV;
	}

	uint32_t wait = next_timeout ? next_timeout : ZFS_TIMEOUT_TICKS;
	next_timeout = 0;
	if (z_msg_recv_match(&m, Z_FS_REPLY, tag, wait) != Z_OK) {
		stale_note(tag);
		fs_known = false;		// look it up again: it may have restarted
		return -Z_ETIMEDOUT;
	}
	if (m.from != fs_pid) return -Z_EIO;

	// Borrowed: `fs`'s memory, valid until we send it something else.
	const z_fsrep_t *rep = (const z_fsrep_t *)z_blob_data(&m.obj);
	uint32_t rl = z_blob_len(&m.obj);
	if (!rep || rl < sizeof(*rep) || ((uintptr_t)rep & 3u)) return -Z_EIO;
	int32_t result = rep->result;
	if (flags) *flags = rep->flags;
	uint32_t n = rep->len;
	if (n > rl - sizeof(*rep)) n = rl - sizeof(*rep);
	if (out && n) {
		if (n > outcap) n = outcap;
		memcpy(out, rep + 1, n);
		if (outlen) *outlen = n;
	}
	return result;
}

static int32_t call(z_fsreq_t *rq, void *buf, uint32_t blen,
	void *out, uint32_t outcap, uint32_t *outlen) {
	return call_f(rq, buf, blen, out, outcap, outlen, NULL);
}

static void req_init(z_fsreq_t *rq, uint32_t op) {
	memset(rq, 0, offsetof(z_fsreq_t, path));
	rq->op = op;
	rq->path[0] = 0;
	rq->path2[0] = 0;
}

static bool req_path(char *dst, const char *src) {
	if (!src) return false;
	uint32_t n = strlen(src);
	if (n >= Z_FS_PATH_MAX) return false;
	memcpy(dst, src, n + 1);
	return true;
}

bool zfs_path_is_ours(const char *path) {
	return path && path[0] == '/' && path[1] == 'f' && path[2] == 's' &&
		(path[3] == 0 || path[3] == '/');
}

int32_t zfs_open_info(const char *path, uint32_t flags, z_fs_info_t *info) {
	z_fsreq_t rq;
	z_fs_info_t fi;
	uint32_t got = 0;
	req_init(&rq, Z_FSOP_OPEN);
	if (!req_path(rq.path, path)) return -Z_ENAMETOOLONG;
	rq.flags = flags;
	int32_t r = call(&rq, NULL, 0, &fi, sizeof(fi), &got);
	if (r >= 0 && info) {
		if (got == sizeof(fi)) *info = fi;
		else if (zfs_fstat(r, info) != 0) memset(info, 0, sizeof(*info));
	}
	return r;
}

int32_t zfs_open(const char *path, uint32_t flags) {
	return zfs_open_info(path, flags, NULL);
}

int32_t zfs_close(int32_t fd) {
	z_fsreq_t rq;
	int32_t e;
	if (wb_fd == fd) wb_flush();
	e = wb_take_err(fd);
	if (wb_ok_fd == fd) wb_ok_fd = -1;
	req_init(&rq, Z_FSOP_CLOSE);
	rq.fd = fd;
	int32_t r = call(&rq, NULL, 0, NULL, 0, NULL);
	return e ? e : r;
}

// In Z_FS_CHUNK requests, while each comes back full. `fs` writes
// straight into p + done through the grant window, or sends the bytes
// in its reply and they are copied there here. A short count -- the end
// of the file, or `fs` sending less by copy while another client holds
// its bulk buffer -- ends the call, as read() may: callers that need
// every byte loop (zfsapp's whole-file reads do).
int32_t zfs_read(int32_t fd, void *buf, uint32_t len) {
	z_fsreq_t rq;
	uint8_t *p = buf;
	uint32_t done = 0;
	if (!buf && len) return -Z_EINVAL;
	req_init(&rq, Z_FSOP_READ);
	rq.fd = fd;
	while (done < len) {
		uint32_t want = len - done, got = 0;
		if (want > Z_FS_CHUNK) want = Z_FS_CHUNK;
		rq.len = want;
#if ZFSC_GRANTS
		int32_t r = call(&rq, p + done, want, p + done, want, &got);
#else
		int32_t r = call(&rq, NULL, 0, p + done, want, &got);
#endif
		if (r < 0) return done ? (int32_t)done : r;
		done += (uint32_t)r;
		if ((uint32_t)r < want) break;
	}
	return (int32_t)done;
}

int32_t zfs_write(int32_t fd, const void *buf, uint32_t len) {
	int32_t e;
	if (!buf && len) return -Z_EINVAL;
	if (wb_fd != fd) wb_flush();
	if ((e = wb_take_err(fd))) return e;
	// Small, to a descriptor known to be writable: into the buffer.
	if (len && len < ZFSC_WBUF && fd == wb_ok_fd && WB_SELF() && (wb || (wb = WB_GET()))) {
		if (wb_len + len > ZFSC_WBUF) {
			wb_flush();
			if ((e = wb_take_err(fd))) return e;
		}
		wb_fd = fd;
		memcpy(wb + wb_len, buf, len);
		wb_len += len;
		if (wb_len == ZFSC_WBUF) wb_flush();
		return (int32_t)len;
	}
	wb_flush();
	int32_t r = write_direct(fd, buf, len);
	if (r >= 0 && (uint32_t)r == len) wb_ok_fd = fd;
	return r;
}

static int32_t write_direct(int32_t fd, const uint8_t *p, uint32_t len) {
	z_fsreq_t rq;
	uint32_t done = 0;
	req_init(&rq, Z_FSOP_WRITE);
	rq.fd = fd;
	while (done < len) {
		uint32_t n = len - done;
		if (n > Z_FS_CHUNK) n = Z_FS_CHUNK;
		rq.len = n;
		int32_t r = call(&rq, (void *)(p + done), n, NULL, 0, NULL);
		if (r < 0) return done ? (int32_t)done : r;
		done += (uint32_t)r;
		if ((uint32_t)r < n) break;		// the disk is full
	}
	return (int32_t)done;
}

int32_t zfs_lseek(int32_t fd, int32_t off, uint32_t whence) {
	z_fsreq_t rq;
	req_init(&rq, Z_FSOP_LSEEK);
	rq.fd = fd;
	rq.off = off;
	rq.whence = whence;
	return call(&rq, NULL, 0, NULL, 0, NULL);
}

int32_t zfs_fstat(int32_t fd, z_fs_info_t *info) {
	z_fsreq_t rq;
	uint32_t got;
	req_init(&rq, Z_FSOP_FSTAT);
	rq.fd = fd;
	int32_t r = call(&rq, NULL, 0, info, sizeof(*info), &got);
	return (r == 0 && got != sizeof(*info)) ? -Z_EIO : r;
}

int32_t zfs_stat(const char *path, z_fs_info_t *info) {
	z_fsreq_t rq;
	uint32_t got;
	req_init(&rq, Z_FSOP_STAT);
	if (!req_path(rq.path, path)) return -Z_ENAMETOOLONG;
	int32_t r = call(&rq, NULL, 0, info, sizeof(*info), &got);
	return (r == 0 && got != sizeof(*info)) ? -Z_EIO : r;
}

static int32_t path_op(uint32_t op, const char *path, const char *path2) {
	z_fsreq_t rq;
	req_init(&rq, op);
	if (!req_path(rq.path, path)) return -Z_ENAMETOOLONG;
	if (path2 && !req_path(rq.path2, path2)) return -Z_ENAMETOOLONG;
	return call(&rq, NULL, 0, NULL, 0, NULL);
}

int32_t zfs_unlink(const char *path) { return path_op(Z_FSOP_UNLINK, path, NULL); }
int32_t zfs_mkfs(const char *path) {
	next_timeout = ZFS_SLOW_TICKS;		// formatting a big card
	return path_op(Z_FSOP_MKFS, path, NULL);
}
int32_t zfs_mount(const char *path) { return path_op(Z_FSOP_MOUNT, path, NULL); }
int32_t zfs_umount(const char *path) { return path_op(Z_FSOP_UMOUNT, path, NULL); }
int32_t zfs_syncall(void) {
	z_fsreq_t rq;
	req_init(&rq, Z_FSOP_SYNCALL);
	return call(&rq, NULL, 0, NULL, 0, NULL);
}
int32_t zfs_mkdir(const char *path) { return path_op(Z_FSOP_MKDIR, path, NULL); }
int32_t zfs_rename(const char *from, const char *to) {
	if (!to) return -Z_EINVAL;
	return path_op(Z_FSOP_RENAME, from, to);
}

int32_t zfs_fsync(int32_t fd) {
	z_fsreq_t rq;
	int32_t e;
	if (wb_fd == fd) wb_flush();
	if ((e = wb_take_err(fd))) return e;
	req_init(&rq, Z_FSOP_FSYNC);
	rq.fd = fd;
	return call(&rq, NULL, 0, NULL, 0, NULL);
}

int32_t zfs_ftruncate(int32_t fd, uint32_t len) {
	z_fsreq_t rq;
	if (len > 0x7fffffffu) return -Z_EINVAL;
	req_init(&rq, Z_FSOP_FTRUNCATE);
	rq.fd = fd;
	rq.off = (int32_t)len;
	return call(&rq, NULL, 0, NULL, 0, NULL);
}

int32_t zfs_statvfs(const char *path, z_fs_vfs_t *out) {
	z_fsreq_t rq;
	uint32_t got;
	req_init(&rq, Z_FSOP_STATVFS);
	next_timeout = ZFS_SLOW_TICKS;		// the first count of FAT32's free clusters
	if (!req_path(rq.path, path)) return -Z_ENAMETOOLONG;
	int32_t r = call(&rq, NULL, 0, out, sizeof(*out), &got);
	return (r == 0 && got != sizeof(*out)) ? -Z_EIO : r;
}

// -- directories --

int32_t zfs_opendir_at(zfs_dir_t *d, const char *path, void *buf, uint32_t cap) {
	z_fsreq_t rq;
	uint32_t fl = 0;
	if (!d || !buf || cap < 64) return -Z_EINVAL;
	req_init(&rq, Z_FSOP_OPENDIR);
	if (!req_path(rq.path, path)) return -Z_ENAMETOOLONG;
	rq.len = cap & ~3u;			// a page is at most this
	d->buf = (uint8_t *)buf;
	d->cap = cap & ~3u;
	d->err = 0;
	d->at = 0;
	d->len = 0;
	// the first page of entries comes with the descriptor
	int32_t r = call_f(&rq, NULL, 0, d->buf, d->cap, &d->len, &fl);
	if (r < 0) return r;
	d->dd = r;
	d->end = (fl & Z_FSREP_END) != 0;	// and fs has closed it
	return 0;
}

const z_fs_dirent_t *zfs_readdir(zfs_dir_t *d) {
	if (!d) return NULL;
	if (d->at >= d->len) {
		z_fsreq_t rq;
		uint32_t fl = 0;
		if (d->end) return NULL;
		req_init(&rq, Z_FSOP_READDIR);
		rq.fd = d->dd;
		rq.len = d->cap;
		int32_t r = call_f(&rq, NULL, 0, d->buf, d->cap, &d->len, &fl);
		d->at = 0;
		if (fl & Z_FSREP_END) d->end = true;
		if (r < 0) {
			d->err = r;
			d->end = true;
			d->len = 0;
			return NULL;
		}
		if (r == 0 || d->len == 0) { d->end = true; d->len = 0; return NULL; }
	}
	const z_fs_dirent_t *e = (const z_fs_dirent_t *)(d->buf + d->at);
	if (((uintptr_t)e & 3u) || e->reclen < sizeof(*e) || (e->reclen & 3u) ||
		d->at + e->reclen > d->len) {
		d->err = -Z_EIO;
		d->end = true;
		d->len = 0;
		return NULL;
	}
	d->at += e->reclen;
	return e;
}

int32_t zfs_dir_error(const zfs_dir_t *d) { return d ? d->err : -Z_EBADF; }

int32_t zfs_closedir_at(zfs_dir_t *d) {
	z_fsreq_t rq;
	int32_t r = 0;
	if (!d) return -Z_EBADF;
	// At the end, fs has closed it already (Z_FSREP_END). An error
	// ended it too; asking fs to close it again does no harm then.
	if (!d->end || d->err) {
		req_init(&rq, Z_FSOP_CLOSEDIR);
		rq.fd = d->dd;
		r = call(&rq, NULL, 0, NULL, 0, NULL);
		if (d->err && r == -Z_EBADF) r = 0;
	}
	d->end = true;
	return r;
}

#ifndef ZFSC_KERNEL
// The same with a heap: the descriptor and a full page together.
typedef struct {
	zfs_dir_t	d;
	uint32_t	page[Z_FS_DIR_BYTES / 4];	// words: entries are aligned
} zfs_dir_heap_t;

zfs_dir_t *zfs_opendir(const char *path) {
	zfs_dir_heap_t *h = malloc(sizeof(*h));
	if (!h) return NULL;
	if (zfs_opendir_at(&h->d, path, h->page, sizeof(h->page)) < 0) {
		free(h);
		return NULL;
	}
	return &h->d;
}

int32_t zfs_closedir(zfs_dir_t *d) {
	int32_t r = zfs_closedir_at(d);
	free(d);		// d is the first member of its zfs_dir_heap_t
	return r;
}
#endif
