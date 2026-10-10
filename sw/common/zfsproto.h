#ifndef ZFSPROTO_H
#define ZFSPROTO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The filesystem driver's protocol: what a client (sw/common/zfsc.c, in
 * apps through zfsapp and in the kernel through kfs.c) sends to `fs`
 * (sw/apps/fs) and what comes back. Since Phase 2b `fs` is the only
 * filesystem: the card at "/", /ram and /tmp, /usb, and the flash
 * archive's files underneath the card.
 * docs/userspace_drivers.md, "Phase 2"; docs/fs_driver.md.
 *
 * POSIX-shaped on purpose: descriptors, open flags, lseek with whence,
 * stat, opendir/readdir. Not the shape of the kernel's Z_SYS_FS_*
 * calls, so that the C library's open()/read()/fopen() can sit straight
 * on top of it, and zfsapp's fs_*() functions too.
 *
 * -- A request --
 *
 *   subject  Z_FS_REQ
 *   tag      the client's sequence number; the reply carries the same
 *   obj      a Z_LIST of two Z_BLOBs, both in the client's memory:
 *              [0] the z_fsreq_t below
 *              [1] the data buffer: what to write, or where to read to
 *                  (length 0 when the call has none)
 *
 * The kernel resolves both blobs for `fs` when it reads the message
 * (docs/messaging.md), so `fs` sees physical addresses it can read
 * directly -- no copy for a write. For a read it writes into the
 * client's buffer through the MPU grant window (docs/mpu.md, "The grant
 * window") when it can; when it cannot (no window on this gateware, or
 * a client that has not allowed drivers) it puts the data in the reply.
 *
 * -- A reply --
 *
 *   subject  Z_FS_REPLY
 *   tag      the request's
 *   obj      a Z_BLOB in `fs`'s memory: a z_fsrep_t (the result) and
 *            then any data (a stat, directory entries, read data that
 *            could not go straight into the client's buffer). Borrowed:
 *            the client copies what it needs before it sends anything
 *            else.
 *
 * A result >= 0 is success (a descriptor, a byte count, a position);
 * < 0 is -Z_E*.
 */

#include <stdint.h>
#include "zsubjects.h"
#include "zfs.h"		// z_fs_info_t

#define Z_FS_REQ		(Z_SUBJ_FS + 0)
#define Z_FS_REPLY		(Z_SUBJ_FS + 1)
Z_SUBJECTS_IN(Z_SUBJ_FS, Z_FS_REPLY);

// The name `fs` registers under, as z_pid_register() completes it.
#define Z_FS_SERVICE	"fs0"

// Operations (z_fsreq_t.op).
#define Z_FSOP_OPEN			1	// path, flags                -> fd, a z_fs_info_t (its size)
#define Z_FSOP_CLOSE		2	// fd                         -> 0
#define Z_FSOP_READ			3	// fd, buffer (len)           -> bytes, 0 at the end
#define Z_FSOP_WRITE		4	// fd, buffer (len)           -> bytes
#define Z_FSOP_LSEEK		5	// fd, off, whence            -> new position
#define Z_FSOP_FSTAT		6	// fd                         -> 0, a z_fs_info_t
#define Z_FSOP_STAT			7	// path                       -> 0, a z_fs_info_t
#define Z_FSOP_UNLINK		8	// path (a file or an empty directory)
#define Z_FSOP_MKDIR		9	// path
#define Z_FSOP_RENAME		10	// path -> path2
#define Z_FSOP_OPENDIR		11	// path                       -> dd, the first entries (Z_FSREP_END)
#define Z_FSOP_READDIR		12	// dd                         -> entries, as z_fs_dirent_t (Z_FSREP_END)
#define Z_FSOP_CLOSEDIR		13	// dd
#define Z_FSOP_FSYNC		14	// fd
#define Z_FSOP_FTRUNCATE	15	// fd, off (the new length)
#define Z_FSOP_STATVFS		16	// path                       -> 0, a z_fs_vfs_t
// Volumes (the kernel shell's `format`, `mount`, `usbmount`/`usbunmount`
// and a reboot's flush). `path` names the volume: "/" the card, "/ram",
// "/usb".
#define Z_FSOP_MKFS			17	// path: a fresh filesystem on that volume
#define Z_FSOP_MOUNT		18	// path: mount it now (the card, a USB drive)
#define Z_FSOP_UMOUNT		19	// path: flush and release it; -Z_EBUSY with files open
#define Z_FSOP_SYNCALL		20	// flush every file open for writing, of every client

// Open flags (z_fsreq_t.flags). Zeitlos's own numbers, not any C
// library's: the libc hooks translate.
#define Z_O_RDONLY		0x0000
#define Z_O_WRONLY		0x0001
#define Z_O_RDWR		0x0002
#define Z_O_ACCMODE		0x0003
#define Z_O_CREAT		0x0040
#define Z_O_EXCL		0x0080
#define Z_O_TRUNC		0x0200
#define Z_O_APPEND		0x0400
// Not POSIX: the card (or /ram, /usb) only, never the flash archive's
// copy underneath -- for the kernel deciding where a program comes from.
#define Z_O_NOFLASH		0x1000

#define Z_SEEK_SET		0
#define Z_SEEK_CUR		1
#define Z_SEEK_END		2

// Errors, as -Z_E* results. The same numbers as newlib's errno values,
// so a libc hook can hand them straight on.
#define Z_EPERM			1
#define Z_ENOENT		2
#define Z_EIO			5
#define Z_EBADF			9
#define Z_EAGAIN		11		// fs busy: every reply slot unread; ask again
#define Z_EACCES		13
#define Z_EBUSY			16
#define Z_EEXIST		17
#define Z_EXDEV			18
#define Z_ENOTDIR		20
#define Z_EISDIR		21
#define Z_EINVAL		22
#define Z_EMFILE		24
#define Z_ENOSPC		28
#define Z_EROFS			30
#define Z_ENAMETOOLONG	91
#define Z_ENOTEMPTY		90
#define Z_ENODEV		19		// no `fs` running
#define Z_ETIMEDOUT		116		// `fs` did not answer

// Paths are at most Z_FS_PATH_MAX (zfs.h) bytes, the NUL included.

// Most bytes one READ or WRITE moves; a bigger call is several.
//
// Big, because every request pays one round trip (about 1-2ms on this
// machine -- docs/fs_driver.md, "What a request costs"), and the same
// whether the data goes through the grant window or comes back in the
// reply. The bound is fairness, not memory: while fs serves one request
// another client's stat waits -- 32KB is ~13ms from a RAM disk and
// ~45ms from the card.
#define Z_FS_CHUNK			32768

// Most bytes of directory entries one READDIR returns (a client keeps
// a buffer this size per open directory). Also the most a reply carries
// for any call but READ.
#define Z_FS_DIR_BYTES		2048

typedef struct {
	uint32_t	op;
	int32_t		fd;
	uint32_t	flags;
	uint32_t	len;		// READ/WRITE: bytes wanted
	int32_t		off;		// LSEEK: offset; FTRUNCATE: the new length
	uint32_t	whence;
	char		path[Z_FS_PATH_MAX];
	char		path2[Z_FS_PATH_MAX];
} z_fsreq_t;

// The reply's header; its data (len bytes) follows it directly.
typedef struct {
	int32_t		result;
	uint32_t	len;		// bytes of data after this header
	uint32_t	flags;		// Z_FSREP_*
} z_fsrep_t;

// The directory has no more entries, and fs has closed it: no READDIR
// or CLOSEDIR follows. Set on OPENDIR's reply (which carries the first
// page of entries) or READDIR's. A directory that fits one page is
// listed in one request.
#define Z_FSREP_END		0x0001

// READDIR's data: entries back to back, each 4-byte aligned.
typedef struct {
	z_fs_info_t	info;
	uint16_t	reclen;		// bytes to the next entry
	uint16_t	namelen;
	char		name[];		// NUL-terminated
} z_fs_dirent_t;

// STATVFS's data.
typedef struct {
	uint32_t	total_kb;
	uint32_t	free_kb;
} z_fs_vfs_t;

#endif
