/*
 * Host test for zeitlos/fed.c -- fed on Zeitlos -- against a REAL Linux
 * fed over REAL TCP. docs/fed.md, "Building it", step 5.
 *
 *   make -C sw/apps/fed test      (needs vm.mmap_min_addr=0; skips otherwise)
 *
 * The real zeitlos/fed.c and core; this file plays the kernel (a mailbox,
 * the clock, the process table, the key/value store) and net: fed's
 * LISTEN, its outbound CONNECTs made into real TCP connections to the
 * Linux daemon, real TCP connections accepted and handed to fed as net
 * hands them (a tagged CONNECT, DATA both ways, acked), and DNS. And a
 * local client on fed0, speaking the same lines as fed.sock on Linux.
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define main fed_main_unused
#include "../zeitlos/fed.c"
#undef main
#include "../../../common/zproc.h"
#include "../core/fradio.h"
#include "../core/fobj.h"

// fed.c's main() (not run here) and node_seed() call these; zrng.c is
// not linked on the host.
bool z_rng_secure(void) { return true; }
bool z_launch_arg_take(char *out, int outlen) { (void)out; (void)outlen; return false; }
void z_rng_bytes(void *b, uint32_t n) { plat_random(b, n); }

extern int plat_quiet;

#define NET    50
#define CLIENT 70
#define MESH   80			// mesh0: the radio (docs/mesh_app.md, "mesh0")

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// -- the mailbox: what fed will read --

#define QN 512
static struct { z_msg_t m; z_blob_t blob; uint8_t data[4096]; } q[QN];
static int qh, qt;

static void deliver(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
	int i = qt;
	qt = (qt + 1) % QN;
	if (qt == qh) { printf("FAIL: the test's mailbox overflowed\n"); exit(1); }
	memset(&q[i].m, 0, sizeof(q[i].m));
	q[i].m.from = from; q[i].m.subject = subject; q[i].m.tag = tag; q[i].m.obj = obj;
}

static void deliver_blob(uint32_t from, uint32_t subject, uint32_t tag, const void *d, uint32_t n) {
	int i = qt;
	memcpy(q[i].data, d, n);
	q[i].blob.len = n; q[i].blob.data = q[i].data;
	z_obj_t o; o.type = Z_BLOB; o.val.ptr = &q[i].blob;
	deliver(from, subject, tag, o);
}

// -- net: sockets --

typedef struct { int fd; bool open, pending_accept; uint32_t id, relay; int unacked; } nsock_t;
static nsock_t ns[16];
static int nns;
static uint32_t next_out_id = 7, next_relay = 100;
static int inbound_accepted, outbound_made;
static int lfd = -1;				// "fed's" listening port, really ours

static nsock_t *by_id(uint32_t id) {
	for (int i = 0; i < nns; i++) if (ns[i].open && !ns[i].pending_accept && ns[i].id == id) return &ns[i];
	return NULL;
}

// -- the radio: what fed put on the air through mesh0 --

static uint32_t mesh_listen;
static uint8_t air[400][256];
static uint32_t airlen[400];
static int nair;
static uint8_t air_channel;

// -- the local client --

static uint32_t client_id;
static bool client_up;
static char client_in[65536];
static uint32_t client_len;

// -- the kernel --

static z_obj_t k_ok, k_fail;
static uint8_t kv_val[64];
static uint32_t kv_len;
static bool kv_has;

static uint32_t now_ticks(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * Z_TICK_HZ + (uint64_t)t.tv_nsec * Z_TICK_HZ / 1000000000u);
}

static void on_send(const z_msg_t *m) {
	if (m->to == NET) {
		switch (m->subject) {
		case Z_NET_LISTEN:
			deliver(NET, Z_NET_LISTEN_REPLY, m->tag, z_obj_uint32(0));
			return;
		case Z_NET_DNS_RESOLVE: {
			z_obj_t r = z_obj_map(2);
			z_map_set(&r, "ok", z_obj_uint32(1));
			z_map_set(&r, "ip", z_obj_uint32(0x7F000001u));
			deliver(NET, Z_NET_DNS_RESOLVE_REPLY, m->tag, r);
			return;
		}
		case Z_PORT_CONNECT: {				// fed's outbound: a real TCP connection
			z_obj_t *ip = z_map_find((z_obj_t *)&m->obj, "ip"), *port = z_map_find((z_obj_t *)&m->obj, "port");
			struct sockaddr_in a;
			memset(&a, 0, sizeof(a));
			a.sin_family = AF_INET;
			a.sin_port = htons((uint16_t)(port ? port->val.uint32 : 0));
			a.sin_addr.s_addr = htonl(ip ? ip->val.uint32 : 0);
			int fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
				if (fd >= 0) close(fd);
				deliver(NET, Z_PORT_REFUSED, 0, z_obj_str("net: connection refused"));
				return;
			}
			nsock_t *s = &ns[nns++];
			memset(s, 0, sizeof(*s));
			s->fd = fd; s->open = true; s->id = next_out_id++;
			outbound_made++;
			deliver(NET, Z_PORT_CONNECTED, 0, z_obj_uint32(s->id));
			return;
		}
		case Z_PORT_CONNECTED: case Z_PORT_REFUSED:		// fed's answer to an inbound CONNECT
			for (int i = 0; i < nns; i++) if (ns[i].open && ns[i].pending_accept && ns[i].relay == m->tag) {
				ns[i].pending_accept = false;
				if (m->subject == Z_PORT_CONNECTED) { ns[i].id = m->obj.val.uint32; inbound_accepted++; }
				else { close(ns[i].fd); ns[i].open = false; }
			}
			return;
		case Z_PORT_DATA: {
			nsock_t *s = by_id(m->tag);
			if (s && m->obj.type == Z_BLOB) {
				z_blob_t *b = m->obj.val.ptr;
				const uint8_t *p = b->data;
				uint32_t n = b->len;
				while (n) { ssize_t w = write(s->fd, p, n); if (w <= 0) break; p += w; n -= (uint32_t)w; }
			}
			deliver(NET, Z_PORT_DATA_ACK, m->tag, z_obj_none());
			return;
		}
		case Z_PORT_DATA_ACK: {
			nsock_t *s = by_id(m->tag);
			if (s && s->unacked) s->unacked--;
			return;
		}
		case Z_PORT_CLOSE: {
			nsock_t *s = by_id(m->tag);
			if (s) { close(s->fd); s->open = false; }
			return;
		}
		}
		return;
	}
	if (m->to == MESH) {
		switch (m->subject) {
		case Z_PORT_CONNECT:
			deliver(MESH, Z_PORT_CONNECTED, 0, z_obj_uint32(9));
			return;
		case Z_PORT_DATA:
			if (m->obj.type == Z_BLOB) {
				z_blob_t *b = m->obj.val.ptr;
				const uint8_t *d = b->data;
				if (d[0] == 'L' && b->len == 3) mesh_listen = (uint32_t)d[1] | ((uint32_t)d[2] << 8);
				if (d[0] == 'S' && b->len > 8 && nair < 400) {
					memcpy(air[nair], d + 8, b->len - 8);
					airlen[nair++] = b->len - 8;
					air_channel = d[5];
				}
			}
			deliver(MESH, Z_PORT_DATA_ACK, m->tag, z_obj_none());
			return;
		}
		return;
	}
	if (m->to == CLIENT) {
		switch (m->subject) {
		case Z_PORT_CONNECTED:
			client_id = m->obj.val.uint32;
			client_up = true;
			return;
		case Z_PORT_DATA:
			if (m->obj.type == Z_BLOB) {
				z_blob_t *b = m->obj.val.ptr;
				if (client_len + b->len < sizeof(client_in)) { memcpy(client_in + client_len, b->data, b->len); client_len += b->len; }
			}
			deliver(CLIENT, Z_PORT_DATA_ACK, m->tag, z_obj_none());
			return;
		}
	}
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = now_ticks();
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND:
		on_send((z_msg_t *)args);
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_READ:
		if (qh == qt) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = q[qh].m;
		qh = (qh + 1) % QN;
		return (uint32_t *)&k_ok;
	case Z_SYS_PROC_STATUS: {
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		if (o->type == Z_STR && !strcmp(o->val.str, "net0")) { o->type = Z_UINT32; o->val.uint32 = NET; return (uint32_t *)&k_ok; }
		if (o->type == Z_STR && !strcmp(o->val.str, "mesh0")) { o->type = Z_UINT32; o->val.uint32 = MESH; return (uint32_t *)&k_ok; }
		return (uint32_t *)&k_fail;
	}
	case Z_SYS_PID_REGISTER: {
		z_obj_t *o = (z_obj_t *)args;
		o->type = Z_STR; o->val.str = "fed0";
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_KV: {
		z_kv_args_t *a = (z_kv_args_t *)args;
		if (a->op == Z_KV_GET) {
			if (!kv_has) { a->result = Z_KV_E_NOENT; break; }
			memcpy(a->val, kv_val, kv_len); a->len = kv_len; a->result = Z_KV_OK;
		} else if (a->op == Z_KV_SET) {
			memcpy(kv_val, a->val, a->len); kv_len = a->len; kv_has = true; a->result = Z_KV_OK;
		}
		return (uint32_t *)&k_ok;
	}
	default:
		break;
	}
	return (uint32_t *)&k_ok;
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// -- net's other half: TCP in, bytes both ways --

static void pump(void) {
	struct pollfd pf;
	if (lfd >= 0) {
		pf.fd = lfd; pf.events = POLLIN; pf.revents = 0;
		if (poll(&pf, 1, 0) > 0) {
			int fd = accept(lfd, NULL, NULL);
			if (fd >= 0 && nns < 16) {
				nsock_t *s = &ns[nns++];
				memset(s, 0, sizeof(*s));
				s->fd = fd; s->open = true; s->pending_accept = true; s->relay = next_relay++;
				z_net_accept_t info = { 0x7F000001u, 40000, 9070, Z_NET_ACCEPT_LOCAL };
				deliver_blob(NET, Z_PORT_CONNECT, s->relay, &info, sizeof(info));
			}
		}
	}
	// As net's relay does (docs/netserve.md): 512 bytes from TCP toward
	// the process at a time, and nothing more until they are acked.
	for (int i = 0; i < nns; i++) {
		nsock_t *s = &ns[i];
		if (!s->open || s->pending_accept || s->unacked >= 1) continue;
		pf.fd = s->fd; pf.events = POLLIN; pf.revents = 0;
		if (poll(&pf, 1, 0) <= 0) continue;
		uint8_t b[512];
		ssize_t r = read(s->fd, b, sizeof(b));
		if (r > 0) { deliver_blob(NET, Z_PORT_DATA, s->id, b, (uint32_t)r); s->unacked++; }
		else { deliver(NET, Z_PORT_CLOSE, s->id, z_obj_none()); close(s->fd); s->open = false; }
	}
}

static void client_send(const void *d, uint32_t n) {
	deliver_blob(CLIENT, Z_PORT_DATA, client_id, d, n);
}

static int run(double secs, bool (*done)(void)) {
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		step();
		pump();
		if (done && done()) return 1;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9 > secs) return 0;
		usleep(1000);
	}
}

// -- the Linux side --

static char dir_b[128], dir_z[128];
static pid_t bpid;

static bool b_client_has(const char *what, int timeout_ms, char *got, int cap) {
	static int fd = -1;
	static char buf[65536];
	static int len;
	if (fd < 0) {
		struct sockaddr_un ua;
		memset(&ua, 0, sizeof(ua));
		ua.sun_family = AF_UNIX;
		snprintf(ua.sun_path, sizeof(ua.sun_path), "%s/fed.sock", dir_b);
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (connect(fd, (struct sockaddr *)&ua, sizeof(ua))) { close(fd); fd = -1; return false; }
		const char *sub = "SUB bc t/*\n";
		if (write(fd, sub, strlen(sub)) < 0) return false;
	}
	if (!what) {							// a PUB instead: `got` is the request
		return write(fd, got, (size_t)cap) == cap;
	}
	struct pollfd pf = { fd, POLLIN, 0 };
	if (poll(&pf, 1, timeout_ms) > 0) {
		ssize_t r = read(fd, buf + len, sizeof(buf) - 1 - (size_t)len);
		if (r > 0) {
			len += (int)r;
			buf[len] = 0;
			// acknowledge whatever objects came, so the next one follows
			for (char *p = buf; (p = strstr(p, "OBJ ")); p += 4) {
				char ack[40];
				unsigned pos = (unsigned)strtoul(p + 4, NULL, 10);
				int n = snprintf(ack, sizeof(ack), "ACK %u\n", pos);
				if (write(fd, ack, (size_t)n) < 0) break;
				p[0] = 'o';						// counted: not again
			}
		}
	}
	return memmem(buf, (size_t)len, what, strlen(what)) != NULL;
}

static bool zeitlos_got_linux(void) { return memmem(client_in, client_len, "hello from linux", 16) != NULL; }

static bool linux_got_zeitlos(void) { return b_client_has("hello from zeitlos", 0, NULL, 0); }

static void client_ack_all(void) {
	static uint32_t acked_upto;
	for (uint32_t i = acked_upto; i + 4 < client_len; i++) {
		if (!memcmp(client_in + i, "OBJ ", 4)) {
			char ack[40];
			unsigned pos = (unsigned)strtoul(client_in + i + 4, NULL, 10);
			int n = snprintf(ack, sizeof(ack), "ACK %u\n", pos);
			client_send(ack, (uint32_t)n);
			acked_upto = i + 4;
		}
	}
}
static bool both(void) { client_ack_all(); return zeitlos_got_linux() && linux_got_zeitlos(); }

// A member heard only by radio, and a stranger.
static uint8_t rsk[2][64], rpk[2][32];
static const char *radio_pk_hex(void) {
	static char h[65];
	for (int i = 0; i < 2; i++) { uint8_t seed[32]; memset(seed, 0x60 + i, 32); crypto_ed25519_key_pair(rsk[i], rpk[i], seed); }
	fobj_hex(rpk[0], 32, h);
	return h;
}

// An object from `who`, into obj: its length.
static int radio_obj(int who, const char *text, uint8_t *obj, uint8_t id[32]) {
	fobj_t o;
	memset(&o, 0, sizeof(o));
	snprintf(o.topic, sizeof(o.topic), "t/forum/general");
	snprintf(o.type, sizeof(o.type), "bbs.post");
	o.format = FOBJ_TEXT;
	o.kind = FOBJ_LOG;
	o.time = (uint64_t)time(NULL);
	o.seq = 1;
	static char payload[600];
	int n = snprintf(payload, sizeof(payload), "%s", text);
	while (n < 500) payload[n++] = '.';
	o.len = (uint32_t)n;
	int r = fobj_make(&o, (const uint8_t *)payload, rsk[who], obj, FOBJ_MAX);
	fobj_t q;
	fobj_parse(obj, (uint32_t)r, 0, &q, NULL);
	memcpy(id, q.id, 32);
	return r;
}

// A packet heard by mesh: an 'R' record into fed.
static void hear(const uint8_t *p, uint32_t n) {
	uint8_t r[16 + 256];
	r[0] = 'R';
	memset(r + 1, 0x11, 4);				// from some node
	memset(r + 5, 0xFF, 4);				// to all
	r[9] = 1;							// channel 1
	r[10] = 300 & 0xFF; r[11] = 300 >> 8;
	memcpy(r + 12, p, n);
	deliver_blob(MESH, Z_PORT_DATA, 9, r, 12 + n);
}

// An object as fragments, heard.
static void hear_object(const uint8_t *obj, int n, const uint8_t id[32]) {
	uint32_t count = ((uint32_t)n + FRADIO_FRAG_DATA - 1) / FRADIO_FRAG_DATA;
	for (uint32_t i = 0; i < count; i++) {
		uint8_t p[FRADIO_PKT];
		uint32_t off = i * FRADIO_FRAG_DATA, len = (uint32_t)n - off;
		if (len > FRADIO_FRAG_DATA) len = FRADIO_FRAG_DATA;
		p[0] = 'F'; memcpy(p + 1, id, FRADIO_ID);
		p[1 + FRADIO_ID] = (uint8_t)n; p[2 + FRADIO_ID] = (uint8_t)(n >> 8);
		p[3 + FRADIO_ID] = (uint8_t)i; p[4 + FRADIO_ID] = (uint8_t)count;
		memcpy(p + 5 + FRADIO_ID, obj + off, len);
		hear(p, 5 + FRADIO_ID + len);
	}
}

int main(int argc, char **argv) {
	const char *fed_linux = argc > 1 ? argv[1] : "./fed-linux";
	char cmd[600], kb[80], kz[65];

	if (!k_install()) { printf("zfed test: skipped (cannot map page 0)\n"); return 77; }
	plat_quiet = getenv("FED_TEST_LOG") ? 0 : 1;
	signal(SIGPIPE, SIG_IGN);
	snprintf(dir_z, sizeof(dir_z), "/tmp/zfed-z-%d", (int)getpid());
	snprintf(dir_b, sizeof(dir_b), "/tmp/zfed-b-%d", (int)getpid());
	snprintf(cmd, sizeof(cmd), "rm -rf %s %s && mkdir -p %s %s", dir_z, dir_b, dir_z, dir_b);
	if (system(cmd)) return 1;

	// "fed's" listening port: really ours, relayed as net would
	lfd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(0x7F000001u); a.sin_port = 0;
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 4)) { perror("listen"); return 1; }
	socklen_t al = sizeof(a);
	getsockname(lfd, (struct sockaddr *)&a, &al);
	int zport = ntohs(a.sin_port);
	// the Linux daemon's port
	int bport;
	{ int t = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in b2 = a; b2.sin_port = 0;
	  bind(t, (struct sockaddr *)&b2, sizeof(b2)); al = sizeof(b2); getsockname(t, (struct sockaddr *)&b2, &al);
	  bport = ntohs(b2.sin_port); close(t); }

	// the Linux node's key
	snprintf(cmd, sizeof(cmd), "%s --dir %s --print-key", fed_linux, dir_b);
	FILE *pk = popen(cmd, "r");
	if (!pk || !fgets(kb, sizeof(kb), pk)) { printf("FAIL: %s did not print a key\n", fed_linux); return 1; }
	pclose(pk);
	kb[strcspn(kb, "\n")] = 0;

	// -- fed on Zeitlos starts: its key made and stored --
	{
		char p[200];
		snprintf(p, sizeof(p), "%s/fed.cfg", dir_z);
		FILE *f = fopen(p, "w");
		fprintf(f, "name: z\nlisten: 9070\npoll_seconds: 1\npeer: %s localhost:%d\nsubscribe: t/*\n"
			"radio: port=300 channel=1 max=2048 pace=1\nmember: %s\n", kb, bport, radio_pk_hex());
		fclose(f);
	}
	CK(fed_start(dir_z), "fed starts on Zeitlos");
	CK(kv_has && kv_len == 32, "its node key made and kept in the key/value store (apps.fed.nodekey)");
	fobj_hex(fnode_public_key(), 32, kz);

	// -- the Linux node, which also dials the Zeitlos one --
	{
		char p[200];
		snprintf(p, sizeof(p), "%s/fed.cfg", dir_b);
		FILE *f = fopen(p, "w");
		fprintf(f, "name: b\nlisten: %d\npoll_seconds: 1\npeer: %s 127.0.0.1:%d\nsubscribe: t/*\n", bport, kz, zport);
		fclose(f);
	}
	bpid = fork();
	if (bpid == 0) {
		char logp[200];
		snprintf(logp, sizeof(logp), "%s/log.txt", dir_b);
		int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(lf, 2);
		execl(fed_linux, "fed-linux", "--dir", dir_b, "--bind", "127.0.0.1", (char *)NULL);
		_exit(127);
	}
	{ char p[200]; snprintf(p, sizeof(p), "%s/fed.sock", dir_b);
	  for (int i = 0; i < 200 && access(p, F_OK); i++) usleep(20000); }

	// -- a local client on fed0 --
	deliver(CLIENT, Z_PORT_CONNECT, 0, z_obj_none());
	run(1, NULL);
	CK(client_up, "a local client connects to fed0");
	const char *sub = "SUB zc t/*\n";
	client_send(sub, (uint32_t)strlen(sub));
	const char *msg = "hello from zeitlos";
	char pub[200];
	int pn = snprintf(pub, sizeof(pub), "PUB t/forum bbs.post text log - %d\n%s", (int)strlen(msg), msg);
	client_send(pub, (uint32_t)pn);
	run(1, NULL);
	CK(memmem(client_in, client_len, "OK ", 3) != NULL, "it publishes");

	// the Linux side publishes, and subscribes
	b_client_has("", 0, NULL, 0);
	{
		const char *lm = "hello from linux";
		char req[200];
		int n = snprintf(req, sizeof(req), "PUB t/forum bbs.post text log - %d\n%s", (int)strlen(lm), lm);
		b_client_has(NULL, 0, req, n);
	}

	int ok = run(30, both);
	CK(ok && zeitlos_got_linux(), "the Linux node's post reaches the Zeitlos subscriber");
	CK(ok && linux_got_zeitlos(), "the Zeitlos node's post reaches the Linux subscriber");
	{
		// each is the other's, and checks: verified by the Linux side's core (fsess) and by ours
		char *p = memmem(client_in, client_len, "hello from linux", 16);
		CK(p && memmem(client_in, client_len, kb, 64) != NULL, "and it came from the Linux node's key");
	}
	run(3, NULL);
	CK(outbound_made >= 1 && inbound_accepted >= 1, "sessions both ways: Zeitlos dialled out (%d), and was dialled (%d)",
		outbound_made, inbound_accepted);
	{
		char p[200], line[400];
		snprintf(p, sizeof(p), "%s/log.txt", dir_b);
		FILE *f = fopen(p, "r");
		int to_done = 0, from_done = 0;
		while (f && fgets(line, sizeof(line), f)) {
			if (strstr(line, "session to") && strstr(line, "done")) to_done++;
			if (strstr(line, "session from") && strstr(line, "done")) from_done++;
		}
		if (f) fclose(f);
		CK(to_done >= 1 && from_done >= 1, "the Linux node's log: sessions to and from it done (%d, %d)", to_done, from_done);
	}

	// -- a stranger at the Zeitlos node's port: not one byte back --
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in z = a;
		z.sin_port = htons((uint16_t)zport);
		connect(fd, (struct sockaddr *)&z, sizeof(z));
		uint8_t junk[69];
		memcpy(junk, "ZFED1", 5);
		for (int i = 5; i < 69; i++) junk[i] = (uint8_t)(i * 37);
		if (write(fd, junk, sizeof(junk)) < 0) { }
		int got = -1, total = 0;
		for (int i = 0; i < 2000 && got != 0; i++) {
			step(); pump();
			struct pollfd pf = { fd, POLLIN, 0 };
			if (poll(&pf, 1, 1) > 0) { uint8_t b[100]; got = (int)read(fd, b, sizeof(b)); if (got > 0) total += got; }
		}
		CK(got == 0 && total == 0, "a stranger: the connection closed, not one byte back (%d)", total);
		close(fd);
	}

	// -- the radio: fed as mesh0's client (docs/fed.md, "The radio link") --
	{
		run(1, NULL);
		CK(mesh_listen == 300, "fed connects to mesh0 and listens on its port, 300 (%u)", (unsigned)mesh_listen);
		uint8_t up[5] = { 'U', 1, 2, 3, 4 };
		deliver_blob(MESH, Z_PORT_DATA, 9, up, 5);
		run(1, NULL);
		// a post published here: announced on the air
		int mark = nair;
		uint32_t cl0 = client_len;
		const char *rp = "a post, to be heard by radio";
		char req[120];
		int rn = snprintf(req, sizeof(req), "PUB t/forum/general bbs.post text log - %d\n%s", (int)strlen(rp), rp);
		client_send(req, (uint32_t)rn);
		run(2, NULL);
		char *ok = memmem(client_in + cl0, client_len - cl0, "OK ", 3);
		uint8_t pid[32] = { 0 };
		for (int i = 0; ok && i < 32; i++) { unsigned v; sscanf(ok + 3 + 2 * i, "%2x", &v); pid[i] = (uint8_t)v; }
		bool announced = false;
		for (int i = mark; i < nair; i++)
			if (air[i][0] == 'I') for (int k = 0; k < air[i][1]; k++) if (!memcmp(air[i] + 2 + k * FRADIO_ID, pid, FRADIO_ID)) announced = true;
		CK(ok && announced && air_channel == 1, "a post here: announced on the air, on channel 1");
		// wanted: its fragments, put back together, are the object exactly
		mark = nair;
		uint8_t w[1 + FRADIO_ID];
		w[0] = 'W'; memcpy(w + 1, pid, FRADIO_ID);
		hear(w, sizeof(w));
		run(2, NULL);
		static uint8_t back[FOBJ_MAX];
		uint32_t total = 0, pieces = 0, count = 0;
		for (int i = mark; i < nair; i++) {
			if (air[i][0] != 'F' || memcmp(air[i] + 1, pid, FRADIO_ID)) continue;
			total = (uint32_t)air[i][1 + FRADIO_ID] | ((uint32_t)air[i][2 + FRADIO_ID] << 8);
			count = air[i][4 + FRADIO_ID];
			memcpy(back + air[i][3 + FRADIO_ID] * FRADIO_FRAG_DATA, air[i] + 5 + FRADIO_ID, airlen[i] - 5 - FRADIO_ID);
			pieces++;
		}
		fobj_t bo;
		CK(pieces == count && count > 0 && !fobj_parse(back, total, 0, &bo, NULL) && !memcmp(bo.id, pid, 32),
			"wanted: its fragments on the air, put back together, are the object exactly (%u of %u)", (unsigned)pieces, (unsigned)count);
		// heard: a member's object comes in; a stranger's does not
		static uint8_t obj[FOBJ_MAX];
		uint8_t id[32];
		int n = radio_obj(0, "heard by radio, from a member", obj, id);
		hear_object(obj, n, id);
		n = radio_obj(1, "heard by radio, from a stranger", obj, id);
		hear_object(obj, n, id);
		for (int i = 0; i < 20; i++) { run(0.1, NULL); client_ack_all(); }
		CK(memmem(client_in, client_len, "heard by radio, from a member", 29) != NULL, "a member's object, heard: stored, and delivered here");
		CK(memmem(client_in, client_len, "heard by radio, from a stranger", 31) == NULL, "a stranger's: refused");
		// the radio down: nothing sent
		uint8_t down[1] = { 'D' };
		deliver_blob(MESH, Z_PORT_DATA, 9, down, 1);
		run(1, NULL);
		mark = nair;
		rn = snprintf(req, sizeof(req), "PUB t/forum/general bbs.post text log - 5\nquiet");
		client_send(req, (uint32_t)rn);
		run(2, NULL);
		CK(nair == mark, "the radio down: nothing put on the air (%d)", nair - mark);
	}

	kill(bpid, SIGTERM);
	waitpid(bpid, NULL, 0);
	fnode_stop();
	printf("zfed: %d checks, %d failed\n", checks, fails);
	if (!fails) { snprintf(cmd, sizeof(cmd), "rm -rf %s %s", dir_z, dir_b); if (system(cmd)) return 1; }
	else printf("logs: %s/log.txt\n", dir_b);
	return fails != 0;
}
