#ifndef Z_NFSPROTO_H
#define Z_NFSPROTO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net's NFS client, as fs and the kernel shell ask it (docs/nfs.md).
 *
 * net speaks NFSv3 over TCP to the server, read-only. fs serves /nfs by
 * asking net, one request at a time, with the server's file handles in
 * hand: net keeps no state per file, so an fs restart or a client that
 * dies leaves nothing behind in net. The kernel shell's `nfs` asks for
 * mounting and status.
 *
 * Request: Z_NET_NFS_REQ, a Z_BLOB holding a z_nfsreq_t, to net.
 * Reply:   Z_NET_NFS_REPLY, same tag, a Z_BLOB: a z_nfsrep_t and then its
 *          data. The blob is net's memory, borrowed: valid until the
 *          asker sends net anything else (copy it first).
 * Event:   Z_NET_NFS_EVENT, Z_UINT32 1 mounted / 0 not, to fs -- so it
 *          lists /nfs at the root, and does not ask net while unmounted.
 */

#include <stdint.h>
#include "zsubjects.h"
#include "zfs.h"			// z_fs_info_t, Z_FS_PATH_MAX

#define Z_NET_NFS_REQ		(Z_SUBJ_NET + 18)
#define Z_NET_NFS_REPLY		(Z_SUBJ_NET + 19)
#define Z_NET_NFS_EVENT		(Z_SUBJ_NET + 20)
Z_SUBJECTS_IN(Z_SUBJ_NET, Z_NET_NFS_EVENT);

#define Z_NFS_FH_MAX		64			// NFS3_FHSIZE
#define Z_NFS_SPEC_MAX		128			// "host:/export"

// The volume: paths below it are the export's.
#define Z_DIR_NFS			"/nfs"

// Operations (z_nfsreq_t.op).
#define Z_NFSOP_LOOKUP		1	// path (below /nfs, "/apps/x")   -> 0, fh, info
#define Z_NFSOP_READ		2	// fh, off, len                    -> bytes, the data; EOF
#define Z_NFSOP_READDIR		3	// fh, cookie, verf, len           -> entries as z_fs_dirent_t; cookie, verf; EOF
#define Z_NFSOP_MOUNT		4	// path "host:/export"             -> 0 once mounted
#define Z_NFSOP_UMOUNT		5	//                                  -> 0
#define Z_NFSOP_STATUS		6	//                                  -> 1 mounted, 0 not; the spec as data

typedef struct {
	uint32_t	op;
	uint32_t	len;			// READ: bytes wanted; READDIR: the page's bytes
	uint64_t	off;			// READ: the offset; READDIR: the cookie (0 first)
	uint8_t		verf[8];		// READDIR: the cookie verifier from the last page
	uint32_t	fh_len;
	uint8_t		fh[Z_NFS_FH_MAX];
	char		path[Z_FS_PATH_MAX];
} z_nfsreq_t;

typedef struct {
	int32_t		result;			// >= 0 done; -Z_E* (zfsproto.h)
	uint32_t	len;			// bytes of data after this header
	uint32_t	flags;			// Z_NFSREP_*
	uint32_t	fh_len;
	uint64_t	cookie;			// READDIR: where the next page starts
	uint8_t		verf[8];
	z_fs_info_t	info;			// LOOKUP
	uint8_t		fh[Z_NFS_FH_MAX];	// LOOKUP
} z_nfsrep_t;

#define Z_NFSREP_EOF		0x0001	// READ: the end of the file; READDIR: no more entries
#define Z_NFSREP_MOUNTING	0x0002	// STATUS: a mount is under way

// Most data one READ returns: a reply is assembled whole in net.
#define Z_NFS_RSIZE			4096

#endif
