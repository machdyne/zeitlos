/*
 * Host test for netserve: the REAL netserve.c, with this file playing
 * the kernel (config, the password, pid names, starting repl), net
 * (accepted connections, DATA, acks) and repl (the backend port).
 *
 *   sudo sysctl -w vm.mmap_min_addr=0      # see sw/common/tests/ztramp.h
 *   make test
 *
 * Exit 0 pass, 1 fail, 77 skipped.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#define main netserve_main_unused
#include "../netserve.c"
#undef main

#define NET  10
#include "../../ssh/ssh_proto.h"
#include "../../../../common/zkv.h"
#include "authkeys_keys.h"
#include "../../../../common/zproc.h"
static uint32_t dead_pid;       /* a backend the test has "killed" */
void fake_fs_authkeys(const char *t);
extern bool fake_rng_secure;

void fake_fs_init(void);
const uint8_t *fake_fs_data(const char *p, uint32_t *len);
extern int fake_fs_open, fake_fs_limit;
#define REPL 20

// -- what netserve sent --

typedef struct {
	uint32_t to, subject, tag, type, u32;
	uint8_t data[1200];
	uint32_t len;
	char str[80];
} out_t;
static out_t out[8000];     // every message netserve sends, for the whole run
static int nout;

// -- what it will read --

static z_msg_t q[64];
static int qn;
static z_blob_t qblob[64];
static uint8_t qdata[64][1200];

static void deliver(uint32_t from, uint32_t subject, uint32_t tag, z_obj_t obj) {
	z_msg_t *m = &q[qn];
	memset(m, 0, sizeof(*m));
	m->from = from;
	m->subject = subject;
	m->tag = tag;
	m->obj = obj;
	qn++;
}

static void deliver_data(uint32_t from, uint32_t subject, uint32_t tag, const void *d, uint32_t n) {
	memcpy(qdata[qn], d, n);
	qblob[qn].len = n;
	qblob[qn].data = qdata[qn];
	z_obj_t o;
	o.type = Z_BLOB;
	o.val.ptr = &qblob[qn];
	deliver(from, subject, tag, o);
}

// -- the kernel --

static z_obj_t k_ok, k_fail;
static uint32_t ticks = 1000;
static bool repl_up;
static int proc_runs, auth_checks;
static int auth_force;
static const char *cfg_telnet = "23 repl0", *cfg_echo = "7", *cfg_allow = NULL, *cfg_http = NULL, *cfg_ssh = NULL, *cfg_ssh_auth = NULL;
static bool has_pw = true, pw_long = true;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32;
		((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_MSG_SEND: {
		z_msg_t *m = (z_msg_t *)args;
		if (nout >= 8000) { printf("FATAL: the test's message log is full\n"); exit(2); }
		out_t *o = &out[nout++];
		memset(o, 0, sizeof(*o));
		o->to = m->to; o->subject = m->subject; o->tag = m->tag; o->type = m->obj.type;
		if (m->obj.type == Z_UINT32) o->u32 = m->obj.val.uint32;
		if (m->obj.type == Z_BLOB) {
			z_blob_t *bl = (z_blob_t *)m->obj.val.ptr;
			o->len = bl->len < sizeof(o->data) ? bl->len : sizeof(o->data);
			memcpy(o->data, bl->data, o->len);
		}
		if (m->obj.type == Z_STR && m->obj.val.str) snprintf(o->str, sizeof(o->str), "%s", m->obj.val.str);
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_MSG_READ:
		if (!qn) return (uint32_t *)&k_fail;
		*(z_msg_t *)args = q[0];
		// the blob header lives in qblob[]; keep it where it is
		memmove(q, q + 1, sizeof(z_msg_t) * (size_t)(--qn));
		return (uint32_t *)&k_ok;
	case Z_SYS_PID_REGISTER: {
		z_obj_t *o = (z_obj_t *)args;
		static char nm[] = "netserve0";
		o->type = Z_STR; o->val.str = nm;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PID_LOOKUP: {
		z_obj_t *o = (z_obj_t *)args;
		if (!strcmp(o->val.str, "net0")) { o->type = Z_UINT32; o->val.uint32 = NET; return (uint32_t *)&k_ok; }
		if (!strcmp(o->val.str, "repl0") && repl_up) { o->type = Z_UINT32; o->val.uint32 = REPL; return (uint32_t *)&k_ok; }
		if (!strcmp(o->val.str, "vi0")) { o->type = Z_UINT32; o->val.uint32 = 22; return (uint32_t *)&k_ok; }
		return (uint32_t *)&k_fail;
	}
	case Z_SYS_PROC_RUN: {
		z_obj_t *o = (z_obj_t *)args;
		proc_runs++;
		o->val.uint32 = 33;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_CFG_GET: {
		z_cfg_get_args_t *a = (z_cfg_get_args_t *)args;
		const char *v = NULL;
		if (a->key && !strcmp(a->key, "apps.netserve.telnet")) v = cfg_telnet;
		if (a->key && !strcmp(a->key, "apps.netserve.echo")) v = cfg_echo;
		if (a->key && !strcmp(a->key, "apps.netserve.allow")) v = cfg_allow;
		if (a->key && !strcmp(a->key, "apps.netserve.http")) v = cfg_http;
		if (a->key && !strcmp(a->key, "apps.netserve.ssh")) v = cfg_ssh;
		if (a->key && !strcmp(a->key, "apps.netserve.ssh_auth")) v = cfg_ssh_auth;
		a->found = v != NULL;
		if (v) snprintf(a->val, a->vallen, "%s", v);
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PROC_STATUS: {
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = (a->pid == dead_pid) ? Z_PROC_STATE_EXITED : Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_KV: {
		static uint8_t hk[32];
		static bool have_hk;
		static int sets;
		z_kv_args_t *a = (z_kv_args_t *)args;
		(void)sets;
		if (a->op == Z_KV_GET) {
			if (!have_hk) { a->result = Z_KV_E_NOENT; return (uint32_t *)&k_ok; }
			memcpy(a->val, hk, 32); a->len = 32; a->result = Z_KV_OK;
		} else if (a->op == Z_KV_SET) {
			memcpy(hk, a->val, 32); have_hk = true; sets++; a->result = Z_KV_OK;
		} else a->result = Z_KV_E_INVAL;
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_AUTH: {
		z_auth_args_t *a = (z_auth_args_t *)args;
		a->result = Z_AUTH_OK;
		if (a->op == Z_AUTH_STATUS) {
			memset(a->st, 0, sizeof(*a->st));
			a->st->flags = (has_pw ? Z_AUTH_HAS_PASSWORD : 0) | (pw_long ? Z_AUTH_NET_OK : 0);
		} else if (a->op == Z_AUTH_CHECK) {
			auth_checks++;
			if (auth_force) { a->result = auth_force; a->wait_ms = 4000; }
			else if (a->pwlen != 13 || memcmp(a->pw, "correct horse", 13)) a->result = Z_AUTH_E_BAD;
		}
		return (uint32_t *)&k_ok;
	}
	default:
		return (uint32_t *)&k_ok;
	}
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// -- driving it --

// One pass of netserve's loop, minus the wait.
static void step(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) on_msg(&m);
	for (int i = 0; i < MAX_SESS; i++) if (sess[i].state) poll_session(&sess[i]);
}

static void steps(int n) { while (n--) step(); }

// Acks every DATA netserve sent to `to` since `from_i`, as net or repl
// would, and returns the bytes concatenated.
static uint32_t collect(uint32_t to, uint32_t tag, int from_i, uint8_t *buf, uint32_t cap) {
	uint32_t n = 0;
	for (int i = from_i; i < nout; i++)
		if (out[i].to == to && out[i].subject == Z_PORT_DATA && out[i].tag == tag) {
			if (n + out[i].len <= cap) { memcpy(buf + n, out[i].data, out[i].len); n += out[i].len; }
			deliver(to, Z_PORT_DATA_ACK, tag, z_obj_none());
		}
	return n;
}

static int find(uint32_t to, uint32_t subject, int from_i) {
	for (int i = from_i; i < nout; i++) if (out[i].to == to && out[i].subject == subject) return i;
	return -1;
}

static bool contains(const uint8_t *h, uint32_t hn, const void *n, uint32_t nn) {
	for (uint32_t i = 0; i + nn <= hn; i++) if (!memcmp(h + i, n, nn)) return true;
	return false;
}

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void connect_from(uint32_t relay, uint32_t ip, uint16_t lport, bool local) {
	static z_net_accept_t info[16];
	static z_blob_t bl[16];
	info[relay].ip = ip; info[relay].port = (uint16_t)(40000 + relay);
	info[relay].lport = lport; info[relay].flags = local ? Z_NET_ACCEPT_LOCAL : 0;
	bl[relay].len = sizeof(info[relay]); bl[relay].data = (uint8_t *)&info[relay];
	z_obj_t o; o.type = Z_BLOB; o.val.ptr = &bl[relay];
	deliver(NET, Z_PORT_CONNECT, relay, o);
}

static void type(uint32_t conn, const char *s, uint32_t n) {
	deliver_data(NET, Z_PORT_DATA, conn, s, n);
}

/* the SSH client's callbacks (section 13) */
static uint8_t c_out[65536];
static uint32_t c_out_n;
static bool cw(void *u, const uint8_t *d, uint32_t n) { (void)u; memcpy(c_out + c_out_n, d, n); c_out_n += n; return true; }
static const char *ssh_pw_for_test;
struct cstate { int ready, closed, hostkey; char why[96]; uint8_t got[16384]; uint32_t got_n; };
void cr(void *u, uint8_t *o, uint32_t n) { (void)u; static uint64_t x = 99; while (n--) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; *o++ = (uint8_t)x; } }
static ssh_proto_t *cli_for_ev;
void cev(void *u, ssh_event_t ev, const uint8_t *d, uint32_t n, const char *tx) {
	struct cstate *C = u;
	switch (ev) {
	case SSH_EV_HOSTKEY: C->hostkey++; ssh_proto_accept_host(cli_for_ev); break;
	case SSH_EV_NEED_PASSWORD: if (ssh_pw_for_test) ssh_proto_password(cli_for_ev, ssh_pw_for_test); break;
	case SSH_EV_READY: C->ready++; break;
	case SSH_EV_DATA: if (C->got_n + n <= sizeof(C->got)) { memcpy(C->got + C->got_n, d, n); C->got_n += n; } break;
	case SSH_EV_CLOSED: C->closed++; snprintf(C->why, sizeof(C->why), "%s", tx ? tx : ""); break;
	default: break;
	}
}

int main(void) {
	uint8_t buf[8192];
	uint32_t n;
	int mark;

	if (!k_install()) { printf("netserve test: skipped (cannot map page 0)\n"); return 77; }

	// -- 0. the policy --
	has_pw = false;
	read_config();
	CK(!svc_telnet.on && svc_echo.on, "no password: telnet stays off, echo does not");
	has_pw = true; pw_long = false;
	read_config();
	CK(!svc_telnet.on, "a short password: telnet stays off");
	pw_long = true;
	cfg_telnet = "off";
	read_config();
	CK(!svc_telnet.on, "\"off\" is off");
	cfg_telnet = "2323 posix0";
	read_config();
	CK(svc_telnet.on && svc_telnet.port == 2323 && !strcmp(svc_telnet.target, "posix0"),
		"port and target parsed");
	cfg_telnet = "23 repl0";
	read_config();
	CK(svc_telnet.on && svc_telnet.port == 23 && svc_echo.port == 7 && !allow_any, "defaults");

	// -- 1. listening --
	net_pid = NET;
	listen_on(svc_telnet.port);
	listen_on(svc_echo.port);
	CK(nout == 2 && out[0].subject == Z_NET_LISTEN && out[0].u32 == 23 && out[1].u32 == 7,
		"LISTEN for 23 and 7");
	deliver(NET, Z_NET_LISTEN_REPLY, 23, z_obj_uint32(0));
	deliver(NET, Z_NET_LISTEN_REPLY, 7, z_obj_uint32(0));
	step();
	CK(svc_telnet.listening && svc_echo.listening, "both listening");

	// -- 2. echo --
	mark = nout;
	connect_from(1, 0xC0A80105, 7, true);
	step();
	int c = find(NET, Z_PORT_CONNECTED, mark);
	CK(c >= 0 && out[c].tag == 1 && out[c].u32 == 1, "echo accepted: CONNECTED with net's tag");
	type(1, "ping\r\n", 6);
	step();
	CK(find(NET, Z_PORT_DATA_ACK, mark) >= 0, "the DATA from net is acked");
	n = collect(NET, 1, mark, buf, sizeof(buf));
	CK(n == 6 && !memcmp(buf, "ping\r\n", 6), "echoed verbatim");
	deliver(NET, Z_PORT_CLOSE, 1, z_obj_none());
	steps(2);
	CK(!sess[0].state, "echo session over and freed");

	// -- 3. the subnet rule --
	mark = nout;
	connect_from(2, 0x08080808, 23, false);
	step();
	c = find(NET, Z_PORT_REFUSED, mark);
	CK(c >= 0 && out[c].tag == 2 && strstr(out[c].str, "subnet"), "a peer off this subnet is refused");

	// -- 4. telnet: negotiation, the password, the backend --
	mark = nout;
	connect_from(3, 0xC0A80106, 23, true);
	step();
	c = find(NET, Z_PORT_CONNECTED, mark);
	uint32_t conn = out[c].u32;
	n = collect(NET, conn, mark, buf, sizeof(buf));
	static const uint8_t offer[] = { IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA };
	CK(n > 6 && !memcmp(buf, offer, 6), "offers WILL ECHO, WILL SGA first");
	CK(contains(buf, n, "not encrypted", 13) && contains(buf, n, "password: ", 10),
		"says it is not encrypted, then asks");

	mark = nout;
	static const uint8_t neg[] = { IAC, DO, OPT_ECHO, IAC, DO, OPT_SGA, IAC, WILL, 31, IAC, DO, 24 };
	type(conn, (const char *)neg, sizeof(neg));
	step();
	n = collect(NET, conn, mark, buf, sizeof(buf));
	static const uint8_t want[] = { IAC, DONT, 31, IAC, WONT, 24 };
	CK(n == sizeof(want) && !memcmp(buf, want, n),
		"acks of our offers ignored; NAWS and TTYPE refused (%u bytes)", n);
	mark = nout;
	type(conn, (const char *)neg, sizeof(neg));
	step();
	n = collect(NET, conn, mark, buf, sizeof(buf));
	CK(n == 0, "never answers an option twice (no loops)");

	mark = nout;
	type(conn, "nope\r\n", 6);
	steps(2);
	n = collect(NET, conn, mark, buf, sizeof(buf));
	CK(contains(buf, n, "Login incorrect", 15) && contains(buf, n, "password: ", 10), "wrong password");
	CK(!contains(buf, n, "nope", 4), "the password is not echoed");

	mark = nout;
	type(conn, "correct ", 8);                      // split across two segments,
	type(conn, "horse\r\0ls\r", 10);                // Enter as CR NUL, then typeahead
	steps(2);
	n = collect(NET, conn, mark, buf, sizeof(buf));
	CK(contains(buf, n, "connecting to repl0", 19), "right password: connecting");
	CK(!contains(buf, n, "horse", 5), "still not echoed");
	CK(proc_runs == 1 && sess[conn - 1].state == S_LAUNCH, "repl was not running: started");
	steps(3);
	CK(proc_runs == 1, "and started only once");
	repl_up = true;
	mark = nout;
	step();
	c = find(REPL, Z_PORT_CONNECT, mark);
	CK(c >= 0 && out[c].tag == 0, "once registered: CONNECT to repl0");
	deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(1));
	mark = nout;
	steps(2);
	n = collect(REPL, 1, mark, buf, sizeof(buf));
	CK(n == 3 && !memcmp(buf, "ls\r", 3), "the typeahead reaches repl, Enter as CR");

	mark = nout;
	type(conn, "ps\r\n", 4);                        // CR LF: one Enter
	type(conn, "x\n", 2);                           // a bare LF: Enter too
	type(conn, "a\xff\xff" "b", 4);                 // IAC IAC: a literal 0xFF
	steps(2);
	n = collect(REPL, 1, mark, buf, sizeof(buf));
	CK(n == 8 && !memcmp(buf, "ps\rx\ra\xff" "b", 8), "Enter normalised; IAC IAC unescaped (%u)", n);

	mark = nout;
	deliver_data(REPL, Z_PORT_DATA, 1, "out\xff\r\n", 6);
	steps(2);
	CK(find(REPL, Z_PORT_DATA_ACK, mark) >= 0, "repl's output acked");
	n = collect(NET, conn, mark, buf, sizeof(buf));
	CK(n == 7 && !memcmp(buf, "out\xff\xff\r\n", 7), "0xFF escaped toward the peer");

	// backpressure: more output than to_net holds, net slow to ack
	mark = nout;
	uint8_t big[400];
	memset(big, 'z', sizeof(big));
	for (int i = 0; i < 4; i++) deliver_data(REPL, Z_PORT_DATA, 1, big, sizeof(big));
	step();
	int acks = 0;
	for (int i = mark; i < nout; i++) if (out[i].to == REPL && out[i].subject == Z_PORT_DATA_ACK) acks++;
	CK(acks < 4, "what does not fit is held, unacked (%d acked)", acks);
	uint32_t total = 0;
	for (int round = 0; round < 20; round++) {
		int m2 = nout;
		steps(1);
		total += collect(NET, conn, m2, buf, sizeof(buf));
	}
	total += collect(NET, conn, mark, buf, 0) * 0;
	acks = 0;
	for (int i = mark; i < nout; i++) if (out[i].to == REPL && out[i].subject == Z_PORT_DATA_ACK) acks++;
	{
		uint32_t all = 0;
		for (int i = mark; i < nout; i++)
			if (out[i].to == NET && out[i].subject == Z_PORT_DATA && out[i].tag == conn) all += out[i].len;
		CK(acks == 4 && all == 1600, "all of it went out, every send acked (%d, %u bytes)", acks, all);
	}

	// One message far larger than any buffer here -- posix sends up to
	// 4 KB at once (OUT_BATCH). All-or-nothing, it could never fit, and
	// `help` and `vi` hung on hardware.
	mark = nout;
	{
		static uint8_t huge[4000];
		for (int i = 0; i < (int)sizeof(huge); i++) huge[i] = (uint8_t)('A' + i % 26);
		deliver_data(REPL, Z_PORT_DATA, 1, huge, sizeof(huge));
		uint32_t all = 0;
		static uint8_t got4k[8192];
		for (int round = 0; round < 40; round++) {
			int m2 = nout;
			step();
			for (int i = m2; i < nout; i++)
				if (out[i].to == NET && out[i].subject == Z_PORT_DATA && out[i].tag == conn) {
					memcpy(got4k + all, out[i].data, out[i].len);
					all += out[i].len;
					deliver(NET, Z_PORT_DATA_ACK, conn, z_obj_none());
				}
		}
		int a4 = 0;
		for (int i = mark; i < nout; i++) if (out[i].to == REPL && out[i].subject == Z_PORT_DATA_ACK) a4++;
		CK(all == sizeof(huge) && !memcmp(got4k, huge, sizeof(huge)) && a4 == 1,
			"a 4000-byte message goes out whole, in pieces, acked once (%u, %d acks)", all, a4);
	}

	// ... and the same from the peer: a paste of 600 bytes in one segment
	mark = nout;
	{
		static char paste[600];
		memset(paste, 'p', sizeof(paste));
		type(conn, paste, sizeof(paste));
		uint32_t all = 0;
		for (int round = 0; round < 20; round++) {
			int m2 = nout;
			step();
			all += collect(REPL, 1, m2, buf, sizeof(buf));
		}
		int a6 = 0;
		for (int i = mark; i < nout; i++) if (out[i].to == NET && out[i].subject == Z_PORT_DATA_ACK) a6++;
		CK(all == sizeof(paste) && a6 == 1, "a 600-byte paste reaches the port whole, acked once (%u, %d)", all, a6);
	}

	// DATA from a process that is not this session's backend, tagged like
	// it: a stale provider sending to a reused pid
	mark = nout;
	deliver_data(77, Z_PORT_DATA, 1, "log line\n", 9);
	step();
	{
		int a = find(77, Z_PORT_DATA_ACK, mark), cl = find(77, Z_PORT_CLOSE, mark);
		n = collect(NET, conn, mark, buf, sizeof(buf));
		CK(a >= 0 && cl >= 0 && out[cl].tag == 1 && !contains(buf, n, "log line", 8),
			"a stranger's DATA: acked, answered with CLOSE, and not shown");
	}

	// repl ends the session
	mark = nout;
	deliver(REPL, Z_PORT_CLOSE, 1, z_obj_none());
	steps(3);
	CK(find(NET, Z_PORT_CLOSE, mark) >= 0, "repl closing closes the TCP side");
	collect(NET, conn, mark, buf, sizeof(buf));
	steps(2);
	CK(!sess[conn - 1].state, "and the session is freed once acked");

	// -- 5. three wrong passwords --
	mark = nout;
	connect_from(4, 0xC0A80107, 23, true);
	step();
	conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
	for (int i = 0; i < 3; i++) { type(conn, "bad\r", 4); steps(2); collect(NET, conn, mark, buf, sizeof(buf)); mark = nout; }
	steps(2);
	CK(find(NET, Z_PORT_CLOSE, 0) >= 0 && (sess[conn - 1].state == S_DRAIN || !sess[conn - 1].state),
		"closed after three");
	collect(NET, conn, 0, buf, sizeof(buf));
	steps(3);

	// -- 6. the kernel says wait --
	mark = nout;
	connect_from(5, 0xC0A80108, 23, true);
	step();
	conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
	auth_force = Z_AUTH_E_WAIT;
	type(conn, "anything\r", 9);
	steps(3);
	n = collect(NET, conn, mark, buf, sizeof(buf));
	CK(contains(buf, n, "try again in 4 s", 16), "the delay is passed on");
	auth_force = 0;
	steps(3);
	collect(NET, conn, 0, buf, sizeof(buf));
	steps(3);

	// -- 7. too slow to log in --
	mark = nout;
	connect_from(6, 0xC0A80109, 23, true);
	step();
	conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
	ticks += 61 * Z_TICK_HZ;
	steps(2);
	n = collect(NET, conn, mark, buf, sizeof(buf));
	CK(contains(buf, n, "timed out", 9) && find(NET, Z_PORT_CLOSE, mark) >= 0, "a login timeout closes");

	// -- 8. the peer vanishes mid-login: nothing leaks --
	mark = nout;
	connect_from(7, 0xC0A8010A, 23, true);
	step();
	conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
	deliver(NET, Z_PORT_CLOSE, conn, z_obj_none());
	steps(2);
	CK(sess[conn - 1].state == S_DRAIN, "waits for its sends to be acked");
	ticks += 6 * Z_TICK_HZ;
	steps(1);
	CK(!sess[conn - 1].state, "and is freed after a while even if they never are");

	// -- 9. two logins while repl is not running start it once --
	repl_up = false;
	proc_runs = 0;
	{
		uint32_t c1, c2;
		mark = nout;
		connect_from(8, 0xC0A8010B, 23, true);
		connect_from(9, 0xC0A8010C, 23, true);
		step();
		c1 = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		c2 = out[find(NET, Z_PORT_CONNECTED, find(NET, Z_PORT_CONNECTED, mark) + 1)].u32;
		type(c1, "correct horse\r", 14);
		type(c2, "correct horse\r", 14);
		steps(3);
		CK(sess[c1 - 1].state == S_LAUNCH && sess[c2 - 1].state == S_LAUNCH, "both waiting for repl");
		CK(proc_runs == 1, "repl started once for both (%d)", proc_runs);
		repl_up = true;
		mark = nout;
		step();
		int a = find(REPL, Z_PORT_CONNECT, mark), b2 = a >= 0 ? find(REPL, Z_PORT_CONNECT, a + 1) : -1;
		CK(a >= 0 && b2 >= 0, "both connect once it is up");
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(3));
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(4));
		step();
		CK(sess[c1 - 1].app.conn_id == 3 && sess[c2 - 1].app.conn_id == 4,
			"answers matched to CONNECTs in order");
	}

	// -- 10. the terminal handoff: the backend gives the session to a
	// child (posix -> vi), and calls it back when the child exits --
	{
		uint32_t cs;
		mark = nout;
		connect_from(10, 0xC0A8010D, 23, true);
		step();
		cs = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		type(cs, "correct horse\r", 14);
		steps(2);
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(5));
		steps(2);
		CK(sess[cs - 1].state == S_OPEN && sess[cs - 1].app.conn_id == 5, "a session on the backend");
		ticks += 10;                // it types after the other sessions did
		type(cs, "vi\r", 3);
		steps(2);                   // sent to the backend, and NOT acked yet

		mark = nout;
		deliver(REPL, Z_TERM_SET_PORT, 0, z_obj_str("vi0"));
		step();
		int cl = find(REPL, Z_PORT_CLOSE, mark), cv = find(22, Z_PORT_CONNECT, mark);
		CK(cl >= 0 && out[cl].tag == 5, "handed off: the session leaves the backend (CLOSE), as term does");
		CK(cv >= 0, "and connects to the child's port");
		deliver(22, Z_PORT_CONNECTED, 0, z_obj_uint32(1));
		steps(2);
		CK(sess[cs - 1].state == S_OPEN && sess[cs - 1].app_pid == 22, "now on the child");
		CK(sess[cs - 1].old_app.pending_count == 1, "the old port's unacked send is remembered");
		deliver(REPL, Z_PORT_DATA_ACK, 5, z_obj_none());      // a late ack for the old port
		step();
		CK(sess[cs - 1].old_app.pending_count == 0, "late acks from the old backend still free its sends");

		mark = nout;
		type(cs, ":q\r", 3);
		deliver_data(22, Z_PORT_DATA, 1, "\x1b[2J~", 6);
		steps(2);
		n = collect(22, 1, mark, buf, sizeof(buf));
		CK(n == 3 && !memcmp(buf, ":q\r", 3), "keys go to the child");
		n = collect(NET, cs, mark, buf, sizeof(buf));
		CK(contains(buf, n, "\x1b[2J~", 6), "and its screen to the peer");

		mark = nout;
		deliver(22, Z_PORT_CLOSE, 1, z_obj_none());          // vi exits
		steps(2);
		CK(sess[cs - 1].state == S_AWAY && find(NET, Z_PORT_CLOSE, mark) < 0,
			"the child closing does not end the session: it waits to be called back");
		type(cs, "ls\r", 3);                                  // typed meanwhile
		steps(1);
		mark = nout;
		z_obj_t map = z_obj_map(2);
		z_map_set(&map, "name", z_obj_str("repl0"));
		z_map_set(&map, "arg", z_obj_none());
		deliver(REPL, Z_TERM_SET_PORT, 0, map);               // posix's call-back form
		step();
		CK(find(REPL, Z_PORT_CONNECT, mark) >= 0, "called back: reconnects to the backend");
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(5));
		mark = nout;
		steps(2);
		n = collect(REPL, 5, mark, buf, sizeof(buf));
		CK(sess[cs - 1].state == S_OPEN && sess[cs - 1].app_pid == REPL && !sess[cs - 1].prev_pid &&
			n == 3 && !memcmp(buf, "ls\r", 3), "back on the backend, with what was typed while away");

		// ... a child that exits WITHOUT closing its port, as vi does on
		// the board: the parent's call-back must still bring it home
		deliver(REPL, Z_TERM_SET_PORT, 0, z_obj_str("vi0"));
		step();
		deliver(22, Z_PORT_CONNECTED, 0, z_obj_uint32(3));
		steps(2);
		CK(sess[cs - 1].state == S_OPEN && sess[cs - 1].app_pid == 22, "on the child again");
		type(cs, "zz", 2);                  // sent to the child, never acked: it is gone
		steps(2);
		CK(sess[cs - 1].app.pending_count >= 1, "a send to the child is outstanding");
		mark = nout;
		deliver(REPL, Z_TERM_SET_PORT, 0, z_obj_str("repl0"));   // no CLOSE from vi first
		step();
		CK(find(REPL, Z_PORT_CONNECT, mark) >= 0 && find(22, Z_PORT_CLOSE, mark) < 0,
			"called back while still attached to the dead child: reconnects, says nothing to it");
		type(cs, "ls\r", 3);                // typed meanwhile
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(5));
		mark = nout;
		steps(2);
		n = collect(REPL, 5, mark, buf, sizeof(buf));
		CK(sess[cs - 1].state == S_OPEN && sess[cs - 1].app_pid == REPL && !sess[cs - 1].prev_pid &&
			n == 3 && !memcmp(buf, "ls\r", 3), "home, with what was typed (%u)", n);

		// ... and if the call-back never comes, it goes back by itself
		deliver(REPL, Z_TERM_SET_PORT, 0, z_obj_str("vi0"));
		step();
		deliver(22, Z_PORT_CONNECTED, 0, z_obj_uint32(2));
		steps(2);
		deliver(22, Z_PORT_CLOSE, 2, z_obj_none());
		steps(2);
		mark = nout;
		ticks += 11 * Z_TICK_HZ;
		steps(2);
		CK(find(REPL, Z_PORT_CONNECT, mark) >= 0 && !strcmp(sess[cs - 1].target, "repl0"),
			"no call-back: reconnects to the backend on its own");
	}

	// -- 11. echo finishes a half-close: everything back, then closed --
	{
		uint32_t ce;
		mark = nout;
		connect_from(11, 0xC0A8010E, 7, true);
		step();
		ce = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		static char blob3k[3000];
		memset(blob3k, 'e', sizeof(blob3k));
		for (int i = 0; i < 6; i++) type(ce, blob3k + i * 500, 500);
		deliver(NET, Z_NET_EOF, ce, z_obj_none());
		uint32_t all = 0;
		for (int round = 0; round < 40; round++) {
			int m2 = nout;
			step();
			all += collect(NET, ce, m2, buf, sizeof(buf));
		}
		CK(all == 3000 && find(NET, Z_PORT_CLOSE, mark) >= 0,
			"echo, then EOF: all 3000 back, then closed (%u)", all);
	}

	// -- 12. HTTP --
	// A clean slate: the sessions the sections above left open are
	// closed by their peers, and their sends acked, so all six slots
	// are free again.
	for (int i = 0; i < MAX_SESS; i++)
		if (sess[i].state) deliver(NET, Z_PORT_CLOSE, (uint32_t)i + 1, z_obj_none());
	steps(3);
	ticks += 6 * Z_TICK_HZ;         // past the drain deadline (acks from gone peers)
	steps(3);
	for (int i = 0; i < MAX_SESS; i++) CK(!sess[i].state, "slot %d free before the HTTP tests", i);
	fake_fs_init();
	cfg_http = "80 /www/";
	read_config();
	CK(svc_http.on && svc_http.port == 80 && !strcmp(http_root, "/www"), "http: port and root parsed");
	deliver(NET, Z_NET_LISTEN_REPLY, 80, z_obj_uint32(0));
	deliver(NET, Z_NET_LISTEN_REPLY, 23, z_obj_uint32(0));
	deliver(NET, Z_NET_LISTEN_REPLY, 7, z_obj_uint32(0));
	step();
	CK(svc_http.listening, "listening on 80");
	{
		static uint32_t relay_id = 20;
		static uint8_t resp[16384];
		// one request; returns the response and whether it was closed
		#define HTTP_REQ(req, got, closed) do { \
			uint32_t _c; int _m = nout; \
			connect_from(relay_id++ % 16, 0xC0A80120, 80, true); \
			step(); \
			if (find(NET, Z_PORT_CONNECTED, _m) < 0) { printf("FAIL: not accepted\n"); fails++; break; } \
			_c = out[find(NET, Z_PORT_CONNECTED, _m)].u32; \
			type(_c, req, (uint32_t)strlen(req)); \
			got = 0; \
			for (int _r = 0; _r < 60; _r++) { int _m2 = nout; step(); got += collect(NET, _c, _m2, resp + got, sizeof(resp) - got); } \
			closed = find(NET, Z_PORT_CLOSE, _m) >= 0; \
			resp[got] = 0; \
		} while (0)
		uint32_t got; bool closed;

		HTTP_REQ("GET /index.html HTTP/1.1\r\nHost: z\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 200 OK\r\n", 17) && strstr((char *)resp, "Content-Type: text/html") &&
			strstr((char *)resp, "Content-Length: 14\r\n") && strstr((char *)resp, "\r\n\r\n<h1>home</h1>\n") && closed,
			"GET a file: 200, type, length, body, closed");

		HTTP_REQ("GET / HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "200 OK") && strstr((char *)resp, "<h1>home</h1>"), "/ is index.html");
		HTTP_REQ("GET /sub/ HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "<p>sub</p>"), "a directory's index.html");
		HTTP_REQ("GET /sub HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "<p>sub</p>"), "... without the trailing slash too");
		HTTP_REQ("GET /old/ HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "<p>htm</p>"), "index.htm when there is no index.html");
		HTTP_REQ("GET /style.CSS?v=2#x HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "Content-Type: text/css") && strstr((char *)resp, "color:red"),
			"the query and fragment ignored; the extension matched in any case");
		HTTP_REQ("GET /a%20b.txt HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "200 OK") && strstr((char *)resp, "text/plain") && strstr((char *)resp, "space"),
			"%%20 decoded");

		HTTP_REQ("GET /big.bin HTTP/1.0\r\n\r\n", got, closed);
		{
			char *body = strstr((char *)resp, "\r\n\r\n");
			uint32_t blen = 0; const uint8_t *want = fake_fs_data("/www/big.bin", &blen);
			CK(body && strstr((char *)resp, "application/octet-stream") && strstr((char *)resp, "Content-Length: 5000") &&
				got - (uint32_t)(body + 4 - (char *)resp) == blen && !memcmp(body + 4, want, blen) && closed,
				"a 5000-byte file, streamed whole (%u bytes of response)", got);
		}

		HTTP_REQ("HEAD /index.html HTTP/1.0\r\n\r\n", got, closed);
		CK(strstr((char *)resp, "Content-Length: 14") && !strstr((char *)resp, "<h1>") && closed,
			"HEAD: the headers, no body");

		HTTP_REQ("GET /nope.html HTTP/1.0\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 404", 12) && closed, "404");
		HTTP_REQ("POST / HTTP/1.0\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 405", 12) && strstr((char *)resp, "Allow: GET, HEAD"), "405, with Allow");
		HTTP_REQ("GET /../secret.txt HTTP/1.0\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 400", 12) && !strstr((char *)resp, "no\n"), "/../ refused");
		HTTP_REQ("GET /%2e%2e/secret.txt HTTP/1.0\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 400", 12), "... and %%2e%%2e, decoded first");
		HTTP_REQ("GET /sub/..%2f..%2fsecret.txt HTTP/1.0\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 400", 12), "... and ..%%2f");
		HTTP_REQ("GET /x%00y HTTP/1.0\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 400", 12), "%%00 refused");
		HTTP_REQ("GET\r\n\r\n", got, closed);
		CK(!strncmp((char *)resp, "HTTP/1.0 400", 12), "a malformed request line: 400");
		HTTP_REQ("GET /index.html HTTP/1.0\n\n", got, closed);
		CK(strstr((char *)resp, "200 OK"), "bare LF line ends accepted");

		// a request in pieces
		{
			uint32_t c; int m0 = nout;
			connect_from(relay_id++ % 16, 0xC0A80120, 80, true);
			step();
			c = out[find(NET, Z_PORT_CONNECTED, m0)].u32;
			type(c, "GET /inde", 9); steps(2);
			type(c, "x.html HTTP/1.0\r\nX: y\r", 22); steps(2);
			CK(sess[c - 1].http == H_REQ, "no answer until the blank line");
			type(c, "\n\r\n", 3); steps(4);
			uint32_t g = collect(NET, c, m0, resp, sizeof(resp));
			resp[g] = 0;
			CK(strstr((char *)resp, "<h1>home</h1>"), "a request in pieces");
		}

		// too slow
		{
			uint32_t c; int m0 = nout;
			connect_from(relay_id++ % 16, 0xC0A80120, 80, true);
			step();
			c = out[find(NET, Z_PORT_CONNECTED, m0)].u32;
			type(c, "GET / HT", 8); steps(2);
			ticks += 11 * Z_TICK_HZ;
			steps(3);
			uint32_t g = collect(NET, c, m0, resp, sizeof(resp));
			resp[g] = 0;
			CK(!strncmp((char *)resp, "HTTP/1.0 408", 12) && find(NET, Z_PORT_CLOSE, m0) >= 0, "408 after 10 s");
		}

		// the peer half-closes before the request ends
		{
			uint32_t c; int m0 = nout;
			connect_from(relay_id++ % 16, 0xC0A80120, 80, true);
			step();
			c = out[find(NET, Z_PORT_CONNECTED, m0)].u32;
			type(c, "GET / HT", 8);
			deliver(NET, Z_NET_EOF, c, z_obj_none());
			steps(3);
			uint32_t g = collect(NET, c, m0, resp, sizeof(resp));
			resp[g] = 0;
			CK(!strncmp((char *)resp, "HTTP/1.0 400", 12), "EOF mid-request: 400");
		}

		// the request half-closed after it ends (curl can): answered in full
		{
			uint32_t c; int m0 = nout;
			connect_from(relay_id++ % 16, 0xC0A80120, 80, true);
			step();
			c = out[find(NET, Z_PORT_CONNECTED, m0)].u32;
			type(c, "GET /big.bin HTTP/1.0\r\n\r\n", 25);
			deliver(NET, Z_NET_EOF, c, z_obj_none());
			uint32_t g = 0;
			for (int r = 0; r < 60; r++) { int m2 = nout; step(); g += collect(NET, c, m2, resp + g, sizeof(resp) - g); }
			CK(g > 5000 && strstr((char *)resp, "200 OK"), "a half-closed request is still answered whole (%u)", g);
		}

		// too long
		{
			static char huge[1400];
			memset(huge, 'a', sizeof(huge) - 1);
			memcpy(huge, "GET /", 5);
			uint32_t c; int m0 = nout;
			connect_from(relay_id++ % 16, 0xC0A80120, 80, true);
			step();
			c = out[find(NET, Z_PORT_CONNECTED, m0)].u32;
			type(c, huge, sizeof(huge) - 1);
			steps(4);
			uint32_t g = collect(NET, c, m0, resp, sizeof(resp));
			resp[g] = 0;
			CK(!strncmp((char *)resp, "HTTP/1.0 431", 12) || !strncmp((char *)resp, "HTTP/1.0 414", 12),
				"an oversized request refused");
		}

		// no file handle free: 503, not a misleading 404
		fake_fs_limit = 0;
		HTTP_REQ("GET /index.html HTTP/1.0\r\n\r\n", got, closed);
		fake_fs_limit = 8;
		CK(!strncmp((char *)resp, "HTTP/1.0 404", 12) || !strncmp((char *)resp, "HTTP/1.0 503", 12),
			"no free file handle: refused (%.12s)", resp);

		steps(10);
		CK(fake_fs_open == 0, "every file closed afterwards (%d open)", fake_fs_open);
		{
			int busy = 0;
			for (int i = 0; i < MAX_SESS; i++) if (sess[i].state) busy++;
			CK(busy == 0, "every session freed once its sends were acked -- not by the 5 s "
				"deadline, which would lose their memory (%d still held)", busy);
		}

		// the subnet rule applies to HTTP too
		{
			int m0 = nout;
			connect_from(relay_id++ % 16, 0x08080808, 80, false);
			step();
			CK(find(NET, Z_PORT_REFUSED, m0) >= 0, "http from off the subnet refused");
		}
	}

	// -- 13. SSH: the real client engine, through netserve, to a backend --
	{
		// a clean slate again
		for (int i = 0; i < MAX_SESS; i++)
			if (sess[i].state) deliver(NET, Z_PORT_CLOSE, (uint32_t)i + 1, z_obj_none());
		steps(3); ticks += 6 * Z_TICK_HZ; steps(3);

		fake_rng_secure = false;
		cfg_ssh = "22 repl0";
		read_config();
		CK(!svc_ssh.on, "no seeded random source: ssh stays off");
		fake_rng_secure = true;
		read_config();
		CK(svc_ssh.on && svc_ssh.port == 22 && !strcmp(svc_ssh.target, "repl0"), "ssh on, port and target parsed");
		uint8_t first_seed[32];
		memcpy(first_seed, host_seed, 32);
		read_config();
		CK(!memcmp(first_seed, host_seed, 32), "the host key is kept, not made anew each start");
		deliver(NET, Z_NET_LISTEN_REPLY, 22, z_obj_uint32(0));
		step();
		CK(svc_ssh.listening, "listening on 22");
	}
	{
		static ssh_proto_t cli;
		static struct cstate C;
		cli_for_ev = &cli;
		static uint32_t conn;
		static int fed;         // messages to net already given to the client
		memset(&C, 0, sizeof(C));

		// the client's bytes go out as DATA from "net"; netserve's DATA to
		// net comes back into the client

		mark = nout;
		fed = mark;
		connect_from(12, 0xC0A80130, 22, true);
		step();
		c = find(NET, Z_PORT_CONNECTED, mark);
		CK(c >= 0, "ssh connection accepted");
		conn = out[c].u32;

		ssh_pw_for_test = "correct horse";
		ssh_proto_init(&cli, &C, cw, cev, cr, "phil");
		#define RUN(rounds) do { for (int _r = 0; _r < (rounds); _r++) { \
			if (c_out_n) { \
				uint32_t _o = 0; \
				while (_o < c_out_n) { uint32_t _k = c_out_n - _o > 500 ? 500 : c_out_n - _o; type(conn, (const char *)c_out + _o, _k); _o += _k; } \
				c_out_n = 0; } \
			step(); \
			for (; fed < nout; fed++) \
				if (out[fed].to == NET && out[fed].subject == Z_PORT_DATA && out[fed].tag == conn) { \
					ssh_proto_feed(&cli, out[fed].data, out[fed].len); \
					deliver(NET, Z_PORT_DATA_ACK, conn, z_obj_none()); } \
		} } while (0)
		RUN(40);
		CK(C.hostkey == 1, "the client saw the host key");
		c = find(REPL, Z_PORT_CONNECT, mark);
		CK(C.ready == 1 && c >= 0, "logged in; the session connected to repl0");
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(9));
		RUN(4);

		// keys to repl
		mark = nout;
		ssh_proto_send(&cli, (const uint8_t *)"(+ 1 2)\r", 8);
		RUN(4);
		n = collect(REPL, 9, mark, buf, sizeof(buf));
		CK(n == 8 && !memcmp(buf, "(+ 1 2)\r", 8), "keystrokes reach repl, unchanged (%u)", n);

		// a large answer from repl, as posix sends (4 KB at once)
		{
			static uint8_t huge[4096];
			for (int i = 0; i < 4096; i++) huge[i] = (uint8_t)('a' + i % 26);
			uint32_t before = C.got_n;
			deliver_data(REPL, Z_PORT_DATA, 9, huge, sizeof(huge));
			RUN(60);
			CK(C.got_n - before == 4096 && !memcmp(C.got + before, huge, 4096),
				"4 KB from repl reaches the client whole (%u)", C.got_n - before);
		}

		// more input than the window, with repl slow to ack
		{
			static uint8_t paste[3000];
			memset(paste, 'p', sizeof(paste));
			mark = nout;
			uint32_t off = 0;
			int acked = mark;
			for (int r = 0; r < 80; r++) {
				if (off < sizeof(paste)) {
					uint32_t k = sizeof(paste) - off > 400 ? 400 : sizeof(paste) - off;
					if (ssh_proto_send(&cli, paste + off, k)) off += k;
				}
				RUN(1);
				for (; acked < nout; acked++)       /* repl acks what reaches it */
					if (out[acked].to == REPL && out[acked].subject == Z_PORT_DATA && out[acked].tag == 9)
						deliver(REPL, Z_PORT_DATA_ACK, 9, z_obj_none());
			}
			uint32_t got = 0;
			for (int i = mark; i < nout; i++)
				if (out[i].to == REPL && out[i].subject == Z_PORT_DATA && out[i].tag == 9) got += out[i].len;
			CK(off == sizeof(paste) && got == sizeof(paste),
				"a 3000-byte paste goes through the window to repl whole (%u sent, %u arrived)", off, got);
		}

		// repl quits: the client is told, and the connection closes
		mark = nout;
		deliver(REPL, Z_PORT_CLOSE, 9, z_obj_none());
		RUN(10);
		CK(C.closed == 1, "repl closing ends the client's session (%s)", C.why);
		CK(find(NET, Z_PORT_CLOSE, mark) >= 0, "and the connection");
		steps(3);
		int used = 0;
		for (int i = 0; i < SSH_MAX; i++) used += ssh_pool[i].used;
		CK(used == 0, "the engine is released and wiped");

		// a wrong password three times
		memset(&C, 0, sizeof(C));
		mark = nout;
		fed = mark;
		connect_from(13, 0xC0A80131, 22, true);
		step();
		conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		ssh_pw_for_test = "wrong";
		ssh_proto_init(&cli, &C, cw, cev, cr, "phil");
		RUN(40);
		for (int i = 0; i < 4 && !C.closed; i++) { ssh_proto_password(&cli, "wrong"); RUN(20); }
		CK(C.closed == 1 && C.ready == 0, "three wrong passwords: disconnected (%s)", C.why);

		// too slow to log in
		memset(&C, 0, sizeof(C));
		mark = nout;
		fed = mark;
		connect_from(14, 0xC0A80132, 22, true);
		step();
		conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		ssh_pw_for_test = NULL;
		ssh_proto_init(&cli, &C, cw, cev, cr, "phil");
		RUN(40);
		ticks += 61 * Z_TICK_HZ;
		RUN(10);
		/* (our client, waiting on its owner for a password, holds what
		 * arrives unread -- so it is the server's side that shows it) */
		CK(find(NET, Z_PORT_CLOSE, mark) >= 0, "a login left waiting: the server closes it after 60 s");
		steps(4); ticks += 6 * Z_TICK_HZ; steps(4);
		used = 0;
		for (int i = 0; i < SSH_MAX; i++) used += ssh_pool[i].used;
		CK(used == 0, "no engine left held");
	}

	// -- 14. SSH keys: the policy and /user/authkeys --
	{
		static char akfile[1024];
		has_pw = false;
		cfg_ssh = "22 posix0";
		cfg_ssh_auth = "key";
		read_config();
		CK(svc_ssh.on && ssh_methods == SSHS_AUTH_PUBLICKEY, "keys only: ssh on without a password");
		cfg_ssh_auth = NULL;
		read_config();
		CK(!svc_ssh.on, "the default (both) still needs the password");
		has_pw = true;
		read_config();
		CK(svc_ssh.on && ssh_methods == (SSHS_AUTH_PASSWORD | SSHS_AUTH_PUBLICKEY), "both, with a password");
		cfg_ssh_auth = "password";
		read_config();
		CK(ssh_methods == SSHS_AUTH_PASSWORD, "passwords only");
		cfg_ssh_auth = NULL;

		fake_fs_authkeys(NULL);
		CK(!ssh_pk_check(NULL, "phil", "ssh-ed25519", ED_BLOB, sizeof(ED_BLOB)), "no /user/authkeys: no key logs in");
		snprintf(akfile, sizeof(akfile), "%s\n", ED_LINE);
		fake_fs_authkeys(akfile);
		CK(ssh_pk_check(NULL, "phil", "ssh-ed25519", ED_BLOB, sizeof(ED_BLOB)), "a listed key is found in the file");
		CK(!ssh_pk_check(NULL, "phil", "rsa-sha2-256", RSA_BLOB, sizeof(RSA_BLOB)), "an unlisted one is not");
		snprintf(akfile, sizeof(akfile), "%s\n%s\nfrom=\"x\" %s\n", ED_LINE, RSA_LINE, ED_LINE);
		CK(ssh_pk_check(NULL, "phil", "rsa-sha2-256", RSA_BLOB, sizeof(RSA_BLOB)),
			"an edit takes effect at the next check");
		uint32_t reported = ak_reported;
		ssh_pk_check(NULL, "phil", "rsa-sha2-256", RSA_BLOB, sizeof(RSA_BLOB));
		CK(ak_reported == reported && ak_list.refused == 1, "a refused line is reported once, not at every login");
		fake_fs_authkeys(NULL);
	}

	// -- 15. the backend dies without a CLOSE --
	{
		for (int i = 0; i < MAX_SESS; i++)
			if (sess[i].state) deliver(NET, Z_PORT_CLOSE, (uint32_t)i + 1, z_obj_none());
		steps(3); ticks += 6 * Z_TICK_HZ; steps(3);
		repl_up = true;

		// telnet
		mark = nout;
		int conn_mark = mark;
		connect_from(15, 0xC0A80140, 23, true);
		step();
		uint32_t tc = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		type(tc, "correct horse\r", 14);
		steps(2);
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(6));
		steps(2);
		CK(sess[tc - 1].state == S_OPEN && sess[tc - 1].app.connected, "(a telnet session on repl)");
		type(tc, "(loop)\r", 7);                     // sent, never acked: repl is about to die
		steps(2);
		collect(NET, tc, conn_mark, buf, 0);        // net acks what it was sent, as the real one does
		steps(1);
		mark = nout;
		dead_pid = REPL;
		ticks += 2 * Z_TICK_HZ;
		steps(3);
		n = collect(NET, tc, mark, buf, sizeof(buf));
		CK(contains(buf, n, "process has gone", 16) && find(NET, Z_PORT_CLOSE, mark) >= 0,
			"telnet: the user is told, and the connection closed");
		CK(find(REPL, Z_PORT_CLOSE, mark) < 0, "nothing is sent to the dead process");
		steps(3);
		CK(sess[tc - 1].state == S_FREE, "the session freed, its unacked sends to repl with it");
		dead_pid = 0;
	}
	{
		// SSH: the message goes through the engine, and the client is told
		static ssh_proto_t cli;
		static struct cstate C;
		static uint32_t conn;
		static int fed;
		cli_for_ev = &cli;
		memset(&C, 0, sizeof(C));
		cfg_ssh = "22 repl0";
		cfg_ssh_auth = NULL;
		read_config();
		mark = nout; fed = mark;
		connect_from(16, 0xC0A80141, 22, true);
		step();
		conn = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		ssh_pw_for_test = "correct horse";
		c_out_n = 0;
		ssh_proto_init(&cli, &C, cw, cev, cr, "phil");
		RUN(40);
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(7));
		RUN(4);
		CK(C.ready == 1, "(an SSH session on repl)");
		dead_pid = REPL;
		ticks += 2 * Z_TICK_HZ;
		RUN(10);
		C.got[C.got_n < sizeof(C.got) ? C.got_n : sizeof(C.got) - 1] = 0;
		CK(contains(C.got, C.got_n, "process has gone", 16) && C.closed == 1,
			"ssh: the message arrives inside the encrypted channel, then the session ends (%s)", C.why);
		dead_pid = 0;
	}
	{
		// a child holding the terminal dies without closing: the session
		// waits for its parent's call-back, as when it closes
		for (int i = 0; i < MAX_SESS; i++)
			if (sess[i].state) deliver(NET, Z_PORT_CLOSE, (uint32_t)i + 1, z_obj_none());
		steps(3); ticks += 6 * Z_TICK_HZ; steps(3);
		mark = nout;
		connect_from(17, 0xC0A80142, 23, true);
		step();
		uint32_t tc = out[find(NET, Z_PORT_CONNECTED, mark)].u32;
		ticks += 10;
		type(tc, "correct horse\r", 14);
		steps(2);
		deliver(REPL, Z_PORT_CONNECTED, 0, z_obj_uint32(8));
		steps(2);
		deliver(REPL, Z_TERM_SET_PORT, 0, z_obj_str("vi0"));
		step();
		deliver(22, Z_PORT_CONNECTED, 0, z_obj_uint32(4));
		steps(2);
		CK(sess[tc - 1].app_pid == 22 && sess[tc - 1].prev_pid == REPL, "(the session handed to vi)");
		dead_pid = 22;
		ticks += 2 * Z_TICK_HZ;
		steps(3);
		CK(sess[tc - 1].state == S_AWAY, "vi dying silently: the session waits for posix's call-back");
		dead_pid = 0;
	}

	printf("netserve test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
