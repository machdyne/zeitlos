/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * NFSv3 over TCP, read-only -- see nfs.h, znfsproto.h and docs/nfs.md.
 *
 * Why these choices:
 *   - v3 over TCP: current Linux servers have v2 and UDP off by default,
 *     and TCP is what net does best (the send window, round 5).
 *   - Read-only: what fetching programs and files needs; nothing on the
 *     server can be damaged from here.
 *   - Stateless towards fs: fs holds the server's file handles and asks
 *     with them, so nothing here is per file. One request at a time,
 *     which is how fs asks anyway.
 *   - From a port below 1024, so the export needs no `insecure`.
 *
 * Mounting is three conversations, one after the other, each its own TCP
 * connection: rpcbind (port 111) for the MOUNT and NFS ports, MOUNT for
 * the export's root handle, then the NFS connection, which stays open --
 * and is opened again if it drops.
 *
 * A reply is assembled whole in rx[] (record marking, RFC 5531 11), and
 * the answer to fs is built in the same buffer: its header in front of
 * the data, which is moved down to meet it -- a READ's data, or a
 * listing converted to fs's z_fs_dirent_t, which only ever gets shorter.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "../../common/zeitlos.h"
#include "../../common/zfsproto.h"
#include "../../common/znfsproto.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../fs/fsvol.h"		// fs_fattime_of(): FAT's packed time, as fs gives it
#include "tcp.h"
#include "netcfg.h"				// netcfg_parse_ipv4()
#include "nfs.h"

#define PMAP_PROG		100000
#define PMAP_VERS		2
#define PMAP_GETPORT	3
#define MOUNT_PROG		100005
#define MOUNT_VERS		3
#define MOUNT_MNT		1
#define NFS_PROG		100003
#define NFS_VERS		3
#define NFS3_GETATTR	1
#define NFS3_LOOKUP		3
#define NFS3_READ		6
#define NFS3_READDIRPLUS 17
#define NFS_PORT_DEFAULT 2049

#ifndef NFS_TIMEOUT
#define NFS_TIMEOUT		(Z_TICK_HZ * 5)	// one request, or one mount step
#endif
#define NFS_RETRY		(Z_TICK_HZ * 30)	// net.cfg's mount, after a failure
#define NFS_RX_CAP		(Z_NFS_RSIZE + 512)
#define NFS_TX_CAP		512
#define NFS_CACHE_PATH	96
#define HDR				((uint32_t)sizeof(z_nfsrep_t))

enum { N_OFF = 0, N_MOUNTING, N_UP };
// What the reply in progress is for.
enum { P_NONE = 0, P_GETPORT_MNT, P_GETPORT_NFS, P_MNT, P_LOOKUP, P_GETATTR,
	P_READ, P_READDIR };

static struct {
	uint8_t st;
	uint8_t phase;
	uint32_t ip;
	char spec[Z_NFS_SPEC_MAX];
	const char *exp;			// the export's path, inside spec
	uint16_t mport, nport;
	uint8_t root[Z_NFS_FH_MAX];
	uint32_t root_len;

	tcp_conn_t *c;
	bool connected;
	uint16_t next_lport;
	uint32_t xid;
	uint32_t tx_len, tx_sent;	// the call waiting to go (tx_sent < tx_len)

	uint8_t mark[4];			// the record mark being read
	uint8_t mark_n;
	uint32_t frag_left;
	bool frag_last;
	uint32_t rx_len;

	bool busy;					// a request is in progress
	uint32_t from, tag;			// whom to answer
	uint32_t deadline;
	z_nfsreq_t rq;
	const char *walk;			// LOOKUP: what is left of the path
	bool walk_cached;			// ... and whether its parent came from the cache
	uint8_t cur[Z_NFS_FH_MAX];
	uint32_t cur_len;

	char cache_path[NFS_CACHE_PATH];	// one directory and its handle
	uint8_t cache_fh[Z_NFS_FH_MAX];
	uint32_t cache_len;

	bool retry;					// net.cfg's mount: try again until it is made
	uint32_t retry_at;
} n;

static uint8_t rx[NFS_RX_CAP] __attribute__((aligned(8)));
static uint8_t tx[NFS_TX_CAP] __attribute__((aligned(4)));
static z_blob_t rep_blob;
// STATUS's answer, apart from rx[], which may hold a request's reply.
static struct { z_nfsrep_t h; char spec[Z_NFS_SPEC_MAX]; } status_rep;

// -- XDR --

static uint32_t tx_at;

static void put32(uint32_t v) {
	if (tx_at + 4 > NFS_TX_CAP) { tx_at = NFS_TX_CAP + 1; return; }
	tx[tx_at++] = (uint8_t)(v >> 24); tx[tx_at++] = (uint8_t)(v >> 16);
	tx[tx_at++] = (uint8_t)(v >> 8);  tx[tx_at++] = (uint8_t)v;
}

static void put64(uint64_t v) {
	put32((uint32_t)(v >> 32));
	put32((uint32_t)v);
}

static void put_opaque(const void *p, uint32_t len, bool fixed) {
	if (!fixed) put32(len);
	if (tx_at + ((len + 3) & ~3u) > NFS_TX_CAP) { tx_at = NFS_TX_CAP + 1; return; }
	memcpy(tx + tx_at, p, len);
	tx_at += len;
	while (tx_at & 3) tx[tx_at++] = 0;
}

typedef struct { const uint8_t *p; uint32_t left; bool bad; } xdr_t;

static uint32_t get32(xdr_t *x) {
	if (x->left < 4) { x->bad = true; x->left = 0; return 0; }
	uint32_t v = ((uint32_t)x->p[0] << 24) | ((uint32_t)x->p[1] << 16) |
		((uint32_t)x->p[2] << 8) | x->p[3];
	x->p += 4;
	x->left -= 4;
	return v;
}

static uint64_t get64(xdr_t *x) {
	uint64_t hi = get32(x);
	return (hi << 32) | get32(x);
}

static const uint8_t *get_opaque(xdr_t *x, uint32_t len) {
	uint32_t padded = (len + 3) & ~3u;
	if (len > x->left || padded > x->left) { x->bad = true; x->left = 0; return NULL; }
	const uint8_t *p = x->p;
	x->p += padded;
	x->left -= padded;
	return p;
}

static void skip(xdr_t *x, uint32_t bytes) { get_opaque(x, bytes); }

// An NFS3 status to a -Z_E* (zfsproto.h).
static int32_t nfs_err(uint32_t st) {
	switch (st) {
	case 1: case 13: return -Z_EACCES;
	case 2:  return -Z_ENOENT;
	case 20: return -Z_ENOTDIR;
	case 21: return -Z_EISDIR;
	case 22: return -Z_EINVAL;
	case 30: return -Z_EROFS;
	case 63: return -Z_ENAMETOOLONG;
	default: return -Z_EIO;
	}
}

// fattr3 into fs's z_fs_info_t: everything is read-only here.
static void get_fattr(xdr_t *x, z_fs_info_t *fi) {
	uint32_t type = get32(x);
	skip(x, 16);				// mode, nlink, uid, gid
	uint64_t size = get64(x);
	skip(x, 8 + 8 + 8 + 8 + 8);	// used, rdev, fsid, fileid, atime
	uint32_t mtime = get32(x);
	skip(x, 4 + 8);				// mtime's nanoseconds, ctime
	memset(fi, 0, sizeof(*fi));
	bool dir = (type == 2);
	fi->type = dir ? Z_FS_TYPE_DIR : Z_FS_TYPE_FILE;
	fi->attr = Z_FS_ATTR_RDONLY | (dir ? Z_FS_ATTR_DIR : 0);
	fi->size = dir ? 0 : (size > 0xffffffffull ? 0xffffffffu : (uint32_t)size);
	if (mtime >= 315532800u) {	// FAT cannot say anything before 1980
		uint32_t ft = fs_fattime_of(mtime);
		fi->fdate = (uint16_t)(ft >> 16);
		fi->ftime = (uint16_t)ft;
	}
}

// post_op_attr: true if there were attributes.
static bool get_post_attr(xdr_t *x, z_fs_info_t *fi) {
	if (!get32(x)) return false;
	get_fattr(x, fi);
	return true;
}

// -- answering --

static z_nfsrep_t *rep(void) { return (z_nfsrep_t *)rx; }

static void answer(uint32_t to, uint32_t tag, int32_t result, uint32_t len) {
	z_nfsrep_t *r = rep();
	r->result = result;
	r->len = len;
	if (!to) return;			// a mount at boot: nobody asked
	rep_blob.len = HDR + len;
	rep_blob.data = rx;
	z_obj_t o;
	o.type = Z_BLOB;
	o.val.ptr = &rep_blob;
	z_msg_new_send(to, Z_NET_NFS_REPLY, tag, o);
}

// The request in progress, finished.
static void done(int32_t result, uint32_t len) {
	n.busy = false;
	n.phase = P_NONE;
	answer(n.from, n.tag, result, len);
}

// A fresh header for an answer with no data, or with the fields set by
// the caller after this.
static void rep_clear(void) {
	memset(rx, 0, HDR);
}

static void tell_fs(bool up) {
	uint32_t pid;
	if (!z_pid_lookup(Z_FS_SERVICE, &pid)) return;
	z_msg_new_send(pid, Z_NET_NFS_EVENT, 0, z_obj_uint32(up ? 1u : 0u));
}

// -- the connection --

static void on_tcp(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len);
static void got_reply(void);

static void conn_drop(void) {
	if (n.c) tcp_abort(n.c);
	n.c = NULL;
	n.connected = false;
	n.tx_len = n.tx_sent = 0;
	n.mark_n = 0;
	n.frag_left = 0;
	n.rx_len = 0;
}

// To `port` on the server, from a port below 1024 (tcp_connect_from()).
static bool conn_open(uint16_t port) {
	conn_drop();
	for (int i = 0; i < 16 && !n.c; i++) {
		if (n.next_lport < 600 || n.next_lport > 1023) n.next_lport = 1023;
		n.c = tcp_connect_from(n.ip, port, n.next_lport--, on_tcp, NULL);
	}
	return n.c != NULL;
}

static void tx_pump(void) {
	if (!n.connected || !n.c) return;
	while (n.tx_sent < n.tx_len) {
		uint16_t mss = tcp_mss(n.c);
		uint32_t k = n.tx_len - n.tx_sent;
		if (k > mss) k = mss;
		if (!tcp_send(n.c, tx + n.tx_sent, (uint16_t)k)) break;
		n.tx_sent += k;
	}
}

// A call's header into tx[], after room for its record mark.
static void call_begin(uint32_t prog, uint32_t vers, uint32_t proc) {
	tx_at = 4;
	put32(++n.xid);
	put32(0);				// CALL
	put32(2);				// RPC version
	put32(prog);
	put32(vers);
	put32(proc);
	// AUTH_SYS: uid 0, gid 0 -- an export squashes them as it likes
	put32(1);
	put32(4 + 12 + 4 + 4 + 4);
	put32(0);				// stamp
	put_opaque("zeitlos", 7, false);
	put32(0);				// uid
	put32(0);				// gid
	put32(0);				// no other groups
	put32(0);				// verifier: AUTH_NONE
	put32(0);
}

static bool call_send(uint8_t phase) {
	if (tx_at > NFS_TX_CAP) return false;
	uint32_t body = tx_at - 4;
	tx[0] = (uint8_t)(0x80 | (body >> 24));
	tx[1] = (uint8_t)(body >> 16);
	tx[2] = (uint8_t)(body >> 8);
	tx[3] = (uint8_t)body;
	n.tx_len = tx_at;
	n.tx_sent = 0;
	n.rx_len = 0;
	n.mark_n = 0;
	n.frag_left = 0;
	n.phase = phase;
	n.deadline = z_uptime_ticks() + NFS_TIMEOUT;
	tx_pump();
	return true;
}

static void on_tcp(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len) {
	if (c != n.c) return;
	switch (ev) {
	case TCP_EVENT_ESTABLISHED:
		n.connected = true;
		tx_pump();
		break;
	case TCP_EVENT_DATA:
		while (len) {
			if (n.mark_n < 4) {
				n.mark[n.mark_n++] = *data++;
				len--;
				if (n.mark_n == 4) {
					n.frag_left = ((uint32_t)(n.mark[0] & 0x7f) << 24) |
						((uint32_t)n.mark[1] << 16) | ((uint32_t)n.mark[2] << 8) | n.mark[3];
					n.frag_last = (n.mark[0] & 0x80) != 0;
				}
				continue;
			}
			uint32_t k = n.frag_left < len ? n.frag_left : len;
			if (n.rx_len + k > NFS_RX_CAP) {	// a reply bigger than asked for
				n.rx_len = NFS_RX_CAP + 1;
				break;
			}
			memcpy(rx + n.rx_len, data, k);
			n.rx_len += k;
			data += k;
			len = (uint16_t)(len - k);
			n.frag_left -= k;
			if (n.frag_left == 0) {
				n.mark_n = 0;
				if (n.frag_last) { got_reply(); n.rx_len = 0; }
			}
		}
		if (n.rx_len > NFS_RX_CAP) {
			printf("nfs: a reply too big for the buffer -- connection dropped\n");
			conn_drop();
			if (n.busy) done(-Z_EIO, 0);
		}
		break;
	case TCP_EVENT_CLOSED:
	case TCP_EVENT_EOF:
		n.c = NULL;
		n.connected = false;
		// The NFS connection closing between requests is fine: the next
		// one opens it again. Mid-request, or mid-mount, it is a failure.
		if (n.phase != P_NONE) {
			uint8_t ph = n.phase;
			n.phase = P_NONE;
			if (ph <= P_MNT) {
				printf("nfs: mount failed: the server closed the connection\n");
				n.st = N_OFF;
			}
			if (n.busy) done(-Z_EIO, 0);
		}
		break;
	}
}

// -- mounting --

static void mount_fail(const char *why) {
	printf("nfs: mount of %s failed: %s%s\n", n.spec, why,
		n.retry ? " (trying again in 30 s)" : "");
	n.retry_at = z_uptime_ticks() + NFS_RETRY;
	conn_drop();
	n.st = N_OFF;
	if (n.busy) { rep_clear(); done(-Z_EIO, 0); }
	n.phase = P_NONE;
}

static void getport(uint32_t prog, uint32_t vers, uint8_t phase) {
	call_begin(PMAP_PROG, PMAP_VERS, PMAP_GETPORT);
	put32(prog);
	put32(vers);
	put32(6);				// TCP
	put32(0);
	call_send(phase);
}

static int32_t mount_start(const char *spec) {
	char host[24];
	const char *colon = strchr(spec, ':');
	uint32_t hl = colon ? (uint32_t)(colon - spec) : 0;
	if (!colon || !hl || hl >= sizeof(host) || colon[1] != '/' ||
		strlen(spec) >= sizeof(n.spec))
		return -Z_EINVAL;
	memcpy(host, spec, hl);
	host[hl] = 0;
	uint32_t ip;
	if (netcfg_parse_ipv4(host, &ip) != 0) return -Z_EINVAL;
	conn_drop();
	if (n.st == N_UP) tell_fs(false);
	n.st = N_MOUNTING;
	n.ip = ip;
	strcpy(n.spec, spec);
	n.exp = n.spec + hl + 1;
	n.cache_len = 0;
	n.root_len = 0;
	printf("nfs: mounting %s\n", n.spec);
	if (!conn_open(111)) { mount_fail("no connection slot"); return -Z_EIO; }
	getport(MOUNT_PROG, MOUNT_VERS, P_GETPORT_MNT);
	return 0;
}

// -- requests --

// The next component of a LOOKUP's path, or the end.
static void walk_next(void);
static void nfs_call(uint8_t phase);

static void lookup_reply(xdr_t *x) {
	uint32_t st = get32(x);
	if (st) { done(nfs_err(st), 0); return; }
	uint32_t fl = get32(x);
	const uint8_t *fh = (fl <= Z_NFS_FH_MAX) ? get_opaque(x, fl) : NULL;
	if (!fh || x->bad) { done(-Z_EIO, 0); return; }
	memcpy(n.cur, fh, fl);
	n.cur_len = fl;
	z_fs_info_t fi;
	bool have = get_post_attr(x, &fi);
	const char *rest = n.walk;
	while (*rest == '/') rest++;
	if (*rest) {
		// Not the last component: note this directory as the cache's
		// when it is the target's parent (what is left has no '/').
		if (!strchr(rest, '/')) {
			uint32_t pl = (uint32_t)(n.walk - n.rq.path);
			while (pl && n.rq.path[pl - 1] == '/') pl--;
			if (pl < NFS_CACHE_PATH) {
				memcpy(n.cache_path, n.rq.path, pl);
				n.cache_path[pl] = 0;
				memcpy(n.cache_fh, n.cur, n.cur_len);
				n.cache_len = n.cur_len;
			}
		}
		walk_next();
		return;
	}
	if (!have || x->bad) {		// no attributes with it: ask for them
		call_begin(NFS_PROG, NFS_VERS, NFS3_GETATTR);
		put_opaque(n.cur, n.cur_len, false);
		nfs_call(P_GETATTR);
		return;
	}
	rep_clear();
	rep()->info = fi;
	rep()->fh_len = n.cur_len;
	memcpy(rep()->fh, n.cur, n.cur_len);
	done(0, 0);
}

static void walk_next(void) {
	const char *w = n.walk;
	while (*w == '/') w++;
	if (!*w) {					// the export's root, or a trailing '/'
		n.walk = w;
		call_begin(NFS_PROG, NFS_VERS, NFS3_GETATTR);
		put_opaque(n.cur, n.cur_len, false);
		nfs_call(P_GETATTR);
		return;
	}
	const char *e = strchr(w, '/');
	uint32_t len = e ? (uint32_t)(e - w) : (uint32_t)strlen(w);
	if (len > 255) { done(-Z_ENAMETOOLONG, 0); return; }
	n.walk = w + len;
	call_begin(NFS_PROG, NFS_VERS, NFS3_LOOKUP);
	put_opaque(n.cur, n.cur_len, false);
	put_opaque(w, len, false);
	nfs_call(P_LOOKUP);
}

static void lookup_start(void) {
	const char *path = n.rq.path;
	const char *last = strrchr(path, '/');
	// The cache holds the target's parent: one LOOKUP instead of a walk.
	if (n.cache_len && last && last[1]) {
		uint32_t pl = (uint32_t)(last - path);
		while (pl && path[pl - 1] == '/') pl--;
		if (pl == strlen(n.cache_path) && !memcmp(path, n.cache_path, pl)) {
			memcpy(n.cur, n.cache_fh, n.cache_len);
			n.cur_len = n.cache_len;
			n.walk = last;
			walk_next();
			return;
		}
	}
	memcpy(n.cur, n.root, n.root_len);
	n.cur_len = n.root_len;
	n.walk = path;
	walk_next();
}

static void read_reply(xdr_t *x) {
	z_fs_info_t fi;
	uint32_t st = get32(x);
	get_post_attr(x, &fi);
	if (st) { done(nfs_err(st), 0); return; }
	uint32_t count = get32(x);
	bool eof = get32(x) != 0;
	uint32_t dl = get32(x);
	const uint8_t *d = get_opaque(x, dl);
	if (!d || x->bad || dl > Z_NFS_RSIZE) { done(-Z_EIO, 0); return; }
	(void)count;
	memmove(rx + HDR, d, dl);		// down (or up) to meet the header
	rep_clear();
	if (eof) rep()->flags = Z_NFSREP_EOF;
	done((int32_t)dl, dl);
}

// READDIRPLUS's entries into fs's z_fs_dirent_t, in place: each comes
// out shorter than it went in, and the input is first moved up clear of
// the header if it starts too near the front.
static void readdir_reply(xdr_t *x) {
	z_fs_info_t fi;
	uint8_t verf[8];
	uint32_t st = get32(x);
	get_post_attr(x, &fi);
	if (st) { done(nfs_err(st), 0); return; }
	const uint8_t *v = get_opaque(x, 8);
	if (!v || x->bad) { done(-Z_EIO, 0); return; }
	memcpy(verf, v, 8);
	uint32_t at = (uint32_t)(x->p - rx);
	if (at < HDR + 16) {
		uint32_t shift = HDR + 16 - at;
		if (n.rx_len + shift > NFS_RX_CAP) { done(-Z_EIO, 0); return; }
		memmove(rx + at + shift, rx + at, x->left);
		x->p += shift;
	}
	uint32_t page = n.rq.len ? n.rq.len : Z_FS_DIR_BYTES;
	if (page > Z_FS_DIR_BYTES) page = Z_FS_DIR_BYTES;
	uint32_t out = HDR, entries = 0;
	uint64_t cookie = n.rq.off;
	bool eof = false, full = false;
	for (;;) {
		if (!get32(x)) { eof = get32(x) != 0; break; }
		skip(x, 8);					// fileid
		uint32_t nl = get32(x);
		const uint8_t *name = get_opaque(x, nl);
		uint64_t ck = get64(x);
		bool have = get_post_attr(x, &fi);
		if (get32(x)) {				// post_op_fh3
			uint32_t fl = get32(x);
			get_opaque(x, fl);
		}
		if (x->bad || !name || nl > 255) { done(-Z_EIO, 0); return; }
		if ((nl == 1 && name[0] == '.') || (nl == 2 && name[0] == '.' && name[1] == '.')) {
			cookie = ck;
			continue;
		}
		uint32_t rl = (uint32_t)((sizeof(z_fs_dirent_t) + nl + 1 + 3) & ~3u);
		if (out - HDR + rl > page) { full = true; break; }	// the next page has it
		z_fs_dirent_t *e = (z_fs_dirent_t *)(rx + out);
		memmove(e->name, name, nl);	// before the header: it may overlap the name
		e->name[nl] = 0;
		if (!have) {
			memset(&fi, 0, sizeof(fi));
			fi.attr = Z_FS_ATTR_RDONLY;
		}
		e->info = fi;
		e->namelen = (uint16_t)nl;
		e->reclen = (uint16_t)rl;
		out += rl;
		entries++;
		cookie = ck;
	}
	if (full) eof = false;
	uint32_t len = out - HDR;
	rep_clear();
	rep()->cookie = cookie;
	memcpy(rep()->verf, verf, 8);
	if (eof) rep()->flags = Z_NFSREP_EOF;
	done((int32_t)entries, len);
}

// A whole reply is in rx[]: the RPC envelope, then the results.
static void got_reply(void) {
	xdr_t x = { rx, n.rx_len, false };
	uint32_t xid = get32(&x);
	if (xid != n.xid || get32(&x) != 1 /* REPLY */) return;	// not ours: ignore it
	if (get32(&x) != 0) {			// MSG_DENIED
		if (n.phase <= P_MNT) mount_fail("the server refused the call");
		else if (n.busy) done(-Z_EACCES, 0);
		return;
	}
	get32(&x);						// verifier flavour
	skip(&x, get32(&x));
	if (get32(&x) != 0 || x.bad) {	// accept_stat: not SUCCESS
		if (n.phase <= P_MNT) mount_fail("the call was not accepted (no such program?)");
		else if (n.busy) done(-Z_EIO, 0);
		return;
	}

	uint8_t ph = n.phase;
	n.phase = P_NONE;
	switch (ph) {
	case P_GETPORT_MNT:
		n.mport = (uint16_t)get32(&x);
		if (!n.mport || x.bad) { mount_fail("no MOUNT service registered (rpcbind)"); return; }
		getport(NFS_PROG, NFS_VERS, P_GETPORT_NFS);
		return;
	case P_GETPORT_NFS:
		n.nport = (uint16_t)get32(&x);
		if (!n.nport) n.nport = NFS_PORT_DEFAULT;
		if (!conn_open(n.mport)) { mount_fail("no connection slot"); return; }
		call_begin(MOUNT_PROG, MOUNT_VERS, MOUNT_MNT);
		put_opaque(n.exp, (uint32_t)strlen(n.exp), false);
		call_send(P_MNT);
		return;
	case P_MNT: {
		uint32_t st = get32(&x);
		if (st) {
			mount_fail(st == 13 ? "access denied (check /etc/exports)" :
				st == 2 ? "no such export" : "the server said no");
			return;
		}
		uint32_t fl = get32(&x);
		const uint8_t *fh = fl <= Z_NFS_FH_MAX ? get_opaque(&x, fl) : NULL;
		if (!fh || x.bad) { mount_fail("a bad reply"); return; }
		memcpy(n.root, fh, fl);
		n.root_len = fl;
		conn_drop();
		n.st = N_UP;
		n.retry = false;
		printf("nfs: %s mounted at " Z_DIR_NFS "\n", n.spec);
		tell_fs(true);
		if (n.busy) { rep_clear(); done(0, 0); }
		return;
	}
	case P_LOOKUP:  lookup_reply(&x); return;
	case P_GETATTR: {
		uint32_t st = get32(&x);
		if (st) { done(nfs_err(st), 0); return; }
		z_fs_info_t fi;
		get_fattr(&x, &fi);
		if (x.bad) { done(-Z_EIO, 0); return; }
		rep_clear();
		rep()->info = fi;
		rep()->fh_len = n.cur_len;
		memcpy(rep()->fh, n.cur, n.cur_len);
		done(0, 0);
		return;
	}
	case P_READ:    read_reply(&x); return;
	case P_READDIR: readdir_reply(&x); return;
	default: return;
	}
}

// The NFS connection, open (or opening) for the call in tx[].
static void nfs_call(uint8_t phase) {
	if (!n.c && !conn_open(n.nport)) { done(-Z_EIO, 0); return; }
	call_send(phase);
}

static bool fh_ok(const z_nfsreq_t *rq) {
	return rq->fh_len && rq->fh_len <= Z_NFS_FH_MAX;
}

void nfs_handle_msg(const z_msg_t *msg) {
	const z_nfsreq_t *rq = (msg->obj.type == Z_BLOB) ?
		(const z_nfsreq_t *)z_blob_data(&msg->obj) : NULL;
	if (!rq || z_blob_len(&msg->obj) < sizeof(z_nfsreq_t) ||
		!memchr(rq->path, 0, sizeof(rq->path))) {
		rep_clear();
		answer(msg->from, msg->tag, -Z_EINVAL, 0);
		return;
	}
	if (rq->op == Z_NFSOP_STATUS) {
		static z_blob_t sb;
		uint32_t l = n.st == N_OFF ? 0 : (uint32_t)strlen(n.spec) + 1;
		memset(&status_rep.h, 0, sizeof(status_rep.h));
		memcpy(status_rep.spec, n.spec, l);
		status_rep.h.result = n.st == N_UP ? 1 : 0;
		status_rep.h.len = l;
		if (n.st == N_MOUNTING) status_rep.h.flags = Z_NFSREP_MOUNTING;
		sb.len = HDR + l;
		sb.data = (uint8_t *)&status_rep;
		z_obj_t o;
		o.type = Z_BLOB;
		o.val.ptr = &sb;
		z_msg_new_send(msg->from, Z_NET_NFS_REPLY, msg->tag, o);
		return;
	}
	if (n.busy) {
		rep_clear();
		answer(msg->from, msg->tag, -Z_EBUSY, 0);
		return;
	}
	if (rq->op == Z_NFSOP_MOUNT || rq->op == Z_NFSOP_UMOUNT) n.retry = false;
	if (rq->op == Z_NFSOP_UMOUNT) {
		conn_drop();
		if (n.st == N_UP) tell_fs(false);
		n.st = N_OFF;
		n.cache_len = 0;
		printf("nfs: unmounted\n");
		rep_clear();
		answer(msg->from, msg->tag, 0, 0);
		return;
	}
	n.busy = true;
	n.from = msg->from;
	n.tag = msg->tag;
	n.rq = *rq;
	n.deadline = z_uptime_ticks() + NFS_TIMEOUT;
	if (rq->op == Z_NFSOP_MOUNT) {
		int32_t r = mount_start(rq->path);
		if (r) { rep_clear(); done(r, 0); }
		return;
	}
	if (n.st != N_UP) { rep_clear(); done(-Z_ENODEV, 0); return; }
	switch (rq->op) {
	case Z_NFSOP_LOOKUP:
		lookup_start();
		return;
	case Z_NFSOP_READ: {
		if (!fh_ok(rq)) { done(-Z_EINVAL, 0); return; }
		uint32_t want = rq->len < Z_NFS_RSIZE ? rq->len : Z_NFS_RSIZE;
		call_begin(NFS_PROG, NFS_VERS, NFS3_READ);
		put_opaque(rq->fh, rq->fh_len, false);
		put64(rq->off);
		put32(want);
		nfs_call(P_READ);
		return;
	}
	case Z_NFSOP_READDIR:
		if (!fh_ok(rq)) { done(-Z_EINVAL, 0); return; }
		call_begin(NFS_PROG, NFS_VERS, NFS3_READDIRPLUS);
		put_opaque(rq->fh, rq->fh_len, false);
		put64(rq->off);
		put_opaque(rq->verf, 8, true);
		put32(1024);				// dircount
		put32(Z_NFS_RSIZE);			// maxcount: what rx[] holds
		nfs_call(P_READDIR);
		return;
	default:
		rep_clear();
		done(-Z_EINVAL, 0);
	}
}

// net.cfg's mount, at start and again every NFS_RETRY until it is made
// -- the network may not be up yet, or the server not on -- unless the
// shell mounts or unmounts something itself meanwhile.
static void boot_try(const char *spec) {
	n.busy = true;
	n.from = 0;
	n.tag = 0;
	n.deadline = z_uptime_ticks() + NFS_TIMEOUT;
	if (mount_start(spec)) {
		printf("nfs: net.cfg's nfs=%s: give it as address:/export\n", spec);
		n.busy = false;
		n.retry = false;
	}
}

void nfs_mount_at_boot(const char *spec) {
	if (!spec || !spec[0] || n.busy) return;
	n.retry = true;
	boot_try(spec);
}

void nfs_poll(void) {
	if (n.retry && n.st == N_OFF && !n.busy && n.phase == P_NONE &&
		(int32_t)(z_uptime_ticks() - n.retry_at) >= 0) {
		char spec[Z_NFS_SPEC_MAX];
		strcpy(spec, n.spec);
		boot_try(spec);
	}
	if (n.tx_sent < n.tx_len) tx_pump();
	if (n.phase == P_NONE) return;
	if ((int32_t)(z_uptime_ticks() - n.deadline) < 0) return;
	uint8_t ph = n.phase;
	n.phase = P_NONE;
	conn_drop();
	if (ph <= P_MNT) {
		mount_fail("no answer from the server");
		return;
	}
	if (n.busy) {
		rep_clear();
		done(-Z_ETIMEDOUT, 0);
	}
}

#ifdef NFS_HOST_TEST
// For tests/test_nfs.c.
bool nfs_test_up(void) { return n.st == N_UP; }
void nfs_test_reset(void) { conn_drop(); memset(&n, 0, sizeof(n)); }
#endif
