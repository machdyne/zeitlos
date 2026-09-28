/*
 * Host test for bbs.c, the Zeitlos port provider: the REAL bbs.c and
 * core, this file playing the kernel (a mailbox, a clock, the process
 * table) and the clients (term locally, netserve for the network).
 * Files go through the Linux platform (linux/plat.c) into /tmp.
 *
 *   make -C sw/apps/bbs test      (needs vm.mmap_min_addr=0; skips otherwise)
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define main bbs_main_unused
#include "../bbs.c"
#undef main
#include "../core/bbs_int.h"

// bbs.c's main() (not run here) is the only caller of these.
bool z_rng_secure(void) { return true; }
void z_rng_bytes(void *b, uint32_t n) { memset(b, 7, n); }

extern int plat_quiet;

#define TERM     30
#define NETSERVE 40
#define STRANGER 99

// -- what bbs sent --
typedef struct { uint32_t to, subject, tag, type, u32; uint8_t data[1100]; uint32_t len; char str[80]; } out_t;
static out_t out[4000];
static int nout;

// -- what it will read --
static z_msg_t q[64];
static int qn;
static z_blob_t qblob[64];
static uint8_t qdata[64][600];

static uint32_t ticks = 1000, dead_pid;
static z_obj_t k_ok, k_fail;

static void deliver(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
	memset(&q[qn], 0, sizeof(q[qn]));
	q[qn].from = from; q[qn].subject = subject; q[qn].tag = tag; q[qn].obj = obj;
	qn++;
}

static void deliver_data(uint32_t from, uint32_t tag, const char *s) {
	uint32_t n = (uint32_t)strlen(s);
	memcpy(qdata[qn], s, n);
	qblob[qn].len = n; qblob[qn].data = qdata[qn];
	z_obj_t o; o.type = Z_BLOB; o.val.ptr = &qblob[qn];
	deliver(from, Z_PORT_DATA, tag, o);
}

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND: {
		z_msg_t *m = (z_msg_t *)args;
		out_t *o = &out[nout++];
		memset(o, 0, sizeof(*o));
		o->to = m->to; o->subject = m->subject; o->tag = m->tag; o->type = m->obj.type;
		if (m->obj.type == Z_UINT32) o->u32 = m->obj.val.uint32;
		if (m->obj.type == Z_BLOB) {
			z_blob_t *bl = m->obj.val.ptr;
			o->len = bl->len < sizeof(o->data) ? bl->len : sizeof(o->data);
			memcpy(o->data, bl->data, o->len);
		}
		if (m->obj.type == Z_STR && m->obj.val.str) snprintf(o->str, sizeof(o->str), "%s", m->obj.val.str);
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_MSG_READ:
		if (!qn) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = q[0];
		memmove(q, q + 1, sizeof(z_msg_t) * (size_t)(--qn));
		return (uint32_t *)&k_ok;
	case Z_SYS_PROC_STATUS: {
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = a->pid == dead_pid ? Z_PROC_STATE_EXITED : Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	default:
		return (uint32_t *)&k_ok;
	}
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

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void steps(int n) { while (n--) step(); }

static int find(uint32_t to, uint32_t subject, int from) {
	for (int i = from; i < nout; i++) if (out[i].to == to && out[i].subject == subject) return i;
	return -1;
}

// Everything bbs sent `to` on `tag` since `from`, concatenated; acked as
// term or netserve would ack it (unless `ack` is false).
static uint32_t collect(uint32_t to, uint32_t tag, int from, char *buf, uint32_t cap, bool ack) {
	uint32_t n = 0;
	for (int i = from; i < nout; i++)
		if (out[i].to == to && out[i].subject == Z_PORT_DATA && out[i].tag == tag) {
			if (n + out[i].len < cap) { memcpy(buf + n, out[i].data, out[i].len); n += out[i].len; }
			if (ack) deliver(to, Z_PORT_DATA_ACK, tag, z_obj_none());
		}
	buf[n] = 0;
	return n;
}

int main(void) {
	static char buf[65536];
	char dir[64];
	int mark;

	if (!k_install()) { printf("bbs zport test: skipped (cannot map page 0)\n"); return 77; }
	plat_quiet = getenv("BBS_TEST_LOG") ? 0 : 1;
	snprintf(dir, sizeof(dir), "/tmp/bbs-zport-%d", (int)getpid());
	mkdir(dir, 0755);
	{ char p[128]; snprintf(p, sizeof(p), "%s/bbs.cfg", dir);
	  FILE *f = fopen(p, "w"); fputs("name: Port BBS\nnodes: 2\n", f); fclose(f); }
	CK(bbs_init(dir), "the core starts");

	// -- 1. a local caller: term, `port bbs0` --
	mark = nout;
	deliver(TERM, Z_PORT_CONNECT, 0, z_obj_none());
	steps(2);
	int c = find(TERM, Z_PORT_CONNECTED, mark);
	CK(c >= 0 && out[c].tag == 0, "term's CONNECT accepted");
	uint32_t tc = c >= 0 ? out[c].u32 : 1;
	CK(bbs_node[0]->state == N_DETECT && !strcmp(bbs_node[0]->transport, "local"), "a local caller, detecting");
	collect(TERM, tc, mark, buf, sizeof(buf), true);
	CK(strstr(buf, "\x1b[6n") != NULL, "the probe goes to term");
	mark = nout;
	deliver_data(TERM, tc, "\x1b[1;2R");
	steps(2);
	deliver_data(TERM, tc, "\x1b[25;80R");
	steps(3);
	uint32_t got = collect(TERM, tc, mark, buf, sizeof(buf), true);
	CK(strstr(buf, "Welcome to") && strstr(buf, "Port BBS") && strstr(buf, "Handle"), "the welcome screen reaches term");
	CK(got == strlen(buf), "no NUL byte in what was sent (%u bytes, %zu before a NUL)", (unsigned)got, strlen(buf));
	CK(bbs_node[0]->charset == CS_UTF8 && bbs_node[0]->rows == 25, "UTF-8, 25 rows: term's answers");

	// -- 2. a network caller through netserve: who, from the identity map --
	{
		static z_obj_table_t t;
		static z_obj_t k[5], v[5];
		const char *keys[5] = { "transport", "auth", "peer", "port", "user" };
		for (int i = 0; i < 5; i++) { k[i].type = Z_STR; k[i].val.str = (char *)keys[i]; }
		v[0].type = Z_STR; v[0].val.str = "ssh";
		v[1].type = Z_STR; v[1].val.str = "none";
		v[2] = z_obj_uint32(0xC0000207);
		v[3] = z_obj_uint32(22);
		v[4].type = Z_STR; v[4].val.str = "phil";
		t.len = 5; t.a = k; t.b = v;
		z_obj_t map; map.type = Z_MAP; map.val.ptr = &t;
		mark = nout;
		deliver(NETSERVE, Z_PORT_CONNECT, 0, map);
		steps(2);
	}
	c = find(NETSERVE, Z_PORT_CONNECTED, mark);
	CK(c >= 0, "netserve's CONNECT accepted -- auth none is fine here: the BBS logs callers in");
	uint32_t nc = c >= 0 ? out[c].u32 : 2;
	CK(!strcmp(bbs_node[1]->transport, "ssh") && !strcmp(bbs_node[1]->peer, "192.0.2.7") &&
		!strcmp(bbs_node[1]->offered, "phil"), "transport, address and offered user from the map (%s %s %s)",
		bbs_node[1]->transport, bbs_node[1]->peer, bbs_node[1]->offered);
	deliver_data(NETSERVE, nc, "\x1b[1;4R");
	steps(2);
	deliver_data(NETSERVE, nc, "\x1b[24;80R");
	steps(3);
	collect(NETSERVE, nc, mark, buf, sizeof(buf), true);
	CK(strstr(buf, "phil") && strstr(buf, "ssh"), "its welcome, with the offered name at the prompt");

	// -- 3. every line busy: CONNECTED, the reason, CLOSE --
	mark = nout;
	deliver(55, Z_PORT_CONNECT, 0, z_obj_none());
	steps(2);
	c = find(55, Z_PORT_CONNECTED, mark);
	collect(55, c >= 0 ? out[c].u32 : 3, mark, buf, sizeof(buf), true);
	steps(2);
	CK(c >= 0 && strstr(buf, "busy") && find(55, Z_PORT_CLOSE, mark) >= 0,
		"a third caller hears every line is busy, then the connection closes");
	steps(2);

	// -- 4. a stranger's DATA: acked (so it can free it) and CLOSEd --
	mark = nout;
	deliver_data(STRANGER, tc, "hello");
	deliver_data(TERM, 77, "wrong tag");
	steps(1);
	CK(find(STRANGER, Z_PORT_DATA_ACK, mark) >= 0 && find(STRANGER, Z_PORT_CLOSE, mark) >= 0,
		"a stranger with a matching tag is rejected: matched by sender AND tag");
	CK(find(TERM, Z_PORT_CLOSE, mark) >= 0 && bbs_node[0]->state != N_FREE,
		"a known sender with a tag it does not have is rejected too, and its real connection stays");

	// -- 5. backpressure: at most eight sends unacked, the rest waits --
	mark = nout;
	// 100 lines: more than eight sends' worth (8 x 512)
	for (int i = 0; i < 100; i++) out_text(bbs_node[0], "0123456789012345678901234567890123456789012345678901234567890123456789\n");
	steps(3);
	int sent = 0;
	for (int i = mark; i < nout; i++) if (out[i].to == TERM && out[i].subject == Z_PORT_DATA) sent++;
	CK(sent == Z_PORT_MAX_PENDING_SENDS, "eight sends in flight, not more (%d)", sent);
	uint32_t total = 0;
	for (int round = 0; round < 20; round++) {
		// term acks each send once, as it reads it
		for (int i = mark; i < nout; i++)
			if (out[i].to == TERM && out[i].subject == Z_PORT_DATA && out[i].tag == tc && out[i].str[0] == 0) {
				deliver(TERM, Z_PORT_DATA_ACK, tc, z_obj_none());
				out[i].str[0] = 'A';
			}
		steps(2);
	}
	for (int i = mark; i < nout; i++) if (out[i].to == TERM && out[i].subject == Z_PORT_DATA) total += out[i].len;
	CK(total >= 100 * 72 && bbs_node[0]->out_len == 0, "as acks come back, all of it goes (%u bytes)", (unsigned)total);

	// -- 6. the caller's process dies: nobody says, the check finds it --
	dead_pid = TERM;
	ticks += 2 * Z_TICK_HZ;
	mark = nout;
	steps(2);
	CK(bbs_node[0]->state == N_FREE && find(TERM, Z_PORT_CLOSE, mark) < 0,
		"a dead peer: its node freed within a second, nothing sent to its pid");
	dead_pid = 0;

	// -- 7. the BBS ends a call: output first, then CLOSE --
	mark = nout;
	for (int i = 0; i < 5; i++) { deliver_data(NETSERVE, nc, "nobody\r"); steps(2); collect(NETSERVE, nc, nout - 1 > mark ? nout - 1 : mark, buf, sizeof(buf), true); steps(1); }
	steps(4);
	collect(NETSERVE, nc, mark, buf, sizeof(buf), true);
	steps(4);
	int cl = find(NETSERVE, Z_PORT_CLOSE, mark);
	int last_data = -1;
	for (int i = mark; i < nout; i++) if (out[i].to == NETSERVE && out[i].subject == Z_PORT_DATA) last_data = i;
	CK(strstr(buf, "Too many tries") && cl > last_data, "too many tries: the words, and only then CLOSE");
	CK(bbs_node[1]->state == N_FREE, "the node is free again");

	// -- 8. a caller hangs up: CLOSE from the peer --
	mark = nout;
	deliver(TERM, Z_PORT_CONNECT, 0, z_obj_none());
	steps(2);
	c = find(TERM, Z_PORT_CONNECTED, mark);
	tc = c >= 0 ? out[c].u32 : 1;
	CK(bbs_node[0]->state == N_DETECT, "(a new call)");
	deliver(TERM, Z_PORT_CLOSE, tc, z_obj_none());
	steps(2);
	CK(bbs_node[0]->state == N_FREE, "CLOSE from the caller frees its node");
	collect(TERM, tc, mark, buf, sizeof(buf), true);
	steps(2);
	{
		int used = 0;
		for (int i = 0; i < CONN_MAX; i++) used += conns[i].used;
		CK(used == 0, "no connection left behind once the acks are in (%d)", used);
	}

	printf("bbs zport test: %d checks, %d failed\n", checks, fails);
	char cmd[100];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (!fails) { int r = system(cmd); (void)r; }
	return fails != 0;
}
