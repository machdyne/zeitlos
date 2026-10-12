/*
 * For test_fs.c: net's NFS client (sw/apps/net/nfs.c), the real one,
 * over a fake TCP to a scripted NFSv3 server -- rpcbind, MOUNT and NFS
 * in one, with a small tree in memory. So what /nfs is tested through is
 * the whole path: zfsc -> fs -> net's nfs.c -> RPC/XDR on the wire ->
 * the server, and back.
 *
 * The fake TCP delivers in pieces (97 bytes a DATA event, sends taken
 * 100 bytes at a time), so record marks and replies are reassembled
 * across segments, as on a real network. Events are delivered by
 * net_pump(), which test_fs.c's z_msg_recv_match() runs while a reply
 * is awaited -- so fs's synchronous wait drives net, as two processes
 * would.
 */

#include "../../net/tcp.h"

// -- the fake TCP --

struct tcp_conn {
	bool used, est_pending;
	uint16_t lport, port;
	tcp_event_handler_t h;
	uint8_t in[2048];			// what the client sent, not yet a whole record
	uint32_t in_len;
	uint8_t out[16384];			// the server's answers, not yet delivered
	uint32_t out_len, out_off;
};
static struct tcp_conn tconns[4];
static uint32_t nfs_low_ports;		// connections from a port below 1024
static uint32_t nfs_calls[24];		// by procedure (NFS program only)
static bool srv_silent;				// the server answers nothing

static void srv_feed(struct tcp_conn *c);

tcp_conn_t *tcp_connect_from(uint32_t ip, uint16_t port, uint16_t lport,
	tcp_event_handler_t h, void *user) {
	(void)ip; (void)user;
	for (int i = 0; i < 4; i++) {
		if (tconns[i].used) continue;
		memset(&tconns[i], 0, sizeof(tconns[i]));
		tconns[i].used = true;
		tconns[i].est_pending = true;
		tconns[i].port = port;
		tconns[i].lport = lport;
		tconns[i].h = h;
		if (lport && lport < 1024) nfs_low_ports++;
		return &tconns[i];
	}
	return NULL;
}

uint16_t tcp_mss(const tcp_conn_t *c) { (void)c; return 100; }

bool tcp_send(tcp_conn_t *c, const uint8_t *d, uint16_t n) {
	if (!c->used || n > 100 || c->in_len + n > sizeof(c->in)) return false;
	memcpy(c->in + c->in_len, d, n);
	c->in_len += n;
	srv_feed(c);
	return true;
}

void tcp_abort(tcp_conn_t *c) { c->used = false; }

// One event, if any is due: an ESTABLISHED, or a piece of an answer.
static bool net_pump(void) {
	for (int i = 0; i < 4; i++) {
		struct tcp_conn *c = &tconns[i];
		if (!c->used) continue;
		if (c->est_pending) {
			c->est_pending = false;
			c->h(c, TCP_EVENT_ESTABLISHED, NULL, 0);
			return true;
		}
		if (c->out_off < c->out_len) {
			uint32_t k = c->out_len - c->out_off;
			if (k > 97) k = 97;
			uint8_t piece[97];
			memcpy(piece, c->out + c->out_off, k);
			c->out_off += k;
			if (c->out_off == c->out_len) c->out_off = c->out_len = 0;
			c->h(c, TCP_EVENT_DATA, piece, (uint16_t)k);
			return true;
		}
	}
	return false;
}

int netcfg_parse_ipv4(const char *s, uint32_t *out) {
	unsigned a, b, c, d;
	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
	*out = (a << 24) | (b << 16) | (c << 8) | d;
	return 0;
}

// -- the tree --

typedef struct { uint16_t id, parent; const char *name; bool dir; uint32_t size; } node_t;
static node_t nodes[80];
static int nnodes;
static char big_names[70][20];

static void tree_init(void) {
	nnodes = 0;
	nodes[nnodes++] = (node_t){ 1, 0, "", true, 0 };
	nodes[nnodes++] = (node_t){ 2, 1, "apps", true, 0 };
	nodes[nnodes++] = (node_t){ 3, 2, "text", false, 10000 };
	nodes[nnodes++] = (node_t){ 4, 1, "hello.txt", false, 5 };
	nodes[nnodes++] = (node_t){ 5, 1, "big", true, 0 };
	nodes[nnodes++] = (node_t){ 6, 1, "noattr", false, 3 };
	for (int i = 0; i < 70; i++) {
		snprintf(big_names[i], sizeof big_names[i], "file_%02d.txt", i);
		nodes[nnodes++] = (node_t){ (uint16_t)(10 + i), 5, big_names[i], false, (uint32_t)i };
	}
}

static node_t *node(uint32_t id) {
	for (int i = 0; i < nnodes; i++) if (nodes[i].id == id) return &nodes[i];
	return NULL;
}

static uint8_t file_byte(const node_t *nd, uint32_t i) {
	if (nd->id == 4) return (uint8_t)"hello"[i];
	if (nd->id == 6) return (uint8_t)"abc"[i];
	return (uint8_t)(i * 7 + 3 + nd->id);
}

// -- XDR, the server's side --

static uint8_t *w;
static void w32(uint32_t v) { w[0] = (uint8_t)(v >> 24); w[1] = (uint8_t)(v >> 16); w[2] = (uint8_t)(v >> 8); w[3] = (uint8_t)v; w += 4; }
static void w64(uint64_t v) { w32((uint32_t)(v >> 32)); w32((uint32_t)v); }
static void wop(const void *p, uint32_t n) { w32(n); memcpy(w, p, n); w += n; while (n & 3) { *w++ = 0; n++; } }
static void wfh(uint32_t id) { uint8_t fh[8] = { 'Z', 'F', 0, 0, (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id }; wop(fh, 8); }

static const uint8_t *r;
static uint32_t r32(void) { uint32_t v = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3]; r += 4; return v; }
static uint64_t r64(void) { uint64_t h = r32(); return (h << 32) | r32(); }
static const uint8_t *rop(uint32_t *n) { *n = r32(); const uint8_t *p = r; r += (*n + 3) & ~3u; return p; }
static uint32_t rfh(void) { uint32_t n; const uint8_t *p = rop(&n); if (n != 8 || p[0] != 'Z') return 0; return ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7]; }

static void wfattr(const node_t *nd) {
	w32(nd->dir ? 2 : 1);
	w32(nd->dir ? 0755 : 0644);
	w32(1); w32(1000); w32(1000);
	w64(nd->size); w64(nd->size);
	w32(0); w32(0);
	w64(1); w64(nd->id);
	w32(1700000000); w32(0);			// atime
	w32(1700000000); w32(0);			// mtime: 2023-11-14 22:13:20 UTC
	w32(1700000000); w32(0);
}

static void srv_call(struct tcp_conn *c, const uint8_t *body, uint32_t len) {
	(void)len;
	uint8_t *rec = c->out + c->out_len;
	r = body;
	uint32_t xid = r32();
	r32(); r32();						// CALL, RPC version
	uint32_t prog = r32(), vers = r32(), proc = r32();
	uint32_t n;
	r32(); rop(&n);						// credential
	r32(); rop(&n);						// verifier
	(void)vers;
	if (srv_silent) return;
	w = rec + 4;
	w32(xid); w32(1); w32(0); w32(0); w32(0); w32(0);	// accepted, SUCCESS
	if (prog == 100000) {				// rpcbind GETPORT
		uint32_t p = r32();
		w32(p == 100005 ? 20048 : p == 100003 ? 2049 : 0);
	} else if (prog == 100005) {		// MOUNT MNT
		const uint8_t *path = rop(&n);
		if (n == 7 && !memcmp(path, "/export", 7)) {
			w32(0); wfh(1); w32(1); w32(1);
		} else w32(13);
	} else if (prog == 100003) {
		if (proc < 24) nfs_calls[proc]++;
		if (proc == 1) {				// GETATTR
			node_t *nd = node(rfh());
			if (!nd) w32(70); else { w32(0); wfattr(nd); }
		} else if (proc == 3) {			// LOOKUP
			node_t *dir = node(rfh());
			const uint8_t *nm = rop(&n);
			node_t *hit = NULL;
			if (dir && !dir->dir) { w32(20); w32(0); }
			else {
				for (int i = 0; dir && i < nnodes; i++)
					if (nodes[i].parent == dir->id && strlen(nodes[i].name) == n &&
						!memcmp(nodes[i].name, nm, n)) hit = &nodes[i];
				if (!hit) { w32(2); w32(0); }
				else {
					w32(0); wfh(hit->id);
					if (hit->id == 6) w32(0);	// no attributes: the client asks
					else { w32(1); wfattr(hit); }
					w32(0);
				}
			}
		} else if (proc == 6) {			// READ
			node_t *nd = node(rfh());
			uint64_t off = r64();
			uint32_t cnt = r32();
			if (!nd || nd->dir) { w32(nd ? 21 : 70); w32(0); }
			else {
				uint32_t k = off >= nd->size ? 0 : (uint32_t)(nd->size - off);
				if (k > cnt) k = cnt;
				w32(0); w32(0);			// no attributes: the data starts early
				w32(k); w32(off + k >= nd->size);
				w32(k);
				for (uint32_t i = 0; i < k; i++) *w++ = file_byte(nd, (uint32_t)off + i);
				while ((uintptr_t)(w - rec) & 3) *w++ = 0;
			}
		} else if (proc == 17) {		// READDIRPLUS
			node_t *dir = node(rfh());
			uint64_t cookie = r64();
			r += 8;
			r32();
			uint32_t maxcount = r32();
			if (!dir || !dir->dir) { w32(dir ? 20 : 70); w32(0); }
			else {
				uint8_t *start = w;
				w32(0);
				if (dir->id == 5) w32(0);	// no directory attributes
				else { w32(1); wfattr(dir); }
				for (int i = 0; i < 8; i++) *w++ = (uint8_t)(0xA0 + i);	// verifier
				const char *nm[80]; node_t *nn[80]; int k = 0;
				nm[k] = "."; nn[k++] = dir;
				nm[k] = ".."; nn[k++] = dir;
				for (int i = 0; i < nnodes; i++)
					if (nodes[i].parent == dir->id) { nm[k] = nodes[i].name; nn[k++] = &nodes[i]; }
				bool eof = true;
				for (int i = (int)cookie; i < k; i++) {
					uint32_t l = (uint32_t)strlen(nm[i]);
					uint32_t need = 4 + 8 + 4 + ((l + 3) & ~3u) + 8 + 4 + 84 + 4 + 12 + 8;
					if ((uint32_t)(w - start) + need > maxcount) { eof = false; break; }
					w32(1); w64(nn[i]->id); wop(nm[i], l); w64((uint64_t)i + 1);
					w32(1); wfattr(nn[i]);
					w32(1); wfh(nn[i]->id);
				}
				w32(0); w32(eof);
			}
		} else {
			w32(10004);					// NFS3ERR_NOTSUPP
		}
	}
	uint32_t body_len = (uint32_t)(w - rec) - 4;
	rec[0] = (uint8_t)(0x80 | (body_len >> 24)); rec[1] = (uint8_t)(body_len >> 16);
	rec[2] = (uint8_t)(body_len >> 8); rec[3] = (uint8_t)body_len;
	c->out_len += body_len + 4;
}

static void srv_feed(struct tcp_conn *c) {
	while (c->in_len >= 4) {
		uint32_t len = ((uint32_t)(c->in[0] & 0x7f) << 24) | ((uint32_t)c->in[1] << 16) |
			((uint32_t)c->in[2] << 8) | c->in[3];
		if (c->in_len < 4 + len) return;
		srv_call(c, c->in + 4, len);
		memmove(c->in, c->in + 4 + len, c->in_len - 4 - len);
		c->in_len -= 4 + len;
	}
}

#define NFS_HOST_TEST
#include "../../net/nfs.c"
